"""Catalogue exact-IPA interfaces and conservatively infer Objective-C receivers.

All selectors and imports are inventoried. Known receiver/selector pairs are
probed by the Android runtime without invoking methods. Unresolved receivers,
dynamic dispatch, and conditional API semantics remain explicit limitations.
"""
from collections import defaultdict
import json
import re
import struct

import capstone
import lief

REVISION = 6
FACTORY_RETURN_CLASSES = {
    ('PHImageManager', 'defaultManager'): 'PHImageManager',
    ('NSNotificationQueue', 'defaultQueue'): 'NSNotificationQueue',
}
RETURN_CLASSES = {
    'view': 'UIView', 'viewIfLoaded': 'UIView', 'layer': 'CALayer',
    'popoverPresentationController': 'UIPopoverPresentationController',
    'sharedApplication': 'UIApplication', 'standardUserDefaults': 'NSUserDefaults',
    'mainBundle': 'NSBundle', 'defaultManager': 'NSFileManager',
    'processInfo': 'NSProcessInfo', 'mainThread': 'NSThread',
    'currentThread': 'NSThread', 'mainScreen': 'UIScreen',
    'defaultCenter': 'NSNotificationCenter', 'currentDevice': 'UIDevice',
    'localPlayer': 'GKLocalPlayer', 'defaultQueue': 'SKPaymentQueue',
    'decimalNumberWithString:': 'NSDecimalNumber',
    'decimalNumberWithString:locale:': 'NSDecimalNumber',
    'decimalNumberWithMantissa:exponent:isNegative:': 'NSDecimalNumber',
    'decimalNumberByMultiplyingBy:': 'NSDecimalNumber',
    'decimalNumberByDividingBy:': 'NSDecimalNumber',
    'decimalNumberByMultiplyingByPowerOf10:': 'NSDecimalNumber',
    'maximumDecimalNumber': 'NSDecimalNumber', 'minimumDecimalNumber': 'NSDecimalNumber',
    'notANumber': 'NSDecimalNumber',
}


