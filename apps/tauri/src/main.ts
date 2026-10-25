import { homeDir, join } from "@tauri-apps/api/path";
import { Command } from "@tauri-apps/plugin-shell";
import { getCurrentWindow } from "@tauri-apps/api/window";
import { invoke } from "@tauri-apps/api/core";
import "./styles.css";

type CaptureChild = {
  write: (input: string | Uint8Array) => Promise<void>;
  kill: () => Promise<void>;
};

const app = document.querySelector<HTMLDivElement>("#app");
if (!app) throw new Error("应用根节点不存在");

let recordingChild: CaptureChild | null = null;
let recordingStartedAt = 0;
let timer: number | undefined;

type ProjectSegment = {
  id: string;
  sourceStart: number;
  sourceDuration: number;
  playbackRate: number;
  outputStart: number;
  outputEnd: number;
};

type ProjectDocument = {
  title: string;
  version: number;
  canvas?: { screenFrame?: string };
  motion?: { screen?: string; cursor?: string };
  openingSequence?: { isEnabled?: boolean; preset?: string; duration?: number };
};
type ProjectSnapshot = {
  project: ProjectDocument;
  fullSourceDuration: number;
  outputDuration: number;
  segments: ProjectSegment[];
  history: { canUndo: boolean; canRedo: boolean };
  session: { dirty: boolean; hasRecovery?: boolean };
};

type ProjectLibraryItem = {
  name: string;
  path: string;
  title?: string;
  createdAt?: string;
  duration?: number;
  error?: string;
};

type CaptureTargets = { monitors: Array<{ index: number; device: string; width: number; height: number; primary: boolean }>; windows: Array<{ hwnd: number; title: string; minimized: boolean }> };
type ProjectLibrary = { root: string; projects: ProjectLibraryItem[] };
type PreviewFrame = { time: number; width: number; height: number; mime?: string; data?: string; decodeSurface?: "DXGI" | "system-memory"; dxgiManager?: boolean; presentation?: "dxgi" | "image"; presented?: boolean; composed?: boolean; camera?: boolean; cursor?: boolean; click?: boolean; spotlight?: boolean; stickers?: boolean; screenFrame?: boolean; opening?: boolean; screenMotion?: boolean; compositionError?: string; presentedFrames?: number; droppedFrames?: number; frameStatistics?: { presentCount: number; presentRefreshCount: number; syncRefreshCount: number; syncQpcTime: number } };
let exportChild: CaptureChild | null = null;
type PreviewImageFrame = PreviewFrame & { mime: string; data: string };

let projectSnapshot: ProjectSnapshot | null = null;
let projectFile = "";
let projectLibrary: ProjectLibrary | null = null;
let previewChild: CaptureChild | null = null;
let previewBuffer = "";
let previewGeneration = 0;
let previewStarting = false;
let previewFrameChain = Promise.resolve();
let previewFramesReceived = 0;
let previewFramesPresented = 0;
let previewFramesDropped = 0;
let previewBackpressureSkips = 0;
const previewPresentationTimes = new Float64Array(30);
let previewPresentationCursor = 0;
let previewPresentationCount = 0;
let latestPreviewFrame: PreviewFrame | null = null;
let previewOverlayMode = false;
let previewOverlayBounds = "";
let previewWriteChain = Promise.resolve();
let previewOverlayUpdateScheduled = false;

const defaultDirectory = async () => join(await homeDir(), "Videos", "DogSC");

