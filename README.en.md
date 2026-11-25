# IOS2Android

[简体中文](README.md) | **English**

IOS2Android converts ARM64 iOS applications into Android APKs. It embeds the original ARM64 code in an ELF binary and runs it with a compatibility runtime. The runtime implements a subset of Darwin, Objective-C, and iOS framework interfaces, with an Android host providing windows, input, networking, and audio. Metal uses a Vulkan backend; shader translation and application packaging take place at build time.

The project is under development. **Support for iOS APIs and their translation to Android is incomplete.** Compatibility depends on the APIs, binary format, and shaders used by an application. Each application version needs adaptation and validation. A successful build does not guarantee that every feature works.

## Project layout

```text
IOS2Android/
├── porter/                    Offline build tools
│   ├── __main__.py            CLI entry point
│   ├── build.py               Configuration, adapter loading, and build pipeline
│   ├── files.py               File operations and caching
│   ├── bundle/                IPA metadata, fonts, NIB, and CoreUI resources
│   ├── image/                 Mach-O processing, binary patches, interface indexing, and unwind data
│   ├── shaders/               Metal/AIR/GLSL parsing, translation, and packaging
│   └── android/               Compilation, APK packaging, and signing
├── runtime/                   Compatibility runtime
│   ├── core/                  Darwin ABI, Objective-C, scheduling, and object lifetimes
│   ├── frameworks/            iOS framework interfaces
│   ├── backends/vulkan/       GPU commands, resources, pipelines, and presentation
│   ├── contracts/             Metal descriptor definitions
│   ├── include/i2a/           Runtime extension API
│   ├── diagnostics/           Runtime diagnostics
│   └── support/               Texture decoding and other support code
├── host/android/              Android host
│   ├── java/                  Activity and platform interfaces
│   ├── res/                   Styles and resources
│   └── templates/             AndroidManifest template
├── pyproject.toml             Python project and dependency configuration
├── LICENSE                    Noncommercial license
├── NOTICE                     Attribution and source notices
├── README.md                  Chinese README
└── README.en.md               English README
```

## Related projects

