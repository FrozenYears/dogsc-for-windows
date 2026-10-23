# macOS 平台依赖清单与阻碍点

> 基线 commit `567fd6e`。本文件回答一个问题：**哪些代码不可移植，必须换实现**。

## 1. 依赖密度总览

`import` 计数（全仓）：Foundation 141、SwiftUI 125、RecorderCore 125、AppKit 123、AVFoundation 42、QuartzCore 29、CoreImage 23、Combine 23、os/OSLog 22、ScreenCaptureKit 12、CoreMedia 12、CoreGraphics 11、CoreText 9、UniformTypeIdentifiers 8、VideoToolbox 7、Metal 7、CoreVideo 5、AVKit 5、AudioToolbox 4。

符号计数（`grep -rhoE` 后排序）：`CGFloat` 945、`CGPoint` 560、`CGRect` 357、`NSColor` 303、`CGSize` 229、`CIImage` 228、`NSView` 220、`NSWindow` 147、`NSEvent` 145、`CMTime` 111、`NSImage` 105。

结论：**几何类型（CG*）是跨模块的公共词汇**，它们在 Windows 端有一一对应的替代物，属于低风险替换；真正的风险集中在 AVFoundation / ScreenCaptureKit / AppKit 三类。

## 2. 硬阻塞点（无系统等价物）

| # | 阻塞点 | 证据 | Windows 现实 |
|---|---|---|---|
| B1 | 系统光标素材 | `CursorAssetLibrary.swift:441-457` 读 HIServices 的 `cursor.pdf` + `info.plist`（阴影色/偏移/模糊） | 无对应资源。必须内置一套光标位图/SVG 并重做热点与尺寸归一化 |
| B2 | 系统壁纸发现 | `SystemWallpaperLibrary.swift:244-323` 扫 `/System/Library/Desktop Pictures`、`com.apple.wallpaper/Store/Index.plist`、aerial `entries.json` | 仅注册表 `HKCU\Control Panel\Desktop\WallPaper` 单张静态图 |
| B3 | 系统声音进程级 Tap | `CoreAudioSystemAudioTap.swift:59-110` 用 `CATapDescription` 按进程精确分流，可排除自身 | Windows 只有 WASAPI loopback（整设备），进程级分离需第三方音频驱动 |
| B4 | `SCRecordingOutput` 原生写盘 | `NativeScreenRecording.swift:128-160` 由系统直接写 HEVC 文件 | 无对应；必须自建 WGC -> 编码器 -> muxer 链路 |
| B5 | `AVMutableComposition.insertTimeRange/scaleTimeRange` | `TimelineCompositionBuilder.swift:104-207` | Media Foundation 无 composition 对象，需自建时间戳重排 |
| B6 | `MTAudioProcessingTap` 分段增益 | `TimelineAudioGainProcessor.swift:21-79` | MF 无 audio tap；需在编码前自混 PCM |
| B7 | `AVPlayerItemVideoOutput` 播放取帧 | `SharedRenderedPreviewSurface.swift:1227-1257` | 需 MediaEngine + `TransferVideoFrame`，或自管解码器 |
| B8 | 全局鼠标事件 | `PointerEventRecorder.swift:405-440` 用 `CGEvent.tapCreate` 监听会话级事件 | 需 `SetWindowsHookEx(WH_MOUSE_LL)` + `GetCursorPos` 采样；UIPI 下高权限窗口事件不可见 |
| B9 | Sparkle 自动更新 | `AppUpdateController.swift`、`Package.swift:13-16` | 需替换（WinSparkle / 自建；或首版关闭） |
| B10 | 屏幕/辅助功能权限模型 | `AppModel.swift:191-194`、`RecorderPermissionView.swift` | Windows 无运行时弹窗授权；屏幕采集只需用户确认，全局钩子无需辅助功能授权 |

## 3. API 映射表

### 3.1 采集

