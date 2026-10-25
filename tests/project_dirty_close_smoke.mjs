import { execFileSync, spawn } from "node:child_process";
import { copyFileSync, cpSync, existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { basename, join, resolve } from "node:path";
import { setTimeout as delay } from "node:timers/promises";

const [appArgument, sourceArgument] = process.argv.slice(2);
if (!appArgument || !sourceArgument) {
  console.error("用法: node tests/project_dirty_close_smoke.mjs <dogsc-tauri.exe> <工程目录>");
  process.exit(2);
}
const appPath = resolve(appArgument);
const source = resolve(sourceArgument);
const sourceProject = join(source, "project.json");
if (!existsSync(sourceProject)) throw new Error(`找不到工程文件: ${sourceProject}`);
const project = JSON.parse(readFileSync(sourceProject, "utf8"));
const presets = ["黑曜石网格", "工程白网格", "暗夜星阵", "建筑极简点", "立体等角网"];
const autoSavePreset = presets.find((item) => item !== project.canvas?.backgroundSource?.patternPreset) ?? presets[0];
const preset = presets.find((item) => item !== project.canvas?.backgroundSource?.patternPreset && item !== autoSavePreset) ?? presets[0];
const fixture = join(tmpdir(), `dogsc-dirty-close-${process.pid}`);
if (existsSync(fixture)) throw new Error(`临时工程目录已存在，拒绝覆盖: ${fixture}`);
mkdirSync(fixture, { recursive: true });
for (const name of ["project.json", "screen.mp4", "pointer.jsonl"]) {
  const from = join(source, name);
  if (existsSync(from)) copyFileSync(from, join(fixture, name));
}
const sourceAssets = join(source, "assets");
if (existsSync(sourceAssets)) cpSync(sourceAssets, join(fixture, "assets"), { recursive: true });

const port = 9400 + Math.floor(Math.random() * 500);
const app = spawn(appPath, [], {
  env: { ...process.env, WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS: `--remote-debugging-port=${port}` },
  stdio: "ignore",
});
let socket;
let requestId = 0;
const pending = new Map();

function cdp(method, params = {}) {
  const id = ++requestId;
  return new Promise((resolveMessage, reject) => {
    const timer = setTimeout(() => {
      pending.delete(id);
      reject(new Error(`CDP 请求超时: ${method}`));
    }, 10000);
    pending.set(id, (message) => {
      clearTimeout(timer);
      if (message.error) reject(new Error(message.error.message));
      else resolveMessage(message);
    });
    socket.send(JSON.stringify({ id, method, params }));
  });
}

async function evaluate(expression) {
  const response = await cdp("Runtime.evaluate", { expression, returnByValue: true, awaitPromise: true });
  if (response.result.exceptionDetails) {
    throw new Error(response.result.exceptionDetails.exception?.description ?? "页面脚本执行失败");
  }
  return response.result.result.value;
}

async function waitFor(description, predicate, timeout = 12000) {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    if (await predicate()) return;
    if (app.exitCode !== null) throw new Error(`Tauri 应用提前退出: ${app.exitCode}`);
    await delay(100);
  }
  throw new Error(`等待超时: ${description}`);
}

function powershell(script) {
  const encoded = Buffer.from(`$ProgressPreference='SilentlyContinue'; ${script}`, "utf16le").toString("base64");
  return execFileSync("powershell.exe", ["-NoProfile", "-NonInteractive", "-EncodedCommand", encoded], { encoding: "utf8" }).trim();
}

function requestClose() {
  const script = `$src='using System;using System.Runtime.InteropServices;public static class DogscClose{[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd,uint msg,IntPtr w,IntPtr l);}'; Add-Type -TypeDefinition $src; $p=Get-Process -Id ${app.pid} -ErrorAction Stop; [void][DogscClose]::PostMessage($p.MainWindowHandle,0x0010,[IntPtr]::Zero,[IntPtr]::Zero)`;
  powershell(script);
}

async function waitForAppExit() {
  await new Promise((resolveExit, rejectExit) => {
    const timer = setTimeout(() => rejectExit(new Error("dirty close 保存后应用未退出")), 12000);
    app.once("exit", (code) => {
      clearTimeout(timer);
      if (code !== 0) rejectExit(new Error(`Tauri 应用退出码异常: ${code}`));
      else resolveExit();
    });
  });
}

