"""Translate ARM64 Mach-O compact unwind records into ELF .eh_frame.

The original code and LSDA stay at their original image-relative offsets.
Only the unwind container changes; Android's libc++abi then executes the
original cleanup/catch landing pads. No runtime frame scanning is needed.
Encoding reference: LLVM libunwind compact_unwind_encoding.h / CompactUnwinder.
"""

from collections import Counter
import json
import struct


FRAME = 0x04000000
FRAMELESS = 0x02000000
MODE_MASK = 0x0F000000
HAS_LSDA = 0x40000000
REGISTER_PAIRS = (
    (0x001, 19, 20), (0x002, 21, 22), (0x004, 23, 24),
    (0x008, 25, 26), (0x010, 27, 28),
    (0x100, 72, 73), (0x200, 74, 75),
    (0x400, 76, 77), (0x800, 78, 79),
)


def _record(data, offset, form):
    size = struct.calcsize(form)
    if offset < 0 or offset + size > len(data):
        raise ValueError(f'Compact unwind record outside section: {offset:#x}')
    return struct.unpack_from(form, data, offset)


def _decode(data):
    version, common_at, common_count, personality_at, personality_count, index_at, index_count = _record(data, 0, '<7I')
    if version != 1 or personality_count > 3 or index_count < 2:
        raise ValueError('Unsupported Mach-O compact unwind header')
    common = _record(data, common_at, f'<{common_count}I')
    personalities = _record(data, personality_at, f'<{personality_count}I')
    indices = [_record(data, index_at + i * 12, '<III') for i in range(index_count)]
    entries, lsdas = [], {}
    for index, (first, page, lsda_first) in enumerate(indices[:-1]):
        next_first, _, lsda_last = indices[index + 1]
        if next_first <= first or not page or lsda_last < lsda_first or (lsda_last - lsda_first) % 8:
            raise ValueError('Invalid compact unwind page index')
        for at in range(lsda_first, lsda_last, 8):
            function, lsda = _record(data, at, '<II')
            if function in lsdas or not first <= function < next_first or not lsda:
                raise ValueError('Invalid compact unwind LSDA index')
            lsdas[function] = lsda
        kind, entry_offset, entry_count = _record(data, page, '<IHH')
        if kind == 2:
            page_entries = [_record(data, page + entry_offset + j * 8, '<II') for j in range(entry_count)]
        elif kind == 3:
            encoding_offset, encoding_count = _record(data, page + 8, '<HH')
            encodings = common + _record(data, page + encoding_offset, f'<{encoding_count}I')
            page_entries = []
            for j in range(entry_count):
                value, = _record(data, page + entry_offset + j * 4, '<I')
                encoding_index = value >> 24
                if encoding_index >= len(encodings):
                    raise ValueError('Compact unwind encoding index outside page')
                page_entries.append((first + (value & 0xFFFFFF), encodings[encoding_index]))
        else:
            raise ValueError(f'Unsupported compact unwind page kind {kind}')
        if not page_entries or page_entries[0][0] != first:
            raise ValueError('Compact unwind page does not start at its index')
        previous = first - 1
        for address, encoding in page_entries:
            if not previous <= address < next_first:
                raise ValueError('Unordered compact unwind function range')
            previous = address
            # Native compact-unwind lookup uses the last record at a shared
            # start address (upper_bound), including linker-coalesced aliases.
            if entries and entries[-1][0] == address:
                entries[-1] = (address, encoding)
            else:
                entries.append((address, encoding))
    if indices[-1][1] != 0:
        raise ValueError('Compact unwind index has no terminal entry')
    ends = [address for address, _ in entries[1:]] + [indices[-1][0]]
    return [(address, end, encoding, lsdas.get(address, 0))
            for (address, encoding), end in zip(entries, ends)], personalities


def _cie(personality, slot):
    label = f'.Lguest_cie_{personality}'
    out = ['.balign 8', label + ':', f'.long {label}_end-{label}_body', label + '_body:', '.long 0', '.byte 1']
    if personality:
        out += ['.asciz "zPLR"', '.uleb128 1', '.sleb128 -8', '.uleb128 30',
                '.uleb128 7', '.byte 0x9b', f'.long guest_text+{slot}-.',
                '.byte 0x1b', '.byte 0x1b']
    else:
        out += ['.asciz "zR"', '.uleb128 1', '.sleb128 -8', '.uleb128 30', '.uleb128 1', '.byte 0x1b']
    out += ['.byte 0x0c', '.uleb128 31', '.uleb128 0', '.balign 8,0', label + '_end:']
    return out


def _saved_register(register, distance):
    # DW_CFA_offset[_extended], in units of the CIE's -8 data alignment.
    if distance % 8:
        raise ValueError('Unaligned ARM64 saved register')
    return ([f'.byte {0x80 + register}'] if register < 64 else ['.byte 0x05', f'.uleb128 {register}']) + [f'.uleb128 {distance // 8}']


