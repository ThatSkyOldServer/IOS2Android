"""Relocate Mach-O DWARF CIE/FDE pointer fields into ELF .eh_frame.

The compiler's CFA program is retained byte for byte. Pointer encodings,
personality indirection, ranges and LSDA references retain their native shape.
"""
import struct


def _leb(data, position, signed=False):
    value, shift = 0, 0
    while True:
        if position >= len(data) or shift > 63:
            raise ValueError('Invalid DWARF LEB128')
        byte = data[position]
        position += 1
        value |= (byte & 127) << shift
        shift += 7
        if not byte & 128:
            if signed and byte & 64:
                value -= 1 << shift
            return value, position


def _pointer(data, position, encoding, address):
    kind = encoding & 15
    sizes = {0: 8, 2: 2, 3: 4, 4: 8, 10: 2, 11: 4, 12: 8}
    if kind not in sizes or position + sizes[kind] > len(data):
        raise ValueError(f'Unsupported DWARF pointer encoding {encoding:#x}')
    size = sizes[kind]
    value = int.from_bytes(data[position:position + size], 'little', signed=kind >= 8)
    application = encoding & 0x70
    if application == 0x10:
        value = (value + address + position) & 0xffffffffffffffff
    elif application:
        raise ValueError(f'Unsupported DWARF pointer base {encoding:#x}')
    return value, position + size, size


def _emit_pointer(encoding, target, base):
    size = {0: 8, 2: 2, 3: 4, 4: 8, 10: 2, 11: 4, 12: 8}.get(encoding & 15)
    if not size or encoding & 0x70 not in (0, 0x10):
        raise ValueError(f'Unsupported DWARF pointer encoding {encoding:#x}')
    directive = {2: '.short', 4: '.long', 8: '.quad'}[size]
    expression = '0' if not target else f'guest_text+{target - base}' + ('-.' if encoding & 0x70 else '')
    return directive + ' ' + expression


def translate_dwarf_unwind(image, references):
    section = image.get_section('__eh_frame')
    if section is None:
        raise ValueError('DWARF compact-unwind entry has no __eh_frame section')
    data, address, base = bytes(section.content), section.virtual_address, image.imagebase
    cies, fdes = {}, {}

    def record(at):
        if at < 0 or at + 8 > len(data):
            raise ValueError('DWARF record outside __eh_frame')
        length, identifier = struct.unpack_from('<II', data, at)
        if length < 4 or length == 0xffffffff or at + 4 + length > len(data):
            raise ValueError('Invalid DWARF record size')
        return identifier, at + 4 + length

    def cie(at):
        if at in cies:
            return cies[at]
        identifier, end = record(at)
        if identifier:
            raise ValueError('DWARF CIE reference is not a CIE')
        position = at + 8
        version = data[position]
        position += 1
        finish = data.find(b'\0', position, end)
        if version not in (1, 3) or finish < 0:
            raise ValueError('Unsupported DWARF CIE version/augmentation')
        augmentation = data[position:finish].decode('ascii')
        position = finish + 1
        code, position = _leb(data, position)
        alignment, position = _leb(data, position, True)
        register, position = _leb(data, position)
        if not augmentation.startswith('z') or any(c not in 'zPLR' for c in augmentation):
            raise ValueError('Unsupported DWARF CIE augmentation: ' + augmentation)
        length, position = _leb(data, position)
        augmentation_end = position + length
        fields, personality, lsda, encoding = [], 0, 0xff, 0
        for key in augmentation[1:]:
            if position >= augmentation_end:
                raise ValueError('Truncated DWARF CIE augmentation')
            value = data[position]
            position += 1
            fields.append('.byte ' + str(value))
            if key == 'P':
                personality, position, _ = _pointer(data, position, value, address)
                fields.append(_emit_pointer(value, personality, base))
            elif key == 'L':
                lsda = value
            else:
                encoding = value
        if position != augmentation_end or position > end:
            raise ValueError('Invalid DWARF CIE augmentation bounds')
        result = dict(at=at, version=version, augmentation=augmentation, code=code, alignment=alignment,
                      register=register, augmentation_bytes=length, fields=fields,
                      encoding=encoding, lsda=lsda, instructions=data[position:end])
        cies[at] = result
        return result

    for first, limit, encoded, _ in references:
        at = encoded & 0x00ffffff
        if at in fdes:
            continue
        distance, end = record(at)
        description = cie(at + 4 - distance)
        position = at + 8
        start, position, _ = _pointer(data, position, description['encoding'], address)
        length, position, _ = _pointer(data, position, description['encoding'] & 15, 0)
        if not start <= base + first < start + length:
            raise ValueError('Compact-unwind DWARF reference differs from its FDE range')
        augmentation_bytes, position = _leb(data, position)
        augmentation_end = position + augmentation_bytes
        lsda = 0
        if description['lsda'] != 0xff and position < augmentation_end:
            original_position = position
            lsda, position, size = _pointer(data, position, description['lsda'], address)
            if not any(data[original_position:original_position + size]):
                lsda = 0
        if position != augmentation_end or position > end:
            raise ValueError('Unsupported DWARF FDE augmentation')
        fdes[at] = dict(cie=description, start=start, length=length, lsda=lsda,
                       augmentation_bytes=augmentation_bytes, instructions=data[position:end])

    assembly = []
    for at, description in sorted(cies.items()):
        label = f'.Lguest_dwarf_cie_{at}'
        assembly += ['.balign 8', label + ':', f'.long {label}_end-{label}_body', label + '_body:', '.long 0',
                     '.byte ' + str(description['version']), '.asciz "' + description['augmentation'] + '"',
                     '.uleb128 ' + str(description['code']), '.sleb128 ' + str(description['alignment']),
                     '.uleb128 ' + str(description['register']), '.uleb128 ' + str(description['augmentation_bytes'])]
        assembly += description['fields']
        if description['instructions']:
            assembly.append('.byte ' + ','.join(str(v) for v in description['instructions']))
        assembly += ['.balign 8,0', label + '_end:']
    for at, description in sorted(fdes.items()):
        label = f'.Lguest_dwarf_fde_{at}'
        cie_label = f'.Lguest_dwarf_cie_{description["cie"]["at"]}'
        encoding = description['cie']['encoding']
        size = {0: 8, 2: 2, 3: 4, 4: 8, 10: 2, 11: 4, 12: 8}[encoding & 15]
        assembly += ['.balign 8', label + ':', f'.long {label}_end-{label}_body', label + '_body:',
                     f'.long .-{cie_label}', _emit_pointer(encoding, description['start'], base),
                     {2: '.short', 4: '.long', 8: '.quad'}[size] + ' ' + str(description['length']),
                     '.uleb128 ' + str(description['augmentation_bytes'])]
        if description['augmentation_bytes']:
            assembly.append(_emit_pointer(description['cie']['lsda'], description['lsda'], base))
        if description['instructions']:
            assembly.append('.byte ' + ','.join(str(v) for v in description['instructions']))
        assembly += ['.balign 8,0', label + '_end:']
    return assembly, dict(cies=len(cies), fdes=len(fdes))
