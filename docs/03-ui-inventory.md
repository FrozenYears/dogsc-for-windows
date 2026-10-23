# UI 结构与迁移特征（现状盘点）

> 基线 commit `567fd6e`。用途：为 Windows 端 UI 技术选型与工作量估算提供事实依据。

## 1. 规模

- `Sources/DogSCApp`：241 个文件、86,590 行；`Sources/RecorderCore`：44 个文件、15,369 行。
- `struct …: View` 声明 236 处；`import SwiftUI` 125 个文件；`import AppKit` 123 个文件；`@MainActor` 308 处；Combine 相关符号 152 处。
- 本地化键 1,385 条（`Resources/zh-Hans.lproj/Localizable.strings`），英文表 1,935 行（`Resources/en.lproj/Localizable.strings`）。

## 2. 窗口所有权（AppKit 命令式）

| 窗口类 | 文件 | 职责 |
|---|---|---|
| `RecorderPanel : NSPanel` | `RecorderPanelController.swift:1004` | 录制条「岛屿」，尺寸随内容弹簧变形、可拖动、浮动层级 |
| `EditorWindowController` | `DogSCApp.swift:644` | 编辑器主窗（`fullSizeContentView`、自绘标题栏、窗口 frame 自动保存） |
| `RecordingCompletionPanel : NSPanel` | `RecordingCompletionWindow.swift:290` | 录制完成决策卡 |
| `RequiredPermissionWindowController` | `RecorderPermissionView.swift:155` | 权限门禁整窗页面 + 拖拽助手 |
| `RecorderMemoPanel : NSPanel` | `RecorderMemo.swift:189` | 悬浮提词备忘录（不进画面） |
| `AppSettingsWindowController` | `AppSettingsWindowController.swift:9` | 设置窗口（高度动画） |
| `CameraPreviewWindowController` | `CameraPreviewWindow.swift:144` | 摄像头预览浮窗 |
| `CaptureAreaSelectionWindow` / `CaptureAreaRecordingOverlayWindow` | `CaptureAreaSelection.swift:206,212` | 区域框选与录制中遮罩 |
| `DisplaySelectionPanel` / `WindowSelectionPanel` / `IOSDeviceSelectionPanel` | `CaptureDisplaySelection.swift:174`、`CaptureWindowSelection.swift:549`、`CaptureDeviceSelection.swift:181` | 屏幕 / 窗口 / 设备选择覆盖层 |
| `AppDialogPanel` / `RecorderConfigurationPanel` / `EditorMarkerPanel` / `FirstUseTourPanel` / `IntroductionPanel` | 见各文件 | 对话框、配置弹层、标记列表、教学遮罩、启动动画 |

窗口层级策略集中在一处：`CaptureWindowPresentationPolicy.swift:15-30`（overlay = floating+1、activeRecorder = floating+2）。

SwiftUI 通过 `NSHostingView` / `NSHostingController` 嵌入（28 处），例如 `RecorderPanelInteraction.swift:34`、`RecorderPermissionView.swift:173`。

## 3. 状态与撤销

- `AppModel : ObservableObject`（`AppModel.swift:141`）：相位、录制 URL、设备清单、权限、错误提示等 40+ `@Published`。
- `EditorStore`（1,423 行）：唯一项目写入入口，所有编辑走 `ProjectCommand` 纯函数；交互期间用 `beginInteraction/updateInteraction/commitInteraction` 分组（`EditorStore.swift:796-940`）。
- 撤销：单一 `UndoManager`，`attachUndoManager`（`:782`）注册快照，`registerUndo` + `setActionName`（`:1374-1381`）。由于模型是值类型快照，撤销实现极简。
- `EditorSessionContext`（17.3KB）聚合 store / mediaSession / playbackController / 资源解析器。

## 4. 本地化

- `AppLocalization`（`AppLocalization.swift:1-66`）：按语言缓存 `Bundle`，`localizedString` 加锁查表；语言切换通过 `ObservationRegistrar.withMutation` 触发所有读取点刷新。
- 统一入口 `appLocalized(_:)`（`AppPreferences.swift:9-11`），错误文案再由 `appErrorDescription` 映射（`:15-63`）。
- 原生菜单/窗口标题另有 `AppWindowLocalization` 与 `AppLanguageBinding`（`AppLocalization.swift:69-120`）。

## 5. 设计系统