try {
  let target;
  const endpointDeadline = Date.now() + 20000;
  while (Date.now() < endpointDeadline) {
    if (app.exitCode !== null) throw new Error(`Tauri 应用提前退出: ${app.exitCode}`);
    try {
      const targets = await fetch(`http://127.0.0.1:${port}/json`).then((response) => response.json());
      target = targets.find((item) => item.type === "page");
      if (target) break;
    } catch {}
    await delay(100);
  }
  if (!target) throw new Error("等待 Tauri WebView 调试端点超时");

  socket = new WebSocket(target.webSocketDebuggerUrl);
  socket.addEventListener("message", (event) => {
    const message = JSON.parse(event.data);
    if (message.id) pending.get(message.id)?.(message);
  });
  await new Promise((resolveSocket, rejectSocket) => {
    socket.addEventListener("open", resolveSocket, { once: true });
    socket.addEventListener("error", rejectSocket, { once: true });
  });
  await waitFor("Tauri 工程编辑控件", async () => await evaluate('Boolean(document.querySelector("#project-path"))'));
  const title = project.title;
  const projectPath = join(fixture, basename(sourceProject));
  await evaluate(`(()=>{document.querySelector("#project-path").value=${JSON.stringify(projectPath)};document.querySelector("#project-open").click();return true})()`);
  await waitFor(`打开工程 ${title}`, async () => await evaluate(`document.querySelector("#project-title").textContent===${JSON.stringify(title)}`));
  if (await evaluate('document.querySelector("#project-save-state").textContent') !== "已保存") {
    throw new Error("无修改的首次打开不应报告 dirty draft");
  }
  await evaluate(`(()=>{document.querySelector("#style-background").value=${JSON.stringify(autoSavePreset)};document.querySelector("#style-apply").click();return true})()`);
  await waitFor("debounced dirty state", async () => await evaluate(
    'document.querySelector("#project-save-state").textContent==="待保存" && !document.querySelector("#style-apply").disabled'));
  await waitFor("debounced autosave", async () => await evaluate(
    'document.querySelector("#project-save-state").textContent==="已保存" && document.querySelector("#project-console").innerText.includes("草稿已自动保存")'));
  const autoSaved = JSON.parse(readFileSync(projectPath, "utf8"));
  if (autoSaved.canvas?.backgroundSource?.patternPreset !== autoSavePreset) throw new Error("debounced autosave did not commit the edit");
  if (existsSync(join(fixture, ".project-draft.json")) || existsSync(join(fixture, ".project-recovery.json"))) {
    throw new Error("successful autosave left dirty recovery artifacts");
  }

  await evaluate(`(()=>{const original=window.setTimeout.bind(window);window.setTimeout=(fn,ms,...args)=>original(fn,ms>=800?30000:ms,...args);document.querySelector("#style-background").value=${JSON.stringify(preset)};document.querySelector("#style-apply").click();return true})()`);

  await waitFor("dirty draft 已排队且编辑操作完成", async () => await evaluate(
    'document.querySelector("#project-save-state").textContent==="待保存" && !document.querySelector("#style-apply").disabled'));
  await delay(300);
  const draftPath = join(fixture, ".project-draft.json");
  const recoveryPath = join(fixture, ".project-recovery.json");
  if (!existsSync(draftPath) || !existsSync(recoveryPath)) throw new Error("dirty draft/recovery manifest 不存在");

  await evaluate("window.confirm=()=>false");
  requestClose();
  await delay(250);
  if (app.exitCode !== null) throw new Error("取消关闭后应用已退出");
  if (await evaluate('document.querySelector("#project-save-state").textContent') !== "待保存") {
    throw new Error("取消关闭不应保存或清除 dirty draft");
  }

  const backupPath = join(fixture, ".draft-backup.tmp");
  copyFileSync(draftPath, backupPath);
  writeFileSync(draftPath, "{", "utf8");
  await evaluate("window.confirm=()=>true");
  requestClose();
  await waitFor("关闭前保存失败状态", async () => await evaluate(
    'document.querySelector("#project-save-state").textContent==="保存失败"'));
  if (app.exitCode !== null) throw new Error("保存失败后窗口被关闭");

  copyFileSync(backupPath, draftPath);
  rmSync(backupPath, { force: true });
  await evaluate('document.querySelector("#project-save").click(); true');
  await waitFor("手动重试保存成功", async () => await evaluate(
    'document.querySelector("#project-save-state").textContent==="已保存"'));
  requestClose();
  await waitForAppExit();
  if (existsSync(draftPath) || existsSync(recoveryPath)) throw new Error("确认保存后仍残留 draft/recovery 文件");
  const saved = JSON.parse(readFileSync(projectPath, "utf8"));
  if (saved.canvas?.backgroundSource?.patternPreset !== preset) throw new Error("确认关闭未将 dirty 修改写入 project.json");
  console.log("project_dirty_close_smoke: clean begin, cancel preserves draft, failed save stays open, retry saves and closes");
} finally {
  socket?.close();
  if (app.exitCode === null) {
    try { execFileSync("taskkill.exe", ["/T", "/F", "/PID", String(app.pid)], { stdio: "ignore" }); } catch {}
  }
  rmSync(fixture, { recursive: true, force: true });
}
