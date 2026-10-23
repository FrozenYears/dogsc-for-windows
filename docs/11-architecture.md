# Windows 移植架构规划（v1）

> 基于 `00`–`03` 基线文档与源码核查。基线 commit `567fd6e`。

## 1. 总原则

1. **模型先行、渲染后置**：`RecorderCore` 15,369 行零平台依赖，先把它完整搬到 Windows 并做到 JSON 双向互通，再动采集与渲染。
2. **两端同源**：不 fork 产品设计，只 fork 实现。工程文件（`.dogscproject`）与时间线语义保持字节级兼容。
3. **能力裁剪显式化**：无系统等价物的功能（系统光标库、系统壁纸库、iOS 设备录制、进程级系统声音）在 UI 上明确降级，不做静默伪实现。
4. **单一合成器**：预览与导出必须共用同一个合成实现，这是 macOS 端成立的前提（`SharedFrameCompositor.swift:8-22`），Windows 端不得拆分。

## 2. 技术栈决策

| 层 | 选型 | 备选 | 决策依据 |
|---|---|---|---|
| 语言 | C++20 | Rust、C# | 需要直接调用 Win32 / D3D11 / D2D / Media Foundation；Swift 的 `struct` 值语义与 `inout` 在 C++ 中可用引用/值类型直接对应；无 GC 停顿，适合 60fps 实时预览 |
| UI | WinUI 3 (C++/WinRT) | WPF、Qt、Avalonia、Dear ImGui | 原生 Windows 观感与 DPI 支持；缺点见 §7 风险 R4 |
| 合成 | Direct2D `ID2D1DeviceContext` + 自定义 HLSL | D3D11 全自绘、Skia | D2D 内置 GaussianBlur/Composite/ColorMatrix/Tile/3DTransform，可覆盖 CI 用到的 14 种滤镜（见 `02` §3.2） |
| 采集 | Windows.Graphics.Capture (WGC) | DXGI Desktop Duplication | WGC 支持显示器/窗口/区域且能设置 `IsCursorCaptureEnabled(false)`，与 mac 端「画面不含光标」策略一致（`ScreenRecorder.swift:246`） |
| 音频 | WASAPI（loopback + capture） | Media Foundation Source Reader | loopback 是唯一系统声音来源；进程级分离不可得 |
| 编解码 | Media Foundation SinkWriter / SourceReader | FFmpeg、NVENC 直调 | 系统自带 H.264/HEVC/AAC；避免 GPL/分发体积问题 |
| 自动更新 | 首版关闭；后续 WinSparkle 或自建 | — | 与 Sparkle 能力不对等，不阻塞主线 |

## 3. 仓库结构

```
dogsc-win/
|- CMakeLists.txt
|- core/                  ← RecorderCore 移植（无平台依赖）
|   model/                 ProjectModel / ProjectMediaModel / ProjectTimeline / CaptureConfiguration
|   editing/               ProjectTimelineEditing / Transforms / EditorTimeline
|   timeline/              TimelineMap / TimelineMediaPlan / OutputFrameSchedule
|   scene/                 FrameScene / CompositionScene / ScreenProjection / CameraContentFill
|   motion/                ZoomAnimation / ScreenMotion / CameraMotion / StickerTransition / PointerTrack / AutoZoomPlanner
|- render/
|   compositor.h           IFrameCompositor::composite(plan, resources, extent) -> Texture
|   d2d/                   图层、遮罩、连续圆角、透视 HLSL、文字光栅化
|- capture/
|   wgc/                   显示器 / 窗口 / 区域采集
|   audio/                 WASAPI loopback + mic
|   writer/                MF SinkWriter 封装（分片、PTS 归一化）
|   pointer/               WH_MOUSE_LL + 光标采样
|- export/
|   reader/                MF SourceReader（含 seek）
|   pipeline/              时间线切片 -> 合成 -> 编码
|- ui/
|   shell/                  录制条、编辑器、设置、引导
|   editor/                 画布、时间线、检查器
|- platform/
|   win32/                   窗口层级、DPI、托盘、全局钩子、字体、偏好
|   json/                    nlohmann/json 或 RapidJSON 封装（保持 Codable 语义）
```

## 4. 模块映射表

