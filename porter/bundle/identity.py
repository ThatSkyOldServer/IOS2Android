import hashlib
import json
import plistlib
import re
import struct
import zipfile
import zlib
from pathlib import PurePosixPath
from xml.sax.saxutils import escape

from .. import BundleIdentity


def inspect_bundle(path):
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        if len(names) != len(set(names)):
            raise ValueError("Duplicate archive entries are unsupported")
        candidates = [
            name
            for name in names
            if name.startswith("Payload/")
            and name.count("/") == 2
            and name.endswith(".app/Info.plist")
        ]
        if len(candidates) != 1:
            raise ValueError("Input must contain exactly one application bundle")
        prefix = candidates[0][: -len("Info.plist")]
        for name in names:
            parts = PurePosixPath(name).parts
            if (
                "\\" in name
                or name.startswith("/")
                or ".." in parts
                or any(":" in p for p in parts)
            ):
                raise ValueError(f"Unsafe archive path: {name}")
        info = plistlib.loads(archive.read(candidates[0]))
        executable = info["CFBundleExecutable"]
        if not isinstance(executable, str) or PurePosixPath(executable).name != executable:
            raise ValueError("Invalid executable name")
        raw = archive.read(prefix + executable)
        if len(raw) < 32 or struct.unpack_from("<II", raw) != (0xFEEDFACF, 0x0100000C):
            raise ValueError("Only thin ARM64 Mach-O inputs are supported")
        commands, command_bytes = struct.unpack_from("<II", raw, 16)
        if command_bytes > len(raw) - 32:
            raise ValueError("Truncated Mach-O commands")
        cursor = 32
        for _ in range(commands):
            if cursor + 8 > 32 + command_bytes:
                raise ValueError("Invalid Mach-O command count")
            command, size = struct.unpack_from("<II", raw, cursor)
            if size < 8 or cursor + size > 32 + command_bytes:
                raise ValueError("Invalid Mach-O command size")
            if command in (0x21, 0x2C):
                if size < 20 or struct.unpack_from("<I", raw, cursor + 16)[0]:
                    raise ValueError("Encrypted executables are unsupported")
            if command == 0x80000034:
                raise ValueError("Chained fixups are not supported by this image ABI")
            cursor += size
        identity = BundleIdentity(
            prefix,
            prefix.rstrip("/").rsplit("/", 1)[1],
            executable,
            info["CFBundleIdentifier"],
            str(info.get("CFBundleShortVersionString", "1.0")),
            str(info["CFBundleVersion"]),
            info.get("CFBundleDisplayName") or info.get("CFBundleName") or executable,
            hashlib.sha256(raw).hexdigest(),
        )
        return identity, info


