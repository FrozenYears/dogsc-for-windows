# DogSC Windows 实际测试与验收手册

本文是当前仓库的可重复验收入口。状态以 `docs/18-feature-matrix.md` 为准；根 `README.md` 的状态段包含较早快照，不作为最新验收结论。

## 1. 验收分级

- **通过**：命令/场景直接满足本手册的断言。
- **部分通过**：功能可运行，但仍有明确未覆盖边界，例如短时预览通过、长时同步未通过。
- **阻塞**：当前机器或工具无法提供直接证据，不得用间接输出替代。

当前环境最低要求：Windows 11 24H2（build 26100+）、WebView2、Visual Studio C++ 工具链、Rust stable、Node.js 20+、pnpm、PowerShell 7；媒体验收还需要 `ffprobe` 在 `PATH` 中。

## 2. 构建与原生测试

在仓库根目录执行：

```powershell
cmake --build out/vcpkg-native --config Release --parallel
ctest --test-dir out/vcpkg-native -C Release --output-on-failure
```

通过条件：`100% tests passed, 0 tests failed out of 7`。这覆盖核心模型、采集时钟、场景几何、时间线编辑、FrameScene、D2D/WARP 和项目历史；不覆盖真实屏幕撕裂、跨 GPU 或长时采集。

媒体解码/合成 smoke：

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File `
  tests/media_preview_smoke.ps1 `
  -Executable out/vcpkg-native/tools/capture_cli/Release/dogsc_capture.exe
```

通过条件：输出 `H264/HEVC frame, seek, stream, EOF and D2D composite passed`。脚本实际检查首帧、0.1 秒 seek、连续 stream、EOF 错误、PNG 签名、DXGI/system-memory telemetry 和 composite frame。

## 3. Tauri Release / 安装包

```powershell
cmake --build out/vcpkg-native --config Release --parallel
Set-Location apps/tauri
pnpm install
pnpm tauri:build
Set-Location ../..
```

检查：

- `apps/tauri/src-tauri/target/release/dogsc-tauri.exe` 存在。
- `apps/tauri/src-tauri/target/release/bundle/nsis/` 下生成 NSIS 安装包。
- 安装器安装后，安装目录包含 `LICENSE`、`THIRD_PARTY_NOTICES.md`、`FFMPEG-LGPL-NOTICE.txt` 和 FFmpeg DLL。
- 从 Windows“应用和功能”卸载后，上述资源被清理。

记录安装包哈希：

```powershell
Get-ChildItem apps/tauri/src-tauri/target/release/bundle/nsis -File |
  Get-FileHash -Algorithm SHA256
```

刷新 FFmpeg provenance 清单：

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File `
  tools/release/collect-ffmpeg-manifest.ps1 `
  -SourceArchive out/release/ffmpeg-n9.0.2.tar.gz `
  -InstallerPath "apps/tauri/src-tauri/target/release/bundle/nsis/DogSC Studio_0.1.0_x64-setup.exe" `
  -PatchDirectory out/tools/vcpkg-src/ports/ffmpeg `
  -MaterialsArchivePath out/release/ffmpeg-materials-9.0.2.zip
```

## 4. 原生采集手工验收

先检查设备与编码器：

```powershell
$out = "out/vcpkg-native/tools/capture_cli/Release/dogsc_capture.exe"
& $out list
& $out encoders
```

逐项记录：显示器尺寸、窗口枚举、可用硬件编码器、激活失败的硬件编码器和软件 fallback。不要把“枚举成功”当作“编码成功”。

短时录制：

```powershell
Remove-Item out/smoke/manual-10s -Recurse -Force -ErrorAction SilentlyContinue
& $out record out/smoke/manual-10s `
  --monitor 0 --fps 60 --codec hevc --duration 10 --system-audio
```

检查：

```powershell
Get-Content out/smoke/manual-10s/capture.json
ffprobe -v error -show_streams -show_format -of json out/smoke/manual-10s/screen.mp4
```

验收点：

- `status` 为 `finalized`。
- `videoFrames = 600`。
- `discardedNonMonotonicFrames = 0`。
- 有系统音频时为 AAC、48 kHz、2 声道。
- 视频/音频容器时长接近 10 秒。
- 运行中实际按键 `P` 暂停/继续，`Q` 停止；检查输出时间线没有倒退。

## 5. 可重复 P6 长时矩阵

脚本会生成 `p6-matrix-report.json`，并自动检查 finalized、输出帧数、非单调丢帧、重复帧阈值、音频 discontinuity 和 ffprobe 结果。

当前 NVIDIA 机器的 HEVC 基线：

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File `
  tests/p6-capture-matrix.ps1 `
  -Executable out/vcpkg-native/tools/capture_cli/Release/dogsc_capture.exe `
  -OutputDirectory out/smoke/p6-manual-60s `
  -FrameRate 60 -DurationSeconds 60 -Codec hevc -SystemAudio `
  -MaxRepeatedFrames 1000 -MaxAudioDiscontinuities 0
```

报告必须单独保留 `capturedFrames`、`repeatedFrames`、`discardedNonMonotonicFrames` 和 `systemAudioDiscontinuities`。输出帧数正确不等于采集端没有重复帧。

