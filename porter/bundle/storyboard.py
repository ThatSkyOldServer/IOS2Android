import io
import json
import plistlib

from nibarchive import NIBArchiveParser


def controller_catalogue(source, prefix):
    """Recover archived controller identities and supported view graphs."""
    catalogue = {}
    members = set(source.namelist())
    for path in sorted(members):
        if not path.startswith(prefix) or not path.endswith('.storyboardc/Info.plist'):
            continue
        metadata = plistlib.loads(source.read(path))
        folder = path.rsplit('/', 1)[0] + '/'
        scenes = {}
        for identifier, name in metadata.get('UIViewControllerIdentifiersToNibNames', {}).items():
            nib_path = folder + name + '.nib'
            if nib_path not in members:
                nib_path += '/runtime.nib'
            archive = NIBArchiveParser().parse(io.BytesIO(source.read(nib_path)))
            objects = []
            for obj in archive.objects:
                fields = list(archive.get_object_values(obj))
                values = {archive.keys[field.key_index].name: field.data for field in fields}
                objects.append((archive.class_names[obj.class_name_index].name, values))

            def text(index):
                cls, fields = objects[index]
                if cls != 'NSString':
                    raise ValueError(f'Expected NSString in {path}/{name}')
                return fields['NS.bytes'].decode('utf-8')

            controller = None
            for cls, fields in objects:
                if cls == 'UIRuntimeOutletConnection' and text(fields['UILabel']) == 'sceneViewController':
                    controller = objects[fields['UIDestination']]
                    break
            if controller is None:
                raise ValueError(f'Unrepresented storyboard controller {path}: {identifier}')
            controller_class, fields = controller
            if controller_class != 'UIClassSwapper' and not controller_class.endswith('Controller'):
                raise ValueError(f'Unsupported controller archive class {controller_class} in {path}')
            scene = {'class': text(fields['UIClassName']) if controller_class == 'UIClassSwapper' else controller_class,
                     'base': text(fields['UIOriginalClassName']) if controller_class == 'UIClassSwapper' else controller_class,
                     'identifier': identifier, 'nib': text(fields['UINibName']) if 'UINibName' in fields else ''}
            if 'UINavigationBar' in fields:
                bar_class, bar = objects[fields['UINavigationBar']]
                if bar_class != 'UINavigationBar':
                    raise ValueError(f'Unrepresented navigation bar in {path}: {identifier}')
                scene['barStyle'] = bar.get('UIBarStyle', 0)
            try:
                _, _, graph = archived_scene(source, folder, name)
                scene['viewGraph'] = json.dumps(graph, separators=(',', ':'))
            except (KeyError, ValueError):
                pass
            scenes[identifier] = scene
        catalogue[folder[len(prefix):].rstrip('/')] = scenes
    return catalogue


def prepare_controller_catalogue(source, prefix, work):
    catalogue = controller_catalogue(source, prefix)
    encoded = json.dumps(catalogue, sort_keys=True, separators=(',', ':'))
    (work / 'storyboard_catalogue.h').write_text(
        'static const char storyboard_catalogue_json[] = ' + json.dumps(encoded) + ';\n', encoding='utf-8')
    (work / 'storyboard-catalogue.json').write_text(json.dumps(catalogue, indent=2) + '\n', encoding='utf-8')


def launch_scene(source, prefix, board):
    members = set(source.namelist())
    candidates = [prefix + board + '.storyboardc/', prefix + 'Base.lproj/' + board + '.storyboardc/']
    development = plistlib.loads(source.read(prefix + 'Info.plist')).get('CFBundleDevelopmentRegion')
    if development:
        candidates.append(prefix + development + '.lproj/' + board + '.storyboardc/')
    candidates += sorted(path.rsplit('/', 1)[0] + '/' for path in members if path.startswith(prefix) and path.endswith('/' + board + '.storyboardc/Info.plist'))
    folder = next((path for path in candidates if path + 'Info.plist' in members), None)
    if folder is None:
        raise ValueError('Storyboard resource was not found: ' + board)
    metadata = plistlib.loads(source.read(folder+'Info.plist'))
    entry = metadata['UIStoryboardDesignatedEntryPointIdentifier']
    filename = metadata['UIViewControllerIdentifiersToNibNames'][entry]
    return archived_scene(source, folder, filename)


