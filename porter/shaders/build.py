import copy
from fnmatch import fnmatchcase
import hashlib
import json
import re
from pathlib import Path
import struct
import subprocess

from .. import SHADER_PACK_VERSION
from ..files import write_json, cache_matches, file_metadata, resolve_file
from .components import sampled_component_masks
from .samplers import sampler_description


def read_embedded_source(raw, address):
    count = struct.unpack_from('<I', raw, 16)[0]
    cursor = 32
    for _ in range(count):
        command, size = struct.unpack_from('<II', raw, cursor)
        if command == 0x19:
            virtual, _, offset, file_size = struct.unpack_from('<QQQQ', raw, cursor+24)
            if virtual <= address < virtual+file_size:
                start = offset+address-virtual
                return raw[start:offset+file_size].split(b'\0', 1)[0]
        cursor += size
    raise ValueError(f'Source address is not file backed: {address:#x}')


def compile_glsl(code, stage, folder, tools):
    tools = Path(tools)
    executables = [tools / name for name in ('glslangValidator.exe', 'spirv-val.exe', 'spirv-cross.exe', 'spirv-opt.exe')]
    # Keep the interface, resource ABI and precision policy. These passes remove
    # call/local-variable scaffolding and repeated values from recovered GLSL;
    # do not use relaxed-float, FP16 conversion or algebraic fast-math passes.
    optimizations = ['--preserve-interface', '--preserve-bindings', '--preserve-spec-constants',
                     '--merge-return', '--inline-entry-points-exhaustive', '--eliminate-dead-functions',
                     '--eliminate-local-single-block', '--eliminate-local-single-store', '--ssa-rewrite',
                     '--ccp', '--eliminate-dead-branches', '--eliminate-dead-code-aggressive',
                     '--redundancy-elimination', '--eliminate-dead-code-aggressive', '--compact-ids']
    inputs = dict(source=code,stage=stage,target='vulkan1.0',optimizations=optimizations,
                  tools=file_metadata([Path(__file__),*executables]))
    folder.mkdir(parents=True, exist_ok=True)
    source, binary, reflection = folder / ('source.' + stage), folder / 'module.spv', folder / 'reflection.json'
    stamp=folder/'compile.cache.json'
    if not cache_matches(stamp,inputs,[source,binary,reflection]):
        stamp.unlink(missing_ok=True)
        source.write_text(code, encoding='utf-8')
        unoptimized = folder / 'unoptimized.spv'
        for command in ([executables[0], '-V', '--target-env', 'vulkan1.0', '-S', stage, '-e', 'main', '-o', unoptimized, source],
                        [executables[3], '--target-env=vulkan1.0', *optimizations, unoptimized, '-o', binary],
                        [executables[1], '--target-env', 'vulkan1.0', binary],
                        [executables[2], binary, '--reflect']):
            result = subprocess.run([str(x) for x in command], capture_output=True, text=True,
                                    encoding='utf-8', errors='replace')
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
        reflection.write_text(result.stdout, encoding='utf-8')
        original = subprocess.run([str(executables[2]), str(unoptimized), '--reflect'], capture_output=True,
                                  text=True, encoding='utf-8', errors='replace')
        if original.returncode:
            raise RuntimeError(original.stdout + original.stderr)
        if shader_interface(json.loads(original.stdout)) != shader_interface(json.loads(result.stdout)):
            raise ValueError('Shader optimization changed the resource or stage interface: ' + str(source))
        write_json(stamp,inputs)
    return folder, binary, reflection


def shader_interface(reflection):
    """Compare ABI shapes independently of SPIR-V result/type identifiers."""
    types = reflection.get('types', {})

    def shape(value):
        if isinstance(value, list):
            return [shape(item) for item in value]
        if isinstance(value, dict):
            return {key: shape(types[item]) if key == 'type' and isinstance(item, str) and item in types else shape(item)
                    for key, item in value.items() if key not in ('variable_id',)}
        return value

    categories = ('entryPoints', 'inputs', 'outputs', 'ubos', 'ssbos', 'images', 'separate_images',
                  'separate_samplers', 'textures', 'subpass_inputs', 'push_constants', 'specialization_constants')
    return {key: sorted((shape(value) for value in reflection.get(key, [])), key=lambda value: json.dumps(value, sort_keys=True))
            for key in categories}


def compile_embedded_shaders(raw, recipes, folder, tools):
    libraries, candidates = [], {}
    for recipe_index, recipe in enumerate(recipes):
        original = read_embedded_source(raw, recipe['address'])
        slot=folder/str(recipe_index)
        base, binary, reflection = compile_glsl(recipe['glsl'], recipe['stage'], slot, tools)
        shader = dict(name=recipe['name'], source_kind='embedded_msl', address=recipe['address'],
                      original_sha256=recipe['sha256'], original_bytes=len(original), stage=recipe['stage'],
                      entry='main', layout={'original_entry': recipe['entry']},
                      source='source.'+recipe['stage'], spirv=binary.name, reflection=reflection.name,
                      literal_samplers=recipe.get('literal_samplers', []),
                      status='validated')
        fallback = recipe.get('compute_fallback')
        if fallback:
            if recipe['stage'] != 'vert' or fallback['contract'] != 'single-point-no-rasterization':
                raise ValueError('Unsupported shader fallback contract')
            _, alternate, alternate_reflection = compile_glsl(fallback['glsl'], 'comp', slot/'fallback', tools)
            shader['fallback'] = dict(path=str(alternate),
                                      reflection=str(alternate_reflection), contract=fallback['contract'])
        name = 'embedded/' + recipe['name'] + '.metal'
        if name in candidates:
            raise ValueError('Duplicate embedded shader identifier: ' + recipe['name'])
        libraries.append(dict(name=name, sha256=recipe['sha256'], bytes=len(original), address=recipe['address'],
                              entries=[dict(entry=recipe['entry'], stage={'vert':'vertex', 'frag':'fragment', 'comp':'kernel'}[recipe['stage']], inputs=recipe['inputs'])]))
        candidates[name] = [(shader, base)]
    return libraries, candidates


