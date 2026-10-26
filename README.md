# DogSC for Windows

DogSC 的 Windows 原生移植。录屏、剪辑、运镜与导出，面向 Windows 11。

> 本项目源自 [laogou717/dogsc](https://github.com/laogou717/dogsc)（MIT，Copyright (c) 2026 神烦老狗）。原始 macOS 实现见上游仓库；本仓库是独立的 Windows 实现。

## 状态

**移植进行中：核心、原生采集、D2D/WARP、项目生命周期、时间线编辑、DXGI/D2D 预览、场景效果、预览音频、MP4/M4A 导出和 Tauri 产品壳已具备可运行切片。**

- 当前已实现：WGC 屏幕/窗口采集、WASAPI、指针轨、可选摄像头、HEVC/H.264、ProRes 422、10 秒分片恢复；TimelineMap/TimelineMediaPlan；dirty draft/自动保存/恢复/撤销重做；D2D 六层合成；DXGI swapchain 预览；cursor/click/camera/sticker/mosaic/spotlight/screen-frame/opening/screen-motion/手动与点击自动缩放；MP4/M4A 导出与取消清理；设置、权限向导、托盘、备忘录、区域选择器和运行时中英文壳。
- 当前已验证：Release clean build、7/7 CTest、H.264/HEVC frame/seek/stream/EOF/D2D composite、真实 GUI 录制/暂停/继续/停止、预览音频时钟、DXGI move/resize/occlusion、自动缩放和 A/B child lifecycle。
- 当前未完成：完整 AutoZoomPlanner legacy repair、时间线拖拽编辑、完整本地化、逐像素基准、麦克风多轨长时同步、120Hz/VRR/多 GPU/实际无撕裂、4K60/跨硬件、FFmpeg 最终法律审查。详见 `docs/18-feature-matrix.md` 与 `docs/20-acceptance-runbook.md`。

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
| [docs/21-current-status.md](docs/21-current-status.md) | **当前实现/验收状态快照**：已实现、实测、未完成、环境阻塞与用户侧缺陷修复 |
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

安装包位于 `apps/tauri/src-tauri/target/release/bundle/nsis/`。当前壳包含录制、暂停/继续/停止、项目库、时间线编辑、场景检查器、DXGI/D2D 预览、预览音频、MP4/M4A 导出、设置/权限向导、托盘、备忘录、区域选择器、语言切换和缩放关键帧。使用 `docs/20-acceptance-runbook.md` 进行用户侧验收。

详细说明见 [`apps/tauri/README.md`](apps/tauri/README.md)。

## Windows 采集依赖与分发

Windows 采集使用 vcpkg manifest 显式启用 FFmpeg 的 `avcodec`、`avformat`、`swresample` 与 `swscale` 功能；为兼容仅含 release archive、没有本地 Git 历史的 vcpkg 安装，manifest 不引用 `builtin-baseline`，FFmpeg 版本由开发者使用的 vcpkg registry revision 决定（因此依赖版本尚未锁定）。请使用动态 `x64-windows` triplet（或其他确认采用动态链接的 Windows triplet），不要启用默认功能、GPL 或 nonfree 功能。运行时必须随应用部署 FFmpeg DLL。

FFmpeg 为 LGPLv2.1-or-later。重新分发 FFmpeg DLL 时，必须同时提供与所发二进制完全对应的 FFmpeg 源码、所用补丁/差异及构建说明，并提供 FFmpeg 及相关库的许可和版权声明；源码应与应用二进制一同可获取。应用应保持动态链接和可替换的 FFmpeg DLL，不得以许可条款禁止用户为调试 FFmpeg 修改而进行反向工程；关于窗口、EULA 与下载页也须给出适当的 FFmpeg/LGPL 声明。发布前还须逐项核对 FFmpeg 官方 [LGPL 合规清单](https://ffmpeg.org/legal.html)，不能将本仓库的 vcpkg 配置视为已完成二进制分发合规。

LGPL 合规不代表专利许可。ProRes 与 AAC 相关专利状况取决于司法辖区及分发/使用场景；商业分发前需单独进行法律评估。

## 许可

[MIT](LICENSE)。原项目版权归 神烦老狗 所有；Windows 实现同样以 MIT 发布。