app.innerHTML = `
  <main class="shell">
    <header class="app-toolbar">
      <div class="traffic-lights" aria-hidden="true"><i></i><i></i><i></i></div>
      <div class="toolbar-brand"><span class="brand-mark">DS</span><strong>DogSC</strong><span class="toolbar-separator">/</span><span id="project-title">未打开工程</span></div>
      <div class="toolbar-center"><button class="toolbar-tab active" type="button">编辑</button><button class="toolbar-tab" type="button" disabled>录制</button></div>
      <div class="toolbar-actions"><span class="system-chip"><i></i> Native</span><button id="settings-button" class="icon-button" type="button" title="设置">⚙</button></div>
    </header>
    <div class="app-body">
      <aside class="rail" aria-label="主导航">
        <nav>
          <button class="rail-item active" type="button" title="编辑"><span class="rail-icon">▣</span><span>编辑</span></button>
          <button id="rail-projects" class="rail-item" type="button" title="项目"><span class="rail-icon">▤</span><span>项目</span></button>
          <button class="rail-item" type="button" disabled title="素材"><span class="rail-icon">◫</span><span>素材</span></button>
        </nav>
        <div class="rail-foot"><span class="led"></span><span id="status-text">READY</span></div>
      </aside>
      <section class="workspace">
        <section class="panel capture-panel">
          <div class="panel-heading"><span class="section-kicker">录制设置</span><span class="panel-index">CAPTURE</span></div>
          <div class="capture-grid">
            <label class="field"><span>输出目录</span><input id="directory" type="text" spellcheck="false" /></label>
            <label class="field"><span>采集源</span><select id="capture-mode"><option value="monitor">显示器</option><option value="window">窗口</option></select></label>
            <label class="field"><span>显示器</span><select id="monitor"><option value="0">正在枚举…</option></select></label>
            <label class="field"><span>窗口</span><select id="window-target" disabled><option value="">没有窗口</option></select></label>
            <label class="field"><span>帧率</span><select id="fps"><option>30</option><option selected>60</option><option>90</option><option>120</option></select></label>
            <label class="field"><span>编码器</span><select id="codec"><option value="auto" selected>自动选择</option><option value="hevc">HEVC</option><option value="h264">H.264</option><option value="prores">ProRes 422</option></select></label>
            <label class="field capture-wide"><span>区域（x,y,width,height）</span><div class="region-control"><input id="region" type="text" spellcheck="false" placeholder="整块显示器" /><button id="region-pick" class="secondary compact" type="button">选择区域</button></div></label>
            <label class="field"><span>时长（秒）</span><input id="duration" type="number" min="0" step="1" placeholder="手动停止" /></label>
          </div>
          <div class="toggle-row"><label><input id="system-audio" type="checkbox" checked /><span class="toggle"></span><span>系统声音</span></label><label><input id="microphone" type="checkbox" /><span class="toggle"></span><span>麦克风</span></label><label><input id="camera" type="checkbox" /><span class="toggle"></span><span>摄像头</span></label></div>
          <div class="actions"><button id="start" class="primary" type="button"><span class="record-glyph"></span>开始录制</button><button id="pause" class="secondary" type="button" disabled>暂停</button><button id="stop" class="secondary danger" type="button" disabled>停止并保存</button></div>
          <div class="capture-status"><strong id="elapsed">00:00</strong><span id="backend">WGC + MF</span><span id="console" class="console" aria-live="polite"><span class="console-line muted">等待录制任务……</span></span></div>
        </section>
        <section class="editor-panel">
          <header class="editor-toolbar">
            <div class="project-open-row"><label class="field"><span>工程</span><input id="project-path" type="text" spellcheck="false" placeholder="打开 project.json 或工程目录" /></label><button id="project-open" class="secondary" type="button">打开</button></div>
            <div class="canvas-tools"><button class="toolbar-pill active" type="button">16:9</button><button class="toolbar-pill" type="button">裁切</button><button class="toolbar-pill" type="button">适应</button><span class="toolbar-divider"></span><span class="zoom-label">100%</span><button class="icon-button small" type="button">−</button><button class="icon-button small" type="button">+</button></div>
            <div class="editor-toolbar-meta"><span id="project-save-state">未打开</span><input id="project-rename-name" class="rename-input" type="text" spellcheck="false" placeholder="工程名称" /><button id="project-rename" class="secondary" type="button" disabled>重命名</button></div>
          </header>
          <div class="editor-grid">
            <main class="canvas-column">
              <div class="canvas-stage">
                <div class="canvas-dots"></div>
                <div class="project-preview"><img id="project-preview-image" alt="工程预览帧" /><div class="project-preview-actions"><button id="project-preview-frame" class="secondary" type="button" disabled>预览当前帧</button><span class="preview-hint">播放控制在时间线</span><span id="project-preview-status">未加载预览</span></div></div>
              </div>
              <div class="canvas-footer"><span>画布</span><span>·</span><span id="project-duration">—</span><span>·</span><span id="project-segment-count">— 片段</span></div>
            </main>
            <aside class="inspector">
              <div class="inspector-tabs"><button class="inspector-tab active" type="button">开场</button><button class="inspector-tab" type="button">画布</button><button class="inspector-tab" type="button">效果</button></div>
              <div class="inspector-section project-library-section"><div class="inspector-heading"><strong>项目</strong><button id="library-refresh" class="icon-button small" type="button">↻</button></div><label class="field"><span>项目库目录</span><input id="library-root" type="text" spellcheck="false" placeholder="D:\\Videos\\DogSC" /></label><div id="project-library" class="project-library"><span class="timeline-empty">刷新后列出工程</span></div></div>
              <div class="inspector-section"><div class="inspector-heading"><strong>画面</strong><button id="style-apply" class="secondary compact" type="button" disabled>应用</button></div><div class="inspector-fields"><label class="field"><span>背景</span><select id="style-background"><option>黑曜石网格</option><option>工程白网格</option><option>暗夜星阵</option><option>建筑极简点</option><option>立体等角网</option></select></label><label class="field"><span>留白</span><input id="style-padding" type="number" min="0" max="1000" step="1" value="100" /></label><label class="field"><span>样机</span><select id="style-screen-frame"><option value="none">无</option><option value="windowDark">深色窗口</option><option value="browserDark">深色浏览器</option><option value="browserLight">浅色浏览器</option><option value="devicePhonePortrait">手机竖屏</option><option value="deviceTabletLandscape">平板横屏</option></select></label><label class="field"><span>屏幕运镜</span><select id="style-screen-motion"><option value="聚焦">聚焦</option><option value="平滑">平滑</option></select></label><label class="field"><span>光标运动</span><select id="style-cursor-motion"><option value="平滑">平滑</option><option value="适中">适中</option><option value="快速">快速</option><option value="无">无</option></select></label><label class="field"><span>摄像头尺寸</span><input id="style-camera-size" type="number" min="0.05" max="1" step="0.01" value="0.29" /></label><label class="field"><span>摄像头形状</span><select id="style-camera-shape"><option value="方形">方形</option><option value="横向">横向</option><option value="纵向">纵向</option><option value="圆形">圆形</option><option value="原始比例">原始比例</option></select></label><label class="camera-hidden-toggle"><input id="style-camera-hidden" type="checkbox" /><span>隐藏摄像头</span></label></div></div>
              <div class="inspector-section"><div class="inspector-heading"><strong>开场</strong></div><div class="inspector-fields"><label class="camera-hidden-toggle"><input id="style-opening-enabled" type="checkbox" /><span>开场动画</span></label><label class="field"><span>样式</span><select id="style-opening-preset"><option value="轻 3D">轻 3D</option><option value="聚拢">聚拢</option><option value="侧滑">侧滑</option></select></label><label class="field"><span>时长</span><input id="style-opening-duration" type="number" min="0.18" max="10" step="0.1" value="2.2" /></label></div></div>
              <div class="inspector-section"><div class="inspector-heading"><strong>光标</strong></div><div class="inspector-fields"><label class="field"><span>点击效果</span><select id="style-cursor-effect"><option value="ripple">波纹</option><option value="glow">光晕</option><option value="pulse">脉冲</option><option value="burst">爆发</option><option value="none">关闭</option></select></label><label class="field"><span>透明度</span><input id="style-cursor-opacity" type="number" min="0.05" max="1" step="0.05" value="0.8" /></label><label class="field"><span>缩放</span><input id="style-cursor-scale" type="number" min="0.4" max="3" step="0.1" value="1" /></label><label class="field"><span>尺寸</span><input id="style-cursor-size" type="number" min="0.25" max="6" step="0.05" value="1.25" /></label></div></div>
              <div class="inspector-section"><div class="inspector-heading"><strong>遮罩</strong><button id="mosaic-apply" class="secondary compact" type="button" disabled>添加</button></div><div class="inspector-fields"><label class="field"><span>开始</span><input id="mosaic-start" type="number" min="0" step="0.01" value="0" /></label><label class="field"><span>时长</span><input id="mosaic-duration" type="number" min="0.01" step="0.01" value="1" /></label><label class="field"><span>样式</span><select id="mosaic-style"><option value="blur">模糊</option><option value="spotlight">聚光</option></select></label><label class="field"><span>强度</span><input id="mosaic-intensity" type="number" min="0" max="1" step="0.05" value="0.55" /></label><label class="field"><span>X/Y/W/H</span><input id="mosaic-rect" type="text" value="0.35,0.39,0.30,0.22" /></label></div></div>
              <div class="inspector-section"><div class="inspector-heading"><strong>贴图</strong><button id="sticker-apply" class="secondary compact" type="button" disabled>添加</button></div><div class="inspector-fields"><label class="field"><span>相对路径</span><input id="sticker-path" type="text" spellcheck="false" placeholder="assets/logo.png" /></label><label class="field"><span>开始</span><input id="sticker-start" type="number" min="0" step="0.01" value="0" /></label><label class="field"><span>时长</span><input id="sticker-duration" type="number" min="0.01" step="0.01" value="1" /></label><label class="field"><span>X/Y/W</span><input id="sticker-geometry" type="text" value="0.5,0.5,0.38" /></label></div></div>
            </aside>
          </div>
          <section class="timeline-panel">
            <div class="timeline-controls"><button class="icon-button small" type="button">◀</button><button id="project-preview-play" class="play-button" type="button" disabled>▶</button><button class="icon-button small" type="button">▶</button><span class="timecode"><input id="edit-time" type="number" min="0" step="0.01" placeholder="0.00" /> / <span class="timeline-duration">—</span></span><span class="timeline-spacer"></span><label class="field timeline-format"><span>格式</span><select id="project-export-format"><option value="mp4">MP4 视频</option><option value="m4a">M4A 音频</option></select></label><label class="field timeline-path"><span>导出</span><input id="project-export-path" type="text" spellcheck="false" /></label><button id="project-export" class="secondary compact" type="button" disabled>导出</button><button id="project-export-cancel" class="secondary danger compact" type="button" disabled>取消</button></div>
            <div class="timeline-ruler"><span>0:00</span><span>0:05</span><span>0:10</span><span>0:15</span><span>0:20</span><span>0:25</span><span>0:30</span><span>0:35</span><span>0:40</span><span>0:45</span><span>0:50</span><span>0:55</span><span>1:00</span></div>
            <div class="timeline-head"><strong>时间线</strong><span class="timeline-save-label">保存状态在窗口顶部</span><div class="project-history-actions"><button id="edit-marker" class="secondary compact" type="button" disabled>标记</button><button id="project-save" class="secondary compact" type="button" disabled>保存</button><button id="project-discard" class="secondary danger compact" type="button" disabled>放弃</button><button id="project-undo" class="secondary compact" type="button" disabled>撤销</button><button id="project-redo" class="secondary compact" type="button" disabled>重做</button></div></div>
            <div id="timeline" class="timeline"><span class="timeline-empty">打开工程后开始编辑</span></div>
            <div class="timeline-edit-row"><label class="field"><span>当前片段</span><select id="edit-segment"><option value="">未选择</option></select></label><label class="field"><span>速度</span><input id="edit-rate" type="number" min="1" max="100" step="0.1" value="1" /></label><div class="editor-actions"><button id="edit-split" class="secondary compact" type="button" disabled>分割</button><button id="edit-trim-left" class="secondary compact" type="button" disabled>左裁切</button><button id="edit-trim-right" class="secondary compact" type="button" disabled>右裁切</button><button id="edit-rate-button" class="secondary compact" type="button" disabled>应用速度</button><button id="edit-move-up" class="secondary compact" type="button" disabled>上移</button><button id="edit-move-down" class="secondary compact" type="button" disabled>下移</button><button id="edit-remove" class="secondary danger compact" type="button" disabled>删除</button></div></div>
            <div class="timeline-actions"><label class="field"><span>运镜</span><input id="screen-motion-start" type="hidden" value="0" /><input id="screen-motion-duration" type="hidden" value="1" /><input id="screen-motion-geometry" type="hidden" value="0.5,0.5,1.2,0,0,0,0.65" /></label><button id="screen-motion-apply" class="secondary compact" type="button" disabled>添加运镜</button><span id="project-console" class="project-console" aria-live="polite"><span class="console-line muted">等待打开工程……</span></span></div>
          </section>
        </section>
      </section>
    </div>
  </main>
`;

