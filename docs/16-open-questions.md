# 产品决策记录

> 开工前需要产品/资源决策的开放项。以下为已决议结果与理由。

## 已决议

| # | 议题 | 决议 | 理由 |
|---|---|---|---|
| D1 | 仓库策略 | 新建 `dogsc-win` 目录，含文档骨架 + Windows 源码，推送至 `FrozenYears/dogsc-for-windows` | 与 macOS 源码分离，避免两套构建体系混杂 |
| D2 | 跨端工程互通 | **不做**。Windows 端是独立软件系统 | 产品定位为纯 Windows 软件，互通会增加 M0 的对拍与共享枚举约束 |
| D3 | 首版平台门槛 | Windows 11 24H2（build 26100+） | 原定 22H2；因 `SetWindowExclusionList`、`MinUpdateInterval`、`IncludeSecondaryWindows` 等 WGC 能力均在 26100+ 才稳定提供，提升门槛可去掉大量降级分支 |
| D4 | 系统声音 | 默认整设备 loopback；「所选窗口 App」作为可选能力预留 | 进程级捕获已可用（`ActivateAudioInterfaceAsync` + `AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS`，Win10 20348+），与 mac 的 `CATapDescription` 能力相当；首版仍以整机 loopback 为默认路径，进程级在 M1 后评估 |
| D5 | iOS 设备录制 | 首版移除 | 需私有投屏协议；UI 隐藏入口，`CaptureConfiguration` 字段保留但不可选 |
| D6 | 光标素材 | 先做 MVP：广泛检索开源图标库，选用许可允许的光标素材；不可行再考虑 AI 生成，优先级滞后 | 避免在素材环节阻塞主功能 |
| D7 | 多段拼接 + 独立音轨封装 | **MF remux 为主路径**（`IMFSinkWriter` 压缩输入 + 相同压缩输出，或 H.264/AVC Remux MFT）；ffmpeg 降为兜底可选项（LGPL 动态链接），仅在 MF 无法处理时启用 | MF 官方支持 remux 不重编码；ffmpeg 仅在异常容器/非标准样本时兜底，减小分发体积与合规面 |
| D8 | 采集排除语义 | **`SetWindowExclusionList` 为主**（Win11 24H2+，按 `WindowId` 在单会话内排除）；旧 build 降级为采集后遮挡回填 | 24H2 起有官方 per-session 排除接口，无需逐帧回填；遮挡仅作降级 |
| D9 | 四路时钟基准 | **QPC 为唯一基准**，各源启动时记录 QPC 偏移 | 与 mac 端 `CMSyncConvertTime` 语义最接近，实现清晰；WASAPI device position 长期存在漂移 |

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
