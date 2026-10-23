# Windows 移植方案（第 1 轮草案）

> 目标：在**不破坏现有 macOS 产品**的前提下，交付一个 Windows 端可用的 DogSC：能录屏、能剪、能导出，工程文件与 macOS 端互通。

## 1. 技术选型（v1 假设）

| 层 | 选择 | 理由 |
|---|---|---|
| 语言 | C++20 | 直接对接 Win32/D3D11/D2D/Media Foundation，无运行时依赖，与 Swift 值语义可对位 |
| UI | WinUI 3 (C++/WinRT) | 原生 Windows 观感、多显示器 DPI 处理成熟、可做无边框浮动窗 |
| 渲染 | Direct2D + D3D11 | 覆盖 Core Image 用到的 14 种滤镜；透视需自定义 HLSL |
| 采集 | Windows.Graphics.Capture (WGC) | 显示器/窗口/区域统一入口，Win10 1903+ |
| 音频 | WASAPI（共享/环回）+ Media Foundation AAC | 系统声音 + 麦克风 |
| 编解码 | Media Foundation SinkWriter/SourceReader | H.264/HEVC/AAC |
| 工程文件 | 复用 JSON schema v27 | 与 macOS 互通 |

## 2. 分层

```
dogsc-win/
|- core/        纯逻辑：模型/校验/时间线/动画/场景求值（对应 RecorderCore）
|- render/      D2D 合成器（对应 SharedFrameRenderer）
|- capture/     WGC + WASAPI + MF 写盘
|- export/      MF 解码-合成-编码流水线
|- ui/          WinUI 3 外壳、编辑器、录制条、设置、引导
|- platform/    Win32 封装：窗口层级、DPI、全局钩子、托盘、字体、偏好
```

## 3. 里程碑

1. **M0 骨架**：core 移植 + JSON 往返 + `OutputFrameSchedule` 对拍。
2. **M1 采集**：WGC 录显示器 -> MP4（H.264）；系统声音 + 麦克风。
3. **M2 渲染**：D2D 合成器对齐 `FrameScene` 全部图层；离屏导出一帧与 mac 端像素对比。
4. **M3 编辑**：时间线 + 检查器 + 画布交互。
5. **M4 导出 + 工程互通**。
6. **M5 录制条 / 权限 / 引导 / 设置 / 托盘**。

## 4. 风险

- 透视与模糊在 D2D 上的语义差异（CI 的惰性 extent）。
- 系统光标与壁纸无等价资源。
- 全局钩子在 UIPI 下受限。
- WinUI 3 浮动窗层级不如 NSPanel 灵活。

## 5. 待定

- 是否保留 iOS 设备录制（需要私有协议）。
- 自动更新方案。
- 是否支持 Windows 10。