const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;
const directory = $("directory") as HTMLInputElement;
const monitor = $("monitor") as HTMLSelectElement;
const region = $("region") as HTMLInputElement;
const captureMode = $("capture-mode") as HTMLSelectElement;
const windowTarget = $("window-target") as HTMLSelectElement;
const regionPick = $("region-pick") as HTMLButtonElement;
const fps = $("fps") as HTMLSelectElement;
const codec = $("codec") as HTMLSelectElement;
const duration = $("duration") as HTMLInputElement;
const systemAudio = $("system-audio") as HTMLInputElement;
const microphone = $("microphone") as HTMLInputElement;
const camera = $("camera") as HTMLInputElement;
const start = $("start") as HTMLButtonElement;
const pause = $("pause") as HTMLButtonElement;
const stop = $("stop") as HTMLButtonElement;
const statusText = $("status-text");
const elapsed = $("elapsed");
const consoleOutput = $("console");
const projectPathInput = $("project-path") as HTMLInputElement;
const projectOpen = $("project-open") as HTMLButtonElement;
const projectTitle = $("project-title");
const projectDuration = $("project-duration");
const projectSegmentCount = $("project-segment-count");
const timeline = $("timeline");
const editTime = $("edit-time") as HTMLInputElement;
const editSegment = $("edit-segment") as HTMLSelectElement;
const editRate = $("edit-rate") as HTMLInputElement;
const editSplit = $("edit-split") as HTMLButtonElement;
const editTrimLeft = $("edit-trim-left") as HTMLButtonElement;
const editTrimRight = $("edit-trim-right") as HTMLButtonElement;
const editRateButton = $("edit-rate-button") as HTMLButtonElement;
const editRemove = $("edit-remove") as HTMLButtonElement;
const editMarker = $("edit-marker") as HTMLButtonElement;
const projectConsole = $("project-console");
const railProjects = $("rail-projects") as HTMLButtonElement;
const libraryRoot = $("library-root") as HTMLInputElement;
const libraryRefresh = $("library-refresh") as HTMLButtonElement;
const projectLibraryElement = $("project-library");
const projectUndo = $("project-undo") as HTMLButtonElement;
const projectRedo = $("project-redo") as HTMLButtonElement;
const projectRenameName = $("project-rename-name") as HTMLInputElement;
const projectRename = $("project-rename") as HTMLButtonElement;
const editMoveUp = $("edit-move-up") as HTMLButtonElement;
const editMoveDown = $("edit-move-down") as HTMLButtonElement;
const projectPreviewImage = $("project-preview-image") as HTMLImageElement;
const projectPreviewFrame = $("project-preview-frame") as HTMLButtonElement;
const projectPreviewStatus = $("project-preview-status");
const projectPreviewPlay = $("project-preview-play") as HTMLButtonElement;
const projectSaveState = $("project-save-state");
const projectSave = $("project-save") as HTMLButtonElement;
const projectDiscard = $("project-discard") as HTMLButtonElement;
const projectExportPath = $("project-export-path") as HTMLInputElement;
const projectExportFormat = $("project-export-format") as HTMLSelectElement;
const projectExportCancel = $("project-export-cancel") as HTMLButtonElement;
const projectExport = $("project-export") as HTMLButtonElement;
const settingsButton = $("settings-button") as HTMLButtonElement;
const styleBackground = $("style-background") as HTMLSelectElement;
const stylePadding = $("style-padding") as HTMLInputElement;
const styleCameraSize = $("style-camera-size") as HTMLInputElement;
const styleCursorSize = $("style-cursor-size") as HTMLInputElement;
const styleCursorEffect = $("style-cursor-effect") as HTMLSelectElement;
const styleCursorOpacity = $("style-cursor-opacity") as HTMLInputElement;
const styleCursorScale = $("style-cursor-scale") as HTMLInputElement;
const styleCameraShape = $("style-camera-shape") as HTMLSelectElement;
const styleCameraHidden = $("style-camera-hidden") as HTMLInputElement;
const styleScreenFrame = $("style-screen-frame") as HTMLSelectElement;
const styleScreenMotion = $("style-screen-motion") as HTMLSelectElement;
const styleCursorMotion = $("style-cursor-motion") as HTMLSelectElement;
const styleOpeningEnabled = $("style-opening-enabled") as HTMLInputElement;
const styleOpeningPreset = $("style-opening-preset") as HTMLSelectElement;
const styleOpeningDuration = $("style-opening-duration") as HTMLInputElement;
const screenMotionStart = $("screen-motion-start") as HTMLInputElement;
const screenMotionDuration = $("screen-motion-duration") as HTMLInputElement;
const screenMotionGeometry = $("screen-motion-geometry") as HTMLInputElement;
const screenMotionApply = $("screen-motion-apply") as HTMLButtonElement;
const mosaicStart = $("mosaic-start") as HTMLInputElement;
const mosaicDuration = $("mosaic-duration") as HTMLInputElement;
const mosaicStyle = $("mosaic-style") as HTMLSelectElement;
const mosaicIntensity = $("mosaic-intensity") as HTMLInputElement;
const mosaicRect = $("mosaic-rect") as HTMLInputElement;
const mosaicApply = $("mosaic-apply") as HTMLButtonElement;
const stickerPath = $("sticker-path") as HTMLInputElement;
const stickerStart = $("sticker-start") as HTMLInputElement;
const stickerDuration = $("sticker-duration") as HTMLInputElement;
const stickerGeometry = $("sticker-geometry") as HTMLInputElement;
const stickerApply = $("sticker-apply") as HTMLButtonElement;
const styleApply = $("style-apply") as HTMLButtonElement;
let autoSaveTimer: number | undefined;
let projectCommandBusy = false;
let previewPlaying = false;
let previewPending = false;

function projectLog(message: string, muted = false) {
  const line = document.createElement("span");
  line.className = `console-line${muted ? " muted" : ""}`;
  line.textContent = message.trim() || " ";
  projectConsole.append(line);
  while (projectConsole.children.length > 6) projectConsole.firstElementChild?.remove();
  projectConsole.scrollTop = projectConsole.scrollHeight;
}

function normalizeProjectFile(value: string) {
  const trimmed = value.trim();
  if (!trimmed) return "";
  return /\.json$/i.test(trimmed) ? trimmed : `${trimmed.replace(/[\\/]+$/, "")}\\project.json`;
}

function formatSeconds(value: number) {
  return `${value.toFixed(2)}s`;
}

async function runProjectCommand(args: string[]): Promise<ProjectSnapshot> {
  const command = Command.sidecar("binaries/dogsc_capture", ["project", ...args]);
  let output = "";
  let errors = "";
  return await new Promise((resolve, reject) => {
    let settled = false;
    const fail = (error: unknown) => {
      if (settled) return;
      settled = true;
      reject(error instanceof Error ? error : new Error(String(error)));
    };
    command.stdout.on("data", (line) => { output += String(line); });
    command.stderr.on("data", (line) => { errors += String(line); });
    command.on("error", fail);
    command.on("close", (event) => {
      if (settled) return;
      if (event.code !== 0) {
        fail(new Error(errors.trim() || `项目命令退出（exit ${event.code ?? "unknown"}）`));
        return;
      }
      try {
        settled = true;
        resolve(JSON.parse(output) as ProjectSnapshot);
      } catch (error) {
        fail(new Error(`项目命令返回了无效 JSON：${String(error)}`));
      }
    });
    void command.spawn().catch(fail);
  });
}

async function runCaptureTargets(): Promise<CaptureTargets> {
  const command = Command.sidecar("binaries/dogsc_capture", ["list"]);
  let output = "";
  let errors = "";
  return await new Promise((resolve, reject) => {
    let settled = false;
    const fail = (error: unknown) => { if (!settled) { settled = true; reject(error instanceof Error ? error : new Error(String(error))); } };
    command.stdout.on("data", (line) => { output += String(line); });
    command.stderr.on("data", (line) => { errors += String(line); });
    command.on("error", fail);
    command.on("close", (event) => {
      if (settled) return;
      if (event.code !== 0) { fail(new Error(errors.trim() || `采集目标枚举退出（exit ${event.code ?? "unknown"}）`)); return; }
      try { settled = true; resolve(JSON.parse(output) as CaptureTargets); }
      catch (error) { fail(new Error(`采集目标返回了无效 JSON：${String(error)}`)); }
    });
    void command.spawn().catch(fail);
  });
}
async function runProjectListCommand(root: string): Promise<ProjectLibrary> {
  const command = Command.sidecar("binaries/dogsc_capture", ["project", "list", root]);
  let output = "";
  let errors = "";
  return await new Promise((resolve, reject) => {
    let settled = false;
    const fail = (error: unknown) => {
      if (settled) return;
      settled = true;
      reject(error instanceof Error ? error : new Error(String(error)));
    };
    command.stdout.on("data", (line) => { output += String(line); });
    command.stderr.on("data", (line) => { errors += String(line); });
    command.on("error", fail);
    command.on("close", (event) => {
      if (settled) return;
      if (event.code !== 0) {
        fail(new Error(errors.trim() || `项目库命令退出（exit ${event.code ?? "unknown"}）`));
        return;
      }
      try {
        settled = true;
        resolve(JSON.parse(output) as ProjectLibrary);
      } catch (error) {
        fail(new Error(`项目库命令返回了无效 JSON：${String(error)}`));
      }
    });
    void command.spawn().catch(fail);
  });
}

async function runProjectRename(path: string, name: string): Promise<{ path: string; name: string }> {
  const command = Command.sidecar("binaries/dogsc_capture", ["project", "rename", path, name]);
  let output = "";
  let errors = "";
  return await new Promise((resolve, reject) => {
    let settled = false;
    const fail = (error: unknown) => {
      if (settled) return;
      settled = true;
      reject(error instanceof Error ? error : new Error(String(error)));
    };
    command.stdout.on("data", (line) => { output += String(line); });
    command.stderr.on("data", (line) => { errors += String(line); });
    command.on("error", fail);
    command.on("close", (event) => {
      if (settled) return;
      if (event.code !== 0) { fail(new Error(errors.trim() || `重命名命令退出（exit ${event.code ?? "unknown"}）`)); return; }
      try { settled = true; resolve(JSON.parse(output) as { path: string; name: string }); }
      catch (error) { fail(new Error(`重命名命令返回了无效 JSON：${String(error)}`)); }
    });
    void command.spawn().catch(fail);
  });
}

