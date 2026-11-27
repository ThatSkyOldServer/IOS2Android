import collections
import re
import struct
from .samplers import decode_air_sampler


def split(text):
    result, stack, quote, start = [], [], False, 0
    for i, char in enumerate(text):
        if char == '"':
            quote = not quote
        if quote:
            continue
        if char in '<[{(':
            stack.append(char)
        elif char in '>]})':
            stack.pop()
        elif char == ',' and not stack:
            result.append(text[start:i].strip()); start = i + 1
    result.append(text[start:].strip())
    return result


def typed(text):
    text = re.sub(r'^(?:zeroext|signext)\s+', '', text.strip())
    match = re.match(r'(<\d+ x [^>]+>|\[.+\]|%"[^"]+"|%[\w.]+|ptr(?: addrspace\(\d+\))?|half|float|void|i\d+)\s*(.*)', text)
    if not match:
        raise ValueError('Cannot parse type: ' + text)
    return match.group(1), re.sub(r'^(?:(?:nocapture|readonly|readnone|zeroext|signext)\s+)+', '', match.group(2))


def shape(type_name):
    match = re.fullmatch(r'<(\d+) x (\w+)>', type_name)
    return (int(match[1]), match[2]) if match else (1, type_name)


def number(value):
    result = format(value, '.17g')
    return result if '.' in result or 'e' in result.lower() else result + '.0'


class Pointer:
    def __init__(self, kind, root, offset=0, type_name=None):
        self.kind, self.root, self.offset, self.type = kind, root, offset, type_name


