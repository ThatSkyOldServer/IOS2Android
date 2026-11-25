"""Convert bundled PVRTC textures to ETC2 before packaging, preserving PVR3 metadata."""

from fnmatch import fnmatchcase
from pathlib import Path, PurePosixPath
import struct

from . import DirectoryOverlay
from .files import cache_matches, file_metadata, write_json


_PVR_HEADER = struct.Struct("<IIQ9I")


def texture_options(options, build):
    if not isinstance(options, dict):
        raise ValueError("Texture conversion configuration must be an object")
    if set(options) - {"pvrtc", "patterns", "builds"}:
        raise ValueError("Unknown texture conversion option")
    builds = options.get("builds", [build])
    if not isinstance(builds, list) or any(not isinstance(value, str) for value in builds):
        raise ValueError("Texture conversion builds must be a list of bundle versions")
    if build not in builds:
        return {}
    if options and (options.get("pvrtc") != "etc2" or not isinstance(options.get("patterns"), list)
                    or not options["patterns"] or any(not isinstance(pattern, str) or not pattern for pattern in options["patterns"])):
        raise ValueError("Texture conversion requires pvrtc=etc2 and explicit resource patterns")
    return options


def _pvr_levels(data):
    if len(data) < _PVR_HEADER.size:
        return None
    magic, _, pixel, color, channel, height, width, depth, surfaces, faces, mips, metadata = _PVR_HEADER.unpack_from(data)
    if magic != 0x03525650 or pixel not in (0, 1, 2, 3):
        return None
    if color not in (0, 1) or channel != 0 or (depth, surfaces, faces) != (1, 1, 1):
        return None
    if not width or not height or width & (width - 1) or height & (height - 1) or not 1 <= mips <= max(width, height).bit_length():
        raise ValueError("Invalid PVRTC dimensions or mip count")
    start = _PVR_HEADER.size + metadata
    levels = []
    offset = start
    bits = 2 if pixel < 2 else 4
    for mip in range(mips):
        w, h = max(1, width >> mip), max(1, height >> mip)
        size = max(w, 16 if bits == 2 else 8) * max(h, 8) * bits // 8
        if offset + size > len(data):
            raise ValueError("Truncated PVRTC mip data")
        levels.append((w, h, data[offset:offset + size]))
        offset += size
    if offset != len(data):
        raise ValueError("Unexpected data after PVRTC mip chain")
    return pixel, start, levels


def _etc2_level(encoded, width, height, pixel, decoder, encoder):
    stored_width = max(width, 16 if pixel < 2 else 8)
    stored_height = max(height, 8)
    pixels = decoder(encoded, stored_width, stored_height, pixel < 2)
    if len(pixels) != stored_width * stored_height * 4:
        raise ValueError("PVRTC decoder returned an invalid image size")
    padded_width, padded_height = (width + 3) & ~3, (height + 3) & ~3
    if (padded_width, padded_height) != (stored_width, stored_height):
        # Decode the PVRTC minimum footprint, then crop and extend edge pixels
        # for ETC2's 4x4 blocks, including 1x1 and 2x2 mip levels.
        rows = []
        for y in range(height):
            row = pixels[y * stored_width * 4:(y * stored_width + width) * 4]
            rows.append(row + row[-4:] * (padded_width - width))
        pixels = b"".join(rows + [rows[-1]] * (padded_height - height))
    # texture2ddecoder returns BGRA. etcpak 0.9.15 consumes RGBA;
    # its bundled README still describes the older BGRA interface.
    rgba = bytearray(pixels)
    rgba[0::4] = pixels[2::4]
    rgba[2::4] = pixels[0::4]
    if not pixel & 1:
        rgba[3::4] = b"\xff" * (padded_width * padded_height)
    result = encoder(bytes(rgba), padded_width, padded_height)
    expected = padded_width * padded_height * (8 if pixel & 1 else 4) // 8
    if len(result) != expected:
        raise ValueError("ETC2 encoder returned an invalid block count")
    return result