| macOS | 位置 | Windows | 风险 |
|---|---|---|---|
| `SCShareableContent` / `SCContentFilter` | `ScreenRecorder.swift:104-160` | `GraphicsCaptureItem`（`IGraphicsCaptureItemInterop.CreateForMonitor/Window`） | 中 |
| `SCStream` + `SCStreamOutput(didOutputSampleBuffer:)` | `ScreenRecorder.swift:352-400` | `Direct3D11CaptureFramePool.FrameArrived` 取 `IDirect3DSurface` | 中 |
| `SCStreamConfiguration`（尺寸/像素格式/帧间隔/queueDepth/showsCursor） | `ScreenRecorder.swift:186-260` | `GraphicsCaptureSession`（无帧率设置，靠丢帧）；尺寸靠 `CreateFreeThreaded` + 缩放 | 中高 |
| `SCContentFilter(desktopIndependentWindow:)` | `ScreenRecorder.swift:146-160` | `CreateForWindow(HWND)`；UWP/受保护窗口不可采 | 中 |
| 显示器像素比 `pointPixelScale` | `ScreenRecorder.swift:126-155` | `GetDpiForMonitor`（Per-Monitor V2） | 低 |
| `SCRecordingOutput` | `NativeScreenRecording.swift` | 无 | 高 |
| iOS 设备录制 | `CameraRecorder.swift`（`MovieCaptureRole.iosDevice`） | 需 USB/无线投屏协议（QuickTime 私有），建议首版移除 | 高 |
| `AVCaptureSession`（摄像头） | `CameraRecorder.swift:1-90` | Media Foundation `IMFSourceReader`（视频捕获源）或 `Windows.Media.Capture` | 中 |
| `AVCaptureAudioDataOutput`（麦克风） | `MicrophoneRecorder.swift:1-40` | WASAPI capture（`IAudioClient`）或 `IMFSourceReader`(audio) | 中 |
| `CATapDescription` 系统声音 | `CoreAudioSystemAudioTap.swift:71-110` | `IAudioClient` loopback（`AUDCLNT_STREAMFLAGS_LOOPBACK`） | 中（功能降级） |
| `AVAssetWriter`（采集写盘） | `ScreenCaptureOutput.swift:243-400` | `IMFSinkWriter`（H.264/HEVC/AAC） | 中高 |
| `CMSampleBuffer` PTS / `CMTime` | 全采集链路 | `IMFSample` + `LONGLONG` 100ns 时间戳 | 低（语义等价） |
| `CMClockGetHostTimeClock` | `AppModel+CaptureLifecycle.swift` 等 16 处 | `QueryPerformanceCounter` | 低 |

### 3.2 渲染

| macOS | 位置 | Windows | 风险 |
|---|---|---|---|
| `CIImage` / `CIFilter`（14 种） | `SharedFrameRenderer.swift` 等 228 处 | D2D `ID2D1Effect`（GaussianBlur/Composite/ColorMatrix/AffineTransform2D/Tile/Border/Flood）或自写 HLSL | 高 |
| `CIPerspectiveTransformWithExtent` | `SharedFrameRenderer.swift:1603-1630` | D2D `3DTransform` effect 或自写透视 pass | 高 |
| `CIGaussianBlur` + extent 裁剪语义 | `SharedFrameRenderer.swift:2080-2102` | `GaussianBlur` effect + 手工中间纹理尺寸 | 高 |
| `CIBlendWithMask` / `CISourceOutCompositing` | 16 处 / 3 处 | `Composite`(DestinationIn / SourceOut) | 中 |
| `CIRoundedRectangleGenerator` + `RoundedRectangle(.continuous)` | `ContinuousCornerMask.swift:72-174` | 自算 superellipse 路径 -> 遮罩纹理 | 中高 |
| `CIAffineTile` / 图案 | `SharedFrameRenderer.swift:1767-1805` | `Tile` effect / `ID2D1BitmapBrush` | 低 |
| `CoreText` 光栅化 chrome 标题 | `SharedFrameRenderer.swift:1144-1172` | DirectWrite `IDWriteTextLayout` -> bitmap | 中 |
| `CIContext(mtlCommandQueue:)` + `CIRenderDestination(mtlTexture:)` | `SharedPreviewRenderPipeline.swift:60-130` | `ID2D1DeviceContext` + D3D11/DXGI 目标纹理 | 中 |
| `CAMetalLayer` drawable 校验 | `SharedPreviewRenderPipeline.swift:103-113` | `IDXGISwapChain1::GetBuffer`（尺寸随 resize 变化，语义不同） | 中 |
| `CADisplayLink` | `SharedPreviewWindowVisibility.swift:12-129` | `DwmFlush` / `IDXGIOutput::WaitForVBlank` / `CompositionTarget::Rendering` | 中 |
| `drawable.presentedTime` | 预览诊断 | DXGI `GetFrameStatistics` | 中 |
| CALayer 独立光标层 | `SharedRenderedPreviewSurface.swift:783-1118` | DComp visual 或退回整帧重绘 | 中高 |

