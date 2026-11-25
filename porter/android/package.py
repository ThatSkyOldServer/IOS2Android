import json
import copy
import os
import re
import shutil
import zipfile
from pathlib import Path
from xml.sax.saxutils import escape
from xml.etree import ElementTree as ET

from .. import DirectoryOverlay
from ..files import build_lock, require_tokens
from ..build import ROOT, outside
from .native import run


def prepare_libraries(context):
    selection = context.host.get("libraries", [])
    if isinstance(selection, str):
        manifest = outside(selection, "Android library manifest")
        selection = [manifest.parent / path for path in json.loads(manifest.read_text(encoding="utf-8"))["libraries"]]
    paths = [outside(path, "Android library") for path in selection]
    directory = context.work / "android-libraries"
    if directory.exists():
        if not directory.resolve().is_relative_to(context.work.resolve()):
            raise ValueError("Generated library directory escapes build workspace")
        shutil.rmtree(directory)
    libraries = []
    for index, path in enumerate(paths):
        if path.suffix.lower() not in (".aar", ".jar") or not path.is_file():
            raise ValueError("Android libraries must be existing AAR or JAR files")
        if path.suffix.lower() == ".jar":
            libraries.append(dict(jars=[path], directory=None, manifest=None))
            continue
        target = directory / str(index)
        target.mkdir(parents=True)
        with zipfile.ZipFile(path) as archive:
            for item in archive.infolist():
                destination = target / item.filename
                if not destination.resolve().is_relative_to(target.resolve()):
                    raise ValueError("Android archive member escapes its directory")
                if not item.is_dir():
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    destination.write_bytes(archive.read(item))
        manifest = target / "AndroidManifest.xml"
        libraries.append(dict(jars=list(target.rglob("*.jar")), directory=target, manifest=manifest if manifest.is_file() else None))
    return libraries


def merge_library_manifests(template, libraries, application_id):
    android = "{http://schemas.android.com/apk/res/android}"
    tools = "{http://schemas.android.com/tools}"
    ET.register_namespace("android", android[1:-1])
    root = ET.fromstring(template)
    application = root.find("application")
    packages = []
    for library in libraries:
        if not library["manifest"]:
            continue
        text = library["manifest"].read_text(encoding="utf-8").replace("${applicationId}", application_id)
        if "${" in text:
            raise ValueError("Android library manifest has unresolved placeholders")
        manifest = ET.fromstring(text)
        package = manifest.get("package", "")
        if package:
            packages.append(package)
        for child in manifest:
            if child.tag == "uses-sdk":
                continue
            if child.tag == "application":
                nodes, parent = list(child), application
            else:
                nodes, parent = [child], root
            for original in nodes:
                node = copy.deepcopy(original)
                name = node.get(android + "name")
                if name and name.startswith("."):
                    name = package + name
                    node.set(android + "name", name)
                existing = next((entry for entry in parent if entry.tag == node.tag and name is not None and entry.get(android + "name") == name), None)
                remove = node.get(tools + "node") == "remove"
                for entry in node.iter():
                    for attribute in list(entry.attrib):
                        if attribute.startswith(tools):
                            del entry.attrib[attribute]
                if remove:
                    if existing is not None:
                        parent.remove(existing)
                elif existing is None:
                    parent.append(node)
                else:
                    for entry in node:
                        if not any(old.tag == entry.tag and old.attrib == entry.attrib for old in existing):
                            existing.append(entry)
    return ET.tostring(root, encoding="unicode"), packages


def compile_library_resources(context, manifest, resources, libraries, packages, sources):
    tools = {name: Path(value) for name, value in context.toolchain.items()}
    compiler = tools["sdk"] / "aapt2.exe"
    if not compiler.is_file():
        raise ValueError("Android AAR libraries require aapt2")
    archives = []
    directories = [library["directory"] / "res" for library in libraries if library["directory"] and (library["directory"] / "res").is_dir()]
    directories.append(resources)
    for index, directory in enumerate(directories):
        target = context.work / f"resources-{index}.zip"
        run(context, compiler, "compile", "--dir", directory, "-o", target)
        archives.append(target)
    arguments = [compiler, "link", "--manifest", manifest, "-I", tools["platform"], "--java", sources,
                 "--auto-add-overlay", "-o", context.work / "assets.apk"]
    if packages:
        arguments += ["--extra-packages", ":".join(dict.fromkeys(packages))]
    for archive in archives:
        arguments += ["-R", archive]
    run(context, *arguments)


