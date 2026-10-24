# Windows 移植最终方案（开工前定稿）

> 基线 commit `567fd6e`（DogSC 0.1.8）。本文件是三轮复审后的收敛结果，取代 `10-plan-r1.md`。所有论断可回溯到 `00`–`03` 基线文档与源码行号。
>
> **产品定位：独立的 Windows 软件系统，不做 macOS 工程文件互通**（决议见 `16-open-questions.md` D2）。上游仅作为行为参照与算法来源。

## 1. 决策摘要

| 项 | 决定 |
|---|---|
| 语言 | C++20（MSVC `/std:c++20`，禁用 `/fp:fast`） |
| 桌面壳与编辑器 UI | Tauri 2 + HTML/CSS/TypeScript（当前仅录制工作区；完整编辑器未完成） |
| HUD 类窗口 | 当前由 Tauri 窗口承载录制工作区；后续按需采用 Win32 顶层窗口 + Direct2D 自绘（完成窗、权限窗、摄像头预览、区域/窗口/屏幕选择器） |
| 合成 | Direct2D `ID2D1DeviceContext` + 少量 HLSL；预览与导出共用同一 `IFrameCompositor` |
| 采集 | Windows.Graphics.Capture（显示器/窗口/区域）+ WASAPI（loopback/麦克风）+ Media Foundation SinkWriter |
| 导出 | Media Foundation SourceReader → 自研时间线调度 → 合成 → SinkWriter |
| 工程文件 | 自行定义的 `project.json`（沿用 schema 版本号 23…27 以免分叉），**不与 macOS 互通** |
| 自动更新 | 首版关闭，并从启动序列移除（`DogSCApp.swift` 的 `startIfEligible()` 对应位置） |
| 最低系统 | Windows 11 24H2（build 26100+；依据 `16-open-questions.md` D3/D11） |

## 2. 模块与工作量

| 模块 | 移植来源 | 源行数 | 性质 |
|---|---|---|---|
| `core/` | `Sources/RecorderCore/*`（44 文件） | 15,369 | 翻译，纯 `Double` 数学，零苹果框架 |
| `orchestration/` | `AppModel*`、`EditorStore`、`ProjectStore`、`ProjectSessionPersistenceController`、`EditorPlaybackController`、`EditorMediaSession`、`EditorSessionContext` | 9,093 | 重写，状态机/持久化/播放时钟 |
| `render/` | `SharedFrameRenderer`、`SharedFrameCursorRenderer`、`SharedFrameOverlayRenderer`、`ContinuousCornerMask`、`SharedFrameCompositor` | 2,888 | 重写（D2D） |
| `render/present/` | `SharedRenderedPreviewSurface`、`SharedPreview*`、`CoreImageFrameColorProfile` | 2,453 | 重写（DXGI + 背压） |
| `capture/` | `ScreenRecorder`、`ScreenCaptureOutput`、`NativeScreenRecording`、`CameraRecorder`、`MicrophoneRecorder`、`PointerEventRecorder`、`CoreAudioSystemAudioTap` 及选择器/几何/拓扑 | 11,961 | 重写 |
| `export/` | `DirectExportPipeline`、`VideoExporter`、`TimelineCompositionBuilder`、`TimelineAudioGainProcessor`、`AudioOnlyExportPipeline` 及配套 | 2,905 | 重写 |
| `ui/editor/` | `EditorView`、`EditorCanvas*`、`EditorTimeline*`、`EditorInspector*`、`EditorControls` 等 | 29,326 | 重写 |
| `ui/shell/` | `Recorder*`、`AppSettings*`、`AppDialog`、图标、本地化、引导 | 约 22,595（86,590 减去以上各项） | 重写 |

合计源文件 285 个 / 101,959 行。

## 3. 必须逐条对齐的契约

### 3.1 持久化
- `RecorderProject` 字段集合与默认值（`ProjectMediaModel.swift:152-250`）
- schema 版本区间 23…27，双向校验（`ProjectModel.swift:8-32`）
- 所有字段「缺省即默认」，不做强制迁移报错（`ProjectMediaModel.swift:228-263`）
- `zoomAnimations` 是 `timeline.zoomClips` 的别名，不重复写盘（`ProjectMediaModel.swift:170-176`）

### 3.2 时间线
- `RecordingSegment` 无 `outputStart`，输出位置累加推导（`ProjectTimeline.swift:3-43`）
- 变速 1×…100×（`ProjectTimeline.swift:12-14`）
- `TransitionTiming` enter/hold/return 语义（`ProjectTimeline.swift:95-130`）
- `TimelineMap` 的 7 类错误同义（`TimelineMap.swift:3-27`）

### 3.3 帧调度与帧契约
- `OutputFrameSchedule`：90000 时基、整数 tick、端开区间、ulp 吸附（`OutputFrameSchedule.swift:1-80`）
- `FrameScene` 字段与图层顺序：`background → screen → cursor → spotlight → camera → stickers`（`SharedFrameRenderer.swift:135-260`）
- `FrameSceneEvaluator.renderPlan` 是唯一逐帧解释器（`FrameScene.swift:487-560`）