### 3.3 播放 / 解码 / 导出

| macOS | 位置 | Windows | 风险 |
|---|---|---|---|
| `AVURLAsset` / `AVAssetTrack` 元数据 | `MediaAnalysis.swift:198-259` | `IMFSourceReader::GetNativeMediaType` 或 `MFGetAttributeSize` | 中 |
| `AVAssetReader` + `TrackOutput` | `DirectExportPipeline.swift:150-230` | `IMFSourceReader`（可设 DXVA 解码） | 中 |
| `AVAssetWriter` + `PixelBufferAdaptor` | `DirectExportPipeline.swift:243-296` | `IMFSinkWriter` + `MFCreateVideoSampleFromSurface` | 中高 |
| HEVC/H.264 硬件编码 | `DirectExportPipeline.swift:11-13, 270-273` | `MFVideoFormat_HEVC`（需 HEVC Video Extensions 或厂商 MFT） | 中高 |
| `AVMutableComposition` | `TimelineCompositionBuilder.swift` | 无；自建切片调度 | 高 |
| `AVAudioMix` + MTAudioProcessingTap | `TimelineAudioGainProcessor.swift` | 自混 PCM | 中高 |
| `AVAssetExportSession`（纯音频） | `AudioOnlyExportPipeline.swift` | `IMFSinkWriter` AAC | 低 |
| `AVAssetImageGenerator` | `EditorMediaSession.swift:45-103` | `IMFSourceReader` seek + 解码首帧 | 中 |
| `AVPlayer` 主时钟 | `EditorPlaybackController.swift:30-70` | `IMFMediaEngine` + 自建时钟 | 高 |
| `ImageIO`（HEIC 缩略图） | `WallpaperImageLoading.swift:15-52` | WIC（HEIF 需扩展包） | 中 |

### 3.4 UI / 系统

| macOS | 位置 | Windows | 风险 |
|---|---|---|---|
| AppKit 窗口/面板层级 | `RecorderPanelController.swift`、`CaptureWindowPresentationPolicy.swift:15-30` | Win32 `WS_EX_TOPMOST` + 自管 Z 序；或 WinUI 顶层窗口 | 中 |
| SwiftUI 视图（236 处） | `Sources/DogSCApp/*.swift` | 见 `04-ui-strategy.md` | 高 |
| `NSEvent` 本地/全局监视（22 处） | `EditorTimelineZoomTransport.swift:842-1024` 等 | `SetWindowsHookEx` / `IMessageFilter` / XAML `PreviewKeyDown` | 中 |
| `NSMenu` 主菜单 + 快捷键 | `EditorMenuBridge.swift` | Win32 `HMENU` 或 XAML `MenuFlyout` + `KeyboardAccelerator` | 中 |
| `NSStatusItem` 菜单栏图标 | `DogSCApp.swift:37-60` | `Shell_NotifyIcon`（托盘） | 低 |
| `NSFont` / `CTFontManagerRegisterFontsForURL` | `AppTypography.swift:13-38` | `AddFontMemResourceEx` / DirectWrite 字体集合 | 低 |
| `NSCursor`（24 处） | `EditorCanvasPreview.swift:764` 等 | `SetCursor` / `LoadCursor` | 低 |
| `NSColor`（303 处） | 设计系统 | `D2D1_COLOR_F` / `Windows.UI.Color` | 低 |
| `NSUserDefaults` | `AppPreferences.swift` | 注册表或 JSON 配置 | 低 |
| `Bundle.localizedString` | `AppLocalization.swift:44-50` | 资源字典 / `.resw` / 自管 JSON | 低 |
| `Observation` 框架 | `AppLocalization.swift:1-15` | 手写观察者 | 低 |
| Combine（152 处） | `AppModel.swift` 等 | 目标框架的响应式（如 `INotifyPropertyChanged` / 自建事件） | 中 |
| `UndoManager` | `EditorStore.swift:782-800` | 自建命令栈（模型已是纯函数，易实现） | 低 |
| `CryptoKit.SHA256` | `EditorTimelineWaveformCache.swift` | BCrypt | 低 |