class Translator:
    def __init__(self, ir, entry, interface_locations, round_fragment_inputs=False):
        self.ir, self.entry = ir, entry
        self.interface_locations = interface_locations
        self.round_fragment_inputs = round_fragment_inputs
        self.types, self.names = {}, {}
        for match in re.finditer(r'^(%[^=]+) = type (.+)$', ir, re.M):
            name, body = match[1].strip(), match[2].strip()
            packed = body.startswith('<{')
            self.types[name] = (split(body[2:-2] if packed else body[1:-1]), packed)
            self.names[name] = 'AirStruct' + str(len(self.names))
        self.values, self.value_types, self.declarations, self.resources = {}, {}, [], []
        self.samplers, self.blocks, self.phis = {}, collections.OrderedDict(), collections.defaultdict(list)
        self.bindings, self.inputs, self.outputs, self.stats = [], [], [], collections.Counter()
        self.stage = {'vertex': 'vert', 'fragment': 'frag', 'kernel': 'comp', 'compute': 'comp'}[entry['stage']]
        self.set = {'vert': 0, 'frag': 1, 'comp': 2}[self.stage]

    def glsl(self, type_name):
        if type_name in self.names:
            return self.names[type_name]
        count, scalar = shape(type_name)
        if scalar in ('float', 'half'):
            return 'float' if count == 1 else f'vec{count}'
        if scalar == 'i1':
            return 'bool' if count == 1 else f'bvec{count}'
        if scalar in ('i8', 'i16', 'i32', 'i64'):
            return 'uint' if count == 1 else f'uvec{count}'
        if scalar == 'void':
            return 'void'
        raise ValueError('Untranslated value type: ' + type_name)

    def layout(self, type_name):
        if type_name in self.types:
            fields, packed = self.types[type_name]
            offset, alignment, offsets = 0, 1, []
            for field in fields:
                size, align, _ = self.layout(field); align = 1 if packed else align
                offset = (offset + align - 1) // align * align
                offsets.append(offset); offset += size; alignment = max(alignment, align)
            return (offset + alignment - 1) // alignment * alignment, alignment, offsets
        match = re.fullmatch(r'\[(\d+) x (.+)\]', type_name)
        if match:
            size, align, _ = self.layout(match[2]); return int(match[1]) * size, align, None
        count, scalar = shape(type_name)
        size = {'half': 2, 'float': 4, 'i1': 1, 'i8': 1, 'i16': 2, 'i32': 4, 'i64': 8}[scalar]
        allocation = size * (4 if count == 3 else count)
        return allocation, allocation, None

    def zero(self, type_name):
        if type_name in self.types:
            return self.glsl(type_name) + '(' + ', '.join(self.zero(t) for t in self.types[type_name][0]) + ')'
        return self.glsl(type_name) + '(0)'

    def value(self, text, type_name):
        text = text.strip()
        if text.startswith('%'):
            return self.values[text]
        if text in ('undef', 'poison', 'zeroinitializer'):
            return self.zero(type_name)
        count, scalar = shape(type_name)
        if count > 1:
            values = [self.value(v, t) for t, v in map(typed, split(text[1:-1]))]
            if len(values) != count:
                raise ValueError('Vector constant lane mismatch')
            return self.glsl(type_name) + '(' + ', '.join(values) + ')'
        if scalar == 'i1':
            return {'true': 'true', 'false': 'false', '0': 'false', '1': 'true'}[text]
        if scalar in ('float', 'half'):
            if text.startswith('0xH'):
                return number(struct.unpack('>e', bytes.fromhex(text[3:]))[0])
            if text.startswith('0x'):
                return number(struct.unpack('>d', bytes.fromhex(text[2:]))[0])
            return number(float(text))
        if scalar.startswith('i'):
            return str(int(text) & 0xffffffff) + 'u'
        if scalar.startswith('ptr') and text.startswith('inttoptr'):
            return int(re.search(r'i64 (-?\d+)', text)[1]) & ((1 << 64) - 1)
        raise ValueError('Unknown value: ' + type_name + ' ' + text)

    def operand(self, text):
        type_name, value = typed(text)
        return type_name, self.value(value, type_name)

    def assign(self, dest, type_name, expression, lines, round_half=False):
        name = 'v' + dest[1:]
        self.values[dest] = name; self.value_types[dest] = type_name
        self.declarations.append(self.glsl(type_name) + ' ' + name + ';')
        if round_half and shape(type_name)[1] == 'half':
            expression = 'airHalf(' + expression + ')'
        lines.append(name + ' = ' + expression + ';')

    def load(self, pointer, type_name):
        if pointer.kind == 'local':
            return pointer.root
        if pointer.kind != 'buffer':
            raise ValueError('Invalid uniform pointer')
        count, scalar = shape(type_name)
        if scalar != 'float' or not isinstance(pointer.offset, int):
            raise ValueError('Unsupported buffer load: ' + type_name)
        if pointer.offset % 4:
            raise ValueError('Misaligned float buffer load')
        if pointer.offset + count * 4 > pointer.type:
            raise ValueError('Buffer load exceeds original AIR allocation')
        words = [f'uintBitsToFloat({pointer.root}.words[{(pointer.offset // 4 + i) // 4}][{(pointer.offset // 4 + i) % 4}])' for i in range(count)]
        return words[0] if count == 1 else self.glsl(type_name) + '(' + ', '.join(words) + ')'

    def pointer(self, dest, text):
        parts = split(text.removeprefix('inbounds ')); type_name = parts[0]
        _, pointer = self.operand(parts[1]); indices = [typed(p)[1] for p in parts[2:]]
        if indices[0] != '0':
            raise ValueError('Nonzero base GEP')
        offset, root = pointer.offset, pointer.root
        current = type_name
        for index in indices[1:]:
            array = re.fullmatch(r'\[(\d+) x (.+)\]', current)
            if array:
                if pointer.kind != 'local':
                    raise ValueError('Dynamic uniform array GEP unsupported')
                root += '[int(' + self.value(index, 'i64') + ')]'; current = array[2]
            elif current in self.types:
                field = int(index)
                offset += self.layout(current)[2][field]; current = self.types[current][0][field]
            else:
                raise ValueError('Unknown GEP type ' + current)
        self.values[dest] = Pointer(pointer.kind, root, offset, pointer.type)

    def call(self, dest, text, lines):
        text = text.removeprefix('tail ').removeprefix('call ')
        prefix, rest = text.split('@', 1); result_type, extra = typed(prefix.strip())
        if extra:
            raise ValueError('Unknown call result modifiers')
        name, parameters = rest.split('(', 1)
        parameters = parameters[:parameters.rfind(')')]
        args = [self.operand(x) for x in split(parameters)] if parameters else []
        values = [x[1] for x in args]
        function = name.removeprefix('air.').split('.')[0]
        math = {'fabs': 'abs', 'fmin': 'min', 'fmax': 'max', 'fast_fmin': 'min', 'fast_fmax': 'max', 'fast_floor': 'floor',
                'fast_sqrt': 'sqrt', 'fast_rsqrt': 'inversesqrt', 'fast_exp2': 'exp2', 'fast_log': 'log',
                'fast_pow': 'pow', 'fast_clamp': 'clamp', 'mix': 'mix', 'dot': 'dot', 'any': 'any', 'all': 'all', 'sign': 'sign', 'log2': 'log2',
                'sqrt': 'sqrt', 'clamp': 'airOrderedClamp'}
        if function in math:
            expression = math[function] + '(' + ', '.join(values) + ')'
        elif function == 'convert':
            expression = self.glsl(result_type) + '(' + values[0] + ')'
        elif function == 'fast_fmod':
            expression = f'({values[0]} - {values[1]} * trunc({values[0]} / {values[1]}))'
        elif function in ('get_width_texture_2d', 'get_height_texture_2d'):
            expression = f'uint(textureSize({values[0]}, int({values[1]})).' + ('x' if function.startswith('get_width') else 'y') + ')'
        elif function in ('sample_texture_2d', 'gather_texture_2d'):
            texture, raw, coord, valid, offset = values[:5]
            if valid != 'true':
                raise ValueError('Untranslated AIR texture operand/sampler')
            sampler = decode_air_sampler(raw)
            if raw not in self.samplers:
                self.samplers[raw] = len(self.samplers)
            binding = 240 + self.samplers[raw]
            sampled = f'sampler2D({texture}, i2aSampler{binding})'
            if sampler['pixel_coordinates']:
                coord = f'(({coord}) / vec2(textureSize({texture}, 0)))'
            signed_offset = 'ivec2(' + offset + ')'
            if function.startswith('gather'):
                expression = f'textureGatherOffset({sampled}, {coord}, {signed_offset}, int({values[5]}))'
            elif values[5] == 'true' or self.stage == 'comp' or sampler['pixel_coordinates']:
                expression = f'textureLodOffset({sampled}, {coord}, {values[6]}, {signed_offset})'
            else:
                if values[6] != '0.0':
                    raise ValueError('Untranslated implicit sample bias')
                expression = f'textureOffset({sampled}, {coord}, {signed_offset})'
        elif function == 'write_texture_2d':
            if values[3] != '0u':
                raise ValueError('Nonzero storage image mip')
            lines.append(f'imageStore({values[0]}, ivec2({values[1]}), {values[2]});'); return
        else:
            raise ValueError('Untranslated AIR intrinsic: ' + name)
        self.assign(dest, result_type, expression, lines, round_half=True)

    def instruction(self, dest, op, text, lines):
        self.stats[op] += 1
        if op == 'tail' or op == 'call':
            self.call(dest, op + ' ' + text, lines); return
        if op in ('fmul', 'fadd', 'fsub', 'fdiv', 'or'):
            t, operands = typed(text.removeprefix('fast ')); parts = split(operands)
            if any(not re.fullmatch(r'!fpmath !\d+', p) for p in parts[2:]):
                raise ValueError('Untranslated arithmetic metadata')
            a, b = parts[:2]
            symbol = {'fmul': '*', 'fadd': '+', 'fsub': '-', 'fdiv': '/', 'or': '||' if t == 'i1' else '|'}[op]
            self.assign(dest, t, f'({self.value(a,t)} {symbol} {self.value(b,t)})', lines, round_half=True); return
        if op in ('icmp', 'fcmp'):
            comparison, rest = text.split(' ', 1); t, operands = typed(rest); a, b = split(operands)
            left, right = self.value(a,t), self.value(b,t)
            symbol = {'eq': '==', 'oeq': '==', 'une': '!=', 'ult': '<', 'olt': '<', 'ogt': '>', 'ole': '<=', 'oge': '>='}[comparison]
            n, _ = shape(t); bt = 'i1' if n == 1 else f'<{n} x i1>'
            if n == 1: expr = f'({left} {symbol} {right})'
            else: expr = {'==':'equal','!=':'notEqual','<':'lessThan','>':'greaterThan','<=':'lessThanEqual','>=':'greaterThanEqual'}[symbol] + f'({left}, {right})'
            self.assign(dest, bt, expr, lines); return
        if op == 'select':
            condition, a, b = map(self.operand, split(text)); t, left = a; right = b[1]
            expr = f'({condition[1]} ? {left} : {right})' if condition[0] == 'i1' else f'mix({right}, {left}, {condition[1]})'
            self.assign(dest, t, expr, lines); return
        if op == 'shufflevector':
            a, b, mask = split(text); ta, va = self.operand(a); tb, vb = self.operand(b)
            tm, raw = typed(mask); n, _ = shape(tm); width, scalar = shape(ta)
            indices = ['0'] * n if raw == 'zeroinitializer' else [typed(x)[1] for x in split(raw[1:-1])]
            lanes = []
            for index in indices:
                if index in ('undef', 'poison'): lanes.append(self.zero(scalar)); continue
                i = int(index); lanes.append(f'({va if i < width else vb})[{i if i < width else i-width}]')
            t = f'<{n} x {scalar}>'; self.assign(dest, t, self.glsl(t) + '(' + ', '.join(lanes) + ')', lines); return
        if op == 'insertelement':
            aggregate, element, index = split(text); t, value = self.operand(aggregate); _, element = self.operand(element)
            index = int(typed(index)[1]); self.assign(dest, t, value, lines)
            lines.append(f'{self.values[dest]}[{index}] = {element};'); return
        if op == 'extractelement':
            aggregate, index = split(text); t, value = self.operand(aggregate); _, scalar = shape(t)
            self.assign(dest, scalar, f'({value})[{int(typed(index)[1])}]', lines); return
        if op == 'insertvalue':
            aggregate, element, index = split(text); t, value = self.operand(aggregate); _, element = self.operand(element)
            self.assign(dest, t, value, lines); lines.append(f'{self.values[dest]}.m{int(index)} = {element};'); return
        if op in ('fptrunc', 'fpext', 'zext', 'bitcast'):
            before, t = text.rsplit(' to ', 1); _, value = self.operand(before)
            if op == 'bitcast':
                if not isinstance(value, Pointer): raise ValueError('Non-pointer bitcast')
                self.values[dest] = value; return
            self.assign(dest, t, self.glsl(t) + '(' + value + ')', lines, round_half=op=='fptrunc'); return
        if op == 'getelementptr':
            self.pointer(dest, text); return
        if op == 'load':
            parts = split(text); t = parts[0]; _, pointer = self.operand(parts[1])
            self.assign(dest, t, self.load(pointer,t), lines); return
        if op == 'alloca':
            t = split(text)[0]; count, item = re.fullmatch(r'\[(\d+) x (.+)\]', t).groups()
            name = 'local' + dest[1:]; self.declarations.append(f'{self.glsl(item)} {name}[{count}];')
            self.values[dest] = Pointer('local',name,type_name=t); return
        if op == 'store':
            value, target, *_ = split(text); _, value = self.operand(value); _, pointer = self.operand(target)
            if pointer.kind != 'local': raise ValueError('Unexpected nonlocal store')
            lines.append(f'{pointer.root} = {value};'); return
        raise ValueError('Untranslated IR instruction: ' + op + ' ' + text)

    def configure(self, parameters):
        for index, param in enumerate(split(parameters)):
            t, dest = typed(param); dest = dest.split()[-1]
            argument = next(a for a in self.entry['inputs'] if a['argument'] == index)
            kind, props = argument['kind'], argument['properties']
            self.value_types[dest] = t
            if kind == 'buffer':
                binding = argument['index']; name = f'i2aBuffer{binding}'
                self.resources.append(f'layout(std140,set={self.set},binding={binding}) uniform AirBuffer{binding} {{ uvec4 words[{(argument["size"]+15)//16}]; }} {name};')
                self.values[dest] = Pointer('buffer', name, type_name=argument['size'])
            elif kind == 'texture':
                binding = 32 + argument['index']; name = f'i2aTexture{binding}'
                if not argument['type'].startswith('texture2d<'): raise ValueError('Non-2D Metal-only image')
                qualifier, type_name = ('writeonly ', 'image2D') if props.get('air.write') else ('', 'texture2D')
                self.resources.append(f'layout(set={self.set},binding={binding}) uniform {qualifier}{type_name} {name};')
                self.values[dest] = name
            elif kind == 'vertex_id':
                self.values[dest] = '(uint(gl_VertexIndex) & 65535u)' if t == 'i16' else 'uint(gl_VertexIndex)'
            elif kind == 'position':
                self.values[dest] = 'gl_FragCoord'
            elif kind == 'fragment_input':
                locations = self.interface_locations
                location = locations[argument['name']]
                name = 'airIn_' + argument['name']; self.resources.append(f'layout(location={location}) in {self.glsl(t)} {name};')
                self.values[dest] = f'airHalf({name})' if self.round_fragment_inputs and shape(t)[1]=='half' else name
            elif kind == 'thread_position_in_grid':
                self.values[dest] = 'gl_GlobalInvocationID.xy'
            elif props.get('air.threads_per_grid'):
                self.values[dest] = '(gl_NumWorkGroups.xy * gl_WorkGroupSize.xy)'
            else:
                raise ValueError('Untranslated function input: ' + kind)

    def return_lines(self, text):
        t, raw = typed(text)
        if t == 'void': return ['return;']
        value = self.value(raw,t)
        if self.stage == 'frag':
            if t in self.types:
                if len(self.types[t][0]) != 1:
                    raise ValueError('Multiple AIR fragment targets require separate outputs')
                value += '.m0'
            return [f'airColor = {value};', 'return;']
        if self.stage == 'vert':
            lines = []
            for i, output in enumerate(self.entry['outputs']):
                props = output['properties']
                if props.get('air.position'): lines.append(f'gl_Position = {value}.m{i};')
                elif 'air.vertex_output' in props: lines.append(f'airOut_{output["name"]} = {value}.m{i};')
                else: raise ValueError('Untranslated vertex output')
            # Metal NDC +Y is up. A positive Vulkan viewport maps +Y down.
            lines += ['gl_Position.y = -gl_Position.y;', 'return;']
            return lines
        raise ValueError('Unexpected return value')

    def translate(self):
        signature = re.search(r'^define (.+?) @([\w]+)\((.*)\).*\{$', self.ir, re.M)
        if not signature or signature[2] != self.entry['entry']: raise ValueError('Entry mismatch')
        self.configure(signature[3]); result_type = signature[1]
        if self.stage == 'frag':
            color_type = self.types[result_type][0][0] if result_type in self.types else result_type
            self.resources.append(f'layout(location=0) out {self.glsl(color_type)} airColor;')
        if self.stage == 'vert':
            for field, output in zip(self.types[result_type][0], self.entry['outputs']):
                if 'air.vertex_output' in output['properties']:
                    location = self.interface_locations[output['name']]
                    self.resources.append(f'layout(location={location}) out {self.glsl(field)} airOut_{output["name"]};')
        if self.stage == 'comp': self.resources.append('layout(local_size_x_id=0,local_size_y_id=1,local_size_z_id=2) in;')
        block = str(len(split(signature[3]))); self.first = block; self.blocks[block] = {'lines': []}
        body = self.ir[signature.end():].split('\n}',1)[0]
        for raw in body.splitlines():
            text = raw.split(';',1)[0].strip()
            if not text: continue
            if re.fullmatch(r'\d+:',text):
                block = text[:-1]; self.blocks[block] = {'lines': []}; continue
            if text.startswith('br '):
                parts = split(text[3:])
                self.blocks[block]['term'] = ('branch', self.operand(parts[0])[1], parts[1].split('%')[1],parts[2].split('%')[1]) if len(parts)==3 else ('jump',parts[0].split('%')[1])
                continue
            if text.startswith('ret '):
                self.blocks[block]['return'] = text[4:]; self.blocks[block]['term'] = ('return',); continue
            dest, instruction = text.split(' = ',1) if ' = ' in text else (None,text)
            op, rest = instruction.split(' ',1)
            if op == 'phi':
                t, options = typed(rest); name='v'+dest[1:]
                self.values[dest]=name; self.value_types[dest]=t; self.declarations += [f'{self.glsl(t)} {name};',f'{self.glsl(t)} phi{dest[1:]};']
                incoming = {pred.strip().lstrip('%'): value.strip() for value,pred in (split(x.strip()[1:-1]) for x in split(options))}
                self.phis[block].append((dest,t,incoming)); continue
            lines=self.blocks[block]['lines']
            self.instruction(dest,op,rest,lines)
        order={b:i for i,b in enumerate(self.blocks)}
        postdom={'exit':{'exit'}}
        for b in reversed(self.blocks):
            term=self.blocks[b]['term']; successors=term[2:] if term[0]=='branch' else term[1:] if term[0]=='jump' else ('exit',)
            if any(s!='exit' and order[s]<=order[b] for s in successors): raise ValueError('Cyclic AIR CFG is not supported')
            postdom[b]={b}|set.intersection(*(postdom[s] for s in successors))
        visited=set()
        def edge(a,b,indent):
            statements=[]
            for dest,t,incoming in self.phis[b]: statements.append(f'phi{dest[1:]} = {self.value(incoming[a],t)};')
            for dest,_,_ in self.phis[b]: statements.append(f'v{dest[1:]} = phi{dest[1:]};')
            return ['    '*indent+s for s in statements]
        def region(b,stop,indent):
            lines=[]
            while b!=stop and b!='exit':
                if b in visited: raise ValueError('Unstructured shared AIR block '+b)
                visited.add(b); info=self.blocks[b]
                lines += ['    '*indent+s for s in info['lines']]
                term=info['term']
                if term[0]=='return':
                    lines += ['    '*indent+s for s in self.return_lines(info['return'])]; break
                if term[0]=='jump':
                    lines+=edge(b,term[1],indent); b=term[1]; continue
                _,condition,yes,no=term
                merge=max(postdom[yes]&postdom[no],key=lambda x:len(postdom[x]))
                lines.append('    '*indent+f'if ({condition}) {{')
                lines+=edge(b,yes,indent+1)+region(yes,merge,indent+1)
                lines.append('    '*indent+'} else {')
                lines+=edge(b,no,indent+1)+region(no,merge,indent+1)
                lines.append('    '*indent+'}'); b=merge
            return lines
        body_lines=region(self.first,'exit',1)
        if visited!=set(self.blocks): raise ValueError('Untranslated AIR blocks')
        for raw,index in self.samplers.items(): self.resources.append(f'layout(set={self.set},binding={240+index}) uniform sampler i2aSampler{240+index}; // original constexpr 0x{raw:016x}')
        structs=[]
        for name,(fields,_) in self.types.items():
            if any(f.startswith('[') for f in fields): continue
            structs.append('struct '+self.names[name]+' { '+' '.join(self.glsl(t)+f' m{i};' for i,t in enumerate(fields))+' };')
        half=['float airHalf(float x) { return unpackHalf2x16(packHalf2x16(vec2(x,0.0))).x; }',
              'vec2 airHalf(vec2 x) { return unpackHalf2x16(packHalf2x16(x)); }',
              'vec3 airHalf(vec3 x) { return vec3(airHalf(x.xy), airHalf(x.z)); }',
              'vec4 airHalf(vec4 x) { return vec4(airHalf(x.xy), airHalf(x.zw)); }',
              'float airOrderedClamp(float x, float lo, float hi) { return isnan(x) ? lo : clamp(x,lo,hi); }']
        return '\n'.join(['#version 450','#extension GL_EXT_samplerless_texture_functions : require',
            '// Derived from original AIR; SSA IDs retain a direct audit trail.',*structs,*half,*self.resources,
            'void main() {',*('    '+s for s in self.declarations),*body_lines,'}',''])


