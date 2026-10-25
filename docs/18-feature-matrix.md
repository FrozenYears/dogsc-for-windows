# 大部分功能迁移矩阵

状态：`已实现` = 当前代码存在；`已验收` = 有真实构建/测试/GUI/媒体证据；`部分` = 有核心或 CLI，但产品链路未闭合；`未实现` = 尚未接入。

| 领域 | 原版能力 | 当前 Windows 状态 | 目标模块 | 参考仓库借鉴点 | 验收方式 |
|---|---|---|---|---|---|
| Capture | 显示器录制 | 已验收短时 CLI/GUI | `src/capture` | xiaoying `recording.js`；adrian `CaptureEngine.cs` | 60 秒/4K60、PTS、恢复 |
| Capture | 窗口录制 | 部分，CLI 支持窗口句柄 | `screen_capture` + 选择器 | adrian 窗口边界与持续捕获 | 窗口移动/遮挡矩阵 |
| Capture | 区域录制 | 核心参数已存在，UI 未接入 | 选择器 + WGC | xiaoying area picker | GUI 选择区域并验收输出尺寸 |
| Capture | 系统声音/麦克风 | CLI 已接入，GUI 参数已接入 | `audio_capture` | 两仓库音频轨生命周期 | 独立音轨、暂停同步 |
| Capture | 摄像头 | CLI opt-in 已接入，编辑预览未接入 | `camera_capture` | adrian camera overlay | 设备首帧、镜像、断开恢复 |
| Capture | 指针轨/点击 | 指针轨已接入 | `pointer_capture` + core | xiaoying pointer recorder | 光标不入原始视频、JSONL 可重放 |
| Capture | 录制标记 | 未实现 | orchestration/project | xiaoying marker flow | 录制中标记→工程→编辑器 |
| Project | project.json 编解码 | 已验收 | `project_json` | 两仓库字段/保存语义 | 读写逐字段稳定 |
| Project | 工程库/最近项目 | P1a 已验收项目库枚举、打开、重命名；最近项目排序未实现 | Tauri + project service | xiaoying `library` | 枚举、打开、重命名、最近项目 |
| Project | 原子保存/previous/recovery | P1a/P1b 已验收 `.project-history` 多步版本、previous、原子替换、draft discard 和 recovery manifest；崩溃恢复清单的启动选择未完成 | project store | adrian save/recovery | 中断写入后恢复 |
| Project | 自动保存/关闭确认 | P1b 已验收：native JSON 语义 dirty 判定，clean begin 不伪造 recovery，save/discard 后可继续编辑；Tauri debounce、flush、两次重试，dirty close confirm/cancel；崩溃 draft/recovery manifest 保留 | orchestration/Tauri + project CLI | 两仓库 dirty-state | clean/no-op/recovery 边界、保存失败重试、dirty close cancel/save GUI smoke |
| Timeline | 分割/裁切/删除 | GUI 已验收 | core + Tauri | 两仓库 timeline controls | 重载后几何一致 |
| Timeline | 变速 | GUI 已验收 | core + Tauri | xiaoying speed regions | 0.5×…100×边界、音画同步 |
| Timeline | 重排/吸附 | P1a GUI 已验收重排；吸附未实现 | core + Tauri | adrian timeline drag | 非 no-op 顺序断言、吸附边界 |
| Timeline | 撤销/重做 | P1a GUI 已验收多步 history；关闭恢复未完成 | editor store + project history | 两仓库 command/history | 多步编辑 undo/redo |
| Timeline | 自动运镜/缩放 | core 求值已存在，UI/预览未接入 | frame tracks + inspector | 两仓库 auto-zoom | 点击分组、spring、剪切桥接 |
| Preview | 媒体解码 | P2a 部分已验收：MF SourceReader 首帧、0.1s seek、EOF 明确错误，H.264/HEVC 320×200 CLI smoke；P2b 增加 SourceReader→FrameSceneEvaluator→D2D/WARP 单帧合成并通过 GUI 0s；损坏媒体/音画同步/连续播放未完成 | `tools/capture_cli/frame.cpp` + D2D + Tauri | xiaoying frame source | 首帧、seek、EOF、坏帧 |
| Preview | 实时播放/拖动 | P2b/P4 partial：sidecar-owned WS_POPUP + DXGI flip-model swapchain/`Present(1,0)`；FrameSceneEvaluator/D2D 输出纹理由同设备 `CopyResource` 直达 backbuffer；主屏与辅助摄像头已开始使用 TimelineMediaPlan output/source 映射；单飞请求、呈现 FPS、丢帧、背压及 `GetFrameStatistics` 已接入；GUI smoke 覆盖 move/resize、scroll hide/show、跨工程回收，长时60/120Hz、VRR、多 GPU、no-tear、音画同步与完整暂停/seek未验收 | D3D11/DXGI + TimelineMediaPlan + playback clock | 两仓库 preview/transport | edited timeline seek、音画同步、长时 frame statistics、no-tear |
| Render | D2D/WARP 六层离屏 | 已实现，像素基准未完成 | `render/d2d` | 两仓库效果组织 | WARP 读回、PSNR 基准 |
| Render | 光标/点击效果 | P3 preview 与单源 H.264 export 已接入 pointer.jsonl、系统光标事件、确定性箭头 fallback、click geometry 与 D2D cursor layer；GUI cursor/click smoke 与 30 帧 export smoke 通过；真实系统光标素材、高清资源、资源装配共用化未完成 | cursor track + D2D compositor + Tauri/export | 两仓库 cursor renderer | 光标位置/热点/点击波纹、导出复用 |
| Render | 摄像头布局 | P3 preview 与单源 H.264 export 已接入可选 camera media、startOffset/sourceStartTime/sourceTimeScale 映射、FrameSceneEvaluator camera scene 与 D2D 圆角/边框/阴影/镜像；camera GUI smoke 与 2560×1600/30 帧 export smoke 通过；逐像素基准未完成 | MediaTimeline + camera reader + compositor/export | adrian camera panel | 形状/镜像/阴影/缩放、逐像素对拍 |
| Render | 背景/圆角/阴影/样机 | D2D background pattern、画布 padding、圆角/边框/阴影与 browserDark screen-frame 已接入 scene；screen-frame/opening GUI smoke 通过；屏幕 3D/运镜 UI 与跨 preset 像素基准未完成 | compositor | 两仓库 layout panels | 画布尺寸、裁切、颜色、screen chrome、导出复用 |
| Render | 模糊/spotlight/mosaic | P3 native D2D 已消费 evaluator 的 blur/spotlight 区域、强度与转场；spotlight GUI smoke 通过；mosaic 编辑 UI 与逐像素基准仍未完成 | compositor + Tauri | adrian Effects UI | 局部范围、过渡、随运镜、像素对拍 |
| Render | 贴图/层级/动画 | P3 preview/export 已接入安全相对路径 WIC PNG→D3D11 texture、sticker layer、透明度/圆角/阴影/动画求值；GUI sticker smoke 通过；素材导入 UI、更多格式与逐像素基准未完成 | compositor + WIC assets + Tauri/export | 两仓库 sticker UI | 素材路径安全、层级/出入场、导出复用 |
| Render | 屏幕 3D/开场 | opening evaluator、screen-frame scene、screen/cursor motion 与 opening preset/duration editor controls 已接入并通过 GUI smoke；screen 3D 透视控件、导出路径与动画像素基准未完成 | D2D + inspector | adrian screen3d | 透视矩阵、动画边界、UI 控件 |
| Export | SourceReader 时间线合成 | P4 partial：`TimelineMap` + `TimelineMediaPlan` 已驱动单源视频导出主屏分段/裁切输出域、摄像头 placement 与 system audio placement；edited timeline 3 帧/0.1s、AAC 48k/2ch/12s smoke 通过；预览 stream、麦克风、变速边界、取消/失败恢复与长时同步未完成 | export pipeline + TimelineMediaPlan + MediaWriter | xiaoying export.js；adrian VideoExporter | 分段/变速/摄像头/音频映射、重开 |
| Export | MP4/M4A 最终导出 | P4 partial：H.264 MP4 含 AAC system audio，独立 M4A CLI 与 Tauri GUI smoke 通过；可选 microphone 第二 AAC 轨代码已接入但无真实资产 smoke，M4A 取消/失败恢复与多轨完整验收未完成 | MF/FFmpeg dynamic | 两仓库 export cancel | MP4/M4A 可重开、取消无半成品 |
| Export | 进度/取消/重试 | P4 partial：progress JSON、`.partial` 原子收尾、Tauri 取消按钮与 sidecar `cleanup-output` 已接入并通过 cleanup smoke；失败重试与完整取消 GUI smoke 未完成 | Tauri orchestration + sidecar | adrian export dialog | 取消后无最终半成品、失败清理、重试 |
| Shell | Tauri 录制壳 | 已验收 | `apps/tauri` | xiaoying Electron UI 仅作交互参照 | 安装包真实 GUI |
| Shell | 中文基础界面 | 已实现部分 | Tauri | 两仓库布局/检查器 | UI smoke |
| Shell | 英文/本地化切换 | 未实现 | localization | xiaoying localization | 运行时切换 |
| Shell | 设置/权限/托盘/备忘录 | P5 partial：Tauri 启动真实枚举显示器/窗口，支持显示器/窗口源切换和区域参数透传；拖拽区域、权限向导、托盘/备忘录未完成 | Tauri/Win32 | adrian dialogs/memo | 权限失败、托盘、保存 |
| Release | FFmpeg 动态 LGPL | 当前配置部分满足 | CMake/vcpkg/package | 不复制 adrian GPL 包 | DLL、源码、补丁、NOTICE 审计 |
| Release | 长时/跨硬件 | 未完成 | capture/export | 两仓库测试矩阵思路 | 4K60、混合显卡、休眠恢复 |

## 参考实现边界

- `xiaoying8666/dogscwindows`：Electron + JavaScript + ffmpeg/WebCodecs；只借鉴项目库、时间线、编辑器交互和 selftest 组织，不能替代当前 C++/Tauri native pipeline。
- `adrian147258/dogsc`：C#/.NET/WPF Windows 适配；只借鉴 Windows 行为、效果/检查器状态、保存恢复和测试思路。
- adrian 仓库 README 明确随附 Gyan FFmpeg/ffprobe 为 GPLv3 构建；本项目不得复制其二进制、构建脚本或 GPL 依赖配置。当前项目仍必须使用动态 FFmpeg/LGPL 边界。
