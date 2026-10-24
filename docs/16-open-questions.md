# 产品决策记录

> 开工前需要产品/资源决策的开放项。以下为已决议结果与理由。

## 已决议

| # | 议题 | 决议 | 理由 |
|---|---|---|---|
| D1 | 仓库策略 | 新建 `dogsc-win` 目录，含文档骨架 + Windows 源码，推送至 `FrozenYears/dogsc-for-windows` | 与 macOS 源码分离，避免两套构建体系混杂 |
| D2 | 跨端工程互通 | **不做**。Windows 端是独立软件系统 | 产品定位为纯 Windows 软件，互通会增加 M0 的对拍与共享枚举约束 |
| D3 | 首版平台门槛 | Windows 11 24H2（build 26100+） | 原定 22H2；因 `SetWindowExclusionList`、`MinUpdateInterval`、`IncludeSecondaryWindows` 等 WGC 能力均在 26100+ 才稳定提供，提升门槛可去掉大量降级分支。**补充**：这些能力的接口编号随 SDK 漂移（`IGraphicsCaptureSession3` → 更高），运行时必须用 `ApiInformation` + QI 探测，不得按接口编号或 SDK 版本硬判，见 D11-5 |
| D4 | 系统声音 | 默认整设备 loopback；「所选窗口 App」作为可选能力预留 | 进程级捕获已可用（`ActivateAudioInterfaceAsync` + `AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS`，Win10 20348+），与 mac 的 `CATapDescription` 能力相当；首版仍以整机 loopback 为默认路径，进程级在 M1 后评估 |
| D5 | iOS 设备录制 | 首版移除 | 需私有投屏协议；UI 隐藏入口，`CaptureConfiguration` 字段保留但不可选 |
| D6 | 光标素材 | 先做 MVP：广泛检索开源图标库，选用许可允许的光标素材；不可行再考虑 AI 生成，优先级滞后 | 避免在素材环节阻塞主功能 |
| D7 | 多段拼接 + 独立音轨封装 | **MF remux 为主路径**（`IMFSinkWriter` 压缩输入 + 相同压缩输出，或 H.264/AVC Remux MFT）；ffmpeg 降为兜底可选项（LGPL 动态链接），仅在 MF 无法处理时启用 | MF 官方支持 remux 不重编码；ffmpeg 仅在异常容器/非标准样本时兜底，减小分发体积与合规面 |
| D8 | 采集排除语义 | **`SetWindowExclusionList` 为主**（Win11 24H2+，按 `WindowId` 在单会话内排除）；旧 build 降级为采集后遮挡回填 | 24H2 起有官方 per-session 排除接口，无需逐帧回填；遮挡仅作降级 |
| D9 | 四路时钟基准 | **QPC 为唯一基准**，各源启动时记录 QPC 偏移 | 与 mac 端 `CMSyncConvertTime` 语义最接近，实现清晰；WASAPI device position 长期存在漂移 |
| D12 | HEVC 崩溃恢复封装 | HEVC 使用约 10 秒独立普通 MP4 分段，停止时经 Media Foundation 压缩样本直通无损合并；保留完整分段恢复入口 | 本机 NVIDIA HEVC MFT 可输出普通 MP4，但 `MFCreateFMPEG4MediaSink` 拒绝 HEVC；原生分段 + MF remux 不加外部库。已实测 12 秒 HEVC Main 2560×1600@60 + AAC 分为 10s/2s 两段并无损合并、恢复文件可解码；跨设备/驱动仍需验证 |
| D13 | ProRes 422 录制 | 采用动态链接的 LGPL FFmpeg/libavcodec ProRes 422 Standard 编码与 MOV mux；不启用 GPL/nonfree 扩展，并随发布提供对应源码、构建信息和许可证声明 | 用户为保留上游 ProRes 422 选择该路径；Microsoft Media Foundation 支持格式文档未列 ProRes。代价是 DLL/包体、CPU 编码负载、LGPL 动态重链接义务和专利辖区风险；最终须实测编码质量/性能并做合规审查 |
| D14 | 剪辑切点的指针轨求值 | 场景输入按保留片段提供 output-range 与映射后的指针事件，保留切点端点桥接和自动缩放重新锚定，不接受平坦事件轨近似 | 上游 `ProjectPointerTrack` 明确基于切点重建路径与 0.28 秒桥接；平坦输出时间事件无法判断删除区间与切点，用户选择优先保留原行为 |

## D15 M2a 与 ProRes 实测更新（2026-10-24）