### 3.4 时序（不可事后修复）
- 首帧 host-time 作为所有辅助轨对齐基准（`AppModel.swift:669-710`）
- 停止时冻结共享 stop host-time（`AppModel.swift:737-760`）
- 指针轨先于摄像头/麦克风/屏幕写入启动（`AppModel.swift:565-580`）
- PTS 归一化到 0、非单调帧丢弃、暂停补偿（`ScreenCaptureOutput.swift:430-520`）

### 3.5 编码参数
- 视频码率 `clamp(pixels*fps/6, 12Mbps, 65/100/130Mbps)`（`ExportPolicy.swift:49-67`）
- 音频 AAC 48kHz 立体声 192kbps（`ScreenCaptureOutput.swift:378-390`）
- 采集：HEVC 原生分辨率 + 硬件优先；H.264 限 4K；禁 B 帧（`ScreenCaptureOutput.swift:295-340`）
- 分片 10 秒（`ScreenCaptureOutput.swift:265-270`）

### 3.6 内部标识（不承诺跨端互通）
- `CursorAssetID` 保留 `automatic / systemArrow / touchDot / hidden` 四值以保证本产品语义完整（`ZoomAnimation.swift:46-63`），但不要求与 macOS 逐值对齐
- `BackgroundSource` 的 `projectImage/projectVideo` 相对路径语义沿用（`ProjectModel.swift:364-400`）；`systemImage/systemVideo` 改为 Windows 绝对路径语义
- schema 版本号沿用 23…27 以免自造分叉，但**不承诺**与 macOS 工程文件互认（见 `16-open-questions.md` D2）

## 4. 阶段与并行

```
M0  核心层 + 对拍工具 + 契约定义（无 UI / 无 GPU）
 ├─ M1  采集（WGC / WASAPI / MF writer）        ┐
 ├─ M2a 离屏合成器（D2D，WARP 可跑）            ├─ 并行
 └─ M4a 导出管线（MF reader/sink，无 UI）        ┘
      M2b 实时预览（DXGI + 背压 + 帧调度）
           M3 编辑 UI（画布 / 时间线 / 检查器）
                M4b 导出 UI + 工程互通
                     M5a 设置 / 托盘 / 菜单 / 本地化
                          M5b 引导与权限页

```
### 4.1 当前实现与验证边界（2026-10-24）

- **M0**：核心模型、时间线、JSON、`scene_dump` 已落地。Release clean build 与 6/6 测试通过。
- **M1**：WGC/WASAPI/MF 采集 CLI 已有本机硬件 smoke；HEVC/H.264 可用厂商硬件 MFT，ProRes 使用动态 FFmpeg。短时采集与恢复验证不等于长时 4K60、干净系统 HEVC 可用性、异机同步或发布合规通过。
- **M2a**：D2D/WARP 离屏 `IFrameCompositor` 已实现完整六层合成，并有实际读回像素与失败路径 smoke。尚无固定基准 PNG/PSNR 对拍，因此 §5 的画质阈值未宣告通过。
- **桌面壳**：Tauri v2 中文录制工作区已构建 NSIS 安装包；P1a GUI 已验证项目库枚举/打开/重命名、分割、裁切、变速、主轨重排和多步撤销/重做；P1b 已接入 draft session、去抖自动保存、flush/两次失败重试、discard 和 beforeunload guard；P2a 已接入单帧 SourceReader→BMP data URI，P2b 已把持久 SourceReader stream 接入 Tauri child、30fps 单飞请求和 SourceReader→D2D/WARP 单帧合成；崩溃恢复清单、吸附、DXGI surface、硬背压、丢帧统计和完整预览仍未完成。
- **后续未完成**：崩溃恢复清单、Tauri/DXGI 实时预览与无撕裂播放、Tauri 完整编辑器、SourceReader 时间线解码/导出、完整资源生命周期、长时采集/跨设备矩阵和 FFmpeg 分发材料。
- **ProRes 性能风险已实测**：本机 1 秒录制，720p30 收到 35 帧；1080p30 收到 5 帧；1440p30 收到 3 帧；后两者各重复补帧 25/27 次。当前 CPU 编码路径仅 720p 样本达标，ProRes 不可作为已通过的实时高清录制路径。

以上状态只代表仓库实现与本机测试，不代表完整产品可发布。

## 5. 阶段验收（开工即可执行）

