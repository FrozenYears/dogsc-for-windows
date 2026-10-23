# 自我复审·第 2 轮：进度可行性、粒度与验收标准

> 复审对象：`11-architecture.md` 的里程碑与模块划分。方法：用逐文件行数校准工作量，用平台调用点数校准风险密度。基线 `567fd6e`。

## A. 工作量校准（实测行数）

| 模块 | 组成（行数） | 合计 |
|---|---|---|
| 核心逻辑 | `RecorderCore/*` 全部 44 文件 | 15,369 |
| 编排层 | `AppModel` 1,091 + `AppModel+CaptureLifecycle` 924 + `AppModel+ProjectCommands` 1,243 + `EditorStore` 1,423 + `ProjectStore` 1,401 + `ProjectSessionPersistenceController` 454 + `EditorPlaybackController` 1,358 + `EditorMediaSession` 759 + `EditorSessionContext` 440 | 9,093 |
| 渲染 + 预览呈现 | `Shared*`（含 `SharedFrameRenderer` 2,128、`SharedRenderedPreviewSurface` 1,262）+ `CoreImageFrameColorProfile` | 5,341 |
| 导出管线 | `DirectExportPipeline` 867 + `VideoExporter` 671 + `TimelineCompositionBuilder` 734 + `TimelineAudioGainProcessor` 195 + `AudioOnlyExportPipeline` 89 + `ExportFileTransaction` 33 + `ExportStatus` 85 + `ExportProgressRelay` 85 + `ExportActivityWatchdog` 54 + `ExportLoopingWallpaperVideoReader` 92 | 2,905 |
| 采集链路 | `ScreenRecorder` 1,139 + `ScreenCaptureOutput` 1,051 + `NativeScreenRecording` 1,079 + `CameraRecorder` 1,185 + `MicrophoneRecorder` 880 + `PointerEventRecorder` 620 + `CoreAudioSystemAudioTap` 385 + 选择器/几何/拓扑/最终化等 | 11,961 |
| UI 主体（编辑器/录制条/设置） | `EditorView` + `EditorCanvas*` + `EditorTimeline*` + `EditorInspector*` + `Recorder*` + `AppSettings*` | 29,326 |
| 其余（图标/本地化/对话框/引导/工具） | `DogSCApp` 总额 86,590 减去以上各项 | 22,595 |

## B. 第二轮发现的问题

### 问题 7（严重）：里程碑粒度不足，M2「渲染」实际包含两件不同风险的事

原计划把 D2D 合成器（5,341 行中的约 2,900 行渲染核心）与 DXGI 呈现层（约 2,100 行 `SharedRenderedPreviewSurface` + `SharedPreview*`）放在同一阶段。两者失败模式完全不同：前者是数学/滤镜保真，后者是时序/呈现。

**修正**：拆成 M2a（离屏合成器 + 单帧对拍）与 M2b（实时预览呈现 + 背压策略）。M2a 可以完全用离线单帧验证，不需要窗口系统。

### 问题 8（严重）：`orchestration/` 缺独立验收标准

9,093 行的编排层如果只写「翻译状态机」无法验收。源码给出了可验证契约：

- `RecordingRunState` 的代际令牌：旧 run 的迟到回调必须无法改动新 run（`RecordingRun.swift:187-224`）；
- 暂停时所有轨道共享同一 stop host-time（`AppModel.swift:737-760`）；
- 自动保存去抖 + 按 epoch 作废（`ProjectSessionPersistenceController.swift:109-350`）。

**修正**：为 `orchestration/` 写三条可执行的验收用例（迟到回调、暂停一致性、保存代际），在 M0 阶段就建立。

### 问题 9（中）：平台调用点分布与「风险等级」不一致

实测调用点：`AVAssetWriter` 37、`AVCaptureDevice` 36、`Stream`(SCStream) 25、`AVPlayerItemVideoOutput` 23、`AVAssetReader` 22、`NSEvent` 本地监视 20、`NSScreen` 76、`NSWindow` 158。

`NSScreen`/`NSWindow` 的调用点远多于采集 API，但原风险表把 UI 排在采集之后。

**修正**：把「窗口与显示器拓扑」单列为中等风险项，并明确 Windows 侧需要一层 `MonitorTopology` 抽象承接 `NSScreen` 的 76 个调用点（含 DPI 与工作区）。

### 问题 10（中）：验收缺少「数值对拍」的具体做法

原文档只写「金标准对拍」。需要落到可执行形式：用同一 `project.json` + 同一组人造 `pointer.jsonl`，在两端分别输出 `FrameScene` 的 JSON 转储（字段级），逐字段比较容差 1e-9；再对导出帧做 PSNR/SSIM 阈值检查。

**修正**：新增 `15-acceptance.md`，定义两份对拍工具与容差。

### 问题 11（低）：M5 的「外壳与发布」被压得过扁

权限引导、首次使用教学、设置五页、托盘、菜单、本地化 1,385 键、60+ 自绘图标——这些合计落在「其余 22,595 行」里，不是收尾工作。

**修正**：M5 拆为 M5a（设置 + 托盘 + 菜单 + 本地化）与 M5b（引导与权限页）。

## C. 结论

阶段划分修正为：M0 核心与对拍工具 → M1 采集 → M2a 离屏合成 → M2b 实时预览 → M3 编辑 UI → M4 导出与工程互通 → M5a 外壳 → M5b 引导。

下一轮复审针对**风险与依赖顺序**（哪些必须先行、哪些可并行、哪些应延后）。
