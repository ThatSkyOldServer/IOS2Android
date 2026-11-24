import importlib.util
import json
import re
import sys
import uuid
import zipfile
from dataclasses import asdict, dataclass, replace
from pathlib import Path

from .files import write_json, build_lock, file_metadata, resolve_file
from .bundle.identity import inspect_bundle
from . import ADAPTER_INTERFACE_VERSION, BundleIdentity, DirectoryOverlay, versions

ROOT = Path(__file__).resolve().parents[1]


def outside(path, label):
    path = Path(path).resolve()
    if path == ROOT or path.is_relative_to(ROOT):
        raise ValueError(f"{label} must be outside the source repository: {path}")
    return path


@dataclass(frozen=True)
class BuildContext:
    ipa: Path
    workspace: Path
    toolchain: dict
    signing: dict
    identity: BundleIdentity
    info: dict
    variant: str
    application_id: str
    version_code: int
    storage: dict
    host: dict
    adapter_root: Path | None
    adapter: dict
    policies: dict
    core_only: bool = False
    shader_diagnostics: bool = False

    @property
    def work(self):
        return (
            self.workspace
            / "build"
            / self.application_id
            / (self.variant + ("-core" if self.core_only else "-shader" if self.shader_diagnostics else ""))
        )

    @property
    def output(self):
        return self.workspace / "out" / self.application_id / self.identity.build / (self.variant + ("-shader" if self.shader_diagnostics else ""))

    @property
    def development(self):
        return self.variant == "dev"

    def adapter_path(self, relative):
        if self.adapter_root is None:
            raise ValueError("This operation requires an explicitly selected adapter")
        return resolve_file(self.adapter_root, relative)

    def load_adapter(self):
        entry = self.adapter.get("builder")
        if not entry:
            return None
        path = self.adapter_path(entry)
        name = "i2a_external_" + self.application_id.replace(".", "_") + "_" + self.variant
        spec = importlib.util.spec_from_file_location(
            name, path, submodule_search_locations=[str(self.adapter_root)]
        )
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        return module

    def summary(self):
        return dict(
            versions=versions(),
            variant=self.variant,
            shader_diagnostics=self.shader_diagnostics,
            input=str(self.ipa),
            identity=asdict(self.identity),
            application_id=self.application_id,
            version_code=self.version_code,
            adapter=str(self.adapter_root) if self.adapter_root else None,
        )


def create_context(
    ipa,
    workspace,
    config_path,
    variant,
    adapter_path=None,
    disabled=(),
    enabled=(),
    policies_enabled=True,
):
    if variant not in ("dev", "release"):
        raise ValueError("Unknown build variant")
    ipa = outside(ipa, "Input")
    workspace = outside(workspace, "Workspace")
    config_path = outside(config_path, "Local configuration")
    config = json.loads(config_path.read_text(encoding="utf-8"))
    identity, info = inspect_bundle(ipa)
    if not re.fullmatch(r"[0-9](?:[A-Za-z0-9._-]*[A-Za-z0-9_-])?", identity.build):
        raise ValueError("Invalid bundle build directory name")
    tools = {key: str(outside(value, "Toolchain")) for key, value in config["toolchain"].items()}
    for dependency in config.get("python_paths", []):
        sys.path.insert(0, str(outside(dependency, "Python dependencies")))
    adapter_root, adapter, policies = None, {}, {}
    if adapter_path:
        adapter_root = outside(adapter_path, "Adapter")
        adapter = json.loads((adapter_root / "manifest.json").read_text(encoding="utf-8"))
        if adapter["interface"] != ADAPTER_INTERFACE_VERSION:
            raise ValueError("Unsupported adapter interface")
        fingerprints = adapter["binary_sha256"]
        if isinstance(fingerprints, str):
            fingerprints = [fingerprints]
        if not isinstance(fingerprints, list) or not fingerprints or any(
            not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value)
            for value in fingerprints
        ):
            raise ValueError("Invalid adapter executable fingerprints")
        if identity.binary_sha256 not in fingerprints:
            raise ValueError("Adapter does not match the original executable")
        if policies_enabled:
            policies = json.loads(json.dumps(adapter.get("policies", {})))
        known = adapter.get("policies", {})
        if set(disabled) & set(enabled) or (enabled and not policies_enabled):
            raise ValueError("Conflicting policy selections")
        for name in [*disabled, *enabled]:
            if name not in known:
                raise ValueError(f"Unknown adapter policy: {name}")
            policies.setdefault(name, dict(known[name]))["enabled"] = name in enabled
        for name, value in known.items():
            if variant not in value.get("variants", ["dev", "release"]):
                policies.setdefault(name, {})["enabled"] = False
    elif disabled or enabled:
        raise ValueError("Policies require an explicitly selected adapter")
    android = dict(config.get("android", {}))
    application_id = android.get("application_id", identity.identifier)
    if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z][A-Za-z0-9_]*)+", application_id):
        raise ValueError("An explicit valid Android application_id is required")
    version_code = int(
        android.get("version_code", identity.build if identity.build.isdecimal() else 1)
    )
    if not 0 < version_code <= 2100000000:
        raise ValueError("Invalid Android version code")
    storage = dict(
        preferences="i2a.runtime",
        uuid_namespace=application_id + ":",
        keychain_alias=application_id + ".generic-password.v1",
        keychain_aad=application_id + ".generic-password.v1",
        defaults_domain=identity.identifier,
        uti_namespace="dyn.i2a.",
    )
    storage.update(android.get("storage", {}))
    signing = dict(config["signing"])
    signing["keystore"] = str(outside(signing["keystore"], "Signing key"))
    if not Path(signing["keystore"]).is_file():
        raise ValueError("Signing key is missing; keys are never generated implicitly")
    return BuildContext(
        ipa,
        workspace,
        tools,
        signing,
        identity,
        info,
        variant,
        application_id,
        version_code,
        storage,
        android.get("host", {}),
        adapter_root,
        adapter,
        policies,
    )


