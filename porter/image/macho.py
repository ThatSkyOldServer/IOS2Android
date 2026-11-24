from dataclasses import dataclass
from pathlib import Path
import re

from ..files import resolve_file
from .. import ImagePatchResult


class ImageOverlay:
    def __init__(self, raw):
        self.raw = raw
        self.data = bytearray(raw)
        self.changes = []

    def replace_bytes(self, offset, before, after, name, evidence=None):
        end = offset + len(before)
        if offset < 0 or end > len(self.raw) or not before or len(before) != len(after) or self.raw[offset:end] != before:
            raise ValueError('Unexpected patch bytes: ' + name)
        for item in self.changes:
            start = int(item['file_offset'],16)
            if offset < start + len(bytes.fromhex(item['before'])) and start < end:
                raise ValueError('Overlapping patches: ' + name)
        self.data[offset:end] = after
        self.changes.append(dict(name=name,file_offset=hex(offset),before=before.hex(),after=after.hex(),evidence=evidence))


def find_signature(data, signature):
    pattern = re.escape(signature) if isinstance(signature,bytes) else b''.join(
        b'.' if token=='?' else re.escape(bytes([int(token,16)])) for token in signature.split())
    if not pattern:
        raise ValueError('Empty patch signature')
    matches = list(re.finditer(b'(?=('+pattern+b'))',data,re.S))
    if len(matches)!=1:
        raise ValueError(f'Patch signature must be unique; found {len(matches)}')
    return matches[0].start()


def encode_arm64_branch(source, target, condition=None):
    delta = target-source
    bits = 26 if condition is None else 19
    if source%4 or target%4 or not -(1 << (bits+1))<=delta<(1 << (bits+1)) or (condition is not None and not 0<=condition<=15):
        raise ValueError('Invalid ARM64 branch range or condition')
    immediate=(delta//4)&((1 << bits)-1)
    return (0x14000000|immediate) if condition is None else (0x54000000|(immediate<<5)|condition)


def binding_offset(raw, image, address, expected):
    for segment in image.segments:
        if segment.virtual_address <= address and address+len(expected) <= segment.virtual_address+segment.file_size:
            offset=segment.file_offset+address-segment.virtual_address
            if expected and raw[offset:offset+len(expected)]==expected:
                return address-image.imagebase
            break
    raise ValueError(f'Native binding bytes differ at {address:#x}')


def mapped_segments(image, byte_count):
    base = image.imagebase
    segments = sorted((s for s in image.segments if s.name != '__PAGEZERO' and s.virtual_size),
                      key=lambda s: s.virtual_address)
    if not segments or segments[0].virtual_address != base or segments[0].file_offset != 0 or not segments[0].file_size:
        raise ValueError('The image header must start the first file-backed segment')
    previous_end = base
    for segment in segments:
        if segment.virtual_address < previous_end or segment.virtual_address % 4096:
            raise ValueError('Overlapping or unaligned mapped segment: ' + segment.name)
        if segment.file_size > segment.virtual_size or segment.file_offset + segment.file_size > byte_count:
            raise ValueError('Invalid file-backed segment bounds: ' + segment.name)
        if segment.init_protection & ~segment.max_protection or segment.max_protection & ~7:
            raise ValueError('Invalid segment protection: ' + segment.name)
        previous_end = segment.virtual_address + segment.virtual_size
        if previous_end - base > 0xffffffff:
            raise ValueError('Mapped image exceeds the 32-bit fixup-offset contract')
    return segments


@dataclass(frozen=True)
class PreparedImage:
    image: object
    executable: Path


def prepare_image(raw, work, patch=None):
    import lief
    original = work / 'guest.bin'
    original.write_bytes(raw)
    parsed = lief.MachO.parse(str(original))
    if parsed is None or len(parsed) != 1:
        raise ValueError('A single ARM64 Mach-O image is required')
    image = parsed.at(0)
    if int(image.header.cpu_type) != 0x0100000c or (image.has_encryption_info and image.encryption_info.crypt_id):
        raise ValueError('Unsupported or encrypted Mach-O image')
    if any(image.get_segment(name) is None for name in ('__TEXT', '__DATA', '__LINKEDIT')):
        raise ValueError('Image ABI requires __TEXT, __DATA and __LINKEDIT')
    mapped_segments(image, len(raw))
    executable = original
    if patch:
        result = patch(raw, image, work)
        if not isinstance(result, ImagePatchResult):
            raise ValueError('Adapter must return ImagePatchResult')
        executable = resolve_file(work, result.image)
        for name in result.files:
            resolve_file(work, name)
        if executable.stat().st_size != len(raw):
            raise ValueError('Image overlay must preserve the original file layout')
    return PreparedImage(image, executable)