async function runProjectCompositeFrame(path: string, seconds: number): Promise<PreviewImageFrame> {
  const command = Command.sidecar("binaries/dogsc_capture", ["project", "composite-frame", path, String(seconds), "960"]);
  let output = "";
  let errors = "";
  return await new Promise((resolve, reject) => {
    let settled = false;
    const fail = (error: unknown) => {
      if (settled) return;
      settled = true;
      reject(error instanceof Error ? error : new Error(String(error)));
    };
    command.stdout.on("data", (line) => { output += String(line); });
    command.stderr.on("data", (line) => { errors += String(line); });
    command.on("error", fail);
    command.on("close", (event) => {
      if (settled) return;
      if (event.code !== 0) { fail(new Error(errors.trim() || `合成预览命令退出（exit ${event.code ?? "unknown"}）`)); return; }
      try { settled = true; resolve(JSON.parse(output) as PreviewImageFrame); }
      catch (error) { fail(new Error(`合成预览命令返回了无效 JSON：${String(error)}`)); }
    });
    void command.spawn().catch(fail);
  });
}

async function runProjectExport(path: string, output: string, format: "mp4" | "m4a"): Promise<{ output: string; frameCount?: number; duration: number; sampleRate?: number; channels?: number }> {
  const command = format === "m4a"
    ? Command.sidecar("binaries/dogsc_capture", ["project", "export-audio", path, output])
    : Command.sidecar("binaries/dogsc_capture", ["project", "export-video", path, output, "30"]);
  let finalText = "";
  let errors = "";
  return await new Promise((resolve, reject) => {
    let settled = false;
    const fail = (error: unknown) => {
      if (settled) return;
      settled = true;
      reject(error instanceof Error ? error : new Error(String(error)));
    };
    command.stdout.on("data", (line) => {
      const text = String(line).trim();
      if (!text) return;
      try {
        const payload = JSON.parse(text) as { type?: string; completed?: number; total?: number };
        if (payload.type === "progress") {
          const percent = payload.total ? Math.round((payload.completed ?? 0) * 100 / payload.total) : 0;
          projectLog(`导出进度 ${percent}%`, true);
          return;
        }
      } catch { /* 最终 JSON 在 close 时统一解析 */ }
      finalText += `${text}\n`;
    });
    command.stderr.on("data", (line) => { errors += String(line); });
    command.on("error", fail);
    command.on("close", (event) => {
      exportChild = null;
      updateEditorButtons();
      if (settled) return;
      if (event.code !== 0) { fail(new Error(errors.trim() || `导出命令退出（exit ${event.code ?? "unknown"}）`)); return; }
      try { settled = true; resolve(JSON.parse(finalText) as { output: string; frameCount: number; duration: number }); }
      catch (error) { fail(new Error(`导出命令返回了无效 JSON：${String(error)}`)); }
    });
    void command.spawn().then((child) => { exportChild = child; updateEditorButtons(); }).catch(fail);
  });
}
async function cleanupExportPartial(output: string) {
  const command = Command.sidecar("binaries/dogsc_capture", ["project", "cleanup-output", `${output}.partial`]);
  await new Promise<void>((resolve, reject) => {
    let errors = "";
    command.stderr.on("data", (line) => { errors += String(line); });
    command.on("error", reject);
    command.on("close", (event) => event.code === 0 ? resolve() : reject(new Error(errors || `清理导出 partial 失败（exit ${event.code ?? "unknown"}）`)));
    void command.spawn().catch(reject);
  });
}

function resetPreviewStats() {
  previewFramesReceived = 0;
  previewFramesPresented = 0;
  previewFramesDropped = 0;
  previewBackpressureSkips = 0;
  previewPresentationTimes.fill(0);
  previewPresentationCursor = 0;
  previewPresentationCount = 0;
  latestPreviewFrame = null;
}

function previewFrameRate() {
  if (previewPresentationCount < 2) return 0;
  const oldest = previewPresentationCount === previewPresentationTimes.length ? previewPresentationCursor : 0;
  const newest = (previewPresentationCursor + previewPresentationTimes.length - 1) % previewPresentationTimes.length;
  const elapsed = previewPresentationTimes[newest] - previewPresentationTimes[oldest];
  return elapsed > 0 ? (previewPresentationCount - 1) * 1000 / elapsed : 0;
}

function updatePreviewStatus(frame = latestPreviewFrame) {
  if (!frame) return;
  const surface = frame.presentation === "dxgi" ? "DXGI swapchain" : frame.decodeSurface === "DXGI" ? "DXGI readback" : frame.dxgiManager ? "DXGI manager/CPU readback" : "CPU fallback";
  const layers = [frame.composed ? "D2D scene" : "screen frame", frame.camera ? "camera" : "", frame.cursor ? "cursor" : "", frame.click ? "click" : "", frame.spotlight ? "spotlight" : "", frame.stickers ? "stickers" : "", frame.screenFrame ? "screen-frame" : "", frame.opening ? "opening" : "", frame.screenMotion ? "screen-motion" : ""].filter(Boolean).join("+");
  const refresh = frame.frameStatistics ? ` · vblank ${frame.frameStatistics.syncRefreshCount}` : "";
  projectPreviewStatus.textContent = `${frame.width}×${frame.height} @ ${frame.time.toFixed(2)}s · ${previewFrameRate().toFixed(0)} fps · ${layers} · ${surface}${refresh} · 帧 ${previewFramesPresented}/${previewFramesReceived} · 丢帧 ${previewFramesDropped} · 背压 ${previewBackpressureSkips}${frame.compositionError ? ` · 合成回退:${frame.compositionError}` : ""}`;
}

async function stopPreviewStream() {
  ++previewGeneration;
  previewStarting = false;
  if (previewChild) {
    try { await previewChild.kill(); } catch { /* 进程已退出 */ }
    previewChild = null;
  }
  previewBuffer = "";
  previewOverlayMode = false;
  previewOverlayBounds = "";
  previewWriteChain = Promise.resolve();
  previewFrameChain = Promise.resolve();
  previewPlaying = false;
  previewPending = false;
  resetPreviewStats();
  projectPreviewPlay.textContent = "播放";
  projectPreviewImage.removeAttribute("src");
  projectPreviewImage.style.visibility = "";
  projectPreviewStatus.textContent = "未加载预览";
}


function writePreviewInput(input: string) {
  const child = previewChild;
  if (!child) return Promise.reject(new Error("预览流进程尚未启动"));
  const write = previewWriteChain.catch(() => undefined).then(() => child.write(input));
  previewWriteChain = write;
  return write;
}

async function updatePreviewOverlayBounds() {
  if (!previewChild || !previewOverlayMode) return;
  const rect = projectPreviewImage.getBoundingClientRect();
  const visible = rect.width > 0 && rect.height > 0 && rect.left >= 0 && rect.top >= 0
    && rect.right <= window.innerWidth && rect.bottom <= window.innerHeight;
  if (!visible) {
    if (previewOverlayBounds !== "hidden") {
      previewOverlayBounds = "hidden";
      await writePreviewInput("H\n");
    }
    previewPlaying = false;
    projectPreviewPlay.textContent = "播放";
    projectPreviewImage.style.visibility = "";
    return;
  }
  const [origin, scale] = await Promise.all([
    getCurrentWindow().innerPosition(),
    getCurrentWindow().scaleFactor(),
  ]);
  const bounds = [
    Math.round(origin.x + rect.left * scale),
    Math.round(origin.y + rect.top * scale),
    Math.max(1, Math.round(rect.width * scale)),
    Math.max(1, Math.round(rect.height * scale)),
  ];
  const key = bounds.join(",");
  if (key === previewOverlayBounds) return;
  previewOverlayBounds = key;
  try { await writePreviewInput(`R ${key.replaceAll(",", " ")}\n`); }
  catch (error) { previewOverlayBounds = ""; throw error; }
}

function schedulePreviewOverlayBounds() {
  if (previewOverlayUpdateScheduled) return;
  previewOverlayUpdateScheduled = true;
  window.requestAnimationFrame(() => {
    previewOverlayUpdateScheduled = false;
    void updatePreviewOverlayBounds().catch((error) => {
      projectPreviewStatus.textContent = `DXGI overlay 定位失败：${String(error)}`;
    });
  });
}
function previewAtEnd(time: number, duration: number) {
  return time >= duration - 1e-6;
}
function recordPreviewPresentation() {
  previewPresentationTimes[previewPresentationCursor] = performance.now();
  previewPresentationCursor = (previewPresentationCursor + 1) % previewPresentationTimes.length;
  previewPresentationCount = Math.min(previewPresentationCount + 1, previewPresentationTimes.length);
}