- 颜色/间距：`EditorDesignTokens.swift`（`EditorTheme`，明暗双值 `NSColor` provider，303 处 `NSColor` 使用）。
- 字体：阿里巴巴普惠体 3 档 OTF，`CTFontManagerRegisterFontsForURL` 进程内注册（`AppTypography.swift:13-38`）。
- 图标：`AppLineIcon` 自绘 60+ 种线性图标（统一 18pt 网格、圆头圆角，`AppLineIcon.swift:5-40`）；其余用 SF Symbols。
- 动效：`RecorderMotion`（弹簧/淡入淡出）与 `SpringMotion`，大量依赖 SwiftUI `withAnimation(completionCriteria:)`（macOS 14+）。

## 6. 编辑器布局

三段式：顶部自绘工具栏（46pt，`EditorChromeMetrics.titleBarHeight`）-> 中部画布 + 右侧检查器（340–480pt 可拖）-> 底部时间线（124–280pt 可拖，双击复位）。

- 检查器 6 个页签：画面 / 开场 / 运镜 / 光标 / 摄像头 / 声音（`RecorderViews.swift:422-440`）。
- 时间线轨道：主录屏、缩放、屏幕运动、摄像运动、马赛克、贴图、摄像头同步（`EditorTimelineTrackVisibility.swift`）。
- 快捷键表集中在 `EditorShortcutCheatsheet.swift:20-45`（空格播放、S/Q/W 剪辑、⌘C/⌘V/⌘⇧V、⌘Z/⌘⇧Z、⌘O/⌘E、⌥X、⌥⌘I、Esc）。

## 7. UI 代码分区（行数依据：对应文件 `wc -l` 汇总）

| 区域 | 代表文件 | 行数 |
|---|---|---|
| 应用外壳/状态 | `DogSCApp.swift`、`AppModel*.swift`、`EditorSessionContext`、`EditorStore`、`EditorView`、`EditorWorkspace*` | 7,972 |
| 时间线 | `EditorTimeline*.swift`（20 个文件） | 10,016 |
| 检查器 | `EditorInspector*.swift`、`MotionInspectorViews.swift` | 7,322 |
| 画布与预览 | `EditorCanvas*.swift`、`EditorPlayback*.swift`、`Shared*.swift` | 12,280 |
| 控件 | `EditorControls`、`EditorBarSlider`、`EditorKnob*`、`Editor*Pad` | 2,774 |
| 录制链路（含采集） | `Recorder*`、`Recording*`、`Capture*`、`Camera*`、`Microphone*`、`Screen*`、`Native*`、`CoreAudio*`、`Pointer*` | 21,542 |
| 导出 | `EditorExport*`、`Export*`、`VideoExporter`、`DirectExportPipeline`、`Timeline*`、`MediaAnalysis` | 4,558 |
| 设置/引导/图标/本地化 | `AppSettings*`、`AppDialog`、`AppPreferences`、`AppLocalization`、`AppTypography`、`AppLineIcon`、`First*`、`Permission*`、`App*` | 13,565 |

## 8. 迁移特征判断

| 类别 | 现状 | 迁移含义 |
|---|---|---|
| **声明式视图**（约 236 个 View） | 纯 SwiftUI 布局、状态绑定、动画 | 需整体重写为目标框架的声明式或即时模式 UI；布局语义（`Spacer`/`GeometryReader`/`matchedGeometryEffect`）需逐处等价 |
| **自绘交互层** | 画布叠加拖拽、裁切手柄、时间线拖拽/吸附/框选（`EditorCanvasOverlayInteraction.swift` 36KB、`EditorTimelineInteraction.swift` 32KB） | 手势逻辑与数学在 `RecorderCore` 已有，可复用；命中测试与坐标需重写 |
| **AppKit 窗口语义** | 浮动层级、无边框、`ignoresMouseEvents`、跨 Space、全屏辅助 | 需 Win32 顶层窗口 + 自管 Z 序；多显示器与 DPI 语义不同 |
| **事件监视** | 22 处 `NSEvent` 本地/全局监视（时间线缩放、滚轮、修饰键、删除键） | 需全局/局部钩子或框架级输入事件 |
| **自绘图标系统** | 60+ 矢量路径 | 需重绘为 SVG/Path 资源，工作量确定但量大 |
| **字体** | 阿里巴巴普惠体 OTF | 可直接随包分发，WPF/WinUI3 均可加载 |
| **本地化** | 1,385 键 × 2 语言 | 迁移为资源表；`appLocalized` 调用点需机械替换 |
| **菜单/快捷键** | `NSMenu` + `keyEquivalent` | 改为窗口菜单 + 加速键表；macOS 的 ⌘ 需映射 Ctrl |

**结论**：UI 层不存在「一键移植」路径。可复用资产是**布局常量、设计 token、图标路径、本地化文案、交互数学**；必须重写的是**框架绑定与窗口/输入语义**。
