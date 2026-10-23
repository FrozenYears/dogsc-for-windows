# DogSC 项目基线文档（macOS 现状）

> 本文件是 Windows 移植工作的**事实基线**。所有结论均带 `file:line` 证据，随源码同步维护。基线 commit `567fd6e`（Release 0.1.8）。

## 1. 项目定位

DogSC 是 macOS 原生录屏 + 轻量剪辑工具：录制显示器/窗口/区域/iPhone，同时录摄像头、麦克风、系统声音；编辑器提供分割/裁剪/重排/变速、自动与手动运镜（缩放）、3D 倾斜透视、聚焦虚化、光标与点击特效、摄像头布局、背景/圆角/阴影/样机外框、贴图与场景预设；导出 MP4/M4A（原始/1080p/1440p/4K，30/60fps）。

- 许可：MIT（`LICENSE`）
- 本地化：简体中文（源语言）+ English（`Resources/zh-Hans.lproj`、`Resources/en.lproj`）
- 平台门槛：macOS 14+ / Apple Silicon（`.macOS(.v14)`，`Package.swift:6`）

## 2. 代码体量

| 目标 | 文件数 | 行数 | 说明 |
|---|---|---|---|
| `Sources/RecorderCore` | 44 | 约 24k | 纯数据模型 + 时间线引擎 + 场景求值 |
| `Sources/DogSCApp` | 241 | 约 78k | AppKit/SwiftUI UI + 采集 + 渲染 + 导出 |
| 合计 | 285 | 101,959 | `find Sources -name '*.swift' \| xargs wc -l` |

最大的 10 个文件（行数）：`SharedFrameRenderer.swift` 2128、`EditorInspectorView.swift` 1704、`EditorView.swift` 1519、`EditorCanvasPreview.swift` 1507、`EditorTimelineLayout.swift` 1481、`EditorControls.swift` 1466、`EditorStore.swift` 1423、`ProjectStore.swift` 1401、`EditorInspectorMediaControls.swift` 1388、`EditorPlaybackController.swift` 1358。

导入统计（`grep -rhoE '^import [A-Za-z_]+' Sources | sort | uniq -c`）：Foundation 141、SwiftUI 125、RecorderCore 125、AppKit 123、AVFoundation 42、QuartzCore 29、CoreImage 23、Combine 23、os 13、ScreenCaptureKit 12、CoreMedia 12、CoreGraphics 11、OSLog 9、CoreText 9、UniformTypeIdentifiers 8、VideoToolbox 7、Metal 7、CoreVideo 5、AVKit 5、AudioToolbox 4。

## 3. 依赖与构建

- SwiftPM，Swift 6 语言模式（`Package.swift:36`）；唯一外部依赖 Sparkle 2.9.4（自动更新，`Package.swift:13-16`、`Package.resolved`）。
- 产物：`.library(RecorderCore)` + `.executable(DogSC)`（`Package.swift:7-10`）。
- 打包靠脚本 `build-app.sh` 手工组装 `.app`/`.dmg`（`README.md` 章节「从源码构建」），用 Apple Development 证书签名。
- CI：`.github/workflows/release.yml`，`macos-26` runner，tag 触发，校验 `Info.plist` 版本后上传 Release。
- App 身份：`cn.laogou.dogsc`（开发版 `cn.laogou.dogsc.dev`），文档类型 `cn.laogou.dogsc-project`，扩展名 `.dogscproject` / `.silkyproject`（`Resources/Info.plist`）。

## 4. 模块地图

```
DogSCApp.swift (@main)
 |- DogSCApplicationDelegate   菜单栏、状态项、Finder 打开、快捷键、更新
 |- WindowCoordinator          窗口所有权：录制条岛屿 / 编辑器 / 完成窗 / 权限向导 / 备忘录
 |- AppModel (ObservableObject) 相位机 AppPhase + 会话 + 偏好 + 设备
 |   |- CaptureSetupController 录制源选择、就绪度、选择器呈现
 |   |- ScreenRecorder / CaptureOutput / NativeScreenRecording
 |   |- MicrophoneRecorder / CameraRecorder / CoreAudioSystemAudioTap
 |   |- PointerEventRecorder   CGEventTap + NSEvent 回退
 |   |- ProjectStore / ProjectSessionPersistenceController  项目包读写与自动保存
 |   \- EditorSessionContext -> EditorStore -> EditorView
 |        |- EditorCanvasPreview + SharedRenderedPreviewView (Metal)
 |        |- EditorTimeline*（布局/交互/缩放/轨道）
 |        |- EditorInspector*（画布/相机/光标/声音/运镜）
 |        \- VideoExporter -> DirectExportPipeline / AudioOnlyExportPipeline
 \- RecorderCore（纯逻辑）
      ProjectModel / ProjectMediaModel / ProjectTimeline / ProjectTimelineEditing*
      EditorTimeline / TimelineMap / TimelineMediaPlan
      FrameScene / CompositionScene / ScreenProjection
      ZoomAnimation / ScreenMotion / CameraMotion / StickerTransition / AutoZoomPlanner / PointerTrack
      CaptureConfiguration / ExportPolicy / OutputFrameSchedule / ProjectValidator
```