def bundle_header(context, source):
    identity = context.identity
    raw = source.read(identity.prefix + "Info.plist")
    catalogue = context.host.get("storekit", {}).get("catalogue", "empty")
    if catalogue not in ("empty", "error"):
        raise ValueError("host.storekit.catalogue must be empty or error")
    support = context.host.get("helpshift", {})
    native_support = bool(context.host.get("libraries") and support.get("platform_id") and support.get("backend", "auto") in ("auto", "sdk"))
    fields = dict(
        GUEST_MAIN_STORYBOARD=context.info.get("UIMainStoryboardFile") or "",
        BUNDLE_ID=identity.identifier,
        GUEST_BUNDLE_NAME=identity.directory,
        GUEST_EXECUTABLE=identity.executable,
        GUEST_IMAGE_PATH=identity.directory + "/" + identity.executable,
        GUEST_APP_NAME=identity.display_name,
        GUEST_UTI_NAMESPACE=context.storage["uti_namespace"],
    )
    header = [
        "static const unsigned char bundle_plist[] = {" + ",".join(str(byte) for byte in raw) + "};"
    ]
    header += ["#define " + name + " " + json.dumps(value) for name, value in fields.items()]
    header += [
        "#define HOST_JNI(name) Java_local_ios_host_##name",
        "#define STOREKIT_EMPTY_CATALOGUE " + str(int(catalogue == "empty")),
        "#define HELPSHIFT_NATIVE_BACKEND " + str(int(native_support)),
        "#define GUEST_APP_BUILD " + str(context.version_code),
    ]
    (context.work / "bundle.h").write_text("\n".join(header) + "\n", encoding="utf-8")


def interface_catalogue(context):
    from .image.interfaces import REVISION, scan_interfaces

    cache = context.workspace / "cache/interfaces" / context.identity.binary_sha256
    cache.mkdir(parents=True, exist_ok=True)
    inputs = dict(
        binary=context.identity.binary_sha256,
        revision=REVISION,
        generator=file_metadata([ROOT / "porter/image/interfaces.py"]),
    )
    output = cache / "catalogue.json"
    with build_lock(cache):
        data = json.loads(output.read_text(encoding="utf-8")) if output.exists() else {}
        if data.get("inputs") != inputs:
            data = scan_interfaces(context.work / "guest.bin")
            data["inputs"] = inputs
            write_json(output, data)
    rows = [
        f"{{{json.dumps(p['class'])},{json.dumps(p['selector'])},{int(p['class_method'])},{p['sites'][0]['call']}UL}}"
        for p in data["inferred_pairs"]
    ]
    (context.work / "interface_audit.h").write_text(
        "struct InterfaceProbe { const char *class_name,*selector; unsigned class_method; Word call; };\n"
        "static const struct InterfaceProbe interface_probes[]={\n" + ",\n".join(rows) + "\n};\n",
        encoding="utf-8",
    )


