import { homeDir, join } from "@tauri-apps/api/path";
import { Command } from "@tauri-apps/plugin-shell";
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

type ProjectSnapshot = {
  project: { title: string; version: number };
  fullSourceDuration: number;
  outputDuration: number;
  segments: ProjectSegment[];
  history: { canUndo: boolean; canRedo: boolean };
};

type ProjectLibraryItem = {
  name: string;
  path: string;
  title?: string;
  createdAt?: string;
  duration?: number;
  error?: string;
};

type ProjectLibrary = { root: string; projects: ProjectLibraryItem[] };

let projectSnapshot: ProjectSnapshot | null = null;
let projectFile = "";
let projectLibrary: ProjectLibrary | null = null;

const defaultDirectory = async () => join(await homeDir(), "Videos", "DogSC");

app.innerHTML = `
  <main class="shell">
    <aside class="rail">
      <div class="mark"><span>DS</span><small>STUDIO</small></div>
      <nav aria-label="主导航">
        <button class="rail-item active" type="button"><span class="rail-icon">◉</span><span>录制</span></button>
        <button id="rail-projects" class="rail-item" type="button"><span class="rail-icon">▦</span><span>项目</span></button>
        <button class="rail-item" type="button" disabled><span class="rail-icon">◒</span><span>素材</span></button>
      </nav>
      <div class="rail-foot"><span class="led"></span><span>Native core</span></div>
    </aside>
    <section class="workspace">
      <header class="topbar">
        <div>
          <p class="eyebrow">WINDOWS CAPTURE DESK <span>·</span> v0.1 native shell</p>
          <h1>开始一段干净的录制</h1>
        </div>
        <div class="top-actions">
          <span class="system-chip"><i></i> WGC ready</span>
          <button class="icon-button" type="button" title="设置" disabled>⚙</button>
        </div>
      </header>
      <div class="content-grid">
        <section class="hero-card">
          <div class="hero-grid"></div>
          <div class="hero-copy">
            <span class="section-kicker">01 / CAPTURE SOURCE</span>
            <h2>把画面留给<br /><em>真正重要的内容</em></h2>
            <p>窗口、显示器或局部区域。光标独立保存，后续可在编辑器中重新合成。</p>
          </div>
          <div class="signal"><span class="signal-dot"></span><span id="status-text">READY TO CAPTURE</span></div>
        </section>
        <section class="panel settings-panel">
          <div class="panel-heading"><span class="section-kicker">02 / SESSION SETUP</span><span class="panel-index">A</span></div>
          <label class="field"><span>输出目录</span><input id="directory" type="text" spellcheck="false" /></label>
          <div class="field-row">
            <label class="field"><span>显示器索引</span><select id="monitor"><option value="0">主显示器 · 0</option><option value="1">第二显示器 · 1</option><option value="2">第三显示器 · 2</option></select></label>
            <label class="field"><span>帧率</span><select id="fps"><option>30</option><option selected>60</option><option>90</option><option>120</option></select></label>
          </div>
          <div class="field-row">
            <label class="field"><span>编码器</span><select id="codec"><option value="auto" selected>自动选择</option><option value="hevc">HEVC</option><option value="h264">H.264</option><option value="prores">ProRes 422</option></select></label>
            <label class="field"><span>时长（秒）</span><input id="duration" type="number" min="0" step="1" placeholder="手动停止" /></label>
          </div>
          <div class="toggle-row"><label><input id="system-audio" type="checkbox" checked /><span class="toggle"></span><span>系统声音</span></label><label><input id="microphone" type="checkbox" /><span class="toggle"></span><span>麦克风</span></label><label><input id="camera" type="checkbox" /><span class="toggle"></span><span>摄像头</span></label></div>
          <div class="actions"><button id="start" class="primary" type="button"><span class="record-glyph"></span>开始录制</button><button id="pause" class="secondary" type="button" disabled>暂停</button><button id="stop" class="secondary danger" type="button" disabled>停止并保存</button></div>
        </section>
        <section class="panel monitor-panel">
          <div class="panel-heading"><span class="section-kicker">03 / NATIVE PIPELINE</span><span class="panel-index">B</span></div>
          <div class="metrics"><div><strong id="elapsed">00:00</strong><span>本次时长</span></div><div><strong>90,000</strong><span>时间基</span></div><div><strong id="backend">WGC + MF</strong><span>采集后端</span></div></div>
          <div class="console" id="console" aria-live="polite"><span class="console-line muted">等待录制任务……</span></div>
          <p class="note">当前为 Tauri 原生壳首个切片。停止后会在输出目录生成媒体文件、指针轨和 project.json。</p>
        </section>
        <section class="panel editor-panel">
          <div class="panel-heading"><span class="section-kicker">04 / PROJECT EDITOR</span><span class="panel-index">C</span></div>
          <div class="project-library-row"><label class="field"><span>项目库目录</span><input id="library-root" type="text" spellcheck="false" placeholder="例如：D:\\Videos\\DogSC" /></label><button id="library-refresh" class="secondary" type="button">刷新项目</button></div>
          <div id="project-library" class="project-library"><span class="timeline-empty">刷新后列出工程目录</span></div>
          <div class="project-open-row">
            <label class="field"><span>project.json 路径或工程目录</span><input id="project-path" type="text" spellcheck="false" placeholder="例如：D:\\Videos\\DogSC\\演示录制" /></label>
            <button id="project-open" class="secondary" type="button">打开工程</button>
          </div>
          <div class="project-rename-row"><label class="field"><span>工程名称</span><input id="project-rename-name" type="text" spellcheck="false" placeholder="新工程目录名" /></label><button id="project-rename" class="secondary" type="button" disabled>重命名</button></div>
          <div class="project-summary"><div><strong id="project-title">未打开工程</strong><span>工程</span></div><div><strong id="project-duration">—</strong><span>输出时长</span></div><div><strong id="project-segment-count">—</strong><span>片段</span></div><div class="project-history-actions"><button id="project-undo" class="secondary" type="button" disabled>撤销</button><button id="project-redo" class="secondary" type="button" disabled>重做</button></div></div>
          <div id="timeline" class="timeline"><span class="timeline-empty">打开录制目录中的 project.json 后开始编辑</span></div>
          <div class="editor-controls">
            <label class="field"><span>时间点（秒）</span><input id="edit-time" type="number" min="0" step="0.01" placeholder="0.00" /></label>
            <label class="field"><span>当前片段</span><select id="edit-segment"><option value="">未选择</option></select></label>
            <label class="field"><span>速度</span><input id="edit-rate" type="number" min="1" max="100" step="0.1" value="1" /></label>
            <div class="editor-actions"><button id="edit-split" class="secondary" type="button" disabled>在时间点分割</button><button id="edit-trim-left" class="secondary" type="button" disabled>左裁切</button><button id="edit-trim-right" class="secondary" type="button" disabled>右裁切</button><button id="edit-rate-button" class="secondary" type="button" disabled>应用速度</button><button id="edit-move-up" class="secondary" type="button" disabled>片段上移</button><button id="edit-move-down" class="secondary" type="button" disabled>片段下移</button><button id="edit-remove" class="secondary danger" type="button" disabled>删除片段</button></div>
          </div>
          <div id="project-console" class="project-console" aria-live="polite"><span class="console-line muted">等待打开工程……</span></div>
        </section>
      </div>
    </section>
  </main>
`;

