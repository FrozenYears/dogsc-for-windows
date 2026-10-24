# Third-party notices

本文件记录本项目用于研究和重写适配的外部仓库。当前只读克隆，不把外部仓库加入 CMake、Tauri bundle 或运行时依赖。

## xiaoying8666/dogscwindows

- URL: <https://github.com/xiaoying8666/dogscwindows>
- Reviewed commit: `3bdcf00d4eb12a107c093e98aac7a894aedd2843`
- License: MIT（仓库 `LICENSE`；Windows 适配 README 也声明 MIT）
- Local research copy: `E:/Projects/Mid/reference-repos/dogscwindows`
- Intended use: 研究项目库、编辑器 store、时间线交互、预览/导出编排和 selftest 组织。
- Boundary: 其 Windows 目录使用 Electron、JavaScript、ffmpeg 和 WebCodecs；不复制 Electron 主进程、renderer 大段 UI、FFmpeg/WebCodecs 依赖布局，也不把它作为当前 C++/Tauri native pipeline 的实现。
- Attribution requirement: 若未来适配其可识别的代码片段，保留 MIT copyright/permission text，并在变更记录中注明文件来源和本地修改。

## adrian147258/dogsc

- URL: <https://github.com/adrian147258/dogsc>
- Reviewed commit: `72ee935ddc9ac479acfec44f41db536db998f50e`
- Application source license: MIT（仓库 `LICENSE`；README 声明原作和 Windows 改编均 MIT）
- Local research copy: `E:/Projects/Mid/reference-repos/adrian-dogsc`
- Intended use: 研究 Windows 采集行为、暂停/恢复、WPF 检查器与效果面板、保存恢复和测试组织。
- Boundary: 不复制 C#/WPF 主进程、窗口体系或整段 UI；核心时间线和场景解释仍以本项目 `src/core` 为唯一真相。
- **FFmpeg restriction**: 该仓库 README 明确随附 FFmpeg/ffprobe 为 Gyan 的 GPLv3 构建。不得复制其 FFmpeg/ffprobe 二进制、GPL 构建/打包脚本、依赖配置或发布布局。本项目继续使用动态 FFmpeg/LGPL 方案，并另行完成对应源码、补丁、NOTICE 和法律审查。
- Attribution requirement: 若未来适配其 MIT 应用代码片段，保留原始 MIT 版权/许可和本地修改说明；GPL FFmpeg 内容不适用 MIT 许可。

## Current project boundary

- 本仓库自身代码继续使用根目录 `LICENSE` 的 MIT 许可。
- 外部参考仅用于行为、数据契约、测试和 UI 交互研究；“功能相似”不等于代码复制许可。
- 外部字体、图标、模型或媒体资产如被引入，必须单独登记来源和许可证，不能由上述仓库的应用 MIT 声明自动覆盖。
