# 产品决策记录

> 开工前需要产品/资源决策的开放项。以下为已决议结果与理由。

## 已决议

| # | 议题 | 决议 | 理由 |
|---|---|---|---|
| D1 | 仓库策略 | 新建 `dogsc-win` 目录，含文档骨架 + Windows 源码，推送至 `FrozenYears/dogsc-for-windows` | 与 macOS 源码分离，避免两套构建体系混杂 |
| D2 | 跨端工程互通 | **不做**。Windows 端是独立软件系统 | 产品定位为纯 Windows 软件，互通会增加 M0 的对拍与共享枚举约束 |
| D3 | 首版平台门槛 | Windows 11 22H2 | WGC、HEVC Video Extensions、Per-Monitor V2 DPI 全部可用，回退路径简单 |
| D4 | 系统声音 | 仅整设备 loopback | 无需第三方驱动；「所选窗口 App」在 UI 禁用并说明 |
| D5 | iOS 设备录制 | 首版移除 | 需私有投屏协议；UI 隐藏入口，`CaptureConfiguration` 字段保留但不可选 |
| D6 | 光标素材 | 先做 MVP：广泛检索开源图标库，选用许可允许的光标素材；不可行再考虑 AI 生成，优先级滞后 | 避免在素材环节阻塞主功能 |

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