证据：`Sources/DogSCApp/DogSCApp.swift:8-70`、`DogSCApp.swift:300-560`（WindowCoordinator）、`Sources/DogSCApp/AppModel.swift:141-200`。

## 5. 应用状态机

`AppPhase`: `setup -> preparing -> recording -> finishing -> recordingComplete -> editor`（`Sources/DogSCApp/AppModel.swift:11-18`）。

每次录制由 `RecordingRunState` 持有代际令牌 `RecordingRunID`，迟到的完成回调无法污染新一次录制（`Sources/DogSCApp/RecordingRun.swift:187-224`）。所有窗口显隐由 `WindowCoordinator.apply(phase:model:)` 统一决策（`DogSCApp.swift:390-460`）。

## 6. 项目包（磁盘契约）

目录布局（`Sources/DogSCApp/ProjectStore.swift:7-27`）：

```
<Name>.dogscproject/          目录包（非 zip），后缀由 Finder 文档类型声明
|- project.json               RecorderProject，ISO8601 日期、prettyPrinted+sortedKeys
|- project.previous.json      上一份已知良好版本（每次提交前保留）
|- project-v<N>-before-migration.json   迁移前原始字节备份
|- .project-save-<UUID>.json.tmp        暂存文件（原子替换用）
|- media/
|   |- screen-0001.mp4        主录屏（H.264/HEVC=mp4，ProRes=mov）
|   |- device-0001.mov        iOS 设备
|   |- camera-0001.mov
|   \- microphone.m4a
|- events/pointer.jsonl      光标轨迹（1 MiB 缓冲批量写）
|- assets/<name>              贴图 / 背景图 / 背景视频的项目副本
|- recovery/manifest.json     录制状态/文件/时长/诊断
\- recovery/recording-performance.jsonl  性能诊断流
```

引用路径一律为**包内相对路径字符串**，解析时做标准化 + 前缀逃逸校验（`ProjectStore.swift:1033-1043`）；Windows 端可原样复用该规则。

- 写入采用「临时文件 -> 原子替换 + 保留上一版」：`stageProject` / `commitStagedProject`（`ProjectStore.swift:682-720`）。
- 自动保存由 `ProjectSessionPersistenceController` 去抖串行执行（`ProjectSessionPersistenceController.swift:109-350`）。
- 默认保存位置 `~/Movies/DogSC/项目`（`ProjectStore.swift:585-594`），可在设置里改。

## 7. 持久化 schema

`RecorderProject`（`Sources/RecorderCore/ProjectMediaModel.swift:152-250`）字段：`version, title, createdAt, capture, canvas, camera, audio, motion, openingSequence, cursorStyle, exportSettings, timeline, media, recordingMarkers`。

`ProjectSchema.minimumSupportedVersion = 23`、`currentVersion = 27`（`Sources/RecorderCore/ProjectModel.swift:8-13`）；解码走「字段缺省 + 旧字段迁移」，越界版本显式报错（`ProjectModel.swift:15-32`、`ProjectMediaModel.swift:228-258`）。

`ProjectTimeline`（`Sources/RecorderCore/ProjectTimeline.swift:244-312`）：`sourceSequence`（fullRecording / edited[RecordingSegment]）、`primarySegmentAudioOverrides`、`zoomClips`、`screenMotionClips`、`cameraMotionClips`、`mosaicClips`、`stickerClips`。

关键设计：**不持久化 outputStart**，输出位置由片段时长累加推导（`ProjectTimeline.swift:3-14`），避免剪切与位置两份真相冲突。

## 8. 采集

| 轨道 | 实现 | 关键点 |
|---|---|---|
| 屏幕 | ScreenCaptureKit `SCStream` + `SCRecordingOutput`（macOS 15+ 原生写）或自定义 `AVAssetWriter` | NV12 video-range、sRGB、`.SDR`、`showsCursor = false`、VFR 保留真实 PTS（`ScreenRecorder.swift:186-300`） |
| 系统声音 | CoreAudio 进程 Tap `CATapDescription`（macOS 14.2+），旧系统回退 SCK 音频流 | 独立 m4a，与视频共享 host clock 以便对齐（`CoreAudioSystemAudioTap.swift:59-110`） |
| 麦克风 | `AVCaptureSession` + `AVCaptureAudioDataOutput` -> AAC | 首样本对齐、暂停补偿（`MicrophoneRecorder.swift:1-120`） |
| 摄像头 | `AVCaptureSession`（可选 `AVCaptureMovieFileOutput` 或逐帧写） | YUV 420 双平面、丢晚帧、镜像（`CameraRecorder.swift:1-90`） |
| 光标 | `CGEvent.tapCreate`（listenOnly）+ NSEvent 全局/本地监视回退 | 记录 `pointer.jsonl`，录制画面**不含**光标（`PointerEventRecorder.swift:380-440`、`:481-619`） |