def compile_shader_replacement(original, base, code, folder, tools):
    output, binary, reflection = compile_glsl(code, original['stage'], folder, tools)
    before = json.loads(resolve_file(base, original['reflection']).read_text(encoding='utf-8'))
    after = json.loads(reflection.read_text(encoding='utf-8'))
    for key in ('entryPoints', 'inputs', 'outputs', 'separate_images', 'separate_samplers', 'ubos', 'ssbos', 'images'):
        def shape(value):
            def normalize(item):
                if isinstance(item, list):
                    return [normalize(v) for v in item]
                if isinstance(item, dict):
                    return {k: normalize(value.get('types', {}).get(v, v) if k == 'type' and isinstance(v, str) else v)
                            for k, v in item.items() if k != 'id'}
                return item
            return normalize(value.get(key, []))
        if shape(before) != shape(after):
            raise ValueError('Replacement changed the shader interface: ' + original['name'] + ' ' + key)
    derived = copy.deepcopy(original)
    derived.update(source='source.' + original['stage'], spirv=binary.name, reflection=reflection.name)
    return derived, output


def apply_shader_repairs(shaders, recipes, root, folder, tools):
    reports = []
    for recipe_index, recipe in enumerate(recipes):
        original, base = shaders[recipe['library']][0]
        if original['original_sha256'] != recipe['original_source_sha256']:
            raise ValueError('Replacement does not match the original shader: ' + recipe['library'])
        if 'source' in recipe:
            source = resolve_file(root,recipe['source'])
            code = source.read_text(encoding='utf-8')
        else:
            source = resolve_file(base,original['source'])
            code = source.read_text(encoding='utf-8')
            for edit in recipe['edits']:
                if code.count(edit['before']) != 1:
                    raise ValueError('Shader edit does not match a unique expression: '+original['name'])
                code = code.replace(edit['before'],edit['after'])
            if recipe.get('helpers'):
                at = code.index('void main')
                code = code[:at]+recipe['helpers']+'\n'+code[at:]
        derived, output = compile_shader_replacement(original, base, code, folder/str(recipe_index), tools)
        shaders[recipe['library']] = [(derived, output)]
        reports.append(dict(source=recipe.get('source',original['source']),library=recipe['library'],reason=recipe.get('reason','')))
    return reports


def rewrite_intrinsic_calls(code, recipe, helpers):
    # The adapter supplies semantics. This pass only rewrites complete GLSL calls;
    # comments, argument evaluation and nested expressions retain their positions.
    functions = recipe['functions']
    powers = recipe.get('constant_powers', {})
    identifiers = [*functions, *functions.values(), *powers.values()]
    if not functions or any(not re.fullmatch(r'[A-Za-z_]\w*', name) for name in identifiers):
        raise ValueError('Invalid shader intrinsic rewrite')
    masked = re.sub(r'/\*.*?\*/|//[^\n]*', lambda m: re.sub(r'[^\n]', ' ', m[0]), code, flags=re.S)
    pattern = r'\b(' + '|'.join(re.escape(name) for name in functions) + r')\s*\('
    edits, counts = [], {}
    for call in re.finditer(pattern, masked):
        depth, end, commas = 1, call.end(), []
        while end < len(masked) and depth:
            char = masked[end]
            if char == ',' and depth == 1:
                commas.append(end)
            depth += (char == '(') - (char == ')')
            end += 1
        if depth:
            raise ValueError('Unclosed shader intrinsic: ' + call[1])
        replacement = functions[call[1]]
        if call[1] == 'pow' and len(commas) == 1:
            exponent = masked[commas[0] + 1:end - 1].strip()
            vector = re.fullmatch(r'vec[234]\s*\(([^()]*)\)', exponent)
            literals = vector[1].split(',') if vector else [exponent]
            if all(re.fullmatch(r'\s*\d+(?:\.\d*)?(?:[eE][+-]?\d+)?\s*', value) for value in literals):
                values = [float(value) for value in literals]
                if values and all(value == values[0] for value in values) and values[0].is_integer():
                    fixed = powers.get(str(int(values[0])))
                    if fixed:
                        replacement = fixed
                        edits.append((commas[0], end - 1, ''))
        edits.append((call.start(), call.start() + len(call[1]), replacement))
        counts[replacement] = counts.get(replacement, 0) + 1
    if not edits:
        return code, counts
    for start, end, replacement in sorted(edits, reverse=True):
        code = code[:start] + replacement + code[end:]
    lines = code.splitlines(keepends=True)
    header = re.sub(r'/\*.*?\*/|//[^\n]*', lambda m: re.sub(r'[^\n]', ' ', m[0]), code, flags=re.S).splitlines()
    insertion = 1
    while insertion < len(lines) and (header[insertion].lstrip().startswith(('#', 'precision ')) or not header[insertion].strip()):
        insertion += 1
    return ''.join(lines[:insertion]) + helpers + '\n' + ''.join(lines[insertion:]), counts