def archived_scene(source, folder, filename):

    def load(name):
        path=folder+name+'.nib'
        if path not in source.namelist():path+='/runtime.nib'
        archive = NIBArchiveParser().parse(io.BytesIO(source.read(path)))
        objects = []
        for obj in archive.objects:
            fields = list(archive.get_object_values(obj))
            values = {archive.keys[field.key_index].name: field.data for field in fields}
            values['_items'] = [field.data for field in fields if archive.keys[field.key_index].name == 'UINibEncoderEmptyKey']
            objects.append((archive.class_names[obj.class_name_index].name, values))
        return objects

    def outlet(objects, name):
        for cls, attributes in objects:
            if cls == 'UIRuntimeOutletConnection' and string(objects, attributes['UILabel']) == name:
                return objects[attributes['UIDestination']]
        raise ValueError(f'Missing {name} outlet in {filename}')

    def string(objects, index):
        cls, values = objects[index]
        if cls != 'NSString':
            raise ValueError('Expected an archived NSString')
        return values['NS.bytes'].decode('utf8')

    objects = load(filename)
    cls, values = outlet(objects, 'sceneViewController')
    if cls != 'UIClassSwapper' and not cls.endswith('Controller'):
        raise ValueError(f'Unsupported scene controller {cls}')
    controller_class = cls
    view_objects = load(string(objects, values['UINibName'])) if 'UINibName' in values else objects
    view_class, root = outlet(view_objects, 'view') if 'UIView' not in values or view_objects is not objects else view_objects[values['UIView']]
    root_index = next(index for index, (_, value) in enumerate(view_objects) if value is root)
    nodes, mapping, constraints = [], {}, []

    def colour(index):
        if index is None:
            return None
        cls, fields = view_objects[index]
        if cls != 'UIColor':
            raise ValueError(f'Unsupported startup colour {cls}')
        alpha = float(fields.get('UIAlpha', 1))
        if 'UIWhite' in fields:
            white = float(fields['UIWhite'])
            return [white, white, white, alpha]
        if 'UIRed' in fields:
            return [float(fields.get(key, 0)) for key in ('UIRed', 'UIGreen', 'UIBlue')] + [alpha]
        raise ValueError('Startup colour has no archived components')

    def visit(index, parent):
        if index in mapping:
            return
        cls, fields = view_objects[index]
        actual=string(view_objects,fields['UIClassName']) if cls=='UIClassSwapper' else cls
        base=string(view_objects,fields['UIOriginalClassName']) if cls=='UIClassSwapper' else cls
        if base not in ('UIView', 'UIImageView', 'UILabel', '_UILayoutGuide', 'UILayoutGuide'):
            raise ValueError(f'Unsupported startup view class {cls}')
        local = len(nodes)
        mapping[index] = local
        bounds = fields.get('UIBounds', [0, 0, 0, 0])
        center = fields.get('UICenter', [bounds[2] / 2, bounds[3] / 2])
        image = ''
        if 'UIImage' in fields:
            image_cls, image_values = view_objects[fields['UIImage']]
            if image_cls != 'UIImageNibPlaceholder':
                raise ValueError(f'Unsupported startup image {image_cls}')
            image = string(view_objects, image_values['UIResourceName'])
        nodes.append({'class': actual, 'base': base, 'parent': parent, 'tag': fields.get('UITag', 0),
                      'hidden': fields.get('UIHidden', False) or base in ('_UILayoutGuide','UILayoutGuide'), 'alpha': fields.get('UIAlpha', 1),
                      'contentMode': fields.get('UIContentMode', 0), 'autoresizing': fields.get('UIAutoresizingMask', 0),
                      'interaction': not fields.get('UIUserInteractionDisabled', False), 'image': image,
                      'background': colour(fields.get('UIBackgroundColor')),
                      'frame': [center[0] - bounds[2] / 2, center[1] - bounds[3] / 2, bounds[2], bounds[3]]})
        if base=='UILabel':
            nodes[-1]['text']=string(view_objects,fields['UIText']) if 'UIText' in fields else ''
            nodes[-1]['textColor']=colour(fields.get('UITextColor'))
            nodes[-1]['alignment']=fields.get('UITextAlignment',0)
            nodes[-1]['lines']=fields.get('UINumberOfLines',1)
        for child in view_objects[fields['UISubviews']][1]['_items'] if 'UISubviews' in fields else []:
            visit(child, local)

    visit(root_index, -1)
    for index,(cls,fields) in enumerate(view_objects):
        if cls in ('UILayoutGuide','_UILayoutGuide') and fields.get('UILayoutGuideOwningView') in mapping:
            visit(index,mapping[fields['UILayoutGuideOwningView']])
    for cls, fields in view_objects:
        if cls not in ('NSLayoutConstraint', '_UILayoutSupportConstraint'):
            continue
        first = fields.get('NSFirstItem')
        second = fields.get('NSSecondItem')
        if first not in mapping or (second is not None and second not in mapping):
            raise ValueError('Startup constraint references an unrepresented view')
        constraints.append({'first': mapping[first], 'second': mapping[second] if second is not None else -1,
                            'attribute': fields.get('NSFirstAttributeV2', fields.get('NSFirstAttribute')),
                            'otherAttribute': fields.get('NSSecondAttributeV2', fields.get('NSSecondAttribute', 0)),
                            'multiplier': fields.get('NSMultiplier', 1),
                            'constant': fields.get('NSConstantV2', fields.get('NSConstant', 0)),
                            'relation': fields.get('NSRelation', 0), 'priority': fields.get('NSPriority', 1000)})
    return string(objects, values['UIClassName']) if controller_class == 'UIClassSwapper' else controller_class, nodes[0]['class'], {'views': nodes, 'constraints': constraints}