async function presentPreviewLine(line: string, generation: number) {
  if (generation !== previewGeneration) return;
  let payload: PreviewFrame | { error: string };
  try {
    payload = JSON.parse(line) as PreviewFrame | { error: string };
  } catch (error) {
    previewPending = false;
    previewPlaying = false;
    previewFramesDropped += 1;
    projectPreviewPlay.textContent = "播放";
    projectPreviewStatus.textContent = `预览返回无效 JSON：${String(error)}`;
    return;
  }
  if ("error" in payload) {
    const requestTime = Number(editTime.value) || 0;
    if (payload.error === "媒体没有可读取的帧" && projectSnapshot &&
        previewAtEnd(requestTime + 1 / 30, projectSnapshot.outputDuration)) {
      previewPending = false;
      previewPlaying = false;
      editTime.value = projectSnapshot.outputDuration.toFixed(3);
      projectPreviewPlay.textContent = "播放";
      if (latestPreviewFrame) {
        updatePreviewStatus();
        projectPreviewStatus.textContent += " · 片尾";
      } else {
        projectPreviewStatus.textContent = "预览已到片尾";
      }
      return;
    }
    previewPending = false;
    previewPlaying = false;
    projectPreviewPlay.textContent = "播放";
    projectPreviewStatus.textContent = `预览失败：${payload.error}`;
    return;
  }

  previewFramesReceived += 1;
  let presented = false;
  if (payload.presentation === "dxgi") {
    projectPreviewImage.style.visibility = "hidden";
    presented = payload.presented === true;
    previewFramesPresented = payload.presentedFrames ?? previewFramesPresented + (presented ? 1 : 0);
    previewFramesDropped = payload.droppedFrames ?? previewFramesDropped + (presented ? 0 : 1);
  } else {
    projectPreviewImage.style.visibility = "";
    if (!payload.data) {
      previewPending = false;
      previewPlaying = false;
      projectPreviewPlay.textContent = "播放";
      projectPreviewStatus.textContent = "预览没有可显示的图像数据";
      return;
    }
    projectPreviewImage.src = payload.data;
    try {
      await projectPreviewImage.decode();
      await new Promise<void>((resolve) => window.requestAnimationFrame(() => resolve()));
      await new Promise<void>((resolve) => window.requestAnimationFrame(() => resolve()));
      presented = true;
      previewFramesPresented += 1;
    } catch {
      if (generation !== previewGeneration) return;
      previewPending = false;
      previewPlaying = false;
      previewFramesDropped += 1;
      projectPreviewPlay.textContent = "播放";
      updatePreviewStatus(payload);
      return;
    }
  }
  if (generation !== previewGeneration) return;

  previewPending = false;
  latestPreviewFrame = payload;
  if (presented) recordPreviewPresentation();
  updatePreviewStatus(payload);


  if (previewPlaying) {
    const next = payload.time + 1 / 30;
    if (projectSnapshot && previewAtEnd(next, projectSnapshot.outputDuration)) {
      previewPlaying = false;
      projectPreviewPlay.textContent = "播放";
    } else {
      editTime.value = (Math.floor(next * 10000000) / 10000000).toFixed(7);
      window.setTimeout(() => { void requestPreviewFrame(); }, 0);
    }
  }
}

function consumePreviewLines(chunk: string, generation: number) {
  if (generation !== previewGeneration) return;
  previewBuffer += chunk;
  let newline = previewBuffer.indexOf("\n");
  while (newline >= 0) {
    const line = previewBuffer.slice(0, newline).trim();
    previewBuffer = previewBuffer.slice(newline + 1);
    if (line) {
      previewFrameChain = previewFrameChain
        .then(() => presentPreviewLine(line, generation))
        .catch((error) => {
          if (generation !== previewGeneration) return;
          previewPending = false;
          previewPlaying = false;
          projectPreviewPlay.textContent = "播放";
          projectPreviewStatus.textContent = `预览处理失败：${String(error)}`;
        });
    }
    newline = previewBuffer.indexOf("\n");
  }
}

async function startPreviewStream() {
  if (!projectFile || previewChild || previewStarting) return;
  previewStarting = true;
  const generation = ++previewGeneration;
  previewBuffer = "";
  previewFrameChain = Promise.resolve();
  previewWriteChain = Promise.resolve();
  resetPreviewStats();
  let ownerHwnd = 0;
  try { ownerHwnd = await invoke<number>("preview_host_hwnd"); } catch {}
  previewOverlayMode = ownerHwnd > 0;
  previewOverlayBounds = "";
  const args = previewOverlayMode
    ? ["project", "stream-window", projectFile, "960", String(ownerHwnd)]
    : ["project", "stream", projectFile, "960"];
  const command = Command.sidecar("binaries/dogsc_capture", args);
  command.stdout.on("data", (line) => consumePreviewLines(String(line), generation));
  command.stderr.on("data", (line) => {
    if (generation === previewGeneration) projectPreviewStatus.textContent = `预览错误：${String(line).trim()}`;
  });
  command.on("close", () => {
    if (generation !== previewGeneration) return;
    previewChild = null;
    previewOverlayMode = false;
    previewOverlayBounds = "";
    previewPending = false;
    if (previewPlaying) {
      previewPlaying = false;
      projectPreviewPlay.textContent = "播放";
    }
  });
  try {
    const child = await command.spawn();
    if (generation !== previewGeneration) {
      try { await child.kill(); } catch { /* 进程已退出 */ }
      return;
    }
    previewChild = child;
    if (previewOverlayMode) await updatePreviewOverlayBounds();
  } catch (error) {
    if (previewChild) {
      try { await previewChild.kill(); } catch { /* 进程已退出 */ }
      previewChild = null;
    }
    previewOverlayMode = false;
    if (generation === previewGeneration) projectPreviewStatus.textContent = `预览启动失败：${String(error)}`;
  } finally {
    if (generation === previewGeneration) previewStarting = false;
  }
}

async function requestPreviewFrame() {
  if (!previewChild) return;
  if (previewPending) {
    previewBackpressureSkips += 1;
    updatePreviewStatus();
    return;
  }
  if (previewOverlayMode) {
    try { await updatePreviewOverlayBounds(); }
    catch (error) {
      projectPreviewStatus.textContent = `DXGI overlay 更新失败：${String(error)}`;
      return;
    }
    if (!previewPlaying) return;
  }
  const seconds = Number(editTime.value) || 0;
  if (previewPlaying && projectSnapshot && previewAtEnd(seconds, projectSnapshot.outputDuration)) {
    previewPlaying = false;
    projectPreviewPlay.textContent = "播放";
    return;
  }
  previewPending = true;
  try { await writePreviewInput(`F ${seconds}\n`); }
  catch (error) {
    previewPending = false;
    previewPlaying = false;
    projectPreviewPlay.textContent = "播放";
    projectPreviewStatus.textContent = `预览请求失败：${String(error)}`;
  }
}
async function loadPreviewFrame() {
  if (!projectFile) return;
  projectPreviewFrame.disabled = true;
  projectPreviewStatus.textContent = "合成中…";
  await stopPreviewStream();
  try {
    const frame = await runProjectCompositeFrame(projectFile, Number(editTime.value) || 0);
    projectPreviewImage.src = frame.data;
    projectPreviewStatus.textContent = `${frame.width}×${frame.height} @ ${frame.time.toFixed(2)}s`;
  } catch (error) {
    projectPreviewStatus.textContent = `合成预览失败：${String(error)}`;
  } finally {
    updateEditorButtons();
  }
}

async function togglePreviewPlay() {
  if (!projectFile) return;
  if (previewPlaying) {
    previewPlaying = false;
    projectPreviewPlay.textContent = "播放";
    return;
  }
  if (projectSnapshot && previewAtEnd(Number(editTime.value), projectSnapshot.outputDuration)) editTime.value = "0";
  await startPreviewStream();
  if (!previewChild) {
    previewPlaying = false;
    return;
  }
  previewPlaying = true;
  projectPreviewPlay.textContent = "暂停";
  await requestPreviewFrame();
}

function renderProjectSnapshot() {
  const snapshot = projectSnapshot;
  if (!snapshot) {
    projectTitle.textContent = "未打开工程";
    projectDuration.textContent = "—";
    projectSegmentCount.textContent = "—";
    projectSaveState.textContent = "未打开";
    projectPreviewImage.removeAttribute("src");
    projectPreviewStatus.textContent = "未加载预览";
    timeline.innerHTML = '<span class="timeline-empty">打开录制目录中的 project.json 后开始编辑</span>';
    updateEditorButtons();
    return;
  }
  projectTitle.textContent = snapshot.project.title;
  projectDuration.textContent = formatSeconds(snapshot.outputDuration);
  projectSegmentCount.textContent = String(snapshot.segments.length);
  projectSaveState.textContent = snapshot.session?.dirty ? "草稿未保存" : "已保存";
  styleScreenFrame.value = snapshot.project.canvas?.screenFrame ?? "none";
  styleScreenMotion.value = snapshot.project.motion?.screen ?? "聚焦";
  styleCursorMotion.value = snapshot.project.motion?.cursor ?? "平滑";
  styleOpeningEnabled.checked = snapshot.project.openingSequence?.isEnabled === true;
  styleOpeningPreset.value = snapshot.project.openingSequence?.preset ?? "轻 3D";
  styleOpeningDuration.value = String(snapshot.project.openingSequence?.duration ?? 2.2);
  const previous = editSegment.value;
  editSegment.innerHTML = '<option value="">未选择</option>';
  for (const segment of snapshot.segments) {
    const option = document.createElement("option");
    option.value = segment.id;
    option.textContent = `${segment.id.slice(0, 8)} · ${formatSeconds(segment.outputEnd - segment.outputStart)}`;
    editSegment.append(option);
  }
  if (snapshot.segments.some((segment) => segment.id === previous)) editSegment.value = previous;
  timeline.innerHTML = "";
  for (const segment of snapshot.segments) {
    const item = document.createElement("button");
    item.type = "button";
    item.className = "timeline-segment";
    item.dataset.segmentId = segment.id;
    item.style.width = `${Math.max(5, (segment.outputEnd - segment.outputStart) / snapshot.outputDuration * 100)}%`;
    item.textContent = `${formatSeconds(segment.outputStart)}—${formatSeconds(segment.outputEnd)}`;
    item.title = segment.id;
    item.addEventListener("click", () => { editSegment.value = segment.id; updateEditorButtons(); });
    timeline.append(item);
  }
  updateEditorButtons();
}

