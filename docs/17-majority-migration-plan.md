# 大部分功能迁移计划（复审确认版）

> 复审日期：2026-10-24。计划经用户确认后执行。
>
> 目标不是把两个参考仓库拼接进本项目，而是在当前 C++20 + D2D + Tauri 架构上，完成可验收的 Windows 桌面 demo 编辑器闭环。

## 1. 已确认的外部参考

| 仓库 | 固定提交 | 许可 | 允许借鉴的范围 | 禁止直接带入的范围 |
|---|---|---|---|---|
| [xiaoying8666/dogscwindows](https://github.com/xiaoying8666/dogscwindows) | `3bdcf00d4eb12a107c093e98aac7a894aedd2843` | MIT | Electron 版项目库、编辑器状态、时间线 UI、预览/导出编排、selftest 组织 | Electron 主进程、WebCodecs/FFmpeg 依赖布局、整段 renderer UI |
| [adrian147258/dogsc](https://github.com/adrian147258/dogsc) | `72ee935ddc9ac479acfec44f41db536db998f50e` | MIT | WPF 版 Windows 采集边界、暂停/恢复、检查器、3D/效果交互、保存恢复和回归测试思路 | WPF 主进程、GPL FFmpeg/ffprobe 二进制、GPL 构建/打包配置 |

两个参考仓库只作为只读研究源，位于 `E:/Projects/Mid/reference-repos/`，不纳入本仓库构建。

### 1.1 许可证边界

- 两个仓库的应用源码均声明 MIT，可在保留版权/许可和适配记录的前提下重写或独立适配。
- `adrian147258/dogsc` 的 README 明确其随附 FFmpeg/ffprobe 是 Gyan 的 GPLv3 构建；本项目当前要求动态 FFmpeg/LGPL 分发边界。因此只借鉴其 C#/WPF 行为、数据契约和测试思路，**不复制其 FFmpeg 二进制、构建脚本或 GPL 依赖配置**。
- `xiaoying8666/dogscwindows` 的 Windows 适配使用 Electron、ffmpeg 和 WebCodecs；它不能替代本项目的 C++/Tauri native pipeline。只借鉴交互与验证流程。
- 所有适配来源、版权和变更边界记录在 `THIRD_PARTY_NOTICES.md`。

## 2. 完成定义

称为“桌面 demo 编辑器大部分功能完成”必须同时满足：

1. 屏幕/窗口/区域录制，麦克风/系统声音/摄像头可选，暂停恢复和异常恢复可验收；
2. 项目库、工程包原子保存、恢复、重命名、重新打开和自动保存可验收；
3. 分割、裁切、重排、变速、吸附、撤销/重做可验收；
4. 实时预览与导出共用 `FrameSceneEvaluator` 和 `IFrameCompositor`；
5. 光标、摄像头、背景、贴图、局部模糊/spotlight、运镜和主要画布参数可编辑；
6. MP4/M4A 导出可取消、失败不发布半成品、结果可重新打开并通过音画同步验收；
7. Tauri 桌面壳具备基础设置、权限/失败提示和中文工作流；
8. 每个阶段都有构建、测试、真实 GUI/媒体证据、工作日志、提交和推送。

明确不纳入首版完成定义：iOS 录制、自动更新、HDR 输出、macOS 工程互通以及发布法律审查本身。它们必须在遗留清单中保留。

## 3. 阶段路线

### P0：功能矩阵与来源审计

- 建立 `docs/18-feature-matrix.md`，逐项标记原版功能、当前状态、目标模块、参考来源和验收命令。
- 建立 `THIRD_PARTY_NOTICES.md`，记录仓库 URL、提交、许可和禁止复制范围。
- 将本计划与许可证限制写入 `docs/16-open-questions.md`。

完成标准：每个目标功能都有唯一归属模块、验收方式和明确的“不做/后置”状态。

### P1：项目生命周期与编辑器状态

- 项目库：枚举工程目录、最近项目、打开、重命名、另存；
- 工程包：原子保存、上一版本、恢复文件、自动保存；
- 编辑器状态：选中对象、播放头、脏状态、关闭确认；
- 主轨重排、吸附、撤销/重做快照栈；
- 借鉴 xiaoying 的 `library/editor/store/timeline` 分层和 adrian 的保存/恢复回归。

验收：打开→编辑→保存→关闭→重新打开，时间线字段、媒体引用和撤销结果保持一致。

### P2：实时预览与播放时钟

- SourceReader/媒体帧源；
- D3D11/DXGI 预览表面；
- `FrameSceneEvaluator` → `IFrameCompositor` → Tauri 预览桥；
- 播放、暂停、拖动、逐帧、背压和丢帧统计。

验收：60 Hz 播放掉帧低于 2%，拖动无撕裂，预览与离屏合成输出一致。

### P3：主要视觉功能

按当前核心模型优先接入：

- 光标平滑、点击波纹、静止隐藏；
- 摄像头布局、形状、镜像、阴影；
- 背景、圆角、描边、阴影；
- 局部模糊、spotlight/mosaic；
- 贴图、层级、出入场动画；
- 屏幕 3D 透视与运镜；
- 开场动画和场景预设。

验收：固定工程场景快照、WARP 像素 smoke、效果边界用例和 GUI 回归均通过。

### P4：导出闭环

- SourceReader 读取屏幕/摄像头/音频轨；
- `TimelineMediaPlan` 调度切段、变速和同步；
- D2D 合成；
- MF SinkWriter / 动态 FFmpeg fallback；
- MP4、M4A、分辨率、帧率、音量、取消和失败恢复。

验收：录制→编辑→预览→导出→重新打开；PTS 单调、音画同步、取消后无半成品发布。

### P5：采集与桌面外壳

- 显示器/窗口/区域选择器；
- 摄像头、麦克风、系统声音选择；
- 权限状态和失败引导；
- 暂停/恢复、录制标记、崩溃恢复；
- 设置、语言、托盘/菜单、完成页。

### P6：硬化与发布边界

- 4K60、混合显卡、受保护窗口、休眠恢复；
- 多设备音画同步；
- Release/NSIS；
- FFmpeg 源码、补丁、NOTICE 和 LGPL 材料；
- 逐项关闭功能矩阵或标记为未完成。

## 4. 执行规则

- 顺序：P0 → P1 → P2 → P3 → P4 → P5 → P6；阶段可在内部拆成多个小提交。
- 每个阶段先写验收标准，再改代码；不得用 UI 假数据替代 native/core 结果。
- 每个阶段必须独立构建、测试、真实 GUI/媒体验收，写 `docs/worklog/`，提交并推送。
- 外部实现只在适配边界内重写；核心时间线、JSON、场景求值仍以本项目实现为唯一真相。
- 未实测的性能、跨硬件和兼容性结论标 `[INFERENCE]`。