def scene_layout(scene):
    count = len(scene['views']) * 4
    basis = [None] * count

    def insert(row, required):
        for index in range(count):
            factor = row[index]
            if abs(factor) < 1e-9:
                continue
            if basis[index] is None:
                basis[index] = [value / factor for value in row]
                return
            row = [value - factor * pivot for value, pivot in zip(row, basis[index])]
        if required and any(abs(value) > 1e-5 for value in row[count:]):
            raise ValueError('Conflicting required launch constraints')

    def attribute(row, item, kind, factor):
        if item < 0:
            return
        terms = {1: (1, 0, 0, 0), 2: (1, 0, 1, 0), 3: (0, 1, 0, 0),
                 4: (0, 1, 0, 1), 5: (1, 0, 0, 0), 6: (1, 0, 1, 0),
                 7: (0, 0, 1, 0), 8: (0, 0, 0, 1), 9: (1, 0, .5, 0), 10: (0, 1, 0, .5)}
        if kind not in terms:
            raise ValueError('Unsupported launch constraint attribute')
        for index, value in enumerate(terms[kind]):
            row[item * 4 + index] += factor * value
        axis = 0 if kind in (1, 2, 5, 6, 9) else 1 if kind in (3, 4, 10) else None
        if axis is not None:
            for _ in range(len(scene['views'])):
                parent = scene['views'][item]['parent']
                if parent < 0:
                    break
                if parent >= item:
                    raise ValueError('Invalid archived view hierarchy')
                row[parent * 4 + axis] += factor
                item = parent

    for index in range(4):
        row = [0.] * (count + 3)
        row[index] = 1
        if index >= 2:
            row[count + index - 2] = 1
        insert(row, True)
    for constraint in sorted(scene['constraints'], key=lambda item: -item['priority']):
        if constraint['relation']:
            raise ValueError('Unsupported launch constraint relation')
        row = [0.] * (count + 3)
        row[-1] = constraint['constant']
        attribute(row, constraint['first'], constraint['attribute'], 1)
        attribute(row, constraint['second'], constraint['otherAttribute'], -constraint['multiplier'])
        insert(row, constraint['priority'] == 1000)
    for item, view in enumerate(scene['views']):
        for index, value in enumerate(view['frame']):
            row = [0.] * (count + 3)
            row[item * 4 + index] = 1
            row[-1] = value
            insert(row, False)
    values = [[0., 0., 0.] for _ in range(count)]
    for index in reversed(range(count)):
        row = basis[index]
        values[index] = [row[count + axis] - sum(row[j] * values[j][axis] for j in range(index + 1, count)) for axis in range(3)]
    frames = [values[index:index + 4] for index in range(0, count, 4)]
    for index, view in enumerate(scene['views']):
        parent = view['parent']
        if parent >= 0:
            if parent >= index:
                raise ValueError('Invalid archived view hierarchy')
            for axis in (0, 1):
                frames[index][axis] = [value + ancestor for value, ancestor in zip(frames[index][axis], frames[parent][axis])]
    return frames