可对 H.264 重复：

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File `
  tests/p6-capture-matrix.ps1 `
  -Executable out/vcpkg-native/tools/capture_cli/Release/dogsc_capture.exe `
  -OutputDirectory out/smoke/p6-manual-h264-60s `
  -FrameRate 60 -DurationSeconds 60 -Codec h264 -SystemAudio `
  -MaxRepeatedFrames 1000 -MaxAudioDiscontinuities 1
```

当前机器曾观察到 H.264 60 秒样本有 1 次 audio discontinuity；若产品要求严格为 0，应让脚本失败并作为缺陷处理，不能静默放宽阈值。

## 6. GUI 自动验收

推荐使用已有样本：

- 工程 A：`out/smoke/p6-long-60s-20261025/project.json`，含 60 秒系统音频。
- 工程 B：`out/smoke/camera-opt-in-final/project.json`，用于摄像头/跨工程 child 生命周期。

从仓库根目录执行完整短时 GUI smoke：

```powershell
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

通过条件：

- 工程 A/B 的 stream child 正确切换，A child 在 B child 启动前退出。
- DXGI swapchain、D2D scene、vblank telemetry 出现。
- move/resize 后仍能呈现。
- 滚动隐藏/恢复后仍能呈现。
- `丢帧 0`、`背压 0`。
- 工程 A 预览音频 `<audio>.currentTime` 在 400ms 后前进且没有 paused。
- settings capability check、Windows 隐私设置入口、权限向导、备忘录自动保存、English 标签、区域选择器、可见 screen-motion 3D geometry 均通过。
- 工程 B 如果没有可兼容的音频轨，允许显示音频失败原因，但不能阻断视频画面与 child lifecycle；这不是音频通过证据。

若需要保存 GUI 截图：

```powershell
$env:DOGSC_SCREENSHOT = "$PWD/out/smoke/gui-acceptance.png"
node tests/stream_child_lifecycle_smoke.mjs `
  apps/tauri/src-tauri/target/release/dogsc-tauri.exe `
  out/smoke/p6-long-60s-20261025/project.json `
  out/smoke/camera-opt-in-final/project.json
Remove-Item Env:DOGSC_SCREENSHOT
```

场景层专项：

```powershell
$env:DOGSC_REQUIRE_CURSOR = "1"
$env:DOGSC_REQUIRE_STICKERS = "1"
$env:DOGSC_REQUIRE_SPOTLIGHT = "1"
$env:DOGSC_REQUIRE_SCREEN_FRAME = "1"
$env:DOGSC_REQUIRE_OPENING = "1"
$env:DOGSC_REQUIRE_SCREEN_MOTION = "1"
$env:DOGSC_EXERCISE_SCENE_CONTROLS = "1"
$env:DOGSC_REQUIRE_DXGI_SURFACE = "1"
node tests/stream_child_lifecycle_smoke.mjs `
  apps/tauri/src-tauri/target/release/dogsc-tauri.exe `
  out/vcpkg-native/p2-frame-smoke/project.json `
  out/smoke/camera-opt-in-final/project.json
```

执行后清除本轮环境变量，避免污染下一次测试：

```powershell
Get-ChildItem Env:DOGSC_* | Remove-Item
```

## 7. GUI 手工验收清单

按以下顺序操作并录屏/截图留证：

1. **采集**：显示器源、窗口源、区域拖拽、帧率、编码器、系统声音、麦克风、摄像头；开始/暂停/继续/停止。
2. **项目库**：刷新、打开、重命名；关闭 dirty 工程，分别验证保存、放弃、取消；重启后验证 recovery。
3. **时间线**：split、左右 trim、rate、move up/down、remove、undo/redo、marker；每次检查工程 JSON 与 UI 顺序。
4. **画面**：背景、padding、screen frame、camera shape/size/hidden、cursor effect、mosaic/spotlight、sticker、opening、screen-motion 3D geometry。
5. **预览**：播放/暂停、输入时间 seek、窗口移动/缩放、滚动隐藏/恢复、工程 A/B 切换；记录状态栏的 surface、FPS、vblank、呈现/丢帧/背压。
6. **音频**：有系统音频工程播放时确认 audio currentTime 前进；2× 工程导出 M4A 后用 ffprobe 核对 30 秒、48 kHz、2 声道。
7. **导出**：MP4、M4A、进度、取消、无效路径、失败清理、重新打开；不得留下最终半成品或未预期 `.partial`。
8. **壳层**：中英文切换、权限向导、备忘录自动保存、托盘恢复/退出、设置持久化。
9. **安装包**：安装、启动、采集、导出、卸载；检查 LICENSE、第三方声明、FFmpeg notice 和 DLL。

## 8. 当前不能宣告通过的项目

以下结果即使上述命令通过，也必须单独标为 blocked：

- 实际屏幕无撕裂：`Present(1,0)` 和 `GetFrameStatistics` 只能提供呈现诊断，不能证明屏幕实际无撕裂。
- 120Hz、VRR、多 GPU、最小化和长时刷新。
- 4K60、AMD/Intel/第二 GPU、休眠恢复。
- 麦克风真实资产、多轨音频和长时音画误差。
- FFmpeg DLL 可替换性验证、最终构建复现归档和法律/专利审查。

验收报告至少附：Git commit、OS/GPU/显示器信息、完整命令、原始 stdout/stderr、`capture.json`、`p6-matrix-report.json`、ffprobe JSON、GUI 截图/录屏和失败项说明。