| macOS 源 | 行数 | Windows 目标 | 迁移方式 |
|---|---|---|---|
| `RecorderCore/*` | 15,369 | `core/*` | 逐文件翻译；JSON 往返与数值求值做金标准对拍 |
| `SharedFrameRenderer.swift` | 2,128 | `render/d2d/*` | 重写；保留分层顺序、几何公式、透视单应矩阵算法 |
| `SharedFrameCursorRenderer` / `OverlayRenderer` / `ContinuousCornerMask` | ~770 | `render/d2d/*` | 重写；遮罩与九宫格 atlas 需自建 |
| `SharedRenderedPreviewSurface` + `SharedPreview*` | ~2,100 | `render/present/*` | 重写（DXGI 交换链 + `DwmFlush`/`WaitForVBlank`） |
| `ScreenRecorder` / `ScreenCaptureOutput` / `NativeScreenRecording` | 3,269 | `capture/wgc` + `capture/writer` | 重写；PTS 归一化、暂停补偿、非单调丢弃逻辑必须保留 |
| `CameraRecorder` / `MicrophoneRecorder` / `CoreAudioSystemAudioTap` | 2,450 | `capture/audio`、`capture/camera` | 重写；首样本 host-time 对齐策略保留 |
| `PointerEventRecorder` | 620 | `capture/pointer` | 重写；坐标归一化与 `PointerTimelineAlignment` 语义保留 |
| `TimelineCompositionBuilder` / `DirectExportPipeline` / `VideoExporter` | 2,272 | `export/*` | 重写；时间线→帧号→PTS 调度逻辑逐行对照 |
| `Editor*`（UI） | ~50,000 | `ui/*` | 重写；布局常量、设计 token、快捷键表、文案表可复用 |
| `ProjectStore` / `ProjectSessionPersistenceController` | ~1,550 | `platform/storage` | 重写；原子写 + 保留上一版 + 去抖自动保存语义保留 |

## 5. 关键契约（必须逐条对齐）

1. **工程文件**：`project.json` 字段名、默认值、版本校验（23..27）、ISO8601 日期、`sortedKeys` 无关（只要求语义等价）。
2. **时间线**：`RecordingSegment` 不存 `outputStart`；输出位置由累加推导。`TimelineMap` 的 7 类错误必须同义。
3. **帧调度**：`OutputFrameSchedule` 90000 时基、端开区间、整数 tick、ulp 级边界吸附。
4. **帧契约**：`FrameScene` 字段与图层顺序（`background → screen → cursor → spotlight → camera → stickers`）。
5. **码率策略**：`clamp(pixels*fps/6, 12Mbps, 65/100/130Mbps)`；音频 192kbps。
6. **暂停语义**：所有轨道共享同一 stop host-time；PTS 归一化后从 0 开始。
7. **原子写**：临时文件 + 替换 + 保留上一版。

## 6. 阶段划分（详见 `13-milestones.md`）

M0 核心模型 → M1 采集 → M2 渲染 → M3 编辑 UI → M4 导出与互通 → M5 外壳与发布准备。

## 7. 主要风险

| # | 风险 | 缓解 |
|---|---|---|
| R1 | D2D 无 CI 的惰性 extent，5K 中间纹理爆显存 | 每个图层显式计算中间纹理尺寸；建立显存预算表并做峰值监控 |
| R2 | MF 无 `AVMutableComposition`，变速切片需自建时间戳 | 以 `TimelineMediaPlan` 为唯一真相，写单元测试覆盖变速/切段边界 |
| R3 | HEVC 硬编依赖厂商 MFT | 运行时探测，缺失则回退 H.264 并在 UI 说明 |
| R4 | WinUI 3 浮动窗层级不如 NSPanel | 录制条等 HUD 改用 Win32 顶层窗口 + 自管 Z 序，UI 内容仍用 XAML 岛 |
| R5 | 全局鼠标钩子在 UIPI/高 DPI 下不完整 | 以 `GetCursorInfo` 轮询兜底；明确不支持提权窗口的指针录制 |
| R6 | 系统光标/壁纸缺失 | 内置光标素材包；壁纸仅支持用户自选图片/视频 |
| R7 | 两端浮点求值差异导致帧不一致 | 所有核心数学使用 `double`，禁用 `-ffast-math`；提供对拍工具 |

## 8. 不在首版范围

iOS 设备录制、系统壁纸自动发现、进程级系统声音、Sparkle 等价自动更新、HDR 导出（macOS 端本就仅 SDR，`CoreImageFrameColorProfile.swift:32-41`）。