def build_host(context, revision, fonts, resources, launch_screen, adapter=None):
    work = context.work
    (work / "r8-mapping.txt").unlink(missing_ok=True)
    shutil.copytree(ROOT / "host/android/res", resources, dirs_exist_ok=True)
    sources, classes, dex = [work / name for name in ("java", "classes", "dex")]
    for folder in (sources, classes, dex):
        if folder.exists():
            if not folder.resolve().is_relative_to(work.resolve()):
                raise ValueError("Generated directory escapes build workspace")
            shutil.rmtree(folder)
        folder.mkdir()
    for path in (ROOT / "host/android/java").glob("*.java"):
        code = path.read_text(encoding="utf-8")
        if not context.development:
            code = re.sub(r"\s*// DEVELOPMENT_BEGIN.*?// DEVELOPMENT_END", "", code, flags=re.S)
        (sources / path.name).write_text(code, encoding="utf-8")
    fields = dict(
        PACKAGE_REVISION=revision,
        BUNDLE_NAME=context.identity.directory,
        APP_NAME=context.identity.display_name,
        GUEST_BINARY_SHA256=context.identity.binary_sha256,
        RUNTIME_PREFERENCES=context.storage["preferences"],
        UUID_NAMESPACE=context.storage["uuid_namespace"],
        DEFAULTS_DOMAIN=context.storage["defaults_domain"],
        KEYCHAIN_ALIAS=context.storage["keychain_alias"],
        KEYCHAIN_AAD=context.storage["keychain_aad"],
        HELPSHIFT_CONFIG=json.dumps(context.host.get("helpshift", {}), separators=(",", ":")),
        LAUNCH_SCREEN=json.dumps(launch_screen, separators=(",", ":")),
    )
    libraries = prepare_libraries(context)
    sdk_present = False
    for library in libraries:
        for path in library["jars"]:
            with zipfile.ZipFile(path) as archive:
                sdk_present |= "com/helpshift/Helpshift.class" in archive.namelist()
    backend = context.host.get("helpshift", {}).get("backend", "auto")
    if backend not in ("auto", "sdk", "web", "disabled"):
        raise ValueError("Unsupported Helpshift backend")
    if backend == "sdk" and (not sdk_present or not context.host.get("helpshift", {}).get("platform_id")):
        raise ValueError("Helpshift SDK backend requires Android platform_id and SDK libraries")
    profile = ["package local.ios.host;", "final class BuildProfile {"]
    profile += [
        "static final String " + name + "=" + json.dumps(value) + ";"
        for name, value in fields.items()
    ]
    profile += ["static final boolean DEVELOPMENT=" + str(context.development).lower() + ";"]
    profile += ["static final boolean HELPSHIFT_SDK=" + str(sdk_present).lower() + ";"]
    profile += [
        "private static final String[] FONT_NAMES={"
        + ",".join(json.dumps(r["name"]) for r in fonts)
        + "};",
        "private static final String[] FONT_REQUIRES={"
        + ",".join(json.dumps(r.get("requires")) for r in fonts)
        + "};",
        "private static final boolean[] FONT_CONTAINS={"
        + ",".join(str(r["mode"] == "contains").lower() for r in fonts)
        + "};",
        "private static final String[][] FONT_FILES={"
        + ",".join("{" + ",".join(json.dumps(f) for f in r["files"]) + "}" for r in fonts)
        + "};",
        "static String fontFile(String name,boolean bold,boolean italic) {",
        "for(int i=0;i<FONT_NAMES.length;i++) { if(FONT_REQUIRES[i]!=null && !name.contains(FONT_REQUIRES[i])) continue; if(FONT_CONTAINS[i] ? name.contains(FONT_NAMES[i]) : name.equals(FONT_NAMES[i])) return FONT_FILES[i][(bold?1:0)+(italic?2:0)]; }",
        "return null;",
        "}",
        "}",
    ]
    (sources / "BuildProfile.java").write_text("\n".join(profile) + "\n", encoding="utf-8")
    template = (ROOT / "host/android/templates/AndroidManifest.xml").read_text(encoding="utf-8")
    if context.info.get("NSPhotoLibraryUsageDescription"):
        permissions = '<uses-permission android:name="android.permission.READ_EXTERNAL_STORAGE" android:maxSdkVersion="32"/><uses-permission android:name="android.permission.READ_MEDIA_IMAGES"/><uses-permission android:name="android.permission.READ_MEDIA_VISUAL_USER_SELECTED"/>'
        template = template.replace("<queries>", permissions + "<queries>")
    if any(context.info.get(key) for key in ("NSLocationWhenInUseUsageDescription", "NSLocationAlwaysUsageDescription", "NSLocationAlwaysAndWhenInUseUsageDescription")):
        permissions = '<uses-permission android:name="android.permission.ACCESS_COARSE_LOCATION"/><uses-permission android:name="android.permission.ACCESS_FINE_LOCATION"/>'
        template = template.replace("<queries>", permissions + "<queries>")
    orientations = context.info.get("UISupportedInterfaceOrientations", [])
    default_orientation = (
        "landscape"
        if orientations and all("Landscape" in x for x in orientations)
        else "unspecified"
    )
    orientation = context.host.get("orientation", default_orientation)
    if orientation not in ("unspecified", "landscape", "portrait", "sensor", "fullSensor", "user"):
        raise ValueError("Unsupported host orientation")
    values = dict(
        APPLICATION_ID=context.application_id,
        VERSION_CODE=str(context.version_code),
        VERSION_NAME=f"{context.identity.version} ({context.identity.build})",
        ORIENTATION=orientation,
        DEBUGGABLE=str(context.development).lower(),
    )
    for name, value in values.items():
        template = template.replace("${" + name + "}", escape(value, {'"': "&quot;"}))
    if context.development:
        template = template.replace(
            "</application>", '<profileable android:shell="true"/></application>'
        )
    manifest = work / "AndroidManifest.xml"
    template, packages = merge_library_manifests(template, libraries, context.application_id)
    manifest.write_text(template, encoding="utf-8")
    host_options = adapter.host_options(context, sources, manifest) if adapter and hasattr(adapter, "host_options") else {}
    tools = {name: Path(value) for name, value in context.toolchain.items()}
    if libraries:
        compile_library_resources(context, manifest, resources, libraries, packages, sources)
    jars = [path for library in libraries for path in library["jars"]]
    run(
        context,
        "javac",
        "--release",
        "8",
        "-encoding",
        "UTF-8",
        "-classpath",
        os.pathsep.join(str(path) for path in [tools["platform"], *jars]),
        "-d",
        classes,
        *sorted(sources.rglob("*.java")),
    )
    host_classes = work / "host-classes.jar"
    with zipfile.ZipFile(host_classes, "w") as archive:
        for path in sorted(classes.rglob("*.class")):
            archive.write(path, path.relative_to(classes).as_posix())
    run(
        context,
        "java",
        "-cp",
        tools["sdk"] / "lib/d8.jar",
        "com.android.tools.r8." + host_options.get("compiler", "D8"),
        "--min-api",
        28,
        "--lib",
        tools["platform"],
        "--output",
        dex,
        *host_options.get("arguments", []),
        host_classes,
        *jars,
    )
    binary = dex / "classes.dex"
    data = b"".join(path.read_bytes() for path in sorted(dex.glob("classes*.dex")))
    for token in (b"businessDiagnostics", b"configureShaderDiagnostics"):
        if (token in data) != context.development:
            raise ValueError("DEX diagnostics differ from the build variant")
    if adapter and hasattr(adapter, "finish_host"):
        adapter.finish_host(context, dex)
    return manifest, binary


