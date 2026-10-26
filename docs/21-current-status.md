# DogSC for Windows：当前详细状态

最后更新：2026-10-26  
状态：**持续移植中；当前 Release 安装包可供用户侧体验，但不是已完成“完美复刻”或跨硬件认证的最终版。**

本报告的权威代码状态以本仓库 Git 提交与 `docs/18-feature-matrix.md` 为准；构建/运行命令见 `docs/20-acceptance-runbook.md`。硬件结果仅代表报告记录的机器和测试时长。

## 1. 当前基线

- 最近已知提交：`35d3678 feat: add timeline drag reorder`。
- 最近记录的用户缺陷修复：`ff8aba5 fix: repair recording panel and user interactions`。
- 当前可见安装包（若构建目录尚未被清理）：`apps/tauri/src-tauri/target/release/bundle/nsis/DogSC Studio_0.1.0_x64-setup.exe`。
- 该安装包是本机构建产物；不要把未重新核对哈希的旧安装包当成当前 commit 的产物。
- 项目定位：C++20/Win32 + Windows Graphics Capture + WASAPI + Media Foundation + Direct2D/WARP + DXGI + Tauri 2；Windows 工程不承诺与 macOS project 文件互通。

## 2. 能力分层

### 已有可运行实现

- 屏幕/窗口目标枚举与采集配置：显示器、窗口句柄、区域、帧率、编码器、系统声音、麦克风、摄像头。
- 原生录制：H.264/HEVC Media Foundation 编码器，ProRes 422 Standard FFmpeg 路径，分段、停止收尾和恢复工具。
- 项目模型与 JSON：schema 23–27、工程 inspect/edit、时间线片段、效果轨、历史/草稿/恢复。
- 编辑器壳：项目库、打开/重命名、split/trim/rate/move/remove、undo/redo、dirty draft、自动保存、恢复、marker。
- 预览和场景：SourceReader、FrameSceneEvaluator、D2D/WARP、sidecar DXGI swapchain；摄像头、cursor/click、background、screen frame、opening、screen motion、mosaic/spotlight、sticker、手动 zoom 与 pointer 点击聚类 auto-zoom。
- 效果编辑：zoom/mosaic/sticker clip 在 timeline 展示；可按 UUID 删除并走 native edit/history。
- 录制壳：常驻录制面板、输出路径默认值、开始/暂停/继续/停止；settings、权限向导、Windows 隐私设置入口、托盘、备忘录、区域 picker、部分简体中文/英文运行时切换。
- 音频：WASAPI 采集、AAC/M4A 导出、Tauri 预览音频缓存/播放、2× 变速 M4A 样本。
- 导出：单源 H.264 MP4 prototype、M4A、progress/cancel/partial cleanup 等已有切片。

### 已经按具体范围实际验收

- 原生 CTest：最近记录为 7/7 passed。
- H.264/HEVC 首帧/seek/stream/EOF/D2D composite CLI smoke。
- GUI 短时录制：输出 `capture.json` finalized、HEVC NVIDIA、60 帧、无非单调帧丢弃。
- GUI Pause/Resume/Stop：按钮状态和最终文件已实际验证。
- 预览 A/B child 生命周期、DXGI/D2D、move/resize、scroll hide/show、vblank telemetry、短时 0 drops/0 backpressure。
- Preview audio：GUI `<audio>.currentTime` 推进断言；有不兼容音轨的工程会显示错误，画面仍继续。
- 缩放：手动 zoom clip UI/native 与 auto-zoom native 命令样本已检查；实际 auto-zoom 点击事件样本生成 2 个自动 ZoomClip。
- 用户报告的录制面板被折叠、空输出目录、黑框图标、英文暂停标签错误已修复并做 GUI 复验。
- 当前移植版本支持 timeline segment 拖拽/pointer 重排；native `move` 顺序更改已实测。自动合成拖拽 GUI smoke 不稳定，不作为通过证据。

## 3. 尚未完整实现或验收的功能

### 产品功能差异

- 原版完整本地化资源尚未移植；当前英文只覆盖主要静态面板/按钮，动态错误、提示、更多 inspector 文案仍有中英混用风险。
- AutoZoomPlanner 仍非逐项复刻：Windows native auto-zoom 是基于 pointer 点击的简化时间/空间聚类；未实现完整 legacy track repair 和原版所有剪切/连续活动边界策略。
- Timeline 仍未提供完整的吸附、拖拽 trim/resize、效果轨拖拽排序/时长调整。
- 屏幕 3D 有可见几何输入，但逐像素导出对拍/PSNR 基准未完成。
- 素材导入与更广泛贴图格式、wallpaper 外部资源、真实系统光标素材集仍未完整。
- 权限向导提供枚举检查和系统设置链接；没有覆盖所有 Windows 权限/API 的正式状态查询和恢复 UX。
- 备忘录当前是按工程路径隔离的本机 localStorage 便笺，不等同于原版完整 memo 窗口/工程文档系统。

