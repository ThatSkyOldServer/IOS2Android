import importlib.util
import json
import shutil
import subprocess
from pathlib import Path

from ..files import write_json, cache_matches, file_metadata
from ..build import ROOT


def run(context, *arguments, capture=False):
    result = subprocess.run(
        [str(arg) for arg in arguments],
        cwd=context.work,
        check=True,
        capture_output=capture,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    return result.stdout if capture else None


def validate(context):
    tools = {name: Path(value) for name, value in context.toolchain.items()}
    required = [
        tools["ndk"] / "bin/clang.exe",
        tools["ndk"] / "bin/clang++.exe",
        tools["sdk"] / "aapt.exe",
        tools["sdk"] / "zipalign.exe",
        tools["sdk"] / "lib/d8.jar",
        tools["sdk"] / "lib/apksigner.jar",
        tools["platform"],
        tools["sqlite"] / "sqlite3.c",
        tools["pvr"] / "PVRTDecompress.cpp",
        tools["pvr"] / "PVRTDecompress.h",
        tools["plist"] / "lib/libplist-2.0.so",
        tools["crypto"] / "libcrypto.so.3",
        tools["crypto"] / "libssl.so.3",
        tools["dispatch"] / "libdispatch.so",
        tools["dispatch"] / "libBlocksRuntime.so",
        tools["ndk"] / "sysroot/usr/lib/aarch64-linux-android/libc++_shared.so",
    ]
    required += [tools["blocks_headers"] / "Block.h", tools["plist"] / "include/plist/plist.h"]
    for name in ("lief", "capstone", "lzfse", "nibarchive"):
        spec = importlib.util.find_spec(name)
        if not spec or not spec.origin:
            raise ValueError("Missing Python dependency: " + name)
    for executable in ("java", "javac"):
        path = shutil.which(executable)
        if not path:
            raise ValueError("Required executable is missing: " + executable)
        required.append(Path(path))
    for path in required:
        if not path.is_file():
            raise ValueError(f"Missing toolchain input: {path}")


def dependencies(context):
    tools = {name: Path(value) for name, value in context.toolchain.items()}
    ndk, work = tools["ndk"], context.work
    inputs = file_metadata(
        [
            tools["sqlite"] / "sqlite3.c",
            tools["pvr"] / "PVRTDecompress.cpp",
            tools["pvr"] / "PVRTDecompress.h",
            ROOT / "runtime/support/texture_decode.cpp",
            ndk / "bin/clang.exe",
            ndk / "bin/clang++.exe",
            Path(__file__),
        ]
    )
    stamp = work / "dependencies.cache.json"
    outputs = [work / "libi2a_sqlite.so", work / "pvrtc.o", work / "texture_decode.o"]
    if not cache_matches(stamp, inputs, outputs):
        stamp.unlink(missing_ok=True)
        run(
            context,
            ndk / "bin/clang.exe",
            "--target=aarch64-linux-android28",
            "-shared",
            "-fPIC",
            "-O2",
            "-DSQLITE_THREADSAFE=1",
            "-Wl,-z,max-page-size=16384,-z,noexecstack,--build-id=sha1",
            tools["sqlite"] / "sqlite3.c",
            "-ldl",
            "-lm",
            "-o",
            outputs[0],
        )
        for source, output in [
            (tools["pvr"] / "PVRTDecompress.cpp", outputs[1]),
            (ROOT / "runtime/support/texture_decode.cpp", outputs[2]),
        ]:
            run(
                context,
                ndk / "bin/clang++.exe",
                "--target=aarch64-linux-android28",
                "-c",
                "-fPIC",
                "-fvisibility=hidden",
                "-O2",
                "-std=c++17",
                "-I" + str(tools["pvr"]),
                source,
                "-o",
                output,
            )
        write_json(stamp, inputs)
    return outputs


def build(context, extension_sources, standalone=False, adapter=None):
    tools = {name: Path(value) for name, value in context.toolchain.items()}
    work = context.work
    dependencies(context)
    compression=ROOT/'third_party/lzfse/src'
    compression_sources=[compression/name for name in ('lzfse_decode.c','lzfse_decode_base.c','lzfse_fse.c','lzvn_decode_base.c')]
    compression_objects=[work/(path.stem+'.o') for path in compression_sources]
    compression_inputs=file_metadata([*compression_sources,*compression.glob('*.h'),tools['ndk']/'bin/clang.exe'])
    compression_stamp=work/'compression.cache.json'
    if not cache_matches(compression_stamp,compression_inputs,compression_objects):
        for source,output in zip(compression_sources,compression_objects):
            run(context,tools['ndk']/'bin/clang.exe','--target=aarch64-linux-android28','-c','-fPIC','-fvisibility=hidden','-O2',source,'-o',output)
        write_json(compression_stamp,compression_inputs)
    output = work / ("libi2a-core.so" if standalone else "libi2a.so")
    composition = work / ("core-composition.c" if standalone else "composition.c")
    composition.write_text(
        '#include "' + (ROOT / "runtime/core/runtime.c").as_posix() + '"\n', encoding="utf-8"
    )
    sources = [] if standalone else extension_sources
    options = adapter.native_options(context, composition) if adapter and hasattr(adapter, "native_options") else []
    run(
        context,
        tools["ndk"] / "bin/clang.exe",
        "--target=aarch64-linux-android28",
        "-shared",
        "-fPIC",
        "-fvisibility=hidden",
        "-Wall",
        "-Werror",
        "-O2" if context.development else "-O3",
        "-DI2A_DEVELOPMENT=" + str(int(context.development)),
        "-DI2A_SHADER_DIAGNOSTICS=" + str(int(context.shader_diagnostics)),
        "-DI2A_HAS_EXTENSION=" + str(int(bool(sources))),
        "-I" + str(work),
        "-I" + str(ROOT / "runtime/include"),
        "-I" + str(ROOT / "runtime/core"),
        "-I" + str(tools["blocks_headers"]),
        "-I" + str(tools["plist"] / "include"),
        "-ffunction-sections",
        "-fdata-sections",
        *options,
        composition,
        *sources,
        work / "image.S",
        work / "pvrtc.o",
        work / "texture_decode.o",
        *compression_objects,
        "-lc++_shared",
        "-L" + str(tools["dispatch"]),
        "-lBlocksRuntime",
        "-L" + str(tools["plist"] / "lib"),
        "-lplist-2.0",
        "-Wl,-T,layout.ld,-z,max-page-size=16384,-z,noexecstack,--no-undefined,--build-id=sha1,--gc-sections",
        "-llog",
        "-ldl",
        "-lm",
        "-lz",
        "-landroid",
        "-laaudio",
        "-lmediandk",
        "-lEGL",
        "-lGLESv3",
        "-o",
        output,
    )
    if adapter and hasattr(adapter, "finish_native"):
        adapter.finish_native(context, output)
    return output


def generate_descriptors(work):
    schema = json.loads(
        (ROOT / "runtime/contracts/metal-descriptors.json").read_text(encoding="utf-8")
    )
    if schema["version"] != 1:
        raise ValueError("Unsupported descriptor schema")
    kinds = schema["kinds"]
    ids = [kind["id"] for kind in kinds]
    properties = schema["properties"]
    names = [item["name"] for item in properties]
    if len(ids) != len(set(ids)) or len(names) != len(set(names)) or len(kinds) > 32:
        raise ValueError("Invalid descriptor identities")
    lines = [
        "enum MetalDescriptorKind { " + ",".join(ids) + ",MD_COUNT };",
        "static const char *const metal_descriptor_names[]={"
        + ",".join(json.dumps(k["name"]) for k in kinds)
        + "};",
        "static const struct { const char *name; unsigned kinds,reference; } metal_properties[]={",
    ]
    for item in properties:
        mask = sum(1 << ids.index(kind) for kind in set(item["kinds"]))
        lines.append("{" + json.dumps(item["name"]) + f",{mask}U,{int(item['reference'])}" + "},")
    lines.append("};")
    defaults, ranges = [], []
    for kind in kinds:
        first = len(defaults)
        for name, value in kind["defaults"].items():
            if (
                name not in names
                or kind["id"] not in properties[names.index(name)]["kinds"]
                or type(value) is not int
                or not 0 <= value < 2**64
            ):
                raise ValueError("Invalid descriptor default")
            defaults.append((names.index(name), value))
        ranges.append((first, len(defaults) - first))
    lines += [
        "static const struct { unsigned property; Word value; } metal_defaults[]={",
        *[f"{{{index},{value}UL}}," for index, value in defaults],
        "};",
        "struct MetalDefaultRange { unsigned first,count; };",
        "static const struct MetalDefaultRange metal_default_ranges[]={",
        *[f"{{{first},{count}}}," for first, count in ranges],
        "};",
    ]
    (work / "descriptor_schema.h").write_text("\n".join(lines) + "\n", encoding="utf-8")