def apply_intrinsic_rewrites(shaders, recipe, root, folder, tools):
    if not recipe:
        return []
    helpers = resolve_file(root, recipe['source']).read_text(encoding='utf-8')
    reports = []
    for library_index, (library, candidates) in enumerate(shaders.items()):
        for index, (original, base) in enumerate(candidates):
            slot = folder/str(library_index)/str(index)
            code = resolve_file(base, original['source']).read_text(encoding='utf-8')
            code, counts = rewrite_intrinsic_calls(code, recipe, helpers)
            derived, output = (original, base)
            if counts:
                derived, output = compile_shader_replacement(original, base, code, slot, tools)
            fallback = original.get('fallback')
            fallback_counts = {}
            if fallback:
                alternate_base = Path(fallback['path']).parent
                alternate = dict(name=original['name'] + '/fallback', stage='comp',
                                 source='source.comp', reflection=Path(fallback['reflection']).name)
                alternate_code = resolve_file(alternate_base, alternate['source']).read_text(encoding='utf-8')
                alternate_code, fallback_counts = rewrite_intrinsic_calls(alternate_code, recipe, helpers)
                if fallback_counts:
                    rewritten, alternate_output = compile_shader_replacement(alternate, alternate_base, alternate_code, slot/'fallback', tools)
                    derived = copy.deepcopy(derived)
                    derived['fallback'].update(path=str(alternate_output/rewritten['spirv']),
                                               reflection=str(alternate_output/rewritten['reflection']))
            candidates[index] = (derived, output)
            reports.append(dict(library=library, name=original['name'], stage=original['stage'],
                                source=recipe['source'], calls=counts, fallback_calls=fallback_counts,
                                reason='adapter_intrinsic_rewrites'))
    write_json(folder/'rewrite-report.json', dict(shaders=reports))
    return reports


