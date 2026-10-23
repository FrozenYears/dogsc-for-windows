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

## 4. 阻碍点分级

**A 级（决定项目可行性）**：AVFoundation 全套时间线/编解码（B5/B6/B7）、ScreenCaptureKit 采集质量（B4）、渲染器重写（3.2 全表）。

**B 级（影响功能完整度，可裁剪）**：系统光标（B1）、系统壁纸（B2）、进程级系统声音（B3）、iOS 设备录制、Sparkle。

**C 级（工作量确定，风险低）**：几何类型、偏好存储、本地化、图标自绘、字体加载。

## 5. 不受影响的部分

`RecorderCore` 24k 行零平台依赖（见 `01-data-model-and-timeline.md`），可直接作为移植期「行为金标准」：同一份 `project.json` 在两端求值必须得到相同 `FrameScene` 数值。