| 阶段 | 验收方式 | 通过标准 |
|---|---|---|
| M0 | 编解码稳定性：`project.json` 读→写→读，字段级对比 | 往返零差异；未知字段按约定忽略 |
| M0 | 场景求值快照：固定 `project.json` + 人造 `pointer.jsonl` → `scene-dump` JSON | 与仓内基准快照逐字段差 ≤ 1e-9 |
| M0 | 剪辑原语单测 | 覆盖 split/trim/move/ripple/retime 的边界与错误类型 |
| M1 | 录 60 秒 4K60 | 帧率 ≥ 58fps、音画偏差 ≤ 1 帧、PTS 单调 |
| M1 | 暂停/恢复 | 恢复后时间轴无跳变，共享 stop 时间一致 |
| M1 | 排除语义回归 | 录制时自身窗口/Dock 不出现在成片中（若按 §4.1-1 采用遮挡方案） |
| M2a | 离屏合成单帧自测 | 固定基准 PNG PSNR ≥ 40dB（无 3D）/ 38dB（含 3D 透视）；当前 WARP 图层像素 smoke 已通过，但缺固定基准图与 PSNR 测量，故未通过该项。 |
| M2b | 预览播放 | 60Hz 下掉帧 < 2%，拖动无撕裂 |
| M3 | 时间线全操作 | 分割/裁切/重排/变速/吸附的几何结果与单测基准一致 |
| M4 | 导出 4K60 5 分钟 | 无丢帧、音画同步、文件可被本产品重新打开 |

## 6. 风险与应对

| # | 风险 | 等级 | 应对 |
|---|---|---|---|
| R1 | D2D 无 CI 惰性 extent，5K 中间纹理爆显存 | 高 | 每个图层显式计算中间纹理尺寸。**D2D `GaussianBlur` 自身就会扩大 extent**：每边外扩 `StandardDeviation × 3 × (DPI/96)`，总尺寸增加 `StandardDeviation × 6 × (DPI/96)`（D11-1）。按此公式分配，配合 `BORDER_MODE` + `SetRenderingControls` 控制精度；显存峰值监控；必要时分块渲染 |
| R2 | MF 无 `AVMutableComposition` | 高 | 以 `TimelineMediaPlan` 为唯一真相；变速/切段单测覆盖边界 |
| R3 | HEVC 编码器可得性不可保证 | 中高 | 软件编码器 `mfh265enc.dll` 来自 Store 包 Microsoft.HEVCVideoExtension，从未安装过的**离线机器**首次初始化会因联网许可失败（`0x800704CF`），即 HEVC 编码可能完全不可用（D11-2）。应对：优先用 `MFTEnumEx`+`MFT_ENUM_FLAG_HARDWARE` 走厂商硬件 MFT（`[INFERENCE]` 不依赖该包，待 M1 实测）；缺失时回退 H.264 并在 UI 声明；安装包给出扩展安装指引 |
| R4 | Tauri 窗口与 Win32 自绘 HUD 混排、DPI 切换 | 中 | 当前录制工作区使用 Tauri 窗口；后续 HUD 若采用 Win32 顶层窗口，使用 Per-Monitor V2 清单（`SetThreadDpiAwarenessContext`），并做混排回归；完整编辑器尚未实现。 |
| R5 | 全局鼠标钩子受 UIPI 限制 | 中 | `WH_MOUSE_LL` + `GetCursorInfo` 轮询兜底；明确不支持提权窗口 |
| R6 | 系统光标/壁纸无等价资源 | 中 | 内置光标包（按共享 ID）；壁纸仅用户自选 |
| R7 | 浮点求值差异 | 中 | 全部 `double`，禁 `fast-math`，对拍工具兜底 |
| R8 | 进程级系统声音不可得 | 低 | 首版只支持整设备 loopback，UI 说明差异 |

## 7. 开工第一批任务（M0）

1. 建仓骨架：CMake + vcpkg（nlohmann/json、DirectXTK 可选），CI 跑 clang-format + 单测。
2. 移植 `OutputFrameSchedule`、`TimelineMap`、`TimelineMediaPlan` 及单测（这三者决定成片帧数与音画同步）。
3. 移植 `ProjectModel` / `ProjectMediaModel` 的 JSON 读写，做「读 → 写 → 读」稳定性测试，并提交一份基准快照。
4. 移植 `FrameSceneEvaluator` + `FrameScene`，输出 JSON 转储工具 `scene-dump`，生成并提交基准快照（后续回归的对照物）。
5. 定义 `CursorAssetID` 内部枚举与 `IFrameCompositor` 接口头文件；检索开源光标素材库（CC0/MIT/Apache-2.0，≥6 类，SVG 或 ≥256px），产出候选清单。
6. 把 `orchestration/` 的三条时序契约写成失败用例（迟到回调、暂停一致性、保存代际），作为后续实现的护栏。
7. 写一份采集层技术验证清单：WGC 排除语义替代方案、MF passthrough 缺失的下限方案、QPC 四路时钟对齐方案（对应 `02-platform-dependency-map.md` §4.1）。

## 8. 明确不做（首版）

iOS 设备录制、系统壁纸自动发现、进程级系统声音、自动更新、HDR 导出、Sparkle 等价机制。

## 9. 待用户确认的开放项

见 `16-open-questions.md`。