## 4. 采集层专项映射（逐能力核对）

| 能力 | macOS 证据 | Windows 等价 | 风险 |
|---|---|---|---|
| 显示器枚举/几何 | `CaptureReadiness.swift:26-33`（`NSScreen`+`CGDisplayCopyDisplayMode`） | `EnumDisplayMonitors`/`GetMonitorInfo`/`GetDpiForMonitor` | 低 |
| 整屏采集 | `ScreenRecorder.swift:101-251`（`SCStream`+`SCContentFilter(display:)`） | WGC `GraphicsCaptureItem`+`Direct3D11CaptureFramePool`，或 DXGI Desktop Duplication | **高** |
| 单窗口采集 | `ScreenRecorder.swift:157`（`desktopIndependentWindow`） | WGC `CreateForWindow`（受 DWM 裁剪/最小化限制） | 中 |
| **排除/例外语义** | `CaptureSurfaceFilter.swift:9-39`（`excludingApplications:exceptingWindows:`） | **无直接等价**，需采集后遮挡或逐窗口合成 | **高** |
| 原生直写 | `NativeScreenRecording.swift:127-183`（`SCRecordingOutput`） | MF `IMFSinkWriter` + H.264/HEVC MFT | 中 |
| 采集面参数 | `ScreenRecorder.swift:216-244`（NV12 videoRange + sRGB + 709 + `.SDR`、`showsCursor=false`） | WGC 帧为 BGRA；需显式转 NV12 并自行保证色彩标签 | 中 |
| 无音频同流 | `ScreenRecorder.swift:247-250`（`capturesAudio=false`，否则 `startCapture()` 挂起） | 无此限制，但建议同样分离，便于独立降级 | 低 |
| 采样与队列深度 | `CaptureStreamCadence.swift:12-30`（`minimumFrameInterval=1/60`、`queueDepth=8`） | WGC `FramePool` 缓冲数 + 自行丢帧；无帧率参数 | 中 |
| **录制中热切换采集项** | `ScreenRecorder.swift:635,664`（`stream.updateContentFilter`） | WGC 不支持中途换 item，只能停流重开（产生帧空洞） | **高** |
| 系统声音 | `CoreAudioSystemAudioTap.swift:73-190`（进程 Tap，可排除自身/按 app 过滤） | WASAPI loopback（整机）；或 Win10 2004+ Process Loopback | 中 |
| 麦克风 | `MicrophoneRecorder.swift:8-19`（`AVCaptureSession`+`AVCaptureAudioDataOutput`） | WASAPI capture / MF Source Reader | 低 |
| 摄像头 | `CameraRecorder.swift:18,66-76`（`AVCaptureSession`+`AVCaptureVideoDataOutput`，NV12，H.264 20Mbps） | MF Source Reader / WinRT MediaCapture | 中 |
| 摄像头帧归一化 | `CameraSampleWriterFrameNormalizer.swift:18-170`（`VTPixelTransferSession`，`kVTScalingMode_Trim`） | D3D11 Video Processor / `MFCreateVideoProcessor` | 低 |
| 摄像头格式协商 | `CameraCaptureFormatSelector.swift:6-38`、`CameraCaptureDeviceConfigurator.swift:44-70`（不固定 FPS） | MF `GetNativeMediaType` 枚举 + 手动选择 | 中 |
| 鼠标事件 | `PointerEventRecorder.swift:405-420`（`CGEvent.tapCreate`，listenOnly，专属线程） | `SetWindowsHookEx(WH_MOUSE_LL)` 或 Raw Input | 中 |
| 事件采样率 | `PointerEventRecorder.swift:286-293,512-513`（240Hz 上限、关闭鼠标合并） | 低级钩子本身无合并；需自行限流 | 低 |
| 光标形状 | `PointerEventRecorder.swift:585-590`（30Hz 轮询 `NSCursor.currentSystem`） | `GetCursorInfo`+`GetIconInfo` | 低 |
| **多段拼接** | `NativeScreenRecording.swift:931-997`（`AVAssetExportSession` passthrough） | MF topology 拼接（需重编码）或外部 ffmpeg | 中 |
| **独立音轨封装** | `NativeScreenRecording.swift:1003-1075`（`AVMutableComposition` passthrough，按 host 时钟正偏移延后/负偏移裁头） | MF Sink Writer（重编码）或 ffmpeg `-c copy` | **高** |
| 统一 host 时钟 | `CaptureRecordingSupport.swift:145-165`（`CMSyncConvertTime` 转到 `CMClockGetHostTimeClock`） | 自建 QPC 基准统一 WASAPI/MF/摄像头/鼠标四路时间戳 | 中 |
| 黑边检测 | `CameraLetterboxAnalysis.swift:74-200`（`AVAssetImageGenerator` 采 9 帧聚类取中位） | MF Source Reader 抽帧 + 同算法 | 低 |
| 热插拔 | `CaptureDeviceLifecycle.swift:100-120` | `WM_DEVICECHANGE`+`IMMNotificationClient` | 低 |
| 性能保障 | `CaptureStreamCadence.swift:34-40`（`ProcessInfo.beginActivity`） | `SetThreadExecutionState`+`AvSetMmThreadCharacteristics` | 低 |
| 权限 | `AppModel.swift:192`、`CaptureReadiness.swift:111`（TCC） | 无 TCC；麦克风/摄像头走隐私设置，屏幕采集只需用户确认 | 低 |