def instrument_math(code, stage, name, first_site, descriptor_set=None, probes=()):
    operations = ('pow', 'sqrt', 'inversesqrt', 'log', 'log2', 'exp2', 'normalize', 'length', 'dot')
    sites = []
    masked = re.sub(r'/\*.*?\*/|//[^\n]*', lambda m: ' ' * len(m[0]), code, flags=re.S)
    calls = list(re.finditer(r'\b(' + '|'.join(operations) + r')\s*\(', masked))
    edits = []
    for call in calls:
        depth, end = 1, call.end()
        while end < len(masked) and depth:
            depth += (masked[end] == '(') - (masked[end] == ')')
            end += 1
        if depth:
            raise ValueError('Unclosed shader operation: ' + name)
        index = first_site + len(sites)
        sites.append(dict(shader=name, stage=stage, operation=call[1],
                          line=code.count('\n', 0, call.start()) + 1,
                          expression=code[call.start():end]))
        edits += [(call.start(), call.start()+len(call[1]), '_i2a_'+call[1]),
                  (end-1, end-1, ', '+str(index)+'u')]
    main = re.search(r'\bvoid\s+main\s*\([^)]*\)\s*\{', masked)
    if not main:
        raise ValueError('Diagnostic shader entry not found: ' + name)
    entry_site = first_site + len(sites)
    sites.append(dict(shader=name, stage=stage, operation='stage-entry',
                      line=code.count('\n', 0, main.start()) + 1, expression='main entry'))
    edits.append((main.end(), main.end(), f'\n_i2a_record(0.0, 0.0, 0.0, {entry_site}u, 0u);\n'))
    depth, main_end = 1, main.end()
    while main_end < len(masked) and depth:
        depth += (masked[main_end] == '{') - (masked[main_end] == '}')
        main_end += 1
    if depth:
        raise ValueError('Unclosed diagnostic entry: ' + name)
    outputs = re.findall(r'layout\s*\([^)]*\)\s*out\s+(?:(?:lowp|mediump|highp)\s+)?(?:float|vec[234])\s+(\w+)\s*;', masked)
    if stage == 'vert' and re.search(r'\bgl_Position\s*=', masked[main.end():main_end]):
        outputs.append('gl_Position')
    observations = [dict(name='output:' + value, expression=value) for value in outputs] + list(probes)
    end_records = []
    for probe in observations:
        index = first_site + len(sites)
        expression = probe['expression']
        if not isinstance(expression, str) or not expression.strip() or any(c in expression for c in ';{}'):
            raise ValueError('Invalid shader observation expression: ' + name)
        sites.append(dict(shader=name, stage=stage, operation=probe['name'],
                          line=code.count('\n', 0, main_end) + 1, expression=expression))
        end_records.append(f'_i2a_observe({expression}, {index}u);')
    if end_records:
        edits.append((main_end - 1, main_end - 1, '\n' + '\n'.join(end_records) + '\n'))
    # Storage-image writes are outputs as well; wrap their value without
    # evaluating coordinates or data twice, and leave all ordinary builds alone.
    for call in re.finditer(r'\bimageStore\s*\(', masked[main.end():main_end]):
        start = main.end() + call.end()
        depth, end, commas = 1, start, []
        while end < main_end and depth:
            char = masked[end]
            if char == ',' and depth == 1:
                commas.append(end)
            depth += (char == '(') - (char == ')')
            end += 1
        if depth or len(commas) != 2:
            raise ValueError('Unsupported diagnostic imageStore signature: ' + name)
        index = first_site + len(sites)
        sites.append(dict(shader=name, stage=stage, operation='imageStore',
                          line=code.count('\n', 0, start) + 1, expression=code[commas[-1] + 1:end - 1]))
        edits.extend([(commas[-1] + 1, commas[-1] + 1, '_i2a_observe('),
                      (end - 1, end - 1, ', ' + str(index) + 'u)')])
    for start, end, value in sorted(edits, reverse=True):
        code = code[:start] + value + code[end:]
    guard = {'frag': '((uint(gl_FragCoord.x) & 31u) == 0u && (uint(gl_FragCoord.y) & 31u) == 0u)',
             'vert': '((uint(gl_VertexIndex) & 31u) == 0u)',
             'comp': '((gl_GlobalInvocationID.x & 31u) == 0u)'}[stage]
    descriptor_set = {'vert': 0, 'frag': 1, 'comp': 2}[stage] if descriptor_set is None else descriptor_set
    helpers = [f'layout(set={descriptor_set}, binding=255, std430) buffer I2AMathDiagnostics {{ uint words[]; }} _i2a_diag;',
        'void _i2a_record(float x, float y, float value, uint site, uint domain) {',
        f'if (_i2a_diag.words[0] == 0u || !{guard}) return;',
        'uint at = 1u + site * 8u;',
        'domain |= (isnan(x) || isinf(x) || isnan(y) || isinf(y)) ? 2u : 0u;',
        'atomicAdd(_i2a_diag.words[at], 1u);',
        'if (domain != 0u) atomicAdd(_i2a_diag.words[at+1u], 1u);',
        'bool bad = isnan(value) || isinf(value);',
        'if (bad) atomicAdd(_i2a_diag.words[at+2u], 1u);',
        'if ((bad || domain != 0u) && atomicCompSwap(_i2a_diag.words[at+3u], 0u, 1u) == 0u) {',
        '_i2a_diag.words[at+4u]=floatBitsToUint(x); _i2a_diag.words[at+5u]=floatBitsToUint(y);',
        '_i2a_diag.words[at+6u]=floatBitsToUint(value); _i2a_diag.words[at+7u]=domain;', '}', '}']
    helpers.append('float _i2a_observe(float x, uint site) { _i2a_record(x, 0.0, x, site, abs(x)>65504.0 ? 4u : 0u); return x; }')
    for size in (2, 3, 4):
        records = ' '.join(f'_i2a_observe(x[{i}], site);' for i in range(size))
        helpers.append(f'vec{size} _i2a_observe(vec{size} x, uint site) {{ {records} return x; }}')
    for op in operations:
        if op in ('normalize', 'length', 'dot'):
            for size in (1, 2, 3, 4):
                value_type = 'float' if size == 1 else f'vec{size}'
                params = f'{value_type} x' + (f', {value_type} y' if op == 'dot' else '')
                result_type = value_type if op == 'normalize' else 'float'
                expression = f'{op}(x, y)' if op == 'dot' else f'{op}(x)'
                domain = ('x == 0.0' if size == 1 else 'all(equal(x, ' + value_type + '(0.0)))') if op == 'normalize' else 'false'
                record = []
                for component in range(size):
                    x = 'x' if size == 1 else f'x[{component}]'
                    y = ('y' if size == 1 else f'y[{component}]') if op == 'dot' else '0.0'
                    value = f'v[{component}]' if op == 'normalize' and size != 1 else 'v'
                    record.append(f'_i2a_record({x}, {y}, {value}, site, ({domain}) ? 1u : 0u);')
                helpers.append(f'{result_type} _i2a_{op}({params}, uint site) {{ {result_type} v={expression}; ' + ' '.join(record) + ' return v; }')
            continue
        binary = op == 'pow'
        domain = {'pow':'(x < 0.0 || (x == 0.0 && y <= 0.0))', 'sqrt':'(x < 0.0)',
                  'inversesqrt':'(x <= 0.0)', 'log':'(x <= 0.0)', 'log2':'(x <= 0.0)', 'exp2':'false'}[op]
        params = 'float x, float y' if binary else 'float x'
        expression = f'{op}(x, y)' if binary else f'{op}(x)'
        helpers.append(f'float _i2a_{op}({params}, uint site) {{ float v={expression}; _i2a_record(x, {"y" if binary else "0.0"}, v, site, {domain} ? 1u : 0u); return v; }}')
        for size in (2,3,4):
            params = f'vec{size} x, vec{size} y' if binary else f'vec{size} x'
            components = ','.join(f'_i2a_{op}(x[{i}], '+(f'y[{i}], ' if binary else '')+'site)' for i in range(size))
            helpers.append(f'vec{size} _i2a_{op}({params}, uint site) {{ return vec{size}({components}); }}')
    lines = code.splitlines(keepends=True)
    insertion = 1
    while insertion < len(lines) and (lines[insertion].strip().startswith(('#extension','precision')) or not lines[insertion].strip()):
        insertion += 1
    return ''.join(lines[:insertion]) + '\n'.join(helpers) + '\n' + ''.join(lines[insertion:]), sites