function updateEditorButtons() {
  const snapshot = projectSnapshot;
  const segments = snapshot?.segments ?? [];
  const hasProject = snapshot !== null;
  const index = hasProject ? segments.findIndex((segment) => segment.id === editSegment.value) : -1;
  const hasSegment = index >= 0;
  editSplit.disabled = !hasProject;
  editTrimLeft.disabled = !hasSegment;
  editTrimRight.disabled = !hasSegment;
  editRateButton.disabled = !hasSegment;
  editMoveUp.disabled = !hasSegment || index === 0;
  editMoveDown.disabled = !hasSegment || index < 0 || index === segments.length - 1;
  editRemove.disabled = !hasSegment;
  projectUndo.disabled = !hasProject || !(snapshot?.history?.canUndo ?? false);
  projectRedo.disabled = !hasProject || !(snapshot?.history?.canRedo ?? false);
  projectRename.disabled = !hasProject || !projectRenameName.value.trim();
  projectPreviewFrame.disabled = !hasProject;
  projectPreviewPlay.disabled = !hasProject;
  projectExport.disabled = !hasProject || projectCommandBusy || !projectExportPath.value.trim();
  projectExportCancel.disabled = exportChild === null;
  styleApply.disabled = !hasProject || projectCommandBusy;
  screenMotionApply.disabled = !hasProject || projectCommandBusy;
  editMarker.disabled = !hasProject || projectCommandBusy;
  mosaicApply.disabled = !hasProject || projectCommandBusy;
  stickerApply.disabled = !hasProject || projectCommandBusy;
  const dirty = snapshot?.session?.dirty ?? false;
  projectSave.disabled = !hasProject || !dirty || projectCommandBusy;
  projectDiscard.disabled = !hasProject || !dirty || projectCommandBusy;
}

function renderProjectLibrary() {
  projectLibraryElement.replaceChildren();
  if (!projectLibrary || projectLibrary.projects.length === 0) {
    const empty = document.createElement("span");
    empty.className = "timeline-empty";
    empty.textContent = "当前目录没有可打开的工程";
    projectLibraryElement.append(empty);
    return;
  }
  for (const item of projectLibrary.projects) {
    const button = document.createElement("button");
    button.type = "button";
    button.className = "project-library-item";
    button.disabled = Boolean(item.error);
    const title = document.createElement("strong");
    title.textContent = item.title ?? item.name;
    const detail = document.createElement("span");
    detail.textContent = item.error ?? (item.duration === undefined ? "无媒体时长" : formatSeconds(item.duration));
    button.append(title, detail);
    button.title = item.path;
    button.addEventListener("click", () => { void loadProject(item.path); });
    projectLibraryElement.append(button);
  }
}

function updateCaptureMode() {
  const isWindow = captureMode.value === "window";
  monitor.disabled = isWindow;
  region.disabled = isWindow;
  windowTarget.disabled = !isWindow;
}
function openRegionPicker() {
  if (captureMode.value === "window") { projectLog("窗口源不需要区域选择", true); return; }
  const dimensions = monitor.selectedOptions[0]?.textContent?.match(/(\d+)×(\d+)/);
  const sourceWidth = Number(dimensions?.[1] ?? 1920);
  const sourceHeight = Number(dimensions?.[2] ?? 1080);
  const modal = document.createElement("div");
  modal.className = "region-modal-backdrop";
  modal.innerHTML = `<div class="region-modal" role="dialog" aria-label="选择录制区域"><div class="region-modal-heading"><strong>选择录制区域</strong><button type="button" class="icon-button small region-close">×</button></div><div class="region-surface"><div class="region-selection"></div></div><div class="region-modal-footer"><span class="region-readout">拖动选取区域</span><div><button type="button" class="secondary compact region-full">整块显示器</button><button type="button" class="primary compact region-apply">使用区域</button></div></div></div>`;
  document.body.append(modal);
  const surface = modal.querySelector<HTMLElement>(".region-surface")!;
  const selection = modal.querySelector<HTMLElement>(".region-selection")!;
  const readout = modal.querySelector<HTMLElement>(".region-readout")!;
  let start: { x: number; y: number } | null = null;
  let rect = { x: 0.1, y: 0.1, width: 0.8, height: 0.8 };
  const update = (next: typeof rect) => {
    rect = { x: Math.max(0, Math.min(1, next.x)), y: Math.max(0, Math.min(1, next.y)), width: Math.max(0.02, Math.min(1, next.width)), height: Math.max(0.02, Math.min(1, next.height)) };
    selection.style.left = `${rect.x * 100}%`; selection.style.top = `${rect.y * 100}%`;
    selection.style.width = `${rect.width * 100}%`; selection.style.height = `${rect.height * 100}%`;
    readout.textContent = `${Math.round(rect.x * sourceWidth)}, ${Math.round(rect.y * sourceHeight)}, ${Math.round(rect.width * sourceWidth)}, ${Math.round(rect.height * sourceHeight)}`;
  };
  update(rect);
  surface.addEventListener("pointerdown", (event) => { const bounds = surface.getBoundingClientRect(); start = { x: (event.clientX - bounds.left) / bounds.width, y: (event.clientY - bounds.top) / bounds.height }; surface.setPointerCapture(event.pointerId); update({ x: start.x, y: start.y, width: 0.02, height: 0.02 }); });
  surface.addEventListener("pointermove", (event) => { if (!start) return; const bounds = surface.getBoundingClientRect(); const x = (event.clientX - bounds.left) / bounds.width; const y = (event.clientY - bounds.top) / bounds.height; update({ x: Math.min(start.x, x), y: Math.min(start.y, y), width: Math.abs(x - start.x), height: Math.abs(y - start.y) }); });
  surface.addEventListener("pointerup", () => { start = null; });
  const close = () => modal.remove();
  modal.querySelector(".region-close")?.addEventListener("click", close);
  modal.querySelector(".region-full")?.addEventListener("click", () => update({ x: 0, y: 0, width: 1, height: 1 }));
  modal.querySelector(".region-apply")?.addEventListener("click", () => { region.value = `${Math.round(rect.x * sourceWidth)},${Math.round(rect.y * sourceHeight)},${Math.round(rect.width * sourceWidth)},${Math.round(rect.height * sourceHeight)}`; close(); });
}

function openSettings() {
  if (document.querySelector(".settings-modal-backdrop")) return;
  const saved = JSON.parse(localStorage.getItem("dogsc.preferences") ?? "{}");
  const modal = document.createElement("div");
  modal.className = "settings-modal-backdrop";
  modal.innerHTML = `<div class="settings-modal" role="dialog" aria-label="设置"><div class="region-modal-heading"><strong>设置</strong><button type="button" class="icon-button small settings-close">×</button></div><div class="settings-modal-body"><label class="field"><span>默认帧率</span><select class="settings-fps"><option value="30">30 fps</option><option value="60">60 fps</option><option value="90">90 fps</option><option value="120">120 fps</option></select></label><label class="field"><span>默认输出目录</span><input class="settings-directory" type="text" /></label><div class="settings-permission"><strong>运行时能力</strong><span>Windows Graphics Capture、Media Foundation、WASAPI</span><small>权限会在首次使用对应功能时由系统返回结果。</small></div></div><div class="region-modal-footer"><span class="region-readout">偏好设置保存在本机</span><button type="button" class="primary settings-save">完成</button></div></div>`;
  document.body.append(modal);
  const fpsInput = modal.querySelector<HTMLSelectElement>(".settings-fps")!;
  const directoryInput = modal.querySelector<HTMLInputElement>(".settings-directory")!;
  fpsInput.value = String(saved.defaultFps ?? fps.value);
  directoryInput.value = String(saved.directory ?? directory.value);
  const close = () => modal.remove();
  modal.querySelector(".settings-close")?.addEventListener("click", close);
  modal.querySelector(".settings-save")?.addEventListener("click", () => {
    const preferences = { defaultFps: Number(fpsInput.value), directory: directoryInput.value.trim() };
    localStorage.setItem("dogsc.preferences", JSON.stringify(preferences));
    fps.value = String(preferences.defaultFps);
    if (preferences.directory) { directory.value = preferences.directory; libraryRoot.value = preferences.directory; }
    close();
  });
}