### 4.1 采集层硬阻塞清单（逐条验证）

1. **排除语义**：`SCContentFilter` 可同时排除自身应用、Dock、Finder 桌面图标，并保留编辑器窗口例外（`CaptureSurfaceFilter.swift:9-39`）。Windows 侧无等价 API，只能采集后遮挡或逐窗口合成，性能与正确性均退化。
2. **原生直写 5K HEVC**：`SCRecordingOutput` 由系统直接写盘（`NativeScreenRecording.swift:127-183`）。需重建 D3D11 纹理 → MF 编码器零拷贝链路。
3. **按 app 过滤系统声音**：`CATapDescription(stereoMixdownOfProcesses:)`（`CoreAudioSystemAudioTap.swift:78-90`）。老 WASAPI loopback 只能整机混音；已决议首版降级为整机 loopback。
4. **passthrough 拼接与 mux**：`concatenate`（`NativeScreenRecording.swift:931-997`）与 `muxSystemAudio`（`:1003-1075`）都依赖 `AVAssetExportSession` passthrough，不重编码。MF 下需重编码；不接受 ffmpeg 外部依赖则成本显著上升。
5. **录制中热切换采集项**：`stream.updateContentFilter(filter)`（`ScreenRecorder.swift:635,664`）让编辑器窗口可自由开关、选区可切换。WGC 不支持中途换采集项。
6. **统一 host clock**：`CMSyncConvertTime(..., to: CMClockGetHostTimeClock())`（`CaptureRecordingSupport.swift:145-165`）依赖 macOS 单一 host clock。Windows 需自建 QPC 基准统一四路时间戳，且需处理各源启动偏移。
7. **权限模型差异**：Accessibility 是 `CGEventTap` 的硬前提（`PointerEventRecorder.swift:405`）。Windows 低级钩子无需授权，但易被安全软件静默拦截，需设计 Raw Input 降级路径。
8. **硬件编码强约束**：macOS 侧「宁可失败不软编」（`ScreenCaptureOutput.swift:340-355`）与 H.264 4096×2304 上限（`ScreenRecorder.swift:189-194`）在 Windows 多 GPU 场景会放大不可录范围，需按厂商能力重定策略。

## 5. 阻碍点分级

**A 级（决定项目可行性）**：AVFoundation 全套时间线/编解码（B5/B6/B7）、ScreenCaptureKit 采集质量（B4）、渲染器重写（3.2 全表）；采集层另有 4 项硬阻塞（4.1 的第 1/2/4/5 条）。

**B 级（影响功能完整度，可裁剪）**：系统光标（B1）、系统壁纸（B2）、进程级系统声音（B3）、iOS 设备录制、Sparkle、采集项热切换。

**C 级（工作量确定，风险低）**：几何类型、偏好存储、本地化、图标自绘、字体加载、显示器枚举、设备热插拔。

## 6. 不受影响的部分

`RecorderCore` 44 个文件 / 15,369 行零平台依赖（见 `01-data-model-and-timeline.md`）：全部只 `import Foundation`，对 `CG*` 类型引用为 0。同一份 `project.json` 求值必须得到相同的 `FrameScene` 数值。