def scan_interfaces(binary):
    raw = binary.read_bytes()
    image = lief.MachO.parse(str(binary)).at(0)
    sections = [(s.virtual_address, s.virtual_address + s.size, s.offset) for s in image.sections]

    def read(address, size=8):
        for start, end, offset in sections:
            if start <= address and address + size <= end:
                return raw[offset + address - start:offset + address - start + size]
        return b''

    def word(address):
        data = read(address)
        return struct.unpack('<Q', data)[0] if len(data) == 8 else 0

    def string(address):
        address &= 0x00ffffffffffffff
        for start, end, offset in sections:
            if start <= address < end:
                position = offset + address - start
                return raw[position:raw.find(b'\0', position)].decode('utf8', errors='replace')
        return ''

    bindings = {b.address: b.symbol.name for b in image.bindings}
    stubs = image.get_section('__stubs')
    indirect = list(image.dynamic_symbol_command.indirect_symbols)
    imports = {stubs.virtual_address + i * stubs.reserved2: s.name for i, s in enumerate(
        indirect[stubs.reserved1:stubs.reserved1 + stubs.size // stubs.reserved2])} if stubs and stubs.reserved2 else {}
    section = image.get_section('__objc_selrefs')
    selectors = {section.virtual_address + i: string(word(section.virtual_address + i)) for i in range(0, section.size, 8)} if section else {}
    classes, owners, ivars, properties = {}, {}, {}, {}

    def methods(address, owner, class_method):
        if not address:
            return
        header = read(address, 8)
        if len(header) != 8:
            raise ValueError(f'Method table outside file-backed metadata at {address:#x}')
        flags, count = struct.unpack('<II', header)
        relative = bool(flags & 0x80000000)
        size = flags & 0xfffc
        if size != (12 if relative else 24):
            raise ValueError(f'Unsupported method table at {address:#x}')
        for i in range(count):
            entry = address + 8 + i * size
            if relative:
                encoded = read(entry, 12)
                if len(encoded) != 12:
                    raise ValueError(f'Truncated relative method entry at {entry:#x}')
                selector, _, implementation = struct.unpack('<iii', encoded)
                selector += entry
                if not flags & 0x40000000:
                    selector = word(selector)
                implementation += entry + 8
            else:
                selector, implementation = word(entry), word(entry + 16)
            owners[implementation] = (owner, class_method, string(selector))

    def class_identity(address):
        bound = bindings.get(address)
        if bound and bound.startswith('_OBJC_CLASS_$_'):
            return bound.removeprefix('_OBJC_CLASS_$_')
        return classes.get(word(address))

    class_list = image.get_section('__objc_classlist')
    for (address,) in struct.iter_unpack('<Q', bytes(class_list.content) if class_list else b''):
        ro = word(address + 32) & ~7
        classes[address] = string(word(ro + 24))
    for address, name in classes.items():
        for entry, class_method in [(address, False), (word(address), True)]:
            ro = word(entry + 32) & ~7
            methods(word(ro + 32), name, class_method)
        ro = word(address + 32) & ~7
        table = word(ro + 48)
        ivars[name] = {}
        if table:
            size, count = struct.unpack('<II', read(table, 8))
            for i in range(count):
                entry = table + 8 + i * size
                encoding = string(word(entry + 16))
                match = re.match(r'@"([^"<]+)', encoding)
                if match:
                    offset = struct.unpack('<i', read(word(entry), 4))[0]
                    ivars[name][offset] = match[1]
                    field = string(word(entry + 8)).lstrip('_')
                    properties[(name, field)] = match[1]
    categories = image.get_section('__objc_catlist')
    if categories:
        for (address,) in struct.iter_unpack('<Q', bytes(categories.content)):
            name = class_identity(address + 8)
            if name:
                methods(word(address + 16), name, False)
                methods(word(address + 24), name, True)

    def reg(name):
        return 'x' + name[1:] if name.startswith('w') else name

    def immediate(value):
        try:
            return int(value.lstrip('#'), 0)
        except ValueError:
            return None

    starts = sorted(image.imagebase + f for f in image.function_starts.functions)
    text = image.get_section('__text')
    starts.append(text.virtual_address + text.size)
    decoder = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    pairs = defaultdict(list)
    references = defaultdict(list)
    calls = defaultdict(list)
    for index, start in enumerate(starts[:-1]):
        instructions = list(decoder.disasm_lite(read(start, starts[index + 1] - start), start))
        targets = set()
        for address, _, mnemonic, operands in instructions:
            if mnemonic.startswith('b.') or mnemonic in ('b', 'cbz', 'cbnz', 'tbz', 'tbnz'):
                value = immediate(operands.split(', ')[-1])
                if value is not None and start < value < starts[index + 1]:
                    targets.add(value)
        registers = {}
        owner = owners.get(start)
        if owner:
            registers['x0'] = ('class' if owner[1] else 'object', owner[0])
        for address, _, mnemonic, operands in instructions:
            if address in targets:
                registers.clear()  # Do not invent merged control-flow values.
            parts = operands.split(', ')
            destination = reg(parts[0]) if parts else ''
            value = None
            if mnemonic in ('adrp', 'adr'):
                value = ('address', immediate(parts[1]))
            elif mnemonic in ('add', 'mov') and len(parts) >= 2:
                source = registers.get(reg(parts[1]))
                if mnemonic == 'mov':
                    value = source or (('address', immediate(parts[1])) if immediate(parts[1]) is not None else None)
                elif len(parts) == 3 and source and source[0] == 'address' and immediate(parts[2]) is not None:
                    value = ('address', source[1] + immediate(parts[2]))
            elif mnemonic in ('ldr', 'ldur', 'ldrsw'):
                match = re.search(r'\[(\w+)(?:, ([^\]]+))?\]', operands)
                if match:
                    base = registers.get(reg(match[1]))
                    offset = immediate(match[2] or '0')
                    if offset is None:
                        other = registers.get(reg(match[2]))
                        offset = other[1] if other and other[0] == 'address' else None
                    if base and offset is not None:
                        if base[0] == 'address':
                            slot = base[1] + offset
                            if slot in selectors:
                                value = ('selector', selectors[slot])
                                references[selectors[slot]].append({'load': hex(address), 'function': hex(start), 'owner': owner})
                            elif class_identity(slot):
                                value = ('class', class_identity(slot))
                            else:
                                data = read(slot, 4 if mnemonic == 'ldrsw' else 8)
                                if data:
                                    value = ('address', int.from_bytes(data, 'little', signed=mnemonic == 'ldrsw'))
                        elif base[0] == 'object' and offset in ivars.get(base[1], {}):
                            value = ('object', ivars[base[1]][offset])
            if mnemonic in ('bl', 'b'):
                target = immediate(operands)
                imported = imports.get(target)
                returned = None
                if imported:
                    calls[imported].append({'call': hex(address), 'function': hex(start)})
                if imported == '_objc_msgSend':
                    receiver, selector = registers.get('x0'), registers.get('x1')
                    if receiver and receiver[0] in ('class', 'object') and selector and selector[0] == 'selector':
                        name = selector[1]
                        pairs[(receiver[1], name, receiver[0] == 'class')].append({'call': hex(address), 'function': hex(start)})
                        if name in ('alloc', 'allocWithZone:', 'new') or name.startswith('init'):
                            returned = ('object', receiver[1])
                        elif name in ('copy', 'mutableCopy', 'retain', 'autorelease'):
                            returned = receiver
                        else:
                            result = properties.get((receiver[1], name)) or FACTORY_RETURN_CLASSES.get((receiver[1], name)) or RETURN_CLASSES.get(name)
                            if result:
                                returned = ('object', result)
                elif imported in ('_objc_retain', '_objc_retainAutoreleasedReturnValue', '_objc_autoreleaseReturnValue'):
                    returned = registers.get('x0')
                for n in range(19):
                    registers.pop(f'x{n}', None)
                if returned:
                    registers['x0'] = returned
            elif mnemonic not in ('cmp', 'cmn', 'tst', 'str', 'stur', 'stp', 'ret', 'cbz', 'cbnz', 'tbz', 'tbnz', 'nop') and not mnemonic.startswith('b.'):
                registers.pop(destination, None)
                if mnemonic == 'ldp' and len(parts) > 1:
                    registers.pop(reg(parts[1]), None)
                if value is not None:
                    registers[destination] = value
        if index and index % 15000 == 0:
            print(f'Interface scan: {index}/{len(starts)-1} functions', flush=True)
    return {'revision': REVISION,
            'functions_scanned': len(starts)-1, 'import_symbols': sorted(set(bindings.values())),
            'selectors': sorted(set(selectors.values())), 'selector_references': dict(references),
            'inferred_pairs': [{'class': c, 'selector': s, 'class_method': m, 'sites': sites}
                               for (c, s, m), sites in sorted(pairs.items())], 'import_calls': dict(calls),
            'limitations': 'Conservative straight-line receiver inference; branch merges and unresolved receivers are not coverage. A bound IMP is not proof of complete API semantics.'}


