# IOS2Android

**简体中文** | [English](README.en.md)

IOS2Android 将 ARM64 iOS 应用转换为 Android APK。原应用的 ARM64 代码嵌入 ELF，由兼容运行库执行。运行库实现部分 Darwin、Objective-C 和 iOS 系统框架接口，通过 Android 宿主提供窗口、输入、网络和音频等能力。Metal 使用 Vulkan 后端，着色器转换与应用打包在构建时完成。

项目仍在开发中，**尚未完整实现全部 iOS 接口及其到 Android 的转换**。应用能否运行取决于使用的接口、镜像格式和着色器，需要针对具体版本进行适配和验证。构建成功不代表所有功能均可用。

## 目录结构

```text
IOS2Android/
├── porter/                    离线构建工具
│   ├── __main__.py            命令行入口
│   ├── build.py               配置、适配加载与构建流程
│   ├── files.py               文件操作与缓存
│   ├── bundle/                IPA 元数据、字体、NIB 和 CoreUI 资源
│   ├── image/                 Mach-O 处理、镜像补丁、接口索引与展开信息
│   ├── shaders/               Metal/AIR/GLSL 解析、转换与打包
│   └── android/               编译、APK 打包与签名
├── runtime/                   兼容运行库
│   ├── core/                  Darwin ABI、Objective-C、调度与对象生命周期
│   ├── frameworks/            iOS 系统框架接口
│   ├── backends/vulkan/       GPU 命令、资源、管线与呈现
│   ├── contracts/             Metal descriptor 定义
│   ├── include/i2a/           运行库扩展接口
│   ├── diagnostics/           运行诊断
│   └── support/               纹理解码等辅助代码
├── host/android/              Android 宿主
│   ├── java/                  Activity 与平台接口
│   ├── res/                   样式与资源
│   └── templates/             AndroidManifest 模板
├── pyproject.toml             Python 项目与依赖配置
├── LICENSE                    非商业使用许可
├── NOTICE                     署名与来源通知
├── README.md                  中文说明
└── README.en.md               英文说明
```

## 参考项目

