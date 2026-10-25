# DogSC Tauri Windows 壳

这是当前原生 C++ 采集管线的 Tauri v2 桌面壳首个切片。

## 前置条件

- Windows 11 24H2+
- Node.js 20+ 与 pnpm
- Rust stable、Visual Studio C++ 工具链、WebView2
- 先在仓库根目录构建 `dogsc_capture.exe`：

```powershell
cmake --build out/vcpkg-native --config Release --parallel
```

## 开发运行

```powershell
cd apps/tauri
pnpm install
pnpm prepare:sidecar
pnpm tauri:dev
```

`prepare:sidecar` 会把根目录 `out/vcpkg-native/tools/capture_cli/Release/` 下的 `dogsc_capture.exe` 和 FFmpeg DLL 复制到 Tauri sidecar 目录。不要把这些构建产物提交到 Git。

## 生成安装包

```powershell
pnpm prepare:sidecar
pnpm tauri:build
```

安装包输出在 `src-tauri/target/release/bundle/nsis/`。当前切片包含录制参数、暂停/继续、停止保存、项目库、项目打开/重命名、时间线分割/裁切/变速/重排/删除、多步撤销重做、dirty draft/自动保存/恢复、画布与检查器、光标/摄像头/贴图/遮罩/运镜/开场控制、SourceReader→D2D→DXGI 原生预览、受限 asset protocol 音频预览、MP4/M4A 导出、取消/失败清理、设置/权限入口、托盘和录制标记。长时 4K60/跨硬件、完整本地化、多轨音画同步和无撕裂外部证据仍未完成。