def compile_diagnostic_shaders(shaders, folder, tools, patterns=None, probes=None):
    if patterns is not None and (not isinstance(patterns, list) or any(not isinstance(value, str) for value in patterns)):
        raise ValueError('Shader diagnostic patterns must be a list of names')
    probes = probes or {}
    sites = []
    for library_index, (library, candidates) in enumerate(shaders.items()):
        for index, (shader, base) in enumerate(candidates):
            if patterns is not None and not any(fnmatchcase(shader['name'], pattern) for pattern in patterns):
                continue
            slot = folder/str(library_index)/str(index)
            source = resolve_file(base, shader['source'])
            code, current = instrument_math(source.read_text(encoding='utf-8'), shader['stage'], shader['name'], len(sites),
                                            probes=probes.get(shader['name'], ()))
            output, binary, reflection = compile_glsl(code, shader['stage'], slot, tools)
            derived = dict(shader, source='source.'+shader['stage'], spirv=binary.name,
                           reflection=reflection.name, math_diagnostics=True)
            sites.extend(current)
            fallback = shader.get('fallback')
            if fallback:
                # The compute fallback retains the vertex descriptor contract (set 0).
                alternate_source = Path(fallback['path']).parent / 'source.comp'
                alternate_name = shader['name'] + '/fallback'
                alternate, current = instrument_math(alternate_source.read_text(encoding='utf-8'), 'comp',
                                                      alternate_name, len(sites), descriptor_set=0)
                alternate_output, alternate_binary, alternate_reflection = compile_glsl(alternate, 'comp', slot/'fallback', tools)
                derived['fallback'] = dict(fallback, path=str(alternate_binary), reflection=str(alternate_reflection))
                sites.extend(current)
            candidates[index] = (derived, output)
    write_json(folder/'sites.json', dict(sites=sites, uninstrumented_vertex=[],
        sampling='fragment 1/1024 pixels; vertex 1/32 indices; compute 1/32 X invocations; original operations unchanged',
        flags={'1': 'invalid operation domain', '2': 'nonfinite input', '4': 'observation exceeds finite FP16 range'},
        limitations='Sampled intrinsic calls, entry and outputs, plus selected expressions; early returns bypass final-output probes'))
    return sites


def metal_data_type(name):
    # Apple's MTLDataType ABI, also used by the original renderer's switch tables.
    scalar = {'float': 3, 'half': 16, 'int': 29, 'uint': 33, 'short': 37,
              'ushort': 41, 'char': 45, 'uchar': 49, 'bool': 53}
    match = re.fullmatch(r'(float|half|int|uint|short|ushort|char|uchar|bool)([2-4])?(?:x([2-4]))?', name)
    if not match:
        raise ValueError(f'Untranslated Metal data type: {name}')
    kind, columns, rows = match.groups()
    if rows:
        if kind not in ('float', 'half'):
            raise ValueError(name)
        return scalar[kind] + 4 + (int(columns) - 2) * 3 + int(rows) - 2
    return scalar[kind] + (int(columns) - 1 if columns else 0)


def metal_texture_type(name):
    kind = name.split('<')[0]
    return {'texture1d': 0, 'texture1d_array': 1, 'texture2d': 2,
            'texture2d_array': 3, 'texture2d_ms': 4, 'texturecube': 5,
            'texturecube_array': 6, 'texture3d': 7}[kind]


def library_digest(data):
    return hashlib.sha256(data).hexdigest()


def c_string(value):
    return json.dumps(value, ensure_ascii=True)


def emit_struct_array(header, c_type, name, rows):
    header.append(f'static const struct {c_type} {name}[] = {{')
    header.extend('    {' + ','.join(row) + '},' for row in rows)
    if not rows:
        header.append('    {0},')
    header.append('};')


def pending_shader_plan(source, prefix, executable, work):
    """Register exact bundled libraries with the runtime's explicit error state.

    A library without compiled entries stays translation_pending and returns
    MTLLibraryErrorDomain on use. Container names are inventory, not recovered
    source or a claim that a portable shader has been generated.
    """
    from .metal import container

    libraries, inventory = [], []
    for path in sorted(name for name in source.namelist() if name.startswith(prefix) and name.endswith('.metallib')):
        raw = source.read(path)
        library = dict(name=path[len(prefix):], path=path, sha256=library_digest(raw), bytes=len(raw), entries=[])
        row = dict(path=path, sha256=library['sha256'], bytes=len(raw), format='MTLB' if raw[:4] == b'MTLB' else 'unknown',
                   status='translation_pending', entries=[])
        try:
            for entry in container(raw):
                library['entries'].append(dict(entry=entry['name'], stage=entry['stage']))
                row['entries'].append({key: entry[key] for key in ('name','stage','type_tag','bitcode_size')})
        except (ValueError, KeyError, struct.error) as error:
            row['container_inspection_error'] = str(error)
        libraries.append(library)
        inventory.append(row)
    write_json(work / 'metal-library-inventory.json', dict(libraries=inventory, portable_modules=0))
    return dict(executable=executable, libraries=libraries, shaders={})