- `src/render/d2d` 已实现 D2D/WARP 离屏合成：背景、屏幕、光标、spotlight、摄像头、stickers 六层，含裁切、圆角、阴影、Gaussian blur、焦点、点击效果、摄像头圆形布局、贴图遮罩和透视投影；`d2d_warp_smoke` 已读回 BGRA 像素并验证多层输出与失败不发布旧帧。
- Release clean build 与 6/6 CTest 通过。该证据覆盖离屏 smoke，不等价于实时预览、完整媒体解码或编辑器完成。
- ProRes 422 Standard 720p30 录制 1 秒收到 35 帧；1080p30 收到 5 帧并重复 25 帧；1440p30 收到 3 帧并重复 27 帧。720p/1080p/1440p 的 wall time 约 1.75/2.36/3.55 秒；这证明当前 CPU ProRes 路径未达到这些分辨率的实时采集验收，不能标记为通过。
- 720p ProRes 分片恢复经 `ffprobe` 确认为 `prores` Standard、`apcn`、`yuv422p10le`、30 帧，以及 AAC LC 48 kHz/2 声道；完整文件经 FFmpeg 解码无报错。当前只证明本机配置的短录制与恢复，不证明跨硬件性能。
- 合规边界仍未关闭：仓库使用 vcpkg manifest 的动态 FFmpeg 库配置，但没有提交对应 FFmpeg 源码、补丁包、构建复现材料或发布 notice；发布前必须按 FFmpeg 官方 LGPL 清单完成材料与法律审查。

## D7–D9 的后续核查要求

以上三项在 2026-10-23 做出决议时，基于当时对 Windows API 的认知。其中两项已被后续检索推翻，必须按新事实重审：

- **D7**：Media Foundation 的 `IMFSinkWriter` 在「输入输出格式相同」时可做 remux（`MFCreateSinkWriterFromURL` + 相同媒体类型），并存在 H.264/AVC Remux MFT 专门处理不完整主图像的场景。ffmpeg 可能并非必需，需重新评估。
- **D8**：Windows.Graphics.Capture 已有 `IDisplayGraphicsCaptureSession.SetWindowExclusionList` / `GetWindowExclusionList`，可按 `WindowId` 在**单个采集会话内**排除窗口。`WDA_EXCLUDEFROMCAPTURE` 之外出现了 per-session 方案，遮挡回填可能完全不必要。
- **D9**：未检索到推翻证据，QPC 方案维持。

重审结论记录为 D10。

## D10 检索复核结论（2026-10-23）

### D7 修正：ffmpeg 降为「可选项」，MF remux 为主路径

Media Foundation 官方文档明确支持 remux 不重编码：`IMFSinkWriter` 在**压缩输入与相同压缩输出**时直接写入容器，不做转码（`Using the Sink Writer`）。另有 `H.264/AVC Remux MFT` + MP4 Sink 专门处理「样本不含完整主图像 / 缺时间戳」的情况，输入 `MFVideoFormat_H264_ES`、输出 `MFVideoFormat_H264`。

结论：多段拼接与独立音轨 mux 优先用 MF remux 实现，ffmpeg 仅作为 MF 无法处理时的兜底（如异常容器、非标准样本）。D7 原文「引入 ffmpeg 作为可选依赖」仍然成立，但不再是首选定路径。

待验证：`[INFERENCE]` MF remux 对分段 MP4（fragmented）与不同编码参数段落的兼容性，需 M1 实测。

### D8 修正：「采集后遮挡」不再必要

`IDisplayGraphicsCaptureSession.SetWindowExclusionList(IIterable<WindowId>)` / `GetWindowExclusionList()` 可按 `WindowId` 在**单个采集会话**内排除窗口，`WindowId` 可由 `GetWindowIdFromWindow(HWND)` 得到。该接口在 Windows 11 24H2（build 26100+）提供，需 QI 到 `IDisplayGraphicsCaptureSession`，旧 build 上 QI 会失败。

结论：优先用 `SetWindowExclusionList` 排除自身窗口，遮挡回填仅作为旧 build 的降级路径。**平台门槛建议从 22H2 提升到 24H2**（见 D11）。

### D9 维持：QPC 为唯一基准

未检索到推翻证据。补充：WASAPI 侧可参考 `IAudioClient::GetDevicePosition` 与 `IAudioClock` 的 QPC 相关性做漂移校正，但基准仍是 QPC。

### 顺带修正的旧认知

