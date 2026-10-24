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

安装包输出在 `src-tauri/target/release/bundle/nsis/`。当前切片包含录制参数、暂停/继续、停止保存、项目加载、分割、裁切、变速和删除主轨片段；重排仅由 native CLI 提供，实时预览、完整项目库、画布、检查器与完整导出仍未接入。
