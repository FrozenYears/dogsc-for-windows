# 自我复审·第 1 轮：技术路线可行性

> 复审对象：`10-plan-r1.md`、`11-architecture.md`。复审方式：回到源码逐条验证断言。基线 `567fd6e`。

## A. 断言验证结果

| # | 原断言 | 复核证据 | 结论 |
|---|---|---|---|
| A1 | 渲染器无自定义 Metal kernel | `grep -rn 'CIColorKernel\|CIWarpKernel\|CIKernel\|MTLFunction\|makeFunction' Sources` = 0 命中 | ✅ 成立 |
| A2 | 预览与导出共用同一合成器 | `SharedFrameCompositor.composite` 仅两处调用：`DirectExportPipeline.swift:634`、`SharedPreviewFramePipeline.swift:513` | ✅ 成立 |
| A3 | `RecorderCore` 零平台依赖 | `RecorderCore` 内无 `import AppKit/SwiftUI/AVFoundation/CoreImage/Metal/CoreText`；`CGPoint/CGRect/CGSize/CGFloat` 命中 **0** 次 | ✅ 成立，且比原文档更强 |
| A4 | 展开自绘、无 `AVVideoComposition` | `TimelineCompositionBuilder.swift:191` 的 `primaryVideoComposition` 恒为 nil；`AVVideoComposition` 仅用于预览播放路径（`EditorCanvasPreviewSupport.swift:180-196`） | ✅ 成立 |
| A5 | 渲染器只依赖 14 种 CI 滤镜 | 实际统计：`CIBlendWithMask` 16、`CIGaussianBlur` 8、`CIColorMatrix` 4、`CISourceOutCompositing` 3、`CIRadialGradient` 3、`CILinearGradient` 3、`CILanczosScaleTransform` 2、`CIRoundedRectangleGenerator`/`CIPerspectiveTransformWithExtent`/`CIMinimumCompositing`/`CIMaskToAlpha`/`CIGaussianGradient`/`CIAffineTile`/`ninePartStretched` 各 1 | ✅ 成立 |
| A6 | 渲染核心约 2,900 行 | `SharedFrameRenderer` 2128 + `SharedFrameCursorRenderer` 282 + `SharedFrameOverlayRenderer` 232 + `ContinuousCornerMask` 218 + `SharedFrameCompositor` 28 = **2,888** | ✅ 成立 |

## B. 第一轮发现的问题

### 问题 1（严重）：`RecorderCore` 的「零 CoreGraphics」被低估，反而说明它更可移植——但当前文档把它写成「几何类型需替换」

`02-platform-dependency-map.md` §1 用全仓 `CGFloat` 945 次来论证几何类型是公共词汇，但 `RecorderCore` 内部命中为 0——它用的是自定义 `CompositionPoint/CompositionRect/CompositionSize/NormalizedPoint`（`CompositionScene.swift:3-28`、`ZoomAnimation.swift:3-18`）。

**修正**：把「几何类型替换」从 `RecorderCore` 的迁移风险里剔除，只在 `DogSCApp` 的渲染/UI 层保留该风险。核心层实际是纯 `Double` 数学。

### 问题 2（严重）：C++20 + WinUI 3 的组合与「录制条岛屿」需求存在冲突未被识别

原计划 R4 只说「WinUI 3 浮动窗层级不如 NSPanel」，但源码显示录制条不只是层级问题：

- 面板必须 `borderless + nonactivatingPanel`（`RecorderPanelController.swift:598`、`:719`），即点击不抢焦点；
- 必须随内容做弹簧变形，且用临时画布容纳 overshoot（`RecorderIslandLayout.swift:13-30`）；
- 阴影画布需 `ignoresMouseEvents` 但岛屿本体要接点击，靠全局/本地鼠标监视在按下前切换（`RecorderPanelInteraction.swift:47-80`）。

WinUI 3 的 XAML Island 无法同时做到「不激活」与「自绘弹簧画布」。

**修正**：录制条、完成窗、权限窗、摄像头预览窗、备忘录窗全部走 **Win32 顶层窗口 + Direct2D 自绘内容**，不进 XAML 树；只有编辑器主窗使用 WinUI 3。这条要写进架构约束，而不是当作风险备注。

### 问题 3（中）：导出链路体量被低估

原文档把导出写作「约 2,272 行」（`TimelineCompositionBuilder` + `DirectExportPipeline` + `VideoExporter`），实际含配套文件为 **2,905 行**（含 `TimelineAudioGainProcessor` 195、`ExportFileTransaction` 33、`ExportStatus` 85、`ExportProgressRelay` 85、`ExportActivityWatchdog` 54、`ExportLoopingWallpaperVideoReader` 92、`AudioOnlyExportPipeline` 89）。

**修正**：里程碑里导出阶段按 2,900 行 / 10 个文件估算，并单列「音频分段增益」与「看门狗/进度」两个子项。

### 问题 4（中）：未识别「编排层」的独立复杂度

`AppModel`（1,091）+ `AppModel+CaptureLifecycle`（924）+ `AppModel+ProjectCommands`（1,243）+ `EditorStore`（1,423）+ `ProjectStore`（1,401）+ `ProjectSessionPersistenceController`（454）+ `EditorPlaybackController`（1,358）+ `EditorMediaSession`（759）+ `EditorSessionContext`（440）= **9,093 行**。

这部分不是 UI，也不是采集，而是**状态机 + 持久化 + 播放时钟**，必须在架构里作为独立模块 `orchestration/` 出现，否则会被误并进 UI 估算。

### 问题 5（低）：`RecorderCore` 实际行数

`wc -l Sources/RecorderCore/*.swift` = **15,369**，而 `00-project-overview.md` 写「约 24k」。原数字来自 `find` 统计口径错误（把部分行重复计入）。**修正**为 15,369；`DogSCApp` 为 86,590，合计 101,959 与总量一致。

### 问题 6（低）：`020` 文档里「Sparkle 自动更新」被列为 B 级，但它其实是 App 启动路径的一部分

`DogSCApp.swift` 在 `applicationDidFinishLaunching` 里调用 `AppUpdateController.shared.startIfEligible()`。首版关闭自动更新时，必须同时改启动序列，不能只是「不打包 Sparkle」。

## C. 第一轮结论

技术路线成立（C++20 + D2D + WGC + MF），但架构文档需要三处结构性修正：

1. 新增 `orchestration/` 模块（9,093 行）；
2. 明确 HUD 类窗口走 Win32 自绘、不走 XAML；
3. 修正 `RecorderCore` 行数与「几何类型风险」的归属。

下一轮复审针对**进度可行性与验收标准**。