### 环境外或长期验收阻塞

- 4K60、AMD/Intel/第二 GPU、混合显卡、长时多轮、休眠恢复。
- 120Hz/VRR、多显示器 DPI/虚拟桌面、最小化/遮挡/长时背压矩阵。
- 实际显示器画面无撕裂的外部采集证据。DXGI `Present(1,0)`、`GetFrameStatistics` 与 vblank 数值不等于实际无撕裂证明；用户不安装 PresentMon 的决定保持有效。
- 麦克风真实资产、多轨组合、长时音画同步；单次 `currentTime` 推进仅证明短时预览音频播放。
- FFmpeg 9.0.2 源码与 vcpkg 补丁已有本地材料和哈希工具，安装包包含 MIT/第三方/FFmpeg notice；仍缺最终源码获取页面闭环、面向发行的可复现构建归档、DLL 可替换验证、完整依赖版权审计与法律/专利审查。

## 4. 最近用户缺陷修复

### 录制按钮看不见/不能点

- 旧录制设置是 `position: fixed` 的折叠浮层，默认隐藏 `.actions`，并可能覆盖时间线。
- 已改成工作区内常驻面板，采集控件和动作按钮在正常布局中显示。
- 可见构建中的开始按钮要求输出目录非空。

### 无法录制

- 空 `dogsc.preferences.directory` 曾覆盖默认输出目录。
- 现在启动初始化回退到 `Videos/DogSC`，设置保存也会保留已有有效目录。
- 开始命令增加非空路径校验；实际 GUI 录制成功生成 `screen.mp4`、`project.json`、`capture.json`。

### Pause/Resume 标签

- 现在使用独立暂停状态和当前 locale；English 标签 `Pause → Resume → Pause`，按钮写入失败时恢复/显示错误。

### 黑框

- 当前空预览图片隐藏 broken-image 图标；预览画布本身仍是深色工作区背景，工程尚未加载时显示该背景属于占位画布，不代表采集黑帧。

### 用户仍可反馈的问题

请提供：安装包 SHA-256、Windows build、GPU/驱动、操作步骤、屏幕录屏、输出目录的 `capture.json`、`project.json`、`screen.mp4` 的 ffprobe 结果。不要只以“全是 bug”作为可定位证据；本仓库不隐藏失败状态。

## 5. 当前可执行验收命令

从仓库根目录运行；更完整逐步说明见 `docs/20-acceptance-runbook.md`。

```powershell
# 构建/单测
cmake --build out/vcpkg-native --config Release --parallel
ctest --test-dir out/vcpkg-native -C Release --output-on-failure

# H.264/HEVC frame/seek/stream/EOF/D2D smoke
pwsh -NoProfile -ExecutionPolicy Bypass -File `
  tests/media_preview_smoke.ps1 `
  -Executable out/vcpkg-native/tools/capture_cli/Release/dogsc_capture.exe

# 真实 Tauri GUI：录制、settings、permission、region、scene、DXGI、audio
$env:DOGSC_EXERCISE_RECORDING = "1"
$env:DOGSC_EXERCISE_SETTINGS = "1"
$env:DOGSC_EXERCISE_REGION = "1"
$env:DOGSC_EXERCISE_SCENE_CONTROLS = "1"
$env:DOGSC_REQUIRE_DXGI_SURFACE = "1"
$env:DOGSC_EXERCISE_WINDOW = "1"
$env:DOGSC_REQUIRE_ZERO_DROPS = "1"
$env:DOGSC_REQUIRE_PREVIEW_AUDIO = "1"
node tests/stream_child_lifecycle_smoke.mjs `
  apps/tauri/src-tauri/target/release/dogsc-tauri.exe `
  out/smoke/p6-long-60s-20261025/project.json `
  out/smoke/camera-opt-in-final/project.json
```

构建面向用户的安装包：

```powershell
Set-Location apps/tauri
pnpm tauri:build
```

产物：`apps/tauri/src-tauri/target/release/bundle/nsis/DogSC Studio_0.1.0_x64-setup.exe`。安装前先关闭 DogSC 托盘和进程；建议卸载旧版本后再装新版本。

## 6. 可信度边界

“按钮可点击”“产物 finalized”“当前机短时 0 drops”是各自测试范围内的证据；它们不能外推成“所有硬件正常”“无撕裂”“4K60 已通过”或“完全复刻”。当前产品仍处于迁移中，所有 blocked 项继续列在 `docs/18-feature-matrix.md`。