def prepare_pvr_textures(source, prefix, cache, patterns, overlays=None):
    """Return file overlays for explicitly selected PVRTC resources in an IPA.

    ETC2 payloads keep the original mip order, dimensions, color space,
    premultiplied-alpha flag and metadata. Consumers must support PVR3 ETC2.
    """
    cache = Path(cache).resolve()
    overlays = overlays or {}
    resources = {}
    directories = {name.rstrip("/") + "/": value for name, value in overlays.items()
                   if isinstance(value, DirectoryOverlay)}
    for member in source.infolist():
        if member.is_dir() or not member.filename.startswith(prefix):
            continue
        name = member.filename[len(prefix):]
        if any(name.startswith(root) for root in directories) or not any(fnmatchcase(name, pattern) for pattern in patterns):
            continue
        resources[name] = (member, [member.filename, member.CRC, member.file_size])
    for root, directory in directories.items():
        for path in sorted(directory.directory.rglob("*")):
            name = root + path.relative_to(directory.directory).as_posix()
            if path.is_file() and any(fnmatchcase(name, pattern) for pattern in patterns):
                resources[name] = (path, file_metadata([path]))
    for name, value in overlays.items():
        if not isinstance(value, DirectoryOverlay) and any(fnmatchcase(name, pattern) for pattern in patterns):
            # Generated byte overlays have no stable file metadata; encode them
            # again rather than adding a content-hash cache to the build.
            resources[name] = (value, None)
    inputs = dict(files=file_metadata([source.filename, __file__]))
    converted = {}
    outputs = {}
    unchanged = []
    codecs = None
    hits = rgba_size = encoded_size = 0
    for resource_index, (name, (resource, metadata)) in enumerate(resources.items()):
        if PurePosixPath(name).is_absolute() or ".." in PurePosixPath(name).parts:
            raise ValueError("Invalid texture resource path: " + name)
        target = (cache / str(resource_index) / "texture.pvr").resolve()
        if not target.is_relative_to(cache):
            raise ValueError("Texture cache path leaves its directory: " + name)
        if isinstance(resource, Path):
            original = resource.read_bytes()
        elif isinstance(resource, str):
            original = resource.encode("utf-8")
        elif isinstance(resource, (bytes, bytearray, memoryview)):
            original = bytes(resource)
        else:
            original = source.read(resource)
        try:
            parsed = _pvr_levels(original)
            if parsed is None:
                unchanged.append(name)
                continue
            pixel, start, levels = parsed
            size = sum(((w + 3) // 4) * ((h + 3) // 4) * (16 if pixel & 1 else 8) for w, h, _ in levels)
            original_rgba_size = sum(w * h * 4 for w, h, _ in levels)
            if size >= original_rgba_size:
                unchanged.append(name)
                continue
            if codecs is None:
                try:
                    import etcpak
                    import texture2ddecoder
                except ImportError as error:
                    raise ValueError("PVRTC conversion requires the Python textures dependencies") from error
                codecs = (etcpak, texture2ddecoder)
                inputs["codecs"] = dict(etcpak=etcpak.__version__, texture2ddecoder=texture2ddecoder.__version__)
            header = bytearray(original[:start])
            struct.pack_into("<Q", header, 8, 23 if pixel & 1 else 22)
            stamp = target.with_suffix(target.suffix + ".json")
            record = dict(inputs, resource=metadata)
            data = target.read_bytes() if metadata is not None and cache_matches(stamp, record, [target]) else b""
            if len(data) == start + size and data[:start] == header:
                hits += 1
            else:
                encoder = etcpak.compress_etc2_rgba if pixel & 1 else etcpak.compress_etc2_rgb
                data = bytes(header) + b"".join(
                    _etc2_level(encoded, w, h, pixel, texture2ddecoder.decode_pvrtc, encoder)
                    for w, h, encoded in levels
                )
                target.parent.mkdir(parents=True, exist_ok=True)
                temporary = target.with_suffix(target.suffix + ".tmp")
                try:
                    temporary.write_bytes(data)
                    temporary.replace(target)
                    write_json(stamp, record)
                finally:
                    temporary.unlink(missing_ok=True)
        except ValueError as error:
            raise ValueError(f"{name}: {error}") from error
        converted[name] = data
        outputs[name] = target.relative_to(cache).as_posix()
        rgba_size += original_rgba_size
        encoded_size += size
    if not resources:
        raise ValueError("Texture conversion patterns matched no resources")
    write_json(cache / "conversion.json", dict(converted=list(converted), outputs=outputs, unchanged=unchanged,
                                             cached=hits, rgba8_payload_bytes=rgba_size, etc2_payload_bytes=encoded_size))
    print(f"Texture conversion: {len(converted)} PVRTC -> ETC2 resources ({hits} cached), {len(unchanged)} unchanged, "
          f"RGBA8 {rgba_size} -> ETC2 {encoded_size} payload bytes", flush=True)
    return converted