def normalize_png(raw):
    offset, header, compressed, optimized = 8, None, [], False
    while offset + 12 <= len(raw):
        length = struct.unpack_from(">I", raw, offset)[0]
        kind, data = raw[offset + 4 : offset + 8], raw[offset + 8 : offset + 8 + length]
        if kind == b"CgBI":
            optimized = True
        elif kind == b"IHDR":
            header = data
        elif kind == b"IDAT":
            compressed.append(data)
        offset += length + 12
    if not header:
        raise ValueError("Missing original icon PNG header")
    width, height, depth, colour, method, filtering, interlace = struct.unpack(">IIBBBBB", header)
    if not optimized:
        return raw, (width, height)
    channels = {2: 3, 6: 4}.get(colour)
    if depth != 8 or not channels or method or filtering or interlace:
        raise ValueError("Unsupported original CgBI icon format")
    pixels = bytearray(zlib.decompress(b"".join(compressed), -15))
    stride = width * channels + 1
    if len(pixels) != stride * height:
        raise ValueError("Incomplete original icon pixels")
    for y in range(height):
        row, mode = y * stride, pixels[y * stride]
        if mode > 4:
            raise ValueError("Invalid original icon PNG filter")
        for x in range(1, stride):
            a = pixels[row + x - channels] if x > channels else 0
            b = pixels[row + x - stride] if y else 0
            c = pixels[row + x - stride - channels] if y and x > channels else 0
            p = a + b - c
            paeth = min(
                ((abs(p - a), a), (abs(p - b), b), (abs(p - c), c)), key=lambda pair: pair[0]
            )[1]
            prediction = (0, a, b, (a + b) // 2, paeth)[mode]
            pixels[row + x] = (pixels[row + x] + prediction) & 255
        pixels[row] = 0
    for y in range(height):
        for x in range(width):
            p = y * stride + 1 + x * channels
            pixels[p], pixels[p + 2] = pixels[p + 2], pixels[p]
            if channels == 4:
                alpha = pixels[p + 3]
                for i in range(3):
                    pixels[p + i] = (
                        min(255, (pixels[p + i] * 255 + alpha // 2) // alpha) if alpha else 0
                    )

    def chunk(kind, data):
        return (
            struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
        )

    return raw[:8] + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(pixels)) + chunk(
        b"IEND", b""
    ), (width, height)


def prepare_app_identity(source, prefix, info, work, catalog=()):
    name = info.get("CFBundleDisplayName") or info["CFBundleName"]
    files = info.get("CFBundleIcons", {}).get("CFBundlePrimaryIcon", {}).get(
        "CFBundleIconFiles"
    ) or info.get("CFBundleIconFiles", [])
    candidates = []
    for filename in files:
        stem = filename.removesuffix(".png")
        pattern = re.compile(re.escape(stem) + r"(?:@[1-4]x)?(?:~(?:iphone|ipad))?\.png")
        for member in source.namelist():
            filename = member.removeprefix(prefix)
            if not member.startswith(prefix) or not pattern.fullmatch(filename):
                continue
            original = source.read(member)
            normalized, size = normalize_png(original)
            candidates.append((size[0] * size[1], filename, original, normalized, size))
    if not candidates:
        primary = info.get("CFBundleIcons", {}).get("CFBundlePrimaryIcon", {})
        icon_name = primary.get("CFBundleIconName") or "AppIcon"
        for filename, path in catalog:
            if "/" in filename or not filename.endswith(".png"):
                continue
            stem = re.sub(r"(?:@[1-4]x)?(?:~(?:iphone|ipad))?$", "", filename.removesuffix(".png"))
            if stem != icon_name:
                continue
            original = path.read_bytes()
            normalized, size = normalize_png(original)
            candidates.append((size[0] * size[1], "Assets.car/" + filename, original, normalized, size))
    if not candidates:
        raise ValueError("Original IPA has no convertible declared icon in standalone resources or Assets.car")
    _, filename, original, normalized, size = max(candidates, key=lambda item: item[0])
    resources = work / "identity-res"
    (resources / "values").mkdir(parents=True, exist_ok=True)
    (resources / "drawable-nodpi").mkdir(exist_ok=True)
    strings = (
        '<resources><string name="app_name">'
        + escape(name).replace("'", "\\'")
        + "</string></resources>\n"
    ).encode("utf8")
    (resources / "values/strings.xml").write_bytes(strings)
    for icon in (resources / "drawable-nodpi").glob("*.png"):
        icon.unlink()
    (resources / "drawable-nodpi/application_icon.png").write_bytes(normalized)
    (work / "app-identity.json").write_text(
        json.dumps({"name": name, "icon_asset": prefix + filename, "icon_size": size}, indent=2),
        encoding="utf8",
    )
    if any(c in name for c in '<>:"/\\|?*'):
        raise ValueError("Original app name cannot be used as an APK filename")
    return name, resources


def font_names(raw):
    if len(raw) < 12 or raw[:4] not in (b"\0\1\0\0", b"OTTO", b"true"):
        raise ValueError("Unsupported registered font container")
    count = struct.unpack_from(">H", raw, 4)[0]
    for index in range(count):
        tag, _, offset, size = struct.unpack_from(">4sIII", raw, 12 + index * 16)
        if tag != b"name":
            continue
        if offset + size > len(raw):
            raise ValueError("Font name table is truncated")
        table = raw[offset : offset + size]
        _, records, strings = struct.unpack_from(">HHH", table)
        result = {}
        for record in range(records):
            platform, encoding, language, kind, length, at = struct.unpack_from(
                ">6H", table, 6 + record * 12
            )
            if kind not in (1, 2, 4, 6, 16, 17) or platform not in (0, 1, 3):
                continue
            if strings + at + length > len(table):
                raise ValueError("Font name record is truncated")
            codec = "utf-16-be" if platform in (0, 3) else "mac_roman"
            value = table[strings + at : strings + at + length].decode(codec)
            if value and (kind not in result or language in (0, 0x409)):
                result[kind] = value
        return result
    raise ValueError("Registered font has no name table")


def font_plan(context, source):
    families, identities = {}, {}
    for file in context.info.get("UIAppFonts", []):
        metadata = font_names(source.read(context.identity.prefix + file))
        family = metadata.get(16, metadata.get(1))
        style = metadata.get(17, metadata.get(2, ""))
        if not family:
            raise ValueError("Registered font has no family name")
        slot = int("Bold" in style) + 2 * int("Italic" in style or "Oblique" in style)
        families.setdefault(family, [None] * 4)[slot] = file
        for name in (family, metadata.get(4), metadata.get(6)):
            if name:
                identities[name] = family
    rules = []
    for rule in context.host.get("font_aliases", []):
        if rule["mode"] not in ("exact", "contains") or len(rule["files"]) != 4:
            raise ValueError("Invalid external font alias")
        for file in rule["files"]:
            if file and context.identity.prefix + file not in source.namelist():
                raise ValueError("External font alias does not match a bundle resource")
        rules.append(rule)
    for name, family in sorted(identities.items()):
        files = families[family]
        regular = files[0] or next(file for file in files if file)
        rules.append(dict(mode="exact", name=name, files=[file or regular for file in files]))
    return rules