def assemble_shader_plan(context, source, spec):
    root=context.adapter_root
    metadata=json.loads(resolve_file(root,spec['metadata']).read_text(encoding='utf-8'))
    if metadata['status']!='complete':
        raise ValueError('Incomplete Metal metadata')
    prefix=context.identity.prefix+spec['library_directory'].strip('/')+'/'
    libraries=metadata['libraries']
    identities={}
    for library in libraries:
        library['path']=prefix+library['name']
        identities[library['path']]=library
    shaders={}
    for group in spec['groups']:
        manifest=resolve_file(root,group['manifest'])
        data=json.loads(manifest.read_text(encoding='utf-8'))
        if data['status']!='complete':
            raise ValueError('Incomplete shader conversion: '+group['manifest'])
        grouped={}
        for shader in data['shaders']:
            key=shader['paired_metallib']['path']
            if key not in identities:
                raise ValueError('Shader candidate has no matching library: '+key)
            for field in ('spirv','reflection'):
                resolve_file(manifest.parent,shader[field])
            grouped.setdefault(key,[]).append((shader,manifest.parent))
        for key,entries in grouped.items():
            if group.get('replace',False):shaders[key]=entries
            else:shaders.setdefault(key,[]).extend(entries)
    repair=json.loads(resolve_file(root,spec['repairs']).read_text(encoding='utf-8')) if spec.get('repairs') else {}
    replacements=[dict(recipe,library=prefix+recipe['library']) for recipe in repair.get('replacements',[])]
    reports=apply_shader_repairs(shaders,replacements,root,context.work/'shader-cache/replacements',context.toolchain['shaders'])
    recipes=spec.get('embedded',repair.get('embedded',[]))
    executable=context.identity.prefix+context.identity.executable
    embedded,generated=compile_embedded_shaders(source.read(executable),recipes,context.work/'shader-cache/embedded',context.toolchain['shaders'])
    shaders.update(generated)
    reports.extend(apply_intrinsic_rewrites(shaders,repair.get('intrinsics'),root,
                   context.work/'shader-cache/intrinsics',context.toolchain['shaders']))
    sites = compile_diagnostic_shaders(shaders, context.work/'shader-cache/diagnostics', context.toolchain['shaders'],
                                      context.host.get('shader_diagnostic_patterns'), context.host.get('shader_diagnostic_probes')) if context.shader_diagnostics else []
    return dict(executable=executable,libraries=libraries+embedded,shaders=shaders,replacements=reports,diagnostics=sites)