def build(context, core_only=False):
    from .android import native, package
    from .bundle.asset_catalog import prepare_asset_catalog
    from .bundle.identity import font_plan, prepare_app_identity
    from .bundle.storyboard import prepare_controller_catalogue, prepare_launch_screen
    from .image.macho import prepare_image
    from .shaders.build import build_shader_pack, pending_shader_plan

    native.validate(context)
    context = replace(context, core_only=core_only)
    context.work.mkdir(parents=True, exist_ok=True)
    adapter = None if core_only else context.load_adapter()

    def event(stage):
        print("Stage: " + stage, flush=True)

    with build_lock(context.work):
        (context.work / "build.json").unlink(missing_ok=True)
        with zipfile.ZipFile(context.ipa) as source:
            event("image")
            raw = source.read(context.identity.prefix + context.identity.executable)
            patch = (
                (lambda raw, image, work: adapter.patch(context, raw, image, work))
                if adapter
                else None
            )
            main_image = prepare_image(raw, context.work, patch)
            from .image.collection import prepare_collection
            prepare_collection(source, context.identity.prefix, context.identity.executable, context.work, main_image)
            if context.development:
                interface_catalogue(context)
            event("bundle")
            prepare_controller_catalogue(source, context.identity.prefix, context.work)
            bundle_header(context, source)
            exclusions = adapter.catalog_exclusions() if adapter else ()
            catalog = (
                prepare_asset_catalog(source, context.identity.prefix, context.work, exclusions)
                if not core_only
                else []
            )
            _, resources = prepare_app_identity(
                source, context.identity.prefix, context.info, context.work, catalog
            )
            launch_screen = prepare_launch_screen(source, context.identity.prefix, context.info, resources, catalog) if not core_only else None
            overlays = adapter.resources(context, source) if adapter else {}
            members = set(source.namelist())
            for name, overlay in overlays.items():
                directory = isinstance(overlay, DirectoryOverlay)
                exists = (
                    any(member.startswith(context.identity.prefix + name.rstrip("/") + "/") for member in members)
                    if directory
                    else context.identity.prefix + name in members
                )
                if name.startswith("/") or ".." in Path(name).parts or not exists:
                    raise ValueError("Overlay must target an existing bundle resource: " + name)
                if directory and not overlay.directory.is_dir():
                    raise ValueError("Missing resource overlay directory: " + str(overlay.directory))
            revision = uuid.uuid4().hex[:12]
            event("shaders")
            if adapter:
                plan = adapter.shader_plan(context, source)
            else:
                has_metal = any(name.startswith(context.identity.prefix) and name.endswith('.metallib') for name in source.namelist())
                allow_pending = context.host.get('allow_incomplete_shaders', False)
                if allow_pending and not context.development:
                    raise ValueError('Incomplete shaders are permitted only in an explicitly configured dev build')
                if has_metal and not core_only and not allow_pending:
                    raise ValueError('Metal libraries need an explicit validated shader plan')
                plan = (dict(executable=context.identity.prefix + context.identity.executable, libraries=[], shaders={})
                        if core_only or not has_metal else pending_shader_plan(source, context.identity.prefix,
                            context.identity.prefix + context.identity.executable, context.work))
            build_shader_pack(source, context.work, plan)
            event("native")
            native.generate_descriptors(context.work)
            extension_sources = (
                [resolve_file(context.adapter_root, path) for path in adapter.native_sources(context)]
                if adapter
                else []
            )
            library = native.build(context, extension_sources, standalone=core_only, adapter=adapter)
            if core_only:
                write_json(
                    context.work / "build.json",
                    dict(context.summary(), status="compiled-core", library=str(library)),
                )
                print(library, flush=True)
                return library
            event("host")
            fonts = font_plan(context, source)
            manifest, dex = package.build_host(context, revision, fonts, resources, launch_screen, adapter)
            event("package")
            candidate = package.build_apk(
                context, source, manifest, resources, catalog, overlays, library, dex, adapter
            )
        summary = dict(
            context.summary(),
            status="complete",
            apk=str(candidate),
            resource_overlays=list(overlays),
            package_revision=revision,
        )
        write_json(context.work / "build.json", summary)
        write_json(context.output / "build.json", summary)
        print(candidate, flush=True)
        return candidate