async function refreshTargets() {
  try {
    const targets = await runCaptureTargets();
    monitor.replaceChildren();
    for (const item of targets.monitors) {
      const option = document.createElement("option");
      option.value = String(item.index);
      option.textContent = `${item.primary ? "主显示器" : "显示器"} · ${item.index} · ${item.width}×${item.height}`;
      monitor.append(option);
    }
    windowTarget.replaceChildren();
    for (const item of targets.windows) {
      const option = document.createElement("option");
      option.value = String(item.hwnd);
      option.textContent = `${item.title}${item.minimized ? " · 最小化" : ""}`;
      windowTarget.append(option);
    }
    if (targets.monitors.length === 0) projectLog("没有可用显示器", false);
    if (targets.windows.length === 0) projectLog("没有可用窗口", true);
    updateCaptureMode();
  } catch (error) {
    monitor.innerHTML = '<option value="0">主显示器 · 0</option>';
    projectLog(`采集目标枚举失败：${String(error)}`, false);
  }
}

async function refreshLibrary() {
  const root = libraryRoot.value.trim();
  if (!root) { projectLog("请输入项目库目录", false); return; }
  libraryRefresh.disabled = true;
  projectLog(`正在扫描项目库 ${root}`, true);
  try {
    projectLibrary = await runProjectListCommand(root);
    renderProjectLibrary();
    projectLog(`项目库已刷新：${projectLibrary.projects.length} 个工程`, false);
  } catch (error) {
    projectLog(`刷新项目库失败：${String(error)}`, false);
  } finally {
    libraryRefresh.disabled = false;
  }
}

function scheduleAutoSave() {
  if (autoSaveTimer !== undefined) window.clearTimeout(autoSaveTimer);
  projectSaveState.textContent = "待保存";
  autoSaveTimer = window.setTimeout(() => { autoSaveTimer = undefined; void saveProjectDraft(); }, 800);
}

async function saveProjectDraft(): Promise<boolean> {
  if (!projectFile || !projectSnapshot?.session?.dirty) return true;
  projectCommandBusy = true;
  projectSaveState.textContent = "保存中…";
  updateEditorButtons();
  try {
    let lastError: unknown;
    for (let attempt = 0; attempt < 2; attempt += 1) {
      try {
        projectSnapshot = await runProjectCommand(["save", projectFile]);
        renderProjectSnapshot();
        projectLog("草稿已自动保存", true);
        return true;
      } catch (error) {
        lastError = error;
        if (attempt === 0) await new Promise((resolve) => window.setTimeout(resolve, 120));
      }
    }
    projectSaveState.textContent = "保存失败";
    projectLog(`自动保存失败：${String(lastError)}`, false);
    return false;
  } finally {
    projectCommandBusy = false;
    updateEditorButtons();
  }
}

async function flushProjectSave() {
  if (autoSaveTimer !== undefined) {
    window.clearTimeout(autoSaveTimer);
    autoSaveTimer = undefined;
  }
  return await saveProjectDraft();
}

async function discardProjectDraft() {
  if (!projectFile || !projectSnapshot?.session?.dirty) return;
  projectCommandBusy = true;
  try {
    projectSnapshot = await runProjectCommand(["discard", projectFile]);
    renderProjectSnapshot();
    projectLog("已放弃未保存草稿", false);
  } catch (error) {
    projectLog(`放弃草稿失败：${String(error)}`, false);
  } finally {
    projectCommandBusy = false;
    updateEditorButtons();
  }
}

async function loadProject(value: string) {
  const path = normalizeProjectFile(value);
  if (!path) { projectLog("请输入 project.json 路径", false); return; }
  if (!(await flushProjectSave())) return;
  await stopPreviewStream();
  projectCommandBusy = true;
  projectOpen.disabled = true;
  projectLog(`正在打开 ${path}`, true);
  try {
    projectSnapshot = await runProjectCommand(["begin", path]);
    projectFile = path;
    projectPathInput.value = path;
    renderProjectSnapshot();
    projectExportPath.value = `${path.replace(/[\\/]+project\.json$/i, "")}\\export.mp4`;
    projectLog(projectSnapshot.session.hasRecovery ? "已发现并恢复未保存草稿" : "工程已打开", false);
  } catch (error) {
    projectLog(`打开工程失败：${String(error)}`, false);
  } finally {
    projectCommandBusy = false;
    projectOpen.disabled = false;
    projectPreviewFrame.disabled = false;
    projectPreviewPlay.disabled = false;
    projectPathInput.value = path;
  }
}

async function applyProjectEdit(operation: string, ...args: string[]) {
  if (!projectFile) return;
  projectCommandBusy = true;
  projectOpen.disabled = true;
  try {
    projectSnapshot = await runProjectCommand(["edit-draft", projectFile, operation, ...args]);
    renderProjectSnapshot();
    projectLog(`已应用 ${operation} 到草稿，自动保存已排队`, false);
    scheduleAutoSave();
  } catch (error) {
    projectLog(`编辑失败：${String(error)}`, false);
  } finally {
    projectCommandBusy = false;
    projectOpen.disabled = false;
    updateEditorButtons();
  }
}
async function applyProjectHistory(action: "undo" | "redo") {
  if (!projectFile || !(await flushProjectSave())) return;
  projectCommandBusy = true;
  projectOpen.disabled = true;
  try {
    projectSnapshot = await runProjectCommand([action, projectFile]);
    renderProjectSnapshot();
    projectLog(action === "undo" ? "已撤销上一组编辑" : "已重做上一组编辑", false);
  } catch (error) {
    projectLog(`${action === "undo" ? "撤销" : "重做"}失败：${String(error)}`, false);
  } finally {
    projectCommandBusy = false;
    projectOpen.disabled = false;
    updateEditorButtons();
  }
}


async function renameProject() {
  if (!projectFile || !projectRenameName.value.trim() || !(await flushProjectSave())) return;
  projectRename.disabled = true;
  try {
    const renamed = await runProjectRename(projectFile, projectRenameName.value.trim());
    projectLog(`工程已重命名为 ${renamed.name}`, false);
    projectRenameName.value = "";
    await loadProject(renamed.path);
    await refreshLibrary();
  } catch (error) {
    projectLog(`重命名失败：${String(error)}`, false);
  } finally {
    updateEditorButtons();
  }
}

async function applyMarker() {
  if (!projectFile) return;
  const time = Number(editTime.value);
  if (!Number.isFinite(time)) { projectLog("标记时间无效", false); return; }
  await applyProjectEdit("marker", String(time));
}

async function applyScreenMotion() {
  if (!projectFile) return;
  const values = screenMotionGeometry.value.split(",").map((value) => Number(value.trim()));
  if (values.length !== 7 || values.some((value) => !Number.isFinite(value))) {
    projectLog("运镜几何必须是 x,y,scale,rotation-x,rotation-y,rotation-z,perspective", false);
    return;
  }
  await applyProjectEdit("screen-motion", screenMotionStart.value, screenMotionDuration.value,
    ...values.map((value) => String(value)));
}

async function applyMosaic() {
  if (!projectFile) return;
  const values = mosaicRect.value.split(",").map((value) => Number(value.trim()));
  if (values.length !== 4 || values.some((value) => !Number.isFinite(value))) {
    projectLog("遮罩矩形必须是 x,y,width,height", false);
    return;
  }
  await applyProjectEdit("mosaic", mosaicStart.value, mosaicDuration.value,
    ...values.map((value) => String(value)), mosaicStyle.value, mosaicIntensity.value);
}

async function applySticker() {
  if (!projectFile || !stickerPath.value.trim()) return;
  const values = stickerGeometry.value.split(",").map((value) => Number(value.trim()));
  if (values.length !== 3 || values.some((value) => !Number.isFinite(value))) {
    projectLog("贴图几何必须是 x,y,width", false);
    return;
  }
  await applyProjectEdit("sticker", stickerStart.value, stickerDuration.value,
    stickerPath.value.trim(), ...values.map((value) => String(value)));
}
async function applyProjectStyles() {
  if (!projectFile || !(await flushProjectSave())) return;
  projectCommandBusy = true;
  try {
    for (const [style, value] of [["background", styleBackground.value], ["padding", stylePadding.value], ["camera-size", styleCameraSize.value], ["cursor-size", styleCursorSize.value], ["cursor-effect", styleCursorEffect.value], ["cursor-opacity", styleCursorOpacity.value], ["cursor-scale", styleCursorScale.value], ["camera-shape", styleCameraShape.value], ["camera-hidden", String(styleCameraHidden.checked)], ["screen-frame", styleScreenFrame.value], ["screen-motion", styleScreenMotion.value], ["cursor-motion", styleCursorMotion.value], ["opening-enabled", String(styleOpeningEnabled.checked)], ["opening-preset", styleOpeningPreset.value], ["opening-duration", styleOpeningDuration.value]]) {
      projectSnapshot = await runProjectCommand(["edit-draft", projectFile, "style", style, value]);
    }
    renderProjectSnapshot();
    scheduleAutoSave();
    projectLog("画面参数已写入草稿，自动保存已排队", false);
  } catch (error) {
    projectLog(`画面参数失败：${String(error)}`, false);
  } finally {
    projectCommandBusy = false;
    updateEditorButtons();
  }
}
async function cancelProjectExport() {
  if (!exportChild) return;
  const output = projectExportPath.value.trim();
  const child = exportChild;
  exportChild = null;
  projectCommandBusy = false;
  updateEditorButtons();
  projectLog("正在取消导出……", true);
  try { await child.kill(); } catch (error) { projectLog(`取消导出失败：${String(error)}`, false); }
  try { await cleanupExportPartial(output); } catch (error) { projectLog(`partial 清理失败：${String(error)}`, false); }
}

