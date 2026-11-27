from dataclasses import dataclass
from pathlib import Path

IMAGE_ABI_VERSION = 2
RUNTIME_INTERFACE_VERSION = 2
SHADER_PACK_VERSION = 2
ADAPTER_INTERFACE_VERSION = 2


def versions():
    return dict(image=IMAGE_ABI_VERSION, runtime=RUNTIME_INTERFACE_VERSION,
                shaders=SHADER_PACK_VERSION, adapter=ADAPTER_INTERFACE_VERSION)


@dataclass(frozen=True)
class BundleIdentity:
    prefix: str
    directory: str
    executable: str
    identifier: str
    version: str
    build: str
    display_name: str
    binary_sha256: str


@dataclass(frozen=True)
class ImagePatchResult:
    image: str
    files: tuple[str, ...] = ()


@dataclass(frozen=True)
class DirectoryOverlay:
    directory: Path
    date_time: tuple[int, int, int, int, int, int] | None = None
