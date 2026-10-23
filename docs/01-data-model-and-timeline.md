# 数据模型 / 时间线 / 场景求值（RecorderCore 基线）

> 本文件描述 Windows 移植必须**逐字段保真**的持久化契约与纯逻辑层。基线 commit `567fd6e`。

## 1. 分层与边界

`RecorderCore` 的 44 个文件（合计 15,369 行）全部只 `import Foundation`（`grep -lE 'import (AppKit|SwiftUI|AVFoundation|CoreImage|Metal|CoreText)' Sources/RecorderCore/*.swift` 无命中），对 `CGPoint/CGRect/CGSize/CGFloat/CGAffineTransform` 的引用为 **0**（自建 `CompositionPoint/CompositionRect/CompositionSize`，见 `CompositionScene.swift:3-27`、`FrameScene.swift:4-14`），因此它是**完全平台无关的纯 `Double` 计算层**，是 Windows 端的移植起点与验收基准。

三层边界：

```
RecorderProject（持久化状态）
  -> FrameSceneEvaluator.renderPlan(...) -> FrameRenderPlan { scene: FrameScene }
     -> 后端无关的不可变帧契约
        -> SharedFrameRenderer（mac 端 Core Image）  <-- Windows 需替换为 D2D/D3D 实现
        -> 媒体资源：CIImage / CMSampleBuffer          <-- Windows 需替换为 D3D 纹理 / IMFSample
```

设计意图写在注释里：渲染器「may read media frames and this value, but must never read `RecorderProject` directly」（`Sources/RecorderCore/FrameScene.swift:384-386`）。

## 2. 持久化根对象

`RecorderProject`（`Sources/RecorderCore/ProjectMediaModel.swift:152-250`）：

| 字段 | 类型 | 说明 |
|---|---|---|
| `version` | Int | schema 版本，写入时统一提升为 `currentVersion` |
| `title` | String | 默认「未命名录制」 |
| `createdAt` | Date | ISO8601 编码 |
| `capture` | `CaptureConfiguration` | 录制源与设备选择 |
| `canvas` | `CanvasStyle` | 画布/背景/裁切/圆角/边框/阴影/样机 |
| `camera` | `CameraStyle` | 摄像头布局与外观 |
| `audio` | `AudioStyle` | 全局音量/静音 |
| `motion` | `MotionStyle` | 弹簧参数、默认缓动 |
| `openingSequence` | `OpeningSequence` | 开场编排 |
| `cursorStyle` | `CursorStyle` | 光标素材、大小、点击特效、倾角 |
| `exportSettings` | `ExportSettings` | 分辨率/帧率 |
| `timeline` | `ProjectTimeline` | 剪辑与动画轨道 |
| `media` | `ProjectMediaManifest?` | 媒体清单（相对路径 + 同步锚点） |
| `recordingMarkers` | `[RecordingMarker]` | 录制期标记 |

- `zoomAnimations` 是**计算属性别名**，不重复持久化（`ProjectMediaModel.swift:170-176`）。
- `ProjectMediaManifest` 要求 `screen` 必填，其余可选；无媒体时为 `nil` 而不是空清单（`ProjectMediaModel.swift:131-149`）。
- 解码策略：全部字段 `decodeIfPresent` + 默认值，并保留 v1/v2 的扁平路径字段做迁移（`ProjectMediaModel.swift:228-263`）。

## 3. Schema 版本

`ProjectSchema.minimumSupportedVersion = 23`、`currentVersion = 27`（`Sources/RecorderCore/ProjectModel.swift:8-13`）。解码/编码双向校验，越界抛 `ProjectSchemaError`（`ProjectModel.swift:15-32`）。

版本演进要点（注释记录）：
- v23：精简契约，移除字幕/素材库视频/进度叠加。
- v24：可自定义画布比例。
- v27：持久化录制标记。

Windows 端**必须**沿用同一版本号与校验语义，否则跨端工程文件不可互认。

## 4. 时间线模型

`ProjectTimeline`（`Sources/RecorderCore/ProjectTimeline.swift:244-312`）：

| 字段 | 类型 |
|---|---|
| `sourceSequence` | `SourceSequence`: `.fullRecording` / `.edited([RecordingSegment])` |
| `primarySegmentAudioOverrides` | `[UUID: PrimarySegmentAudioOverrides]` |
| `zoomClips` | `[ZoomAnimationClip]` |
| `screenMotionClips` | `[ScreenMotionClip]` |
| `cameraMotionClips` | `[CameraMotionClip]` |
| `mosaicClips` | `[MosaicClip]` |
| `stickerClips` | `[StickerClip]` |

`RecordingSegment`：`id, sourceStart, sourceDuration, playbackRate`，**不持久化 outputStart**，输出位置由累加推导（`ProjectTimeline.swift:3-43`）。变速上限 100×（`ProjectTimeline.swift:12-14`）。

`TransitionTiming`：`startTime, duration, leadInDuration, leadInProgressOffset, easing, customCurve, returnDuration`，语义为 enter/hold/return（`ProjectTimeline.swift:95-130`）。所有动画轨道共用该时间契约。

## 5. 剪辑操作

`ProjectTimelineEditing`（935 行）提供纯函数式编辑原语，全部**输入旧项目、输出新项目**，不改原地状态：

| 操作 | 位置 |
|---|---|
| `splitPrimarySegment` | `ProjectTimelineEditing.swift:244` |
| `trimPrimarySegment` | `:291` |
| `movePrimarySegment` | `:465` |
| `insertingScreenMotion` / `insertingCameraMotion` | `:767` / `:838` |