| 原判断 | 检索结果 |
|---|---|
| 进程级系统声音在 Windows 不可得 | **可得**：`ActivateAudioInterfaceAsync` + `AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS`（Win10 20348+），可按进程树包含/排除，与 mac 的 `CATapDescription` 能力相当 |
| D2D 透视需自写 HLSL | **不必须**：`CLSID_D2D13DTransform`（任意 4x4 矩阵）与 `CLSID_D2D13DPerspectiveTransform` 均为内置 effect，可覆盖现有单应矩阵需求 |
| 系统声音只能整机 loopback | 见上，进程级也可得；整机 loopback 仍是默认路径 |

## D11 第二轮检索复核（2026-10-23）

对前一轮遗留的「Windows 上没有 XXX」类断言做二次检索。结论分三类：**维持**、**修正**、**需实测**。

### 维持

| 断言 | 复核结果 | 来源 |
|---|---|---|
| QPC 作为唯一时钟基准 | 维持。WGC 帧的 `Direct3D11CaptureFrame.SystemRelativeTime` 本身就是「合成器渲染该帧时的 QPC 时刻」，与 `QueryPerformanceCounter` 同源，四路对齐可行 | <https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.direct3d11captureframe.systemrelativetime> |
| 进程级系统声音可得 | 维持。`AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS` + `VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK` 经 `ActivateAudioInterfaceAsync` 激活，可按进程树 include/exclude | <https://learn.microsoft.com/en-us/windows/win32/api/audioclientactivationparams/ns-audioclientactivationparams-audioclient_process_loopback_params> |
| D2D 有内置 3D / 透视变换 | 维持，可强化。`CLSID_D2D13DTransform` 支持任意 4x4 矩阵，最低 Windows 8 + Platform Update（即 Windows 10+ 全支持） | <https://learn.microsoft.com/en-us/windows/win32/direct2d/3d-transform> |
| 光标是否入镜可程序控制 | 维持。`GraphicsCaptureSession.IsCursorCaptureEnabled`，Win10 2004（19041+）起可用 | <https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession.iscursorcaptureenabled> |
| HUD 不适合走 XAML Island 透明 | 维持，但**理由要改**。原文隐含「做不到」；实际是「WinUI 3 的 XAML Island / SwapChainPanel 透明与点击穿透在合成层上不可靠」——`SetLayeredWindowAttributes(LWA_COLORKEY)` 对 WinUI 3 窗口无效（按设计拒绝），`SwapChainPanel` 被明确定义为 external content 不支持透明，且 `IsAlwaysOnTop = true` 时点击穿透存在已知缺陷 | <https://github.com/microsoft/microsoft-ui-xaml/issues/2956>、<https://github.com/microsoft/microsoft-ui-xaml/issues/8469>、<https://learn.microsoft.com/en-us/windows/apps/develop/composition/visual-layer>、<https://github.com/castorix/WinUI3_SwapChainPanel_Layered/issues/5> |
| 离屏合成可在 WARP 上跑 | 维持，可强化。MS 文档明确建议「需要保证中间纹理精度时用 `D3D_DRIVER_TYPE_WARP`」，`ID2D1DeviceContext::IsBufferPrecisionSupported` + `SetRenderingControls` 控制精度 | <https://learn.microsoft.com/en-us/windows/win32/direct2d/precision-and-clipping-in-effect-graphs> |

### 修正

1. **D2D `GaussianBlur` 会扩大输出 extent**。半径换算约 `StandardDeviation × 3`，输出尺寸在每边增加 `StandardDeviation × 3 × (DPI/96)`，即总尺寸增加 `StandardDeviation × 6 × (DPI/96)`，并有 `BORDER_MODE`（soft / hard）控制边缘处理。
   - 影响：`02` §3.2 第 3 行「需手工中间纹理尺寸」的判断**被证实**，并给出了可直接用于实现的公式。
   - 参考：<https://learn.microsoft.com/en-us/windows/win32/direct2d/gaussian-blur>

2. **HEVC 编码依赖 Store 扩展，离线部署是真实阻碍**。`mfh265enc.dll` 由 Microsoft.HEVCVideoExtension（Store 包）提供；从未安装过该包的离线机器在 MF 初始化时因联网许可失败返回 `0x800704CF` / `0x80072EE7`。一旦成功授权，卸载后许可状态仍保留。
   - 影响：`15` 的 R3 只写「运行时探测 + 回退 H.264」，不够；需补「离线机器上 HEVC 编码**可能完全不可用**，安装包需引导或内置扩展部署路径」。
   - `[INFERENCE]` 硬件编码器（Intel/NVIDIA/AMD 的 HEVC MFT）不依赖该 Store 包；`MFTEnumEx` + `MFT_ENUM_FLAG_HARDWARE` 可优先选中它们。需 M1 实测确认。
   - 参考：<https://learn.microsoft.com/en-us/windows/win32/medfound/h-265---hevc-video-encoder>、<https://stackoverflow.com/questions/78082351/media-foundation-cannot-initialise-transform-cannot-connect-to-windows-update>、<https://www.simonmourier.com/blog/Media-Foundation-cannot-initialise-H-265-HEVC-Video-Decoder-transform-cannot-con/>

