# DogSC for Windows

DogSC 的 Windows 原生移植。录屏、剪辑、运镜与导出，面向 Windows 11。

> 本项目源自 [laogou717/dogsc](https://github.com/laogou717/dogsc)（MIT，Copyright (c) 2026 神烦老狗）。原始 macOS 实现见上游仓库；本仓库是独立的 Windows 实现。

## 状态

**移植进行中：M0 核心基线、M1 原生采集、M2a 离屏渲染、P1a/P1b 项目生命周期、P2a/P2b 预览基础与 P4 视频导出 prototype 已实现。**

- 已实现：C++20 工程模型/时间线求值及 JSON；WGC 屏幕与窗口采集、WASAPI、指针轨、可选 MF 摄像头、HEVC/H.264 硬编与 FFmpeg ProRes 422 Standard、10 秒分片及无损合并/恢复；D2D/WARP 六层离屏合成；Tauri 项目库、语义 dirty draft、debounce 自动保存、flush/重试、discard/recovery、关闭确认、时间线编辑与多步历史；MF SourceReader 持久 stream、NV12 DXGI decoder ingress、FrameSceneEvaluator/D2D 实时合成、sidecar DXGI swapchain overlay、实时 pointer.jsonl/cursor click/camera scene 资源、PNG fallback/static composite；单源 H.264 MP4 export prototype。
- 已验证：Windows Release clean build；7/7 CTest；H.264/HEVC frame/seek/stream/EOF/D2D composite；真实 GUI dirty close/cancel/retry；跨工程 child 回收。打包 GUI D2D/DXGI camera smoke：960×600、45fps、30/30 frames、0 drops；cursor/click smoke：960×600、43fps、30/30 frames、0 drops；两者均报告 DXGI swapchain 与 vblank；这是短样本，不代表长时60Hz/no-tear验收。
- 尚未完成：SourceReader NV12 texture 仍 staging-readback→CPU YUV→BGRA upload 到 D2D 输入（D2D 输出纹理通过同设备 `CopyResource` 直达 swapchain；非零拷贝）；实时与单源 H.264 export 已接入 camera/cursor/sticker/spotlight、edited video TimelineMediaPlan、system audio AAC、独立 M4A CLI/Tauri GUI、progress JSON、partial 原子收尾和 Tauri 取消按钮，screen-frame/opening 与 screen/cursor motion editor controls 已通过 smoke；实时 cursor 仍是单一箭头 fallback，贴图更多格式/导入 UI、wallpaper 外部资源、screen 3D 透视控件和完整像素对拍未完成；预览 stream 统一时间线仍在接入，麦克风真实资产 smoke、多轨音频、M4A 取消/失败恢复/重试、PresentMon/capture-based no-tear、长时60Hz/120Hz/VRR/多 GPU/最小化/DPI 验收、长时 drop<2% 与完整拖动/暂停语义；区域拖拽选择、权限/设置/托盘/语言/录制标记；长时4K60/跨设备与 FFmpeg LGPL 分发合规。

## 决策基线

| 项 | 决定 |
|---|---|
| 语言 | C++20（禁用 `/fp:fast`） |
| 编辑器 UI | Tauri 2 + HTML/CSS/TypeScript（当前录制与项目时间线编辑切片） |
| HUD 窗口 | Win32 顶层窗口 + Direct2D 自绘 |
| 合成 | Direct2D + 少量 HLSL，预览与导出共用 `IFrameCompositor` |
| 采集 | Windows.Graphics.Capture + WASAPI + Media Foundation SinkWriter |
| 导出 | Media Foundation SourceReader → 自研时间线调度 → 合成 → SinkWriter |
| 最低系统 | Windows 11 24H2（build 26100+） |
| 工程互通 | **不做**，Windows 端为独立软件系统 |
| 自动更新 | 首版关闭 |
| 系统声音 | 仅整设备 loopback |
| iOS 设备录制 | 首版移除 |
| 光标素材 | 自绘 mac 观感素材包 |

## 文档

| 文件 | 内容 |
|---|---|
| [docs/00-project-overview.md](docs/00-project-overview.md) | macOS 现状基线：模块地图、状态机、项目包布局、采集/渲染/导出/UI |
| [docs/01-data-model-and-timeline.md](docs/01-data-model-and-timeline.md) | 持久化契约、时间线引擎、帧契约、动画算法 |
| [docs/02-platform-dependency-map.md](docs/02-platform-dependency-map.md) | 10 个硬阻塞点 + 四类 API 映射表 |
| [docs/03-ui-inventory.md](docs/03-ui-inventory.md) | 窗口所有权、状态与撤销、本地化、设计系统、UI 分区工作量 |
| [docs/11-architecture.md](docs/11-architecture.md) | 技术栈、仓库结构、模块映射、关键契约 |
| [docs/12-review-r1.md](docs/12-review-r1.md) | 复审 1：技术路线可行性 |
| [docs/13-review-r2.md](docs/13-review-r2.md) | 复审 2：进度可行性与验收粒度 |
| [docs/14-review-r3.md](docs/14-review-r3.md) | 复审 3：依赖顺序与不可逆决策 |
| [docs/15-final-plan.md](docs/15-final-plan.md) | **开工依据**：工作量、契约、阶段、验收标准、M0 任务 |
| [docs/16-open-questions.md](docs/16-open-questions.md) | 已决议的开放项记录 |
| [docs/17-majority-migration-plan.md](docs/17-majority-migration-plan.md) | 复审确认的大部分功能迁移路线与停止标准 |
| [docs/18-feature-matrix.md](docs/18-feature-matrix.md) | 原版功能、当前状态、目标模块与验收矩阵 |
| [docs/20-acceptance-runbook.md](docs/20-acceptance-runbook.md) | Windows 实际构建、CLI、GUI、长时矩阵与发布验收步骤 |
| [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) | 两个参考仓库的来源、许可证与禁止复制边界 |
| [docs/10-plan-r1.md](docs/10-plan-r1.md) | 初版方案（已被 15 取代，保留作决策留痕） |

## 目录结构（规划）

```
core/            纯逻辑：模型/校验/时间线/动画/场景求值（对应 macOS RecorderCore，15,369 行）
render/          Direct2D 合成器
render/present/  DXGI 实时预览
capture/         WGC + WASAPI + MF 写入
export/          MF 解码 → 合成 → 编码
orchestration/   状态机 / 持久化 / 播放时钟
ui/              WinUI 3 编辑器 + Win32 HUD
apps/tauri/       Tauri v2 桌面壳、中文录制工作区与 C++ sidecar
```

## Tauri 桌面壳（当前可运行切片）

先构建 C++ Release 目标，再启动 Tauri：

```powershell
cmake --build out/vcpkg-native --config Release --parallel
cd apps/tauri
pnpm install
pnpm tauri:dev
```

生成可双击安装包：

```powershell
pnpm tauri:build
```

安装包位于 `apps/tauri/src-tauri/target/release/bundle/nsis/`。当前界面可以配置显示器、帧率、编码器、系统声音、麦克风、摄像头和输出目录，并通过 sidecar 启动/暂停/继续/停止 C++ 原生采集。它还不是完整编辑器；项目库、时间线编辑、实时预览和最终导出仍未接入。

详细说明见 [`apps/tauri/README.md`](apps/tauri/README.md)。

## Windows 采集依赖与分发

Windows 采集使用 vcpkg manifest 显式启用 FFmpeg 的 `avcodec`、`avformat`、`swresample` 与 `swscale` 功能；为兼容仅含 release archive、没有本地 Git 历史的 vcpkg 安装，manifest 不引用 `builtin-baseline`，FFmpeg 版本由开发者使用的 vcpkg registry revision 决定（因此依赖版本尚未锁定）。请使用动态 `x64-windows` triplet（或其他确认采用动态链接的 Windows triplet），不要启用默认功能、GPL 或 nonfree 功能。运行时必须随应用部署 FFmpeg DLL。

FFmpeg 为 LGPLv2.1-or-later。重新分发 FFmpeg DLL 时，必须同时提供与所发二进制完全对应的 FFmpeg 源码、所用补丁/差异及构建说明，并提供 FFmpeg 及相关库的许可和版权声明；源码应与应用二进制一同可获取。应用应保持动态链接和可替换的 FFmpeg DLL，不得以许可条款禁止用户为调试 FFmpeg 修改而进行反向工程；关于窗口、EULA 与下载页也须给出适当的 FFmpeg/LGPL 声明。发布前还须逐项核对 FFmpeg 官方 [LGPL 合规清单](https://ffmpeg.org/legal.html)，不能将本仓库的 vcpkg 配置视为已完成二进制分发合规。

LGPL 合规不代表专利许可。ProRes 与 AAC 相关专利状况取决于司法辖区及分发/使用场景；商业分发前需单独进行法律评估。

## 许可

[MIT](LICENSE)。原项目版权归 神烦老狗 所有；Windows 实现同样以 MIT 发布。