const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;
const directory = $("directory") as HTMLInputElement;
const monitor = $("monitor") as HTMLSelectElement;
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

function renderProjectSnapshot() {
  const snapshot = projectSnapshot;
  if (!snapshot) {
    projectTitle.textContent = "未打开工程";
    projectDuration.textContent = "—";
    projectSegmentCount.textContent = "—";
    timeline.innerHTML = '<span class="timeline-empty">打开录制目录中的 project.json 后开始编辑</span>';
    updateEditorButtons();
    return;
  }
  projectTitle.textContent = snapshot.project.title;
  projectDuration.textContent = formatSeconds(snapshot.outputDuration);
  projectSegmentCount.textContent = String(snapshot.segments.length);
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
  const hasProject = snapshot !== null && projectFile.length > 0;
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

async function loadProject(value: string) {
  const path = normalizeProjectFile(value);
  if (!path) { projectLog("请输入 project.json 路径", false); return; }
  projectOpen.disabled = true;
  projectLog(`正在打开 ${path}`, true);
  try {
    projectSnapshot = await runProjectCommand(["inspect", path]);
    projectFile = path;
    projectPathInput.value = path;
    renderProjectSnapshot();
    projectLog("工程已打开", false);
  } catch (error) {
    projectLog(`打开工程失败：${String(error)}`, false);
  } finally {
    projectOpen.disabled = false;
    updateEditorButtons();
  }
}

async function applyProjectEdit(operation: string, ...args: string[]) {
  if (!projectFile) return;
  projectOpen.disabled = true;
  try {
    projectSnapshot = await runProjectCommand(["edit", projectFile, operation, ...args]);
    renderProjectSnapshot();
    projectLog(`已应用 ${operation}，project.previous.json 已保留`, false);
  } catch (error) {
    projectLog(`编辑失败：${String(error)}`, false);
  } finally {
    projectOpen.disabled = false;
    updateEditorButtons();
  }
}

async function applyProjectHistory(action: "undo" | "redo") {
  if (!projectFile) return;
  projectOpen.disabled = true;
  try {
    projectSnapshot = await runProjectCommand([action, projectFile]);
    renderProjectSnapshot();
    projectLog(action === "undo" ? "已撤销上一组编辑" : "已重做上一组编辑", false);
  } catch (error) {
    projectLog(`${action === "undo" ? "撤销" : "重做"}失败：${String(error)}`, false);
  } finally {
    projectOpen.disabled = false;
    updateEditorButtons();
  }
}

async function renameProject() {
  if (!projectFile || !projectRenameName.value.trim()) return;
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
void defaultDirectory().then((value) => { directory.value = value; libraryRoot.value = value; });


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
    const args = ["record", directory.value.trim(), "--monitor", monitor.value, "--fps", fps.value, "--codec", codec.value];
    const seconds = Number(duration.value);
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