| Project | Description and reference |
|---|---|
| [nx2android](https://github.com/shirai91/nx2android) | Ports Nintendo Switch software to Android. A reference for the offline build pipeline and host architecture. |
| [Maclator](https://github.com/linuxkid473/maclator) | A macOS compatibility project. A reference for precomputed ABI and descriptor metadata, and resource lifetime management. |
| [CARParser](https://github.com/Timac/CARParser) | A parser for Apple CoreUI `.car` resources. A reference for resource formats and field definitions. |

## Notes

### Building

The current build environment uses Python 3.11+, Android NDK r27d for Windows, SDK Build Tools 35 / API 35, and a JDK. Python dependencies are declared in `pyproject.toml`. External JSON configuration specifies native dependencies, shader tools, and signing details. `toolchain.shaders` must provide `glslangValidator.exe` and `spirv-val.exe` to compile and validate the built-in Vulkan 1.0 presentation shaders. The toolchain and dependencies must be installed separately.

Run these commands from the repository root to view usage:

```bash
python -B -m porter --help
python -B -m porter build --help
```

The CLI provides `inspect`, `build`, `check-core`, and `shader`. The `check-core` command compiles only the core runtime, without linking application adapters, and does not produce a runnable APK. Dev APKs enable Android debugging for ADB access to private storage; release APKs disable it.

External adapters supply application patches, resource replacements, service configuration, and shader translation rules. Keep IPAs, toolchains, configuration, signing keys, adapters, and build outputs outside the repository.

Metal applications without a shader plan fail the build by default. A dev configuration used for startup diagnosis may explicitly set `android.host.allow_incomplete_shaders=true` to retain pending library metadata; using those libraries still reports missing translation. This option is not available for release builds.

Online services reuse portable endpoints and configuration from the original application where available. iOS account, payment, push, and third-party platform identifiers cannot be used directly on Android. Without compatible configuration, the runtime reports failure or unavailability. Helpshift can use its web backend when a valid domain is available.

Resources are normally read from the installed APK. A real-path requirement or unavailable shared-memory support is handled per file; temporary mapping files are released when no longer in use. Startup removes resource copies left by older runtime versions. Shaders and driver compilation results are cached in private storage. Updates remove old caches even when the application version stays the same. Unavailable caches are regenerated.

Texture conversion is optional. Resource paths are selected in the `textures` build configuration. Currently, it converts PVRTC1 to ETC2 in 2D PVR3 files; the application must be able to load ETC2. Other formats keep their original files. Conversion runs during the build, requires no external adapter, and adds no runtime encoding or decoding. Python dependencies are declared in the `textures` extra in `pyproject.toml`.

A loaded adapter may return the effective texture configuration through the optional `texture_options(context)` hook. Returning an empty configuration disables conversion to match the source application's format support.

APKs are written to `out/<application-id>/<CFBundleVersion>/dev|release/` in the external workspace. Build different input versions sequentially when sharing a cache. Use separate workspaces for parallel builds.

Each process launch replaces `Android/data/<package>/files/crash.log`, then appends exceptions and crashes only. Normal release logging stays disabled. Native records explain when a stack cannot be read further. Use the matching symbols beside the APK to locate failures.

### Compatibility

The runtime targets ARM64 Android 9 and later. Window, picker, and permission APIs are selected according to platform capabilities. GPU drivers, WebView, photo providers, and location services still affect feature availability. Validation does not cover every Android version or vendor ROM.

- General Foundation/UIKit object destruction remains incomplete. Automatic weak-reference clearing currently covers only objects whose lifetimes are managed by the runtime.
- Encrypted binaries, chained fixups, reexports, and resolver exports are unsupported. Dependency collection is limited to resolvable ARM64 images in the original IPA; loading new images at runtime and binding thread-local exports across images remain unimplemented.
- System Swift libraries are not supplied automatically. Dynamic class relocation, full bridging between Swift objects and host Foundation objects, and Core Data persistence remain unimplemented.
- Full CoreUI trait selection remains unimplemented. Ambiguous atlases or default rendition aliases remain unavailable, and original catalogs are retained.
- Scene lifecycle, complex NIBs, Auto Layout, general scrolling, and text editing still have compatibility gaps.
- Navigation and modal presentation still lack complete native transitions, popover arrows, custom presentation controllers, and complex multi-window behavior.
- Full CoreAnimation, three-dimensional layer composition, and layer-group ordering remain unimplemented. Attributed-text drawing contexts and some text layout options are unsupported.
- Tables still lack constraint-based automatic row heights, NIB cells, editing/reordering, insertion/deletion animations, and complete grouped styling. Offscreen row geometry may use estimated heights.
- Web pages support basic loading and interaction. Multiple windows, downloads, and some navigation and scripting APIs remain unsupported.
- Photo support still lacks camera capture, cropping, video, iCloud versions, library change observation, and complete query options. Legacy iOS photo identifiers cannot directly identify Android files.
- Metal-to-Vulkan and MSL/AIR translation cover only the interfaces and translation paths currently implemented.
- Audio format support is limited, and real-time AudioQueue playback is not implemented. Android's public playback information does not fully expose other apps' audio ownership or mixing state, so secondary-audio hints are approximate.
- Full calendar/location notification triggers, notification actions and attachments, Bonjour browsing/resolution, and gesture failure dependencies and arbitration are not implemented.
- Background location, region monitoring, heading, and some authorization APIs are not implemented. Geocoding depends on the device's Android service.

### License and attribution

This project is licensed under [PolyForm Noncommercial 1.0.0](LICENSE).

**This repository is intended for learning, research, and noncommercial development. Commercial use and use for profit are not permitted.** The license allows noncommercial use, modification, redistribution, and reuse of code. See `LICENSE` for the full terms.

Source repository: [ThatSkyOldServer/IOS2Android](https://github.com/ThatSkyOldServer/IOS2Android). Code reused from this project must credit the original authors or link to the source repository.

### Redistribution

- You may copy, modify, and redistribute the project for noncommercial purposes, including source code, compiled binaries, and modified versions.
- Every redistribution must include the `LICENSE` text or its official URL and preserve every line beginning with `Required Notice:` in [NOTICE](NOTICE), including the authors and source repository URL.
- Redistribution and subsequent use remain subject to the noncommercial terms. The project and modified versions may not be used commercially or for profit, and project code may not be republished under a license that permits commercial use.
- Third-party tools, libraries, and SDKs retain their own licenses and attribution requirements. This license does not authorize redistribution of the original iOS applications, their assets, or third-party dependencies. Distributing them requires separate permission or compliance with their applicable licenses.

The runtime LZFSE decoder comes from Apple under its [BSD-3-Clause license](third_party/lzfse/LICENSE). The license text is also included in generated APKs.

See `LICENSE` for the complete and governing terms.