def _write_directory_overlay(package, prefix, overlay, files):
    for path in sorted(overlay.directory.rglob("*")):
        if not path.is_file():
            continue
        target = prefix + path.relative_to(overlay.directory).as_posix()
        member = (
            zipfile.ZipInfo(target, overlay.date_time)
            if overlay.date_time is not None
            else zipfile.ZipInfo.from_file(path, target)
        )
        if target in files:
            package.writestr(member, files[target])
            continue
        member.file_size = path.stat().st_size
        with path.open("rb") as incoming, package.open(member, "w") as outgoing:
            shutil.copyfileobj(incoming, outgoing, 1024 * 1024)


def build_apk(context, source, manifest, resources, catalog, overlays, native, dex, adapter):
    work, identity = context.work, context.identity
    tools = {name: Path(value) for name, value in context.toolchain.items()}
    assets = work / "assets.apk"
    directories = {
        name.rstrip("/") + "/": overlay
        for name, overlay in overlays.items()
        if isinstance(overlay, DirectoryOverlay)
    }
    files = {"assets/" + identity.directory + "/" + name: value for name, value in overlays.items()
             if not isinstance(value, DirectoryOverlay)}
    if not context.host.get("libraries"):
        run(context, tools["sdk"] / "aapt.exe", "package", "-f", "-M", manifest, "-I", tools["platform"], "-S", resources, "-F", assets)
    with zipfile.ZipFile(assets, "a") as package:
        package.write(work / "shaders.bin", "assets/i2a/shaders.bin", compress_type=zipfile.ZIP_STORED)
        package.write(work / "diagnostics.json", "assets/i2a/diagnostics.json", compress_type=zipfile.ZIP_DEFLATED)
        for directory in sorted((work / "android-libraries").glob("*")):
            for path in sorted((directory / "assets").rglob("*")):
                if path.is_file():
                    package.write(path, "assets/" + path.relative_to(directory / "assets").as_posix(), compress_type=zipfile.ZIP_DEFLATED)
        for member in source.infolist():
            name = member.filename.removeprefix(identity.prefix)
            if (
                not member.filename.startswith(identity.prefix)
                or member.is_dir()
                or name == identity.executable
                or name.startswith(("_CodeSignature/", "SC_Info/"))
                or any(name.startswith(root) for root in directories)
            ):
                continue
            target = "assets/" + identity.directory + "/" + name
            if name in overlays:
                package.writestr(target, overlays[name])
            else:
                with source.open(member) as incoming, package.open(target, "w") as outgoing:
                    shutil.copyfileobj(incoming, outgoing, 1024 * 1024)
        for root, overlay in directories.items():
            _write_directory_overlay(package, "assets/" + identity.directory + "/" + root, overlay, files)
        members = set(source.namelist())
        for name, path in catalog:
            if identity.prefix + name not in members and not any(name.startswith(root) for root in directories):
                package.write(
                    path,
                    "assets/" + identity.directory + "/" + name,
                    compress_type=zipfile.ZIP_DEFLATED,
                )
    unsigned = work / "unsigned.apk"
    shutil.copyfile(assets, unsigned)
    libraries = {
        "libi2a.so": native,
        "libi2a_sqlite.so": work / "libi2a_sqlite.so",
        "libplist-2.0.so": tools["plist"] / "lib/libplist-2.0.so",
        "libi2a_crypto.so": tools["crypto"] / "libcrypto.so.3",
        "libi2a_ssl.so": tools["crypto"] / "libssl.so.3",
        "libdispatch.so": tools["dispatch"] / "libdispatch.so",
        "libBlocksRuntime.so": tools["dispatch"] / "libBlocksRuntime.so",
        "libc++_shared.so": tools["ndk"] / "sysroot/usr/lib/aarch64-linux-android/libc++_shared.so",
    }
    with zipfile.ZipFile(unsigned, "a") as package:
        package.write(
            work / "guest.bin",
            "assets/" + identity.directory + "/" + identity.executable,
            compress_type=zipfile.ZIP_DEFLATED,
        )
        for name, path in libraries.items():
            package.write(path, "lib/arm64-v8a/" + name, compress_type=zipfile.ZIP_DEFLATED)
        for path in sorted(dex.parent.glob("classes*.dex")):
            package.write(path, path.name)
        for directory in sorted((work / "android-libraries").glob("*")):
            for path in sorted((directory / "jni/arm64-v8a").glob("*.so")):
                package.write(path, "lib/arm64-v8a/" + path.name, compress_type=zipfile.ZIP_DEFLATED)
        package.write(
            tools["pvr"] / "LICENSE.md",
            "assets/licenses/Imagination-PVRTC.txt",
            compress_type=zipfile.ZIP_DEFLATED,
        )
        package.write(ROOT/'third_party/lzfse/LICENSE','assets/licenses/Apple-LZFSE.txt',compress_type=zipfile.ZIP_DEFLATED)
    package_options = adapter.prepare_package(context, unsigned) if adapter and hasattr(adapter, "prepare_package") else None
    package_options = package_options or {}
    alignment = package_options.get("alignment", 16384)
    native_alignment = package_options.get("native_page_alignment")
    align_options = ["-P", native_alignment] if native_alignment is not None else []
    run(context, tools["sdk"] / "zipalign.exe", *align_options, "-f", alignment, unsigned, "aligned.apk")
    signer = tools["sdk"] / "lib/apksigner.jar"
    sign = context.signing
    run(
        context,
        "java",
        "-jar",
        signer,
        "sign",
        "--ks",
        sign["keystore"],
        "--ks-pass",
        "pass:" + sign["password"],
        "--v4-signing-enabled",
        "false",
        "--alignment-preserved",
        "true",
        "--out",
        "candidate.apk",
        "aligned.apk",
    )
    verified = run(
        context,
        "java",
        "-jar",
        signer,
        "verify",
        "--verbose",
        "--print-certs",
        "candidate.apk",
        capture=True,
    )
    (work / "signature.txt").write_text(verified, encoding="utf-8")
    candidate = work / "candidate.apk"
    native_bytes, dex_bytes = native.read_bytes(), b"".join(path.read_bytes() for path in sorted(dex.parent.glob("classes*.dex")))
    require_tokens(
        native_bytes,
        **(
            {"present": ["[DIAG-cpu] Sampling started"]}
            if context.development
            else {"absent": ["[DIAG-cpu] Sampling started"]}
        ),
    )
    if adapter:
        for target, rule in adapter.variant_tokens(context).items():
            require_tokens({"native": native_bytes, "dex": dex_bytes}[target], **rule)
    context.output.mkdir(parents=True, exist_ok=True)
    published = context.output / "application.apk"
    with build_lock(context.output):
        candidate.replace(published)
    return published