3. **WGC 支持 HDR**，原文未提。`Direct3D11CaptureFramePool` 可用 `DXGI_FORMAT_R16G16B16A16_FLOAT` 避免高光裁切；`IDXGIOutput5::DuplicateOutput1` 可无损抓 HDR。
   - 影响：结论（首版不做 HDR）仍成立，理由改为「macOS 端本就仅 SDR，产品范围不含 HDR」，而非「Windows 做不到」。
   - 参考：<https://learn.microsoft.com/en-us/windows/apps/develop/media-authoring-processing/screen-capture>、<https://learn.microsoft.com/en-us/windows/win32/direct3darticles/high-dynamic-range>

4. **D2D 无内置 squircle**。`D2D1_ROUNDED_RECT` 的四角是四分之一**椭圆**，不支持连续圆角；连续圆角必须用 `ID2D1PathGeometry` + `ID2D1GeometrySink::AddBezier` 自绘。
   - 影响：`02` §3.2「自算 superellipse 路径 → 遮罩纹理」方向正确，补明「不能用 `DrawRoundedRectangle`」。
   - 参考：<https://learn.microsoft.com/en-us/windows/win32/api/d2d1/ns-d2d1-d2d1_rounded_rect>、<https://learn.microsoft.com/en-us/windows/win32/direct2d/how-to-draw-and-fill-a-complex-shape>

5. **接口编号会随 SDK 漂移**。`IGraphicsCaptureSession3`（含 `MinUpdateInterval`）在 26100 引入；全新 SDK 中存在 `IGraphicsCaptureSession7` 等更高编号。
   - 影响：D3 的「24H2 提供」需补充「**运行时用 `ApiInformation` + `QueryInterface` 探测能力，不按接口编号硬编码**」，否则升级 SDK 会误判。
   - 参考：<https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession.minupdateinterval?view=winrt-28000>

6. **`WDA_EXCLUDEFROMCAPTURE` 有版本语义差异**。Win10 19041+ 起为透明排除；更早版本等同 `WDA_MONITOR`（黑块）。24H2 对 `DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED` 交换链的捕获行为又有变化（捕获 API 返回全黑）。
   - 影响：D8 的降级路径必须写明「受保护窗口在 24H2 上会被捕获为黑块，这是 OS 行为，非本产品缺陷」；`SetWindowExclusionList` 对受保护窗口同样无能为力。
   - 参考：<https://github.com/microsoft/Windows.UI.Composition-Win32-Samples/issues/64>、<https://learn.microsoft.com/en-nz/answers/questions/5548731/directcomposition-hw-protected-swap-chain-capture>

### 需实测

- `[INFERENCE]` `SetWindowExclusionList` 对 UWP 窗口、XAML Island 窗口的实际生效范围。
- `[INFERENCE]` 厂商 HEVC 硬件 MFT 在无 Store 扩展的干净系统上是否仍可枚举并编码（决定 R3 的严重程度）。
- `[INFERENCE]` WGC 窗口采集在窗口最小化/被遮挡时是否仍产出内容（影响「窗口采集」的用户预期说明）。
- `[INFERENCE]` D2D effect graph 在 5K 分辨率、多层 blur 下的显存峰值与 WARP 回退耗时。

## D2 的连带影响

不做互通后，以下约束**可以放宽**：

- `CursorAssetID` 不必与 macOS 逐值对齐，只需在本产品内稳定；
- `BackgroundSource` 的 `systemImage(absolutePath:)` 可改为 Windows 路径语义；
- M0 的 JSON 往返对拍改为「本产品自身的编解码稳定性测试」，不再需要 mac 端参与；
- schema 版本号仍沿用 23…27 以避免自造分叉，但不承诺与 mac 工程文件互认。

## D6 的验收条件

光标素材选型须满足：

1. 至少覆盖箭头、文本、十字、手形、禁止、调整大小六类；
2. 许可允许商用与再分发（CC0 / MIT / Apache-2.0）；
3. 提供 SVG 或高分辨率 PNG（≥ 256px）；
4. 具备热点定义能力（hotspot 坐标）。

若开源库无法同时满足，再启动 AI 生成或自绘，优先级排在 M3 之后。