def _fde(index, first, end, encoding, lsda):
    personality = (encoding >> 28) & 3
    label = f'.Lguest_fde_{index}'
    out = ['.balign 8', label + ':', f'.long {label}_end-{label}_body', label + '_body:',
           f'.long .-.Lguest_cie_{personality}', f'.long guest_text+{first}-.', f'.long {end-first}']
    if personality:
        out += ['.uleb128 4', f'.long guest_text+{lsda}-.' if lsda else '.long 0']
    else:
        out += ['.uleb128 0']
    if encoding & MODE_MASK == FRAME:
        # CFA=FP+16; X19 (if saved) is FP-8, X20 is FP-16, etc.
        out += ['.byte 0x0c', '.uleb128 29', '.uleb128 16']
        out += _saved_register(29, 16) + _saved_register(30, 8)
        distance = 24
    else:
        stack_size = ((encoding & 0x00FFF000) >> 12) * 16
        out += ['.byte 0x0c', '.uleb128 31', f'.uleb128 {stack_size}', '.byte 0x08', '.uleb128 30']
        distance = 0
    for flag, low, high in REGISTER_PAIRS:
        if encoding & flag:
            out += _saved_register(low, distance) + _saved_register(high, distance + 8)
            distance += 16
    out += ['.balign 8,0', label + '_end:']
    return out


def prepare_guest_unwind(image, work):
    section = image.get_section('__unwind_info')
    if section is None:
        raise ValueError('Guest image has no compact unwind information')
    if int(image.header.cpu_type) != 0x0100000C:
        raise ValueError('Compact unwind translation currently requires ARM64')
    data = bytes(section.content)
    entries, personalities = _decode(data)
    text = image.get_segment('__TEXT')
    base = image.imagebase
    executable_ranges = [(s.virtual_address-base,s.virtual_address-base+s.virtual_size)
                         for s in image.segments if s.init_protection & 4]
    readable_ranges = [(s.virtual_address-base,s.virtual_address-base+s.virtual_size)
                       for s in image.segments if s.init_protection & 1]
    for slot in personalities:
        if not any(segment.max_protection & 2 and segment.virtual_address - base <= slot
                   and slot + 8 <= segment.virtual_address - base + segment.virtual_size
                   for segment in image.segments):
            raise ValueError('Guest personality slot is outside writable image data')
    assembly = ['.section .eh_frame,"a",%progbits']
    needed = {0}
    supported, dwarf, skipped = [], [], 0
    for first, end, encoding, lsda in entries:
        mode = encoding & MODE_MASK
        personality = (encoding >> 28) & 3
        if not any(lo <= first < end <= hi for lo,hi in executable_ranges) or personality > len(personalities):
            raise ValueError('Invalid guest unwind function/personality range')
        if mode == 0x03000000:
            dwarf.append((first, end, encoding, lsda))
            continue
        if bool(encoding & HAS_LSDA) != bool(lsda) or lsda and not any(lo <= lsda < hi for lo,hi in readable_ranges):
            raise ValueError('Guest compact unwind LSDA flag disagrees with its index')
        if mode == 0:
            if lsda or personality:
                raise ValueError('Cannot discard an unwind entry with exception handlers')
            skipped += 1
            continue
        if mode not in (FRAME, FRAMELESS):
            raise ValueError(f'Unsupported ARM64 unwind mode {mode:#x} at {base+first:#x}')
        allowed = 0xF0000000 | MODE_MASK | 0xF1F | (0x00FFF000 if mode == FRAMELESS else 0)
        if encoding & ~allowed or lsda and not personality:
            raise ValueError(f'Unsupported ARM64 unwind flags {encoding:#x}')
        needed.add(personality)
        supported.append((first, end, encoding, lsda))
    for personality in sorted(needed):
        assembly += _cie(personality, personalities[personality - 1] if personality else 0)
    for index, entry in enumerate(supported):
        assembly += _fde(index, *entry)
    dwarf_metadata = dict(cies=0, fdes=0)
    if dwarf:
        from .dwarf import translate_dwarf_unwind
        translated, dwarf_metadata = translate_dwarf_unwind(image, dwarf)
        assembly += translated
    metadata = {
        'format': 'Mach-O ARM64 compact unwind -> ELF eh_frame',
        'input_entries': len(entries), 'translated_entries': len(supported),
        'cant_unwind_entries': skipped, 'lsda_entries': sum(bool(row[3]) for row in supported),
        'modes': dict(Counter(hex(row[2] & MODE_MASK) for row in entries)),
        'personality_slots': list(personalities),
        'dwarf': dwarf_metadata,
    }
    (work / 'guest-unwind.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
    return assembly
