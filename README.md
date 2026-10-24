# DogSC for Windows

DogSC 的 Windows 原生移植。录屏、剪辑、运镜与导出，面向 Windows 11。

> 本项目源自 [laogou717/dogsc](https://github.com/laogou717/dogsc)（MIT，Copyright (c) 2026 神烦老狗）。原始 macOS 实现见上游仓库；本仓库是独立的 Windows 实现。

## 状态

**移植进行中：M0 核心基线完成；M1 原生采集主路径与 M2a 离屏渲染内核已实现，集成验收继续进行。**

- 已实现：C++20 工程模型/时间线求值及 JSON；WGC 屏幕与窗口采集、WASAPI、指针轨、可选 MF 摄像头、HEVC/H.264 硬编与 FFmpeg ProRes 422 Standard、10 秒分片及无损合并/恢复；D2D/WARP 完整六层离屏合成与几何、透视、效果输入。
- 已验证：Windows Release clean build，6/6 CTest；WARP 多层场景读回像素断言；ProRes 720p30/1080p30/1440p30 与 HEVC 1440p30 实录。ProRes 高分辨率实时性存在明显瓶颈，见本轮工作日志。
- 尚未完成：WinUI 3 编辑器、DXGI 实时预览、SourceReader 时间线导出工作流、长时 4K60 与跨设备/权限验收；ProRes 实时性能与 LGPL 分发合规仍未通过。

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

## Windows 采集依赖与分发

Windows 采集使用 vcpkg manifest 显式启用 FFmpeg 的 `avcodec`、`avformat`、`swresample` 与 `swscale` 功能；为兼容仅含 release archive、没有本地 Git 历史的 vcpkg 安装，manifest 不引用 `builtin-baseline`，FFmpeg 版本由开发者使用的 vcpkg registry revision 决定（因此依赖版本尚未锁定）。请使用动态 `x64-windows` triplet（或其他确认采用动态链接的 Windows triplet），不要启用默认功能、GPL 或 nonfree 功能。运行时必须随应用部署 FFmpeg DLL。

FFmpeg 为 LGPLv2.1-or-later。重新分发 FFmpeg DLL 时，必须同时提供与所发二进制完全对应的 FFmpeg 源码、所用补丁/差异及构建说明，并提供 FFmpeg 及相关库的许可和版权声明；源码应与应用二进制一同可获取。应用应保持动态链接和可替换的 FFmpeg DLL，不得以许可条款禁止用户为调试 FFmpeg 修改而进行反向工程；关于窗口、EULA 与下载页也须给出适当的 FFmpeg/LGPL 声明。发布前还须逐项核对 FFmpeg 官方 [LGPL 合规清单](https://ffmpeg.org/legal.html)，不能将本仓库的 vcpkg 配置视为已完成二进制分发合规。

LGPL 合规不代表专利许可。ProRes 与 AAC 相关专利状况取决于司法辖区及分发/使用场景；商业分发前需单独进行法律评估。

## 许可

[MIT](LICENSE)。原项目版权归 神烦老狗 所有；Windows 实现同样以 MIT 发布。