def shader_requirements(blob, reflection, stage):
    # Shared bit layout with MetalShaderRequirement in the runtime. Inspect
    # offline SPIR-V once; the device compares these flags without scanning code.
    capabilities = {10: 1 << 5, 11: 1 << 6, 22: 1 << 7, 35: 1 << 8,
                    49: 1 << 4, 55: 1 << 2, 56: 1 << 3}
    words = struct.unpack('<' + 'I' * (len(blob) // 4), blob)
    flags, cursor = 0, 5
    while cursor < len(words):
        count, opcode = words[cursor] >> 16, words[cursor] & 0xffff
        if not count or cursor + count > len(words):
            raise ValueError('Invalid SPIR-V instruction while reading requirements')
        if opcode == 17 and count >= 2:
            flags |= capabilities.get(words[cursor + 1], 0)
        cursor += count
    writes = any(not resource.get('readonly', False)
                 for category in ('ssbos', 'images')
                 for resource in reflection.get(category, []))
    if writes:
        flags |= 1 << 16
        if stage in (1, 2):
            flags |= 1 << (stage - 1)
    return flags


def shader_workgroup(reflection):
    point = reflection['entryPoints'][0]
    values = point.get('workgroup_size', [1, 1, 1])
    specialized = point.get('workgroup_size_is_spec_constant_id', [False, False, False])
    if len(values) != 3 or len(specialized) != 3:
        raise ValueError('Invalid compute workgroup reflection')
    constants = {value['id']: value for value in reflection.get('specialization_constants', [])}
    size, ids = [], []
    for value, dynamic in zip(values, specialized):
        if dynamic:
            constant = constants.get(value)
            if constant is None or constant.get('type') not in ('uint', 'int'):
                raise ValueError('Missing compute workgroup specialization default')
            dimension = int(constant['default_value'])
            identity = int(value)
        else:
            dimension, identity = int(value), 0xffffffff
        if dimension < 1 or dimension > 0xffffffff or identity < 0 or identity > 0xffffffff:
            raise ValueError('Compute workgroup exceeds uint32 metadata')
        size.append(dimension)
        ids.append(identity)
    return size, ids


def build_shader_pack(source, work, plan):
    all_libraries = plan.get('libraries', [])
    shaders = plan.get('shaders', {})
    executable = source.read(plan['executable'])
    members, arguments, attributes, bindings, inputs, outputs, entries, libraries = ([] for _ in range(8))
    literal_descriptions, literal_indices = [], {}
    binary = bytearray()
    modules = {}
    report = []
    for library in all_libraries:
        is_source = 'address' in library
        original = read_embedded_source(executable, library['address']) if is_source else source.read(library['path'])
        if library_digest(original) != library['sha256'] or len(original) != library['bytes']:
            raise ValueError('Original Metal library changed: ' + library['name'])
        first_entry = len(entries)
        candidates_for_library = shaders.get(library['name'] if is_source else library['path'], [])
        for shader, shader_directory in candidates_for_library:
            if shader['status'] != 'validated' or (not is_source and shader['paired_metallib']['sha256'] != library['sha256']):
                raise ValueError('Mismatched shader candidate: ' + library['name'])
            candidates = [e for e in library['entries'] if e['entry'] == shader['layout']['original_entry']]
            if len(candidates) != 1 or len(library['entries']) != len(candidates_for_library):
                raise ValueError('Ambiguous Metal entry: ' + library['name'])
            native = candidates[0]
            stage = {'vert': 1, 'frag': 2, 'comp': 3}[shader['stage']]
            if native['stage'] not in {1: ('vertex',), 2: ('fragment',), 3: ('kernel','compute')}[stage]:
                raise ValueError('Shader stage mismatch')
            blob = (shader_directory / shader['spirv']).read_bytes()
            if len(blob)<20 or len(blob) % 4 or struct.unpack_from('<I', blob)[0] != 0x07230203:
                raise ValueError('Invalid or changed SPIR-V: ' + shader['name'])
            if blob not in modules:
                binary.extend(b'\0' * (-len(binary) % 16))
                modules[blob] = len(binary)
                binary.extend(blob)
            reflection = json.loads((shader_directory / shader['reflection']).read_text(encoding='utf8'))
            if len(reflection['entryPoints']) != 1 or any(reflection['entryPoints'][0][k] != value for k,value in [('name',shader['entry']),('mode',shader['stage'])]):
                raise ValueError('SPIR-V entry reflection mismatch')
            requirements = shader_requirements(blob, reflection, stage)
            fallback_offset, fallback_size, fallback_requirements = 0, 0, 0
            fallback = shader.get('fallback')
            if fallback:
                if stage != 1 or fallback['contract'] != 'single-point-no-rasterization':
                    raise ValueError('Unsupported shader fallback plan')
                alternate = Path(fallback['path']).read_bytes()
                if len(alternate)<20 or len(alternate) % 4 or struct.unpack_from('<I', alternate)[0] != 0x07230203:
                    raise ValueError('Invalid fallback module')
                alternate_reflection = json.loads(Path(fallback['reflection']).read_text(encoding='utf-8'))
                if alternate_reflection['entryPoints'] != [{'name': 'main', 'mode': 'comp', 'workgroup_size': [1, 1, 1], 'workgroup_size_is_spec_constant_id': [False, False, False]}]:
                    points = alternate_reflection['entryPoints']
                    if len(points) != 1 or points[0]['name'] != 'main' or points[0]['mode'] != 'comp' or points[0].get('workgroup_size') != [1, 1, 1]:
                        raise ValueError('Fallback requires one compute invocation')
                if reflection.get('inputs') or reflection.get('outputs'):
                    raise ValueError('Fallback vertex cannot have stage inputs or outputs')
                for category in ('ubos', 'ssbos', 'separate_images', 'separate_samplers', 'images'):
                    shape = lambda r: sorted((v['set'], v['binding'], v.get('block_size', 0)) for v in r.get(category, []))
                    if shape(reflection) != shape(alternate_reflection):
                        raise ValueError('Fallback resource contract differs')
                if alternate not in modules:
                    binary.extend(b'\0' * (-len(binary) % 16))
                    modules[alternate] = len(binary)
                    binary.extend(alternate)
                fallback_offset, fallback_size = modules[alternate], len(alternate)
                fallback_requirements = shader_requirements(alternate, alternate_reflection, 3)
            # The alternate compute program may use different channels. Do not
            # narrow a binding based solely on the main program in that case.
            component_masks = sampled_component_masks(blob) if not fallback and reflection.get('separate_images') else {}
            first_binding = len(bindings)
            active = set()
            for key, descriptor, kind, binding_base in [('ubos', 6, 'buffer', 0),
                                               ('ssbos', 7, 'buffer', 0),
                                               ('separate_images', 2, 'texture', 32),
                                               ('separate_samplers', 0, 'sampler', 160),
                                               ('images', 3, 'texture', 32)]:
                for resource in reflection.get(key, []):
                    if shader.get('math_diagnostics') and key=='ssbos' and resource['binding']==255 and resource['set']==stage-1:
                        bindings.append([str(stage-1), '255', '7', '4294967294', '0', '0', '0', '0'])
                        continue
                    index = resource['binding'] - binding_base
                    if resource['set'] != stage - 1 or index < 0 or resource.get('array'):
                        raise ValueError('Unsupported descriptor shape: ' + shader['name'])
                    literal = next((s for s in shader.get('literal_samplers',[]) if s['binding']==resource['binding']), None) if kind=='sampler' else None
                    if literal:
                        index = 0xffffffff
                        description = sampler_description(literal)
                        if description not in literal_indices:
                            literal_indices[description] = len(literal_descriptions) + 1
                            literal_descriptions.append([str(value) for value in description])
                    else:
                        active.add((kind, index))
                    native_buffer = next((a for a in native['inputs'] if a['kind']=='buffer' and a['index']==index), None) if kind=='buffer' else None
                    if kind=='buffer' and not native_buffer:
                        raise ValueError('Missing original buffer ABI: ' + shader['name'])
                    bindings.append([str(n) for n in (resource['set'], resource['binding'], descriptor,
                                    index, resource.get('block_size', 0), native_buffer['size'] if native_buffer else 0)] + [str(literal_indices[description]) if literal else '0'])
                    bindings[-1].append(str(component_masks.get((resource['set'], resource['binding']), 0) if key=='separate_images' else 0))
            first_argument = len(arguments)
            first_attribute = len(attributes)
            active_locations = {i['location'] for i in reflection.get('inputs', [])}
            for arg in native['inputs']:
                kind = arg['kind']
                if kind == 'vertex_input':
                    attributes.append([c_string(arg['name']), str(arg['index']), str(metal_data_type(arg['type'])),
                                       str(int(arg['index'] in active_locations))])
                    continue
                if kind not in ('buffer', 'texture', 'sampler'):
                    continue
                is_active = (kind, arg['index']) in active
                # Inactive buffers retain their real struct information. The
                # original renderer consults isActive before allocating them.
                if not is_active and kind != 'buffer':
                    continue
                first_member = len(members)
                for member in arg.get('members', []):
                    members.append([c_string(member['name']), str(metal_data_type(member['type'])),
                                    str(member['offset']), str(member['size'])])
                align = arg['properties'].get('air.arg_type_align_size', 0)
                access = 1 if arg['properties'].get('air.read_write') else 2 if arg['properties'].get('air.write') else 0
                dtype = 1 if kind == 'buffer' else (16 if '<half' in arg['type'] else 3) if kind == 'texture' else 0
                arguments.append([c_string(arg['name'])] + [str(n) for n in (
                    {'buffer': 0, 'texture': 2, 'sampler': 3}[kind], arg['index'], dtype,
                    arg.get('size', 0), align, metal_texture_type(arg['type']) if kind == 'texture' else 0,
                    access, int(is_active), first_member, len(members) - first_member)])
            first_input, first_output = len(inputs), len(outputs)
            for target, key in [(inputs, 'inputs'), (outputs, 'outputs')]:
                for value in reflection.get(key, []):
                    if value.get('array'):
                        raise ValueError('Interface array needs explicit translation: ' + shader['name'])
                    target.append([c_string(value['name']), c_string(value['type']), str(value['location'])])
            workgroup, workgroup_ids = shader_workgroup(reflection) if stage == 3 else ([1, 1, 1], [0xffffffff] * 3)
            entries.append([c_string(shader['name']), c_string(native['entry']), c_string(shader['entry'])] + [str(n) for n in (
                stage, modules[blob], len(blob), first_argument, len(arguments) - first_argument,
                first_attribute, len(attributes) - first_attribute, first_binding, len(bindings) - first_binding,
                first_input, len(inputs) - first_input, first_output, len(outputs) - first_output, fallback_offset, fallback_size,
                requirements, fallback_requirements)] +
                ['{' + ','.join(map(str, workgroup)) + '}', '{' + ','.join(map(str, workgroup_ids)) + '}'])
        libraries.append([c_string(library['name']), '{' + ','.join(str(x) for x in bytes.fromhex(library['sha256'])) + '}',
                          str(len(original)), str(first_entry), str(len(entries) - first_entry), str(int(is_source))])
        report.append({'name': library['name'], 'sha256': library['sha256'],
                       'entries': len(entries) - first_entry, 'status': 'packed' if candidates_for_library else 'translation_pending'})
    header = [f'#define I2A_SHADER_PACK_VERSION {SHADER_PACK_VERSION}', f'#define I2A_SHADER_PACK_BYTES {len(binary)}']
    sites=plan.get('diagnostics',[])
    header += [f'#define I2A_SHADER_DIAGNOSTIC_COUNT {len(sites)}',
               'struct ShaderDiagnosticSite { const char *shader,*operation; unsigned line; };']
    emit_struct_array(header, 'ShaderDiagnosticSite', 'shader_diagnostic_sites',
          [[c_string(site['shader']), c_string(site['operation']), str(site['line'])] for site in sites])
    emit_struct_array(header, 'ShaderMemberSpec', 'shader_members', members)
    emit_struct_array(header, 'ShaderArgumentSpec', 'shader_arguments', arguments)
    emit_struct_array(header, 'ShaderAttributeSpec', 'shader_attributes', attributes)
    emit_struct_array(header, 'ShaderSamplerSpec', 'shader_literal_samplers', literal_descriptions)
    emit_struct_array(header, 'ShaderBindingSpec', 'shader_bindings', bindings)
    emit_struct_array(header, 'ShaderInterfaceSpec', 'shader_inputs', inputs)
    emit_struct_array(header, 'ShaderInterfaceSpec', 'shader_outputs', outputs)
    emit_struct_array(header, 'ShaderSpec', 'shader_specs', entries)
    emit_struct_array(header, 'ShaderLibrarySpec', 'shader_libraries', libraries)
    (work / 'shaders.h').write_text('\n'.join(header) + '\n', encoding='utf8')
    (work / 'shaders.bin').write_bytes(binary)
    (work / 'shaders.S').unlink(missing_ok=True)
    summary = dict(version=SHADER_PACK_VERSION, libraries=len(libraries), entries=len(entries),
                   unique_modules=len(modules), packed_bytes=len(binary), arguments=len(arguments),
                   members=len(members), bindings=len(bindings), libraries_detail=report,
                   translation_sources=plan.get('replacements', []))
    (work / 'shader-pack.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
    print(f"Shader pack: {len(entries)} entries, {len(modules)} distinct modules, {len(binary)} bytes")
    return summary