| 项目 | 简介与参考内容 |
|---|---|
| [nx2android](https://github.com/shirai91/nx2android) | 将 Nintendo Switch 程序移植到 Android 的项目。本仓库参考其离线构建流程与宿主分层。 |
| [Maclator](https://github.com/linuxkid473/maclator) | macOS 程序兼容运行项目。本仓库参考其 ABI 与 descriptor 元数据预计算、资源生命周期管理。 |
| [CARParser](https://github.com/Timac/CARParser) | Apple CoreUI `.car` 资源解析工具，用于参考资源格式与字段定义。 |

## 补充说明

### 构建

当前构建环境使用 Python 3.11+、Windows Android NDK r27d、SDK Build Tools 35 / API 35 和 JDK。Python 依赖在 `pyproject.toml` 中声明；原生依赖、着色器工具和签名信息通过外部 JSON 配置指定。`toolchain.shaders` 目录需提供 `glslangValidator.exe` 和 `spirv-val.exe`，用于内置 Vulkan 1.0 呈现着色器的编译与校验。工具链与依赖需自行准备。

在仓库根目录运行以下命令查看用法：

```bash
python -B -m porter --help
python -B -m porter build --help
```

命令包括 `inspect`、`build`、`check-core` 和 `shader`。其中 `check-core` 只编译核心运行库，不链接应用适配器，也不生成可运行 APK。dev 包启用 Android 调试，方便用 ADB 查看私有目录；release 包关闭调试。

应用补丁、资源替换、服务配置和着色器转换规则由外部适配器提供。IPA、工具链、配置、签名密钥、适配器和构建产物均放在仓库外。

未提供 shader plan 的 Metal 应用默认停止构建。仅用于启动诊断的 dev 配置可显式设置 `android.host.allow_incomplete_shaders=true`，保留待转换库信息；调用这些库时仍返回转换缺失错误。该选项不适用于 release。

shader plan 提供每个模块的实际入口、资源绑定和反射信息。常量采样器在构建时转换为过滤、寻址和 LOD 参数，运行时不依赖特定应用的原始常量或资源目录。未知 AIR 编码会停止转换；其他转换器可直接提供采样器描述。

在线服务优先复用原应用的通用地址与配置；iOS 专属的账号、支付、推送和第三方平台标识不能直接用于 Android。没有可复用配置时，运行库按未配置处理，返回相应失败或不可用状态；Helpshift 有有效 domain 时可使用网页后端。

资源通常直接从已安装 APK 读取；需要真实文件路径或系统不支持共享内存时，按需处理单个文件，临时映射文件随使用结束释放。启动时清理旧版运行库留下的资源副本。着色器和驱动编译结果缓存在应用私有目录，更新后清理旧缓存，即使应用版本号没有变化。缓存不可用时会重新生成。

纹理转换是可选的构建功能，资源路径由 `textures` 配置指定。目前支持二维 PVR3 中的 PVRTC1 转 ETC2，源程序需要能够读取 ETC2；其他格式保留原文件。转换在构建时完成，无需外部适配器，也不增加运行时编解码。Python 依赖见 `pyproject.toml` 的 `textures` extra。

已加载的外部适配器可通过可选的 `texture_options(context)` 返回有效纹理配置；返回空配置会停用转换，用于与原程序的格式支持保持一致。

APK 输出到外部工作区的 `out/<application-id>/<CFBundleVersion>/dev|release/`，默认文件名为 `application.apk`；外部适配器可在 `prepare_package` 返回的选项中用 `filename` 指定 APK 文件名。不同版本共用构建缓存时需顺序构建；并行构建应使用独立工作区。

应用每次进程启动会重写 `Android/data/<包名>/files/crash.log`，随后只追加异常和崩溃。release 正常日志关闭。原生栈无法继续读取时会注明原因；定位时需使用 APK 旁的同版符号文件。

### 兼容范围

运行目标为 ARM64 Android 9 及以上版本。窗口、选图和权限按系统能力选择接口；GPU 驱动、WebView、图库及定位服务仍会影响可用功能。目前没有覆盖所有系统版本和厂商 ROM 的验证。

- Foundation/UIKit 的通用对象回收尚未完成；弱引用自动清零目前只覆盖运行库托管生命周期的对象。
- 字符串格式化支持本地化数字；`stringsdict` 复数规则和对象的区域化描述仍未实现。
- 加密镜像、chained fixups、导出重转发与 resolver 尚不支持。依赖收集限于原 IPA 中可解析的 ARM64 镜像；运行时加载新镜像及跨镜像 TLS 导出尚未实现。
- Swift 系统库不会自动补充；动态类迁移、Swift 对象与宿主 Foundation 对象的完整桥接、Core Data 持久化仍未实现。
- CoreUI 的完整 trait 选择尚未实现；无法唯一解析的 atlas 或默认图像变体保持不可用，原始 catalog 保留。
- Scene 生命周期、复杂 NIB、Auto Layout、通用滚动和文本编辑仍有兼容缺口。
- 导航与模态尚缺完整原生转场、Popover 箭头、自定义呈现控制器和复杂多窗口行为。
- 完整 CoreAnimation、三维图层合成和层组排序尚未实现；富文本绘图上下文及部分文字排版选项仍不支持。图案颜色尚不能用作文字前景、图层边框或阴影。
- 表格尚缺约束驱动的自动行高、NIB 单元格、编辑/移动、插删动画和完整分组样式；离屏行几何可能使用估计高度。
- 网页支持基本加载和交互，多窗口、下载及部分导航和脚本接口仍未实现。
- 照片接口尚缺相机、裁剪、视频、iCloud 版本、图库变更观察及完整查询条件；iOS 旧图库标识不能直接对应 Android 文件。
- Metal → Vulkan 支持图形和计算命令、动态 viewport/scissor、颜色 MSAA resolve，以及设备支持的深度/模板 resolve。shader 能力和线程组信息在构建时记录，运行时校验设备支持。多 viewport、非整组线程派发、部分 resolve 过滤与独立深度/模板目标仍未支持；MSL/AIR 转换仅覆盖已有路径。
- 音频格式支持仍有限，实时 AudioQueue 播放尚未实现。Android 公开信息无法完整识别其他应用的音频归属与混音状态，次要音频静音提示仍为近似实现。
- 通知的完整日历/位置触发、分类动作与附件，Bonjour 浏览/解析，以及手势失败依赖与完整仲裁尚未实现。
- 后台定位、区域监控、航向和部分定位授权接口尚未实现；地理编码依赖设备上的 Android 服务。

### 许可与署名

本项目采用 [PolyForm Noncommercial 1.0.0](LICENSE) 许可。

**本仓库供学习、研究和非商业开发使用，不允许用于商业用途或盈利活动。** 许可允许非商业使用、修改、分发和代码引用，完整条款见 `LICENSE`。

原始仓库：[ThatSkyOldServer/IOS2Android](https://github.com/ThatSkyOldServer/IOS2Android)。引用本项目代码时，须注明原作者或原始仓库。

### 分发规则

- 允许出于非商业目的复制、修改和分发本项目，包括源码、编译产物及修改版本。
- 每次分发必须提供 `LICENSE` 许可文本或其官方链接，并保留 [NOTICE](NOTICE) 中所有以 `Required Notice:` 开头的署名行，包括作者和原始仓库地址。
- 分发和后续使用均须遵守非商业限制，不得将本项目或修改版本用于商业用途或盈利活动，也不得以允许商用的许可重新发布本项目代码。
- 第三方工具、库与 SDK 保留各自的许可及署名要求。本许可不授予原 iOS 应用、应用资源或第三方依赖的分发权利；如需一同分发，须另行取得授权或满足其适用许可。

运行时 LZFSE 解码器来自 Apple，使用 [BSD-3-Clause 许可](third_party/lzfse/LICENSE)；其许可文本也随 APK 打包。

完整授权条件以 `LICENSE` 为准。
