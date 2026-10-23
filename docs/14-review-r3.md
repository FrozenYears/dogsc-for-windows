# 自我复审·第 3 轮：依赖顺序、并行度与不可逆决策

> 复审对象：前两轮修正后的阶段划分。方法：用模块间实际引用验证「谁必须先做」。基线 `567fd6e`。

## A. 依赖关系实测

| 关系 | 证据 | 结论 |
|---|---|---|
| `DogSCApp` 依赖 `RecorderCore` | `import RecorderCore` 在 App 内出现 **125** 次 | 核心层必须先行 |
| UI 依赖核心类型 | `FrameRenderPlan` 31、`TimelineMap` 28、`FrameScene` 26、`TimelineMediaPlan` 26、`ProjectTimelineEditing` 19、`ProjectValidator` 16 | UI 不能在核心未定时开工 |
| 渲染层不依赖采集层 | `SharedFrameRenderer`/`SharedFrameCursorRenderer`/`SharedFrameOverlayRenderer`/`SharedFrameCompositor` 对 `ScreenRecorder`/`CaptureOutput`/`CameraRecorder`/`MicrophoneRecorder` 命中 **0** | 渲染与采集可并行 |
| 渲染层不依赖 UI | 同上四文件对 `EditorView`/`EditorInspector`/`EditorTimeline`/`EditorCanvas` 命中 **0** | 渲染器可独立验收 |
| 导出层不依赖 UI | `DirectExportPipeline`/`VideoExporter`/`TimelineCompositionBuilder` 对 `EditorView`/`EditorInspector`/`EditorTimeline` 命中 **0** | 导出可在无 UI 情况下先跑通 |
| 采集层不依赖 UI | `ScreenRecorder`/`CameraRecorder`/`MicrophoneRecorder`/`PointerEventRecorder`/`ScreenCaptureOutput` 对 `EditorView`/`EditorInspector` 命中 **0** | 采集可独立验收 |
| UI 依赖渲染层 | 9 个文件引用 `SharedRenderedPreviewView`/`SharedPreviewFramePipeline`/`SharedFrameCompositor` | UI 必须在渲染后 |

## B. 第三轮发现的问题

### 问题 12（严重）：原计划的串行阶段浪费了实际存在的并行度

实测表明「采集」「渲染」「导出」三者互不依赖，只共同依赖核心层。原计划把它们排成 M1 → M2 → M4 串行，等于人为延长工期。

**修正**：M1（采集）、M2a（离屏合成）、M4a（导出管线，先不回放 UI）在 M0 完成后**并行推进**；M2b（实时预览）依赖 M2a；M3（编辑 UI）依赖 M2b + 编排层；M5 依赖 M3。

### 问题 13（严重）：编排层的时序不可逆性未在计划中体现

源码中有三处「一旦做错就无法事后补救」的时序契约：

1. 首帧 host-time 作为所有辅助轨的对齐基准（`AppModel.swift:669-710`）；
2. 停止时冻结一个共享 stop host-time（`AppModel.swift:737-760`）；
3. 指针轨必须早于摄像头/麦克风/屏幕写入启动（`AppModel.swift:565-580` 注释 CUR-001/PTR-001）。

这三条如果等到 UI 阶段才实现，会导致所有已录素材的时间轴错误，且无法通过后处理修复。

**修正**：把「时序契约」提升为 M1 的验收条件，而不是 M3 的 UI 细节。

### 问题 14（中）：Windows 侧「光标素材」需要提前定型

`CursorAssetID` 是**持久化**字段（`ZoomAnimation.swift:46-63`），录制时写入 `pointer.jsonl`。若 Windows 端的光标 ID 集合与 macOS 不一致，跨端打开工程会丢失光标选择。

**修正**：在 M0 定义跨端共享的 `CursorAssetID` 枚举（`automatic/systemArrow/touchDot/hidden` 至少保留），Windows 素材包按这些 ID 提供位图；不引入 Windows 专有 ID。

### 问题 15（中）：合成数值验证需要「无窗口」的自动化路径

`FrameSceneEvaluator.renderPlan` 是纯函数（`FrameScene.swift:487-560`），但 D2D 合成需要设备上下文。

**修正**：M2a 验收使用 **WARP 软件设备**（`D3D_DRIVER_TYPE_WARP`）跑离线合成，保证 CI 环境无 GPU 也能回归测试。

### 问题 16（低）：「撤销」在 Windows 端比 macOS 更简单，但需要早定

`EditorStore` 用单一 `UndoManager` 注册项目快照（`EditorStore.swift:1374-1381`），因为模型是值类型。Windows 端同样可以「快照栈」，不需要命令模式反向操作。该决策应写进 M3 输入，避免中途改设计。

## C. 三轮复审后的收敛结论

### C1. 必须修正的三处文档事实

1. `RecorderCore` 行数：15,369（非 24k）；
2. 几何类型风险只属于 `DogSCApp`，核心层是纯 `Double` 数学；
3. HUD 类窗口（录制条/完成窗/权限窗/摄像头预览/备忘录）必须走 Win32 自绘，不进 XAML。

### C2. 阶段最终形态

```
M0  核心层 + 对拍工具 + 时序契约定义      （无 UI、无 GPU 依赖）
 ├─ M1  采集链路（WGC/WASAPI/MF writer）  ┐
 ├─ M2a 离屏合成器（D2D + WARP 验收）     ├─ 并行
 └─ M4a 导出管线（MF reader/sink，无 UI）  ┘
      M2b 实时预览呈现（DXGI + 背压）
           M3 编辑 UI（画布/时间线/检查器）
                M4b 导出 UI + 工程互通
                     M5a 设置/托盘/菜单/本地化
                          M5b 引导与权限页
```

### C3. 不可延后的四件事

| 项 | 原因 |
|---|---|
| 时序契约（首帧对齐 / 共享 stop / 指针先行） | 事后无法修复素材时间轴 |
| `CursorAssetID` 跨端枚举 | 持久化字段，影响工程互通 |
| `FrameScene` 字段与图层顺序 | 预览/导出/对拍三处共用 |
| `OutputFrameSchedule` 时基与边界规则 | 决定成片帧数与音画同步 |

## D. 剩余未验证项（明确标注）

以下结论为**推断**，需在 M0 用实测取代：

- [INFERENCE] D2D `GaussianBlur` 与 `CIGaussianBlur` 在相同 radius 下的视觉效果差异（需像素对拍）；
- [INFERENCE] MF `IMFSinkWriter` 在 4K60 HEVC 下的稳定性与码率控制精度；
- [INFERENCE] WGC 在窗口模式下的帧率上限与 `IsCursorCaptureEnabled(false)` 的实际行为；
- [INFERENCE] WinUI 3 与 Win32 自绘窗口混排时的 DPI 切换表现。