def prepare_launch_screen(source, prefix, info, resources, catalog):
    from .identity import normalize_png

    board = info.get('UILaunchStoryboardName')
    result = {'name': board or '', 'background': [0., 0., 0., 1.], 'views': []}
    if board:
        _, _, scene = launch_scene(source, prefix, board)
        layouts = scene_layout(scene)
        result['background'] = scene['views'][0]['background'] or result['background']
        converted = dict(catalog)
        members = set(source.namelist())
        for index, view in enumerate(scene['views']):
            node = dict(view, layout=layouts[index], images=[])
            if view['image']:
                name = view['image']
                stem, suffix = (name.rsplit('.', 1)[0], '.' + name.rsplit('.', 1)[1]) if '.' in name else (name, '.png')
                for scale in (1, 2, 3):
                    asset = stem + (f'@{scale}x' if scale > 1 else '') + suffix
                    raw = source.read(prefix + asset) if prefix + asset in members else converted[asset].read_bytes() if asset in converted else None
                    if raw is None:
                        continue
                    extension = suffix.lower()
                    normalized = normalize_png(raw)[0] if extension == '.png' else raw
                    if extension not in ('.png', '.jpg', '.jpeg', '.webp', '.gif'):
                        raise ValueError('Unsupported launch image format: ' + suffix)
                    resource = f'launch_image_{index}_{scale}'
                    (resources / 'drawable-nodpi' / (resource + extension)).write_bytes(normalized)
                    node['images'].append({'resource': resource, 'scale': scale})
                if not node['images']:
                    raise ValueError('Missing original launch image: ' + name)
            result['views'].append(node)
    r, g, b, a = result['background']
    colour = '#' + ''.join(f'{round(max(0, min(1, value)) * 255):02x}' for value in (a, r, g, b))
    base = '<item name="android:windowBackground">' + colour + '</item>'
    (resources / 'values/launch.xml').write_text('<resources><style name="HostLaunchTheme" parent="HostTheme">' + base + '</style></resources>\n', encoding='utf-8')
    modern = resources / 'values-v31'
    modern.mkdir(exist_ok=True)
    (modern / 'launch.xml').write_text('<resources><style name="HostLaunchTheme" parent="HostTheme">' + base + '<item name="android:windowSplashScreenBackground">' + colour + '</item><item name="android:windowSplashScreenAnimatedIcon">@drawable/launch_empty</item></style></resources>\n', encoding='utf-8')
    drawable = resources / 'drawable'
    drawable.mkdir(exist_ok=True)
    (drawable / 'launch_empty.xml').write_text('<shape xmlns:android="http://schemas.android.com/apk/res/android"><solid android:color="#00000000"/></shape>\n', encoding='utf-8')
    return result