轨道枚举 `ProjectTimelineTrack`：`primaryRecording, zoom, screenMotion, cameraMotion, mosaic, sticker`（`ProjectTimelineEditing.swift:9-16`）。错误枚举细化到「片段重叠/ID 不匹配/越界裁切」等 20 余种（`:18-45`），错误文案为中文（`:48-83`）。

`EditorTimeline`（938 行）提供与 UI 无关的命中测试与几何换算：`fitZoomAnimation`、`manualZoomRangeKeyframes`、`xPosition`、`split`、`moving`、`resizing`、`zoomHitZone`、`motionHitZone`、`fitMotionTiming`、`motionReturnNormalizations` 等（`EditorTimeline.swift:47-873`）。Windows 时间线 UI 应**直接复用这套算法**，只替换坐标与手势层。

## 6. 映射层

`TimelineMap` -> `TimelineMediaPlan` -> `TimelineMediaSlice`：把「源素材区间」映射到「输出时钟」（`Sources/RecorderCore/TimelineMediaPlan.swift:5-83`）。

- `TimelineMediaSlice`：`outputStart, sourceStart, duration, sourceTimeScale`，`sourceTime(atOutputTime:)` 做线性映射并夹紧边界（`:19-35`）。
- 边界容差 `1e-9`，仅用于对齐已授权剪切点，不会吞掉真实媒体间隙（`:42-45`）。
- 辅助轨（摄像头/麦克风）通过与主轨求交得到各自 slice，**间隙保持间隙**（`:47-83`）。

## 7. 帧契约

`FrameScene`（`Sources/RecorderCore/FrameScene.swift:385-420`）：`time, canvasSize, color, background, screen, camera?, cursor?, stickers, stickerBackdrop, layerOrder`。

`FrameRenderPlan`：`presentationTime, outputDuration, frameRate, scene`（`:423-440`）。

`FrameSceneEvaluator.renderPlan(...)` 是**唯一**读取项目的逐帧解释器（`:487-560`）；参数包括 `pointerTrack / zoomTrack / screenMotionTrack / cameraMotionTrack / stickerTrack / cameraTimeline`。`refreshingCursor(...)` 允许预览在视频帧缓存不变时以显示时钟单独刷新光标（`:449-485`）。

`CompositionScene` 提供几何求值：`ScreenSceneEvaluation`（含 `projectedQuad`）、`ScreenFrameGeometry.decorationInsetsAtScaleOne`、`CompositionSceneEvaluator`（`Sources/RecorderCore/CompositionScene.swift:30-141`）。屏幕 3D 投影的纯数学在 `ScreenProjection.project`（`ScreenProjection.swift:279`）。

## 8. 动画算法（纯数值，可直接移植）

| 模块 | 文件/位置 | 算法要点 |
|---|---|---|
| 缩放 | `ZoomAnimation.swift`（943 行） | 关键帧 + 贝塞尔/弹簧缓动，`ZoomInterpolator` 求值（`:641`） |
| 自动运镜 | `AutoZoomPlanner.swift`（651 行） | 由点击事件聚类生成缩放片段 |
| 屏幕运动 | `ScreenMotion.swift`（611 行） | 3D 倾斜/透视/锚点插值 |
| 摄像头运动 | `CameraMotion.swift`（423 行） | 位置/尺寸/圆角过渡 |
| 贴纸 | `StickerTransition.swift`（266 行） | 入场/退场/回弹 |
| 指针 | `PointerTrack.swift`（894 行） | 弹簧跟随、点击相位、速度倾角；`PointerSample` / `PointerTrackEvaluation`（`:23-51`） |
| 聚焦 | `FocusEffect.swift` | 线性/径向遮罩参数 |
| 光标几何 | `CursorRenderGeometry.swift` | 点击特效逐帧几何（`:6-144`） |

所有弹簧参数以 `MotionStyle` 持久化（质量/刚度/阻尼成对出现，`ProjectModel.swift:1248-1290`），默认值 `screen 2.4/210/42`、`cursor 2.8/450/66`（`:1265-1272`），旧默认值有一次性升级逻辑（`AppModel.swift:108-140`）。

## 9. 配置与输出契约

- `CaptureConfiguration`（`Sources/RecorderCore/CaptureConfiguration.swift:159-400`）：录制源、显示/窗口 ID、区域（归一化）、设备 ID、帧率、编码、分辨率上限、隐藏桌面图标/Dock、系统声音范围、麦克风/摄像头。**Windows 需保留字段结构**，`displayID/windowID` 语义改为 Windows 侧的 Monitor/Window 句柄映射。
- `OutputFrameSchedule`（`OutputFrameSchedule.swift:1-80`）：90000 时基、整数 tick、端开区间，帧数由 `duration * fps` 向上取整并做 ulp 级边界吸附。
- `ExportEncodingPolicy.videoBitrate`（`ExportPolicy.swift:49-67`）：`clamp(pixels*fps/6, 12Mbps, 65/100/130Mbps)`。
- `ProjectValidator`（`ProjectValidator.swift:46-179`）：只校验不改写，越界/非有限值直接抛错。

## 10. 可移植性结论

| 分组 | 内容 | 处理方式 |
|---|---|---|
| **天然可移植**（15,369 行 / 44 文件） | 全部 `RecorderCore`：模型、校验、剪辑原语、时间映射、帧契约、动画数学 | 逐文件翻译或在 Windows 端重建同名类型；用同一份 JSON 做金标准对拍 |
| **需替换** | 渲染器、采集、编解码、UI、系统资源（光标/壁纸/字体） | 见 `02-platform-dependency-map.md` |

风险点：Swift 值语义（`struct` + `inout`）与 `Sendable` 约束在目标语言中需要等价表达；`Codable` 的默认/兼容解码策略必须手工复刻，否则旧工程无法打开。
