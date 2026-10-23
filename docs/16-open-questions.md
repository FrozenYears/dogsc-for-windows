# 开工前待确认项

> 以下为真正需要产品/资源决策的开放项，技术层面无默认答案。

## Q1. 光标素材来源

macOS 端运行时读取系统光标（`CursorAssetLibrary.swift:198-202`、`:441-449`）。Windows 端必须内置素材。

- A. 自绘一套与 mac 观感一致的 SVG 光标（可控，需设计投入）
- B. 用 Windows 系统光标位图（`IDC_ARROW` 等，观感与 mac 不一致）
- C. 混合：默认系统光标，另提供 mac 风格包

**影响**：`pointer.jsonl` 的 `cursorAssetID` 跨端一致性；工程互通的视觉保真。

## Q2. 跨端工程互通的验收口径

- A. 严格双向：mac ↔ Win 往返后 `project.json` 语义完全一致（推荐）
- B. 单向：仅支持 Win 打开 mac 工程
- C. 不做互通

**影响**：M0 是否必须做 JSON 往返对拍；决定 `CursorAssetID` 与 `BackgroundSource` 的共享范围。

## Q3. 首版平台门槛

- A. Windows 11 22H2 起（推荐，WGC/HEVC 支持完整）
- B. Windows 10 1903 起（WGC 可用但 HEVC 扩展常缺失）

**影响**：编解码回退策略、UI 框架特性可用性。

## Q4. 系统声音范围

- A. 仅整设备 loopback（推荐，能力与 mac 的「全部系统声音」对齐，缺「所选 App」）
- B. 引入第三方虚拟声卡实现进程级分离（增加安装依赖与合规风险）

**影响**：`SystemAudioScope.selectedApplication` 在 Windows 端的呈现方式（禁用并说明 vs 隐藏）。

## Q5. iOS 设备录制

- A. 首版移除（推荐；需私有投屏协议）
- B. 延后单独评估

**影响**：`CaptureSource.device` 与 `MovieCaptureRole.iosDevice` 的迁移范围。