采集编码参数：HEVC 原生分辨率 + 强制硬件 + `ConstantQualityFactor 0.90`；H.264 限 4K；ProRes 422 走 `.mov`；分片 10 秒；禁 B 帧（`ScreenCaptureOutput.swift:250-400`）。

## 9. 渲染

**唯一渲染器**：`FrameScene`（不可变帧契约）-> `SharedFrameRenderer.render`（Core Image 图）-> 预览或导出。预览与导出共用同一 `FrameSceneEvaluator.renderPlan`（`Sources/RecorderCore/FrameScene.swift:385-560`、`SharedFrameCompositor.swift:8-22`）。

- 图层顺序：`background -> screen -> cursor -> spotlight -> camera -> stickers`（`SharedFrameRenderer.swift:135-260`）。
- 全部效果为 Core Image 内置滤镜（`CIGaussianBlur`、`CIBlendWithMask`、`CIColorMatrix`、`CISourceOutCompositing`、`CILinearGradient`、`CIRadialGradient`、`CIPerspectiveTransformWithExtent`、`CIAffineTile` 等），**无自定义 Metal kernel**（全仓无 `CIColorKernel`/`CIWarpKernel`/`MTLFunction`）。
- 3D 透视：自写 8x8 单应矩阵求解（`SharedFrameRenderer.swift:1641-1720`），屏幕像素只采样一次（`directPerspectiveTransform`，`:1400-1425`）。
- 连续圆角遮罩用 IOSurface 覆盖图 + 九宫格拉伸（`ContinuousCornerMask.swift:72-174`）。
- 色彩合同：linear sRGB 工作空间 -> sRGB 输出，仅支持 SDR（`CoreImageFrameColorProfile.swift:32-61`）。

## 10. 导出

- `AVAssetWriter` + `AVAssetReader` 双队列（视频/音频）流水线（`DirectExportPipeline.swift:243-560`）：H.264 在长边 <= 4096 时使用，否则 HEVC；码率 `clamp(pixels*fps/6, 12Mbps, 65/100/130Mbps)`（`ExportPolicy.swift:44-67`）；AAC 192kbps。
- 时间线拼接 `AVMutableComposition.insertTimeRange` + `scaleTimeRange`（`TimelineCompositionBuilder.swift:104-207`），视频画面**完全自绘**（`primaryVideoComposition = nil`，`:191`）。
- 音频混音 `AVMutableAudioMix` + `MTAudioProcessingTap` 分段增益（`TimelineAudioGainProcessor.swift:21-79`）。
- 帧调度 `OutputFrameSchedule`（90000 时基、端开区间、整数 tick）（`OutputFrameSchedule.swift:1-80`）。
- 原子落盘：临时文件 + 替换（`ExportFileTransaction.swift:14-31`）。

## 11. UI 架构

- AppKit 持有窗口与菜单（`DogSCApplicationDelegate`、`EditorWindowController`、`RecorderPanelController`、`RecorderMemoController`），SwiftUI 负责内容（236 个 `: View` 声明，123 个文件 `import AppKit`）。
- 状态：`AppModel` / `EditorStore` / `EditorSessionContext`（`ObservableObject` + Combine，152 处 Combine 符号）；撤销为单一 `UndoManager`（`EditorStore.swift:782-800`、`EditorInspectorTransactions.swift`）。
- 录制条是「浮动岛屿」面板，按内容尺寸做弹簧变形，可拖动（`DogSCApp.swift:100-260`）。
- 设计系统：`EditorDesignTokens.swift` + 阿里巴巴普惠体 OTF（`AppTypography.swift:13-38`，`CTFontManagerRegisterFontsForURL`）+ 自绘线性图标（`AppLineIcon.swift`）。
- 本地化：`.strings` + `Bundle` 查表 + `Observation` 触发的运行期切换（`AppLocalization.swift:1-66`），`appLocalized(_:)` 是唯一入口（`AppPreferences.swift:9-11`）。

## 12. 权限模型

| 权限 | 检查 API | 用途 |
|---|---|---|
| 屏幕录制 | `CGPreflightScreenCaptureAccess()` / `CGRequestScreenCaptureAccess()` | 采集 |
| 辅助功能 | `AXIsProcessTrusted()` | 全局光标事件 |
| 麦克风 / 摄像头 | `AVCaptureDevice.requestAccess(for:)` | 音频 / 相机 |

门禁逻辑在 `CaptureReadiness` + `RecorderPermissionView`：未授权时以整窗页面阻塞录制，并引导拖拽 App 到系统设置（`RecorderPermissionView.swift:50-150`、`:200-340`）。
