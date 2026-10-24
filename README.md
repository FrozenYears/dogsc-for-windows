# DogSC for Windows

DogSC 的 Windows 原生移植。录屏、剪辑、运镜与导出，面向 Windows 11。

> 本项目源自 [laogou717/dogsc](https://github.com/laogou717/dogsc)（MIT，Copyright (c) 2026 神烦老狗）。原始 macOS 实现见上游仓库；本仓库是独立的 Windows 实现。

## 状态

**移植进行中；核心逻辑 M0 已完成，Windows 原生采集 M1 正在实现。**

- 已完成：平台调研与方案；核心时间线/JSON/场景求值；WGC 屏幕与窗口采集、WASAPI 音频、全局指针轨、可选 MF 摄像头、硬件编码探针、十秒分段和 MF 无损合并/恢复（M1 仍需长时和跨硬件验收）。
- 尚未完成：WinUI 3 编辑器、D2D 合成/预览、完整编辑/导出工作流、长时 4K60 及跨设备/权限回归。

## 决策基线

| 项 | 决定 |
|---|---|
| 语言 | C++20（禁用 `/fp:fast`） |
| 编辑器 UI | WinUI 3（C++/WinRT） |
| HUD 窗口 | Win32 顶层窗口 + Direct2D 自绘 |
| 合成 | Direct2D + 少量 HLSL，预览与导出共用 `IFrameCompositor` |
| 采集 | Windows.Graphics.Capture + WASAPI + Media Foundation SinkWriter |
| 导出 | Media Foundation SourceReader → 自研时间线调度 → 合成 → SinkWriter |
| 最低系统 | Windows 11 24H2（build 26100+） |
| 工程互通 | **不做**，Windows 端为独立软件系统 |
| 自动更新 | 首版关闭 |
| 系统声音 | 仅整设备 loopback |
| iOS 设备录制 | 首版移除 |
| 光标素材 | 自绘 mac 观感素材包 |

## 文档

| 文件 | 内容 |
|---|---|
| [docs/00-project-overview.md](docs/00-project-overview.md) | macOS 现状基线：模块地图、状态机、项目包布局、采集/渲染/导出/UI |
| [docs/01-data-model-and-timeline.md](docs/01-data-model-and-timeline.md) | 持久化契约、时间线引擎、帧契约、动画算法 |
| [docs/02-platform-dependency-map.md](docs/02-platform-dependency-map.md) | 10 个硬阻塞点 + 四类 API 映射表 |
| [docs/03-ui-inventory.md](docs/03-ui-inventory.md) | 窗口所有权、状态与撤销、本地化、设计系统、UI 分区工作量 |
| [docs/11-architecture.md](docs/11-architecture.md) | 技术栈、仓库结构、模块映射、关键契约 |
| [docs/12-review-r1.md](docs/12-review-r1.md) | 复审 1：技术路线可行性 |
| [docs/13-review-r2.md](docs/13-review-r2.md) | 复审 2：进度可行性与验收粒度 |
| [docs/14-review-r3.md](docs/14-review-r3.md) | 复审 3：依赖顺序与不可逆决策 |
| [docs/15-final-plan.md](docs/15-final-plan.md) | **开工依据**：工作量、契约、阶段、验收标准、M0 任务 |
| [docs/16-open-questions.md](docs/16-open-questions.md) | 已决议的开放项记录 |
| [docs/10-plan-r1.md](docs/10-plan-r1.md) | 初版方案（已被 15 取代，保留作决策留痕） |

## 目录结构（规划）

```
core/            纯逻辑：模型/校验/时间线/动画/场景求值（对应 macOS RecorderCore，15,369 行）
render/          Direct2D 合成器
render/present/  DXGI 实时预览
capture/         WGC + WASAPI + MF 写入
export/          MF 解码 → 合成 → 编码
orchestration/   状态机 / 持久化 / 播放时钟
ui/              WinUI 3 编辑器 + Win32 HUD
platform/        Win32 封装、字体、偏好、JSON
```

## 许可

[MIT](LICENSE)。原项目版权归 神烦老狗 所有；Windows 实现同样以 MIT 发布。