async function exportProject() {
  if (!projectFile || !projectExportPath.value.trim() || !(await flushProjectSave())) return;
  projectCommandBusy = true;
  projectExport.disabled = true;
  projectExportCancel.disabled = true;
  try {
    const format = projectExportFormat.value as "mp4" | "m4a";
    const result = await runProjectExport(projectFile, projectExportPath.value.trim(), format);
    projectLog(format === "m4a"
      ? `M4A 导出完成：${result.duration.toFixed(2)}s · ${result.sampleRate ?? 0}Hz`
      : `MP4 导出完成：${result.frameCount ?? 0} 帧，${result.duration.toFixed(2)}s`, false);
  } catch (error) {
    try { await cleanupExportPartial(projectExportPath.value.trim()); } catch (cleanupError) {
      projectLog(`导出失败且 partial 清理失败：${String(cleanupError)}`, false);
    }
    projectLog(`导出失败：${String(error)}`, false);
  } finally {
    projectCommandBusy = false;
    updateEditorButtons();
  }
}


projectOpen.addEventListener("click", () => { void loadProject(projectPathInput.value); });
railProjects.addEventListener("click", () => { document.querySelector(".editor-panel")?.scrollIntoView({ behavior: "smooth" }); });
libraryRefresh.addEventListener("click", () => { void refreshLibrary(); });
projectUndo.addEventListener("click", () => { void applyProjectHistory("undo"); });
projectRedo.addEventListener("click", () => { void applyProjectHistory("redo"); });
editSegment.addEventListener("change", updateEditorButtons);
editSplit.addEventListener("click", () => {
  const time = Number(editTime.value);
  if (Number.isFinite(time)) void applyProjectEdit("split", String(time));
});
editTrimLeft.addEventListener("click", () => { void applyProjectEdit("trim", editSegment.value, "left", editTime.value); });
editTrimRight.addEventListener("click", () => { void applyProjectEdit("trim", editSegment.value, "right", editTime.value); });
editRateButton.addEventListener("click", () => { void applyProjectEdit("rate", editSegment.value, editRate.value); });
editMoveUp.addEventListener("click", () => {
  const index = projectSnapshot?.segments.findIndex((segment) => segment.id === editSegment.value) ?? -1;
  if (index > 0) void applyProjectEdit("move", editSegment.value, String(index - 1));
});
editMoveDown.addEventListener("click", () => {
  const index = projectSnapshot?.segments.findIndex((segment) => segment.id === editSegment.value) ?? -1;
  if (index >= 0) void applyProjectEdit("move", editSegment.value, String(index + 1));
});
editRemove.addEventListener("click", () => { void applyProjectEdit("remove", editSegment.value); });
projectRenameName.addEventListener("input", updateEditorButtons);
projectRename.addEventListener("click", () => { void renameProject(); });
projectPreviewFrame.addEventListener("click", () => { void loadPreviewFrame(); });
projectPreviewPlay.addEventListener("click", () => { projectPreviewStatus.textContent = "正在启动预览…"; void togglePreviewPlay(); });
const previewOverlayResizeObserver = new ResizeObserver(schedulePreviewOverlayBounds);
previewOverlayResizeObserver.observe(projectPreviewImage);
window.addEventListener("scroll", schedulePreviewOverlayBounds, true);
window.addEventListener("resize", schedulePreviewOverlayBounds);
void getCurrentWindow().onMoved(schedulePreviewOverlayBounds);
void getCurrentWindow().onResized(schedulePreviewOverlayBounds);
projectSave.addEventListener("click", () => { void flushProjectSave(); });
projectDiscard.addEventListener("click", () => { void discardProjectDraft(); });
projectExportPath.addEventListener("input", updateEditorButtons);
projectExport.addEventListener("click", () => { void exportProject(); });
projectExportCancel.addEventListener("click", () => { void cancelProjectExport(); });
styleApply.addEventListener("click", () => { void applyProjectStyles(); });
mosaicApply.addEventListener("click", () => { void applyMosaic(); });
screenMotionApply.addEventListener("click", () => { void applyScreenMotion(); });
editMarker.addEventListener("click", () => { void applyMarker(); });
stickerApply.addEventListener("click", () => { void applySticker(); });
let closeConfirmationInProgress = false;
void getCurrentWindow().onCloseRequested(async (event) => {
  if (!projectCommandBusy && !projectSnapshot?.session?.dirty) return;
  event.preventDefault();
  if (closeConfirmationInProgress) return;
  if (projectCommandBusy) {
    window.alert("工程操作仍在进行，请等待完成后再关闭。");
    return;
  }
  if (!projectSnapshot?.session?.dirty) return;
  closeConfirmationInProgress = true;
  try {
    if (!window.confirm("工程有未保存的修改。保存并关闭？\n选择“取消”可返回编辑器。")) return;
    if (!(await flushProjectSave())) return;
    await getCurrentWindow().destroy();
  } catch (error) {
    projectLog(`关闭前保存失败：${String(error)}`, false);
  } finally {
    closeConfirmationInProgress = false;
  }
});
void defaultDirectory().then((value) => { const saved = JSON.parse(localStorage.getItem("dogsc.preferences") ?? "{}"); directory.value = saved.directory ?? value; libraryRoot.value = saved.directory ?? value; if (saved.defaultFps) fps.value = String(saved.defaultFps); });
captureMode.addEventListener("change", updateCaptureMode);
regionPick.addEventListener("click", openRegionPicker);
settingsButton.addEventListener("click", openSettings);
updateCaptureMode();
void refreshTargets();


function log(message: string, muted = false) {
  const line = document.createElement("span");
  line.className = `console-line${muted ? " muted" : ""}`;
  line.textContent = message.trim() || " ";
  consoleOutput.append(line);
  while (consoleOutput.children.length > 8) consoleOutput.firstElementChild?.remove();
  consoleOutput.scrollTop = consoleOutput.scrollHeight;
}

function setRecording(active: boolean) {
  start.disabled = active;
  pause.disabled = !active;
  stop.disabled = !active;
  directory.disabled = active;
  monitor.disabled = active;
  fps.disabled = active;
  codec.disabled = active;
  duration.disabled = active;
  systemAudio.disabled = active;
  microphone.disabled = active;
  camera.disabled = active;
  statusText.textContent = active ? "RECORDING IN PROGRESS" : "READY TO CAPTURE";
  document.body.classList.toggle("recording", active);
}

function startTimer() {
  recordingStartedAt = Date.now();
  timer = window.setInterval(() => {
    const seconds = Math.floor((Date.now() - recordingStartedAt) / 1000);
    elapsed.textContent = `${String(Math.floor(seconds / 60)).padStart(2, "0")}:${String(seconds % 60).padStart(2, "0")}`;
  }, 250);
}

function stopTimer() {
  if (timer !== undefined) window.clearInterval(timer);
  timer = undefined;
}

start.addEventListener("click", async () => {
  try {
    const args = ["record", directory.value.trim()];
    if (captureMode.value === "window") {
      if (!windowTarget.value) throw new Error("没有可用窗口目标");
      args.push("--window", windowTarget.value);
    } else {
      args.push("--monitor", monitor.value);
    }
    args.push("--fps", fps.value, "--codec", codec.value);
    const seconds = Number(duration.value);
    if (captureMode.value !== "window" && region.value.trim()) args.push("--region", region.value.trim());
    if (Number.isFinite(seconds) && seconds > 0) args.push("--duration", String(seconds));
    if (systemAudio.checked) args.push("--system-audio");
    if (microphone.checked) args.push("--microphone");
    if (camera.checked) args.push("--camera");
    const command = Command.sidecar("binaries/dogsc_capture", args);
    command.stdout.on("data", (line) => log(String(line)));
    command.stderr.on("data", (line) => log(String(line)));
    command.on("error", (error) => log(`sidecar 启动失败：${String(error)}`));
    command.on("close", (event) => {
      recordingChild = null;
      stopTimer();
      setRecording(false);
      log(`录制进程结束（exit ${event.code ?? "unknown"}）`);
      if (event.code === 0) void loadProject(directory.value);
    });
    recordingChild = await command.spawn();
    setRecording(true);
    startTimer();
    log("已启动原生采集进程", false);
  } catch (error) {
    recordingChild = null;
    setRecording(false);
    log(`启动失败：${String(error)}`);
  }
});

pause.addEventListener("click", async () => {
  if (!recordingChild) return;
  await recordingChild.write("P\n");
  pause.textContent = pause.textContent === "暂停" ? "继续" : "暂停";
  log(pause.textContent === "继续" ? "已请求暂停" : "已请求继续", true);
});

stop.addEventListener("click", async () => {
  if (!recordingChild) return;
  stop.disabled = true;
  await recordingChild.write("Q\n");
  log("已请求停止，正在完成并保存分段……", true);
});
