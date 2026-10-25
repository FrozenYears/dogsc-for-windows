import { execFileSync, spawn } from "node:child_process";
import { createServer } from "node:net";
import { setTimeout as delay } from "node:timers/promises";
import { copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { resolve, join } from "node:path";

const [appArgument, projectAArgument, projectBArgument] = process.argv.slice(2);
if (!appArgument || !projectAArgument || !projectBArgument) {
  console.error("用法: node tests/stream_child_lifecycle_smoke.mjs <dogsc-tauri.exe> <工程 A> <工程 B>");
  process.exit(2);
}

function projectPath(value) {
  const path = resolve(value);
  const file = existsSync(path) && !path.toLowerCase().endsWith(".json") ? join(path, "project.json") : path;
  if (!existsSync(file)) throw new Error(`找不到工程文件: ${file}`);
  return file;
}

const appPath = resolve(appArgument);
let projectA = projectPath(projectAArgument);
const projectB = projectPath(projectBArgument);
let exportCancelDirectory = "";
if (process.env.DOGSC_EXERCISE_EXPORT_CANCEL === "1" || process.env.DOGSC_EXERCISE_M4A_EXPORT === "1" || process.env.DOGSC_EXERCISE_M4A_CANCEL === "1") {
  exportCancelDirectory = mkdtempSync(join(tmpdir(), "dogsc-export-smoke-"));
}
let temporaryProjectA = "";
if (process.env.DOGSC_REQUIRE_CURSOR === "1" || process.env.DOGSC_REQUIRE_STICKERS === "1" || process.env.DOGSC_REQUIRE_SPOTLIGHT === "1" || process.env.DOGSC_REQUIRE_SCREEN_FRAME === "1" || process.env.DOGSC_REQUIRE_OPENING === "1") {
  temporaryProjectA = mkdtempSync(join(tmpdir(), "dogsc-scene-smoke-"));
  const sourceProject = JSON.parse(readFileSync(projectA, "utf8"));
  if (process.env.DOGSC_REQUIRE_CURSOR === "1") {
    sourceProject.cursorStyle = { ...sourceProject.cursorStyle, assetID: "system.arrow" };
  }
  if (process.env.DOGSC_REQUIRE_SCREEN_FRAME === "1") {
    sourceProject.canvas = { ...sourceProject.canvas, screenFrame: "browserDark" };
  }
  if (process.env.DOGSC_REQUIRE_OPENING === "1") {
    sourceProject.openingSequence = { ...sourceProject.openingSequence, isEnabled: true, duration: 1.0, stagger: 0.1 };
  }
  for (const reference of Object.values(sourceProject.media ?? {})) {
    if (!reference?.relativePath) continue;
    const source = resolve(projectA, "..", reference.relativePath);
    copyFileSync(source, join(temporaryProjectA, reference.relativePath));
  }
  if (process.env.DOGSC_REQUIRE_STICKERS === "1") {
    mkdirSync(join(temporaryProjectA, "assets"), { recursive: true });
    writeFileSync(join(temporaryProjectA, "assets", "logo.png"), Buffer.from(
      "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=", "base64"));
    sourceProject.timeline = sourceProject.timeline ?? {};
    sourceProject.timeline.stickerClips = [{
      id: "cdf57400-255e-4149-9a82-b5b579815e83",
      timing: { startTime: 0, duration: 1 }, relativePath: "assets/logo.png",
      position: { x: 0.5, y: 0.5 }, width: 0.18, rotationDegrees: 0, opacity: 1,
      cornerRadius: 8, borderWidth: 0, borderColor: "#FFFFFF", shadowOpacity: 0,
      shadowRadius: 0, shadowOffsetX: 0, shadowOffsetY: 0, animation: "pop",
      animationCurve: "swift", enterDuration: 0.2, exitDuration: 0.2,
      backdropBlur: 0, backdropBlurIncludesCamera: false, hidesScreen: false,
      hidesCamera: false, layerIndex: 0,
    }];
  }
  if (process.env.DOGSC_REQUIRE_SPOTLIGHT === "1") {
    sourceProject.timeline = sourceProject.timeline ?? {};
    sourceProject.timeline.mosaicClips = [{
      id: "ebfabc8c-30b9-4e47-b43a-a13c747ee98e",
      timing: { startTime: 0, duration: 1 }, sourceRect: { x: 0.3, y: 0.3, width: 0.4, height: 0.3 },
      cornerRadius: 0.08, style: "spotlight", intensity: 0.7, spotlightDimming: 0.22,
      transitionStyle: "none", transitionInDuration: 0.3, transitionOutDuration: 0.3,
    }];
  }
  const pointerPath = join(temporaryProjectA, sourceProject.media.pointerEvents.relativePath);
  let pointerText = readFileSync(pointerPath, "utf8");
  if (process.env.DOGSC_REQUIRE_CURSOR === "1") {
    pointerText = pointerText.replaceAll("system.hidden", "system.arrow").replaceAll('"kind":"move"', '"kind":"leftClick"');
  }
  writeFileSync(pointerPath, pointerText);
  writeFileSync(join(temporaryProjectA, "project.json"), JSON.stringify(sourceProject, null, 2));
  projectA = join(temporaryProjectA, "project.json");
}
const titleA = JSON.parse(readFileSync(projectA, "utf8")).title;
const titleB = JSON.parse(readFileSync(projectB, "utf8")).title;
if (projectA.toLowerCase() === projectB.toLowerCase()) throw new Error("必须提供两个不同的工程文件");
const port = await new Promise((resolvePort, rejectPort) => {
  const server = createServer();
  server.once("error", rejectPort);
  server.listen(0, "127.0.0.1", () => {
    const address = server.address();
    server.close((error) => error ? rejectPort(error) : resolvePort(typeof address === "object" && address ? address.port : 0));
  });
});
const app = spawn(appPath, [], {
  env: { ...process.env, WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS: `--remote-debugging-port=${port}` },
  stdio: "ignore",
});

let socket;
let nextId = 0;
const pending = new Map();

function evaluate(expression) {
  return send("Runtime.evaluate", { expression, returnByValue: true, awaitPromise: true });
}

function send(method, params = {}) {
  const id = ++nextId;
  return new Promise((resolveMessage, reject) => {
    const timer = setTimeout(() => {
      pending.delete(id);
      reject(new Error(`CDP 请求超时: ${method}`));
    }, 30000);
    pending.set(id, (message) => {
      clearTimeout(timer);
      if (message.error) reject(new Error(message.error.message));
      else resolveMessage(message);
    });
    socket.send(JSON.stringify({ id, method, params }));
  });
}

async function waitFor(description, predicate, timeout = 15000) {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    if (await predicate()) return;
    await delay(100);
  }
  throw new Error(`等待超时: ${description}`);
}

async function evaluateValue(expression) {
  const result = await evaluate(expression);
  if (result.result.exceptionDetails) {
    throw new Error(result.result.exceptionDetails.exception?.description ?? "页面脚本执行失败");
  }
  return result.result.result.value;
}

async function openProject(path, title) {
  await evaluateValue(`(()=>{const p=document.querySelector("#project-path");p.value=${JSON.stringify(path)};document.querySelector("#project-open").click();return true})()`);
  await waitFor(`工程打开: ${title}`, async () => await evaluateValue(
    `document.querySelector("#project-title").textContent === ${JSON.stringify(title)}`));
}
async function revealPreview() {
  await evaluateValue('document.querySelector("#project-preview-image").scrollIntoView({block:"center",inline:"center"}); true');
  await waitFor("preview image 在窗口内可见", async () => await evaluateValue(
    '(()=>{const r=document.querySelector("#project-preview-image").getBoundingClientRect();return r.width>0&&r.height>0&&r.left>=0&&r.top>=0&&r.right<=innerWidth&&r.bottom<=innerHeight})()'));
}

async function streamProcesses() {
  const script = "$ProgressPreference='SilentlyContinue'; $ErrorActionPreference='Stop'; Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -and $_.CommandLine -like '*dogsc_capture*project*stream*' } | Select-Object ProcessId,CommandLine | ConvertTo-Json -Compress";
  const encoded = Buffer.from(script, "utf16le").toString("base64");
  const output = execFileSync("powershell.exe", ["-NoProfile", "-EncodedCommand", encoded], { encoding: "utf8" }).trim();
  if (!output) return [];
  const value = JSON.parse(output);
  return Array.isArray(value) ? value : [value];
}
function windowCommand(action) {
  const script = `$ProgressPreference='SilentlyContinue'; $ErrorActionPreference='Stop'; Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class DogscSmokeWindow {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr hWnd, int x, int y, int width, int height, bool repaint);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int command);
}
'@; $h=(Get-Process -Id ${app.pid}).MainWindowHandle; if ($h -eq 0) { throw '找不到 Tauri 主窗口' }; $r=New-Object DogscSmokeWindow+RECT; [DogscSmokeWindow]::GetWindowRect($h,[ref]$r) | Out-Null; switch ('${action}') { 'move-resize' { [DogscSmokeWindow]::MoveWindow($h,$r.Left+16,$r.Top+16,[Math]::Max(640,$r.Right-$r.Left+32),[Math]::Max(480,$r.Bottom-$r.Top+24),$true) | Out-Null }; 'minimize' { [DogscSmokeWindow]::ShowWindow($h,6) | Out-Null }; 'restore' { [DogscSmokeWindow]::ShowWindow($h,9) | Out-Null } }; [DogscSmokeWindow]::GetWindowRect($h,[ref]$r) | Out-Null; [pscustomobject]@{left=$r.Left;top=$r.Top;width=$r.Right-$r.Left;height=$r.Bottom-$r.Top} | ConvertTo-Json -Compress`;
  const encoded = Buffer.from(script, "utf16le").toString("base64");
  const output = execFileSync("powershell.exe", ["-NoProfile", "-NonInteractive", "-InputFormat", "None", "-EncodedCommand", encoded], { encoding: "utf8", timeout: 10000 }).trim();
  return JSON.parse(output);
}

async function waitForStream(path, present) {
  try {
    await waitFor(`${present ? "启动" : "退出"} ${path} 的 stream child`, async () => {
      const processes = await streamProcesses();
      const found = processes.some((item) => item.CommandLine.toLowerCase().includes(path.toLowerCase()));
      return present ? found : !found;
    });
  } catch (error) {
    const status = await evaluateValue('document.querySelector("#project-preview-status").textContent');
    throw new Error(`${String(error)}；previewStatus=${status}`);
  }
}

try {
  const deadline = Date.now() + 20000;
  let target;
  while (Date.now() < deadline) {
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
  await waitFor("Tauri 工程控件就绪", async () => await evaluateValue('Boolean(document.querySelector("#project-path") && document.querySelector("#project-preview-play"))'));
  if (process.env.DOGSC_EXERCISE_REGION === "1") {
    await evaluateValue('document.querySelector("#region-pick").click(); true');
    await waitFor("区域选择器打开", async () => await evaluateValue('Boolean(document.querySelector(".region-surface"))'));
    await evaluateValue('(()=>{const s=document.querySelector(".region-surface"),r=s.getBoundingClientRect();const a={bubbles:true,clientX:r.left+r.width*.2,clientY:r.top+r.height*.2,pointerId:1};s.dispatchEvent(new PointerEvent("pointerdown",a));s.dispatchEvent(new PointerEvent("pointermove",{...a,clientX:r.left+r.width*.8,clientY:r.top+r.height*.75}));s.dispatchEvent(new PointerEvent("pointerup",{...a,clientX:r.left+r.width*.8,clientY:r.top+r.height*.75}));document.querySelector(".region-apply").click();return true})()');
    await waitFor("区域选择器写入参数", async () => await evaluateValue('document.querySelector("#region").value.split(",").length === 4'));
  }

  await openProject(projectA, titleA);
  if (process.env.DOGSC_EXERCISE_SCENE_CONTROLS === "1") {
    await evaluateValue('(()=>{document.querySelector("#style-screen-frame").value="browserDark";document.querySelector("#style-screen-motion").value="平滑";document.querySelector("#style-cursor-motion").value="适中";document.querySelector("#style-opening-enabled").checked=true;document.querySelector("#style-opening-preset").value="侧滑";document.querySelector("#style-opening-duration").value="1.2";document.querySelector("#style-apply").click();return true})()');
    await waitFor("scene styles 自动保存", async () => await evaluateValue('document.querySelector("#project-save-state").textContent === "已保存"'));
    await evaluateValue('(()=>{document.querySelector("#screen-motion-geometry").value="0.5,0.5,1.2,6,-4,0,0.65";document.querySelector("#screen-motion-apply").click();return true})()');
    await waitFor("screen motion clip 自动保存", async () => await evaluateValue('document.querySelector("#project-save-state").textContent === "已保存"'));
  }
  if (process.env.DOGSC_EXERCISE_MARKER === "1") {
    await evaluateValue('document.querySelector("#edit-time").value="0.05";document.querySelector("#edit-marker").click(); true');
    await waitFor("recording marker 自动保存", async () => await evaluateValue('document.querySelector("#project-save-state").textContent === "已保存"'));
  }
  await revealPreview();
  await evaluateValue('document.querySelector("#edit-time").value="0";document.querySelector("#project-preview-play").click(); true');
  await waitForStream(projectA, true);
  let previewStatusA = "";
  await waitFor("工程 A 的帧呈现统计", async () => {
    const status = await evaluateValue('document.querySelector("#project-preview-status").textContent');
    if (status.startsWith("预览失败") || status.startsWith("预览错误")) throw new Error(`工程 A 预览失败: ${status}`);
    const ready = status.includes("丢帧") &&
      (process.env.DOGSC_REQUIRE_CURSOR !== "1" || status.includes("click")) &&
      (process.env.DOGSC_REQUIRE_STICKERS !== "1" || status.includes("stickers")) &&
      (process.env.DOGSC_REQUIRE_SPOTLIGHT !== "1" || status.includes("spotlight")) &&
      (process.env.DOGSC_REQUIRE_SCREEN_FRAME !== "1" || status.includes("screen-frame")) &&
      (process.env.DOGSC_REQUIRE_OPENING !== "1" || status.includes("opening")) &&
      (process.env.DOGSC_REQUIRE_SCREEN_MOTION !== "1" || status.includes("screen-motion"));
    if (ready) previewStatusA = status;
    return ready;
  });
  if (process.env.DOGSC_SCREENSHOT) {
    await send("Page.enable");
    const screenshot = await send("Page.captureScreenshot", { format: "png" });
    writeFileSync(process.env.DOGSC_SCREENSHOT, Buffer.from(screenshot.result.data, "base64"));
  }
  if (!previewStatusA) previewStatusA = await evaluateValue('document.querySelector("#project-preview-status").textContent');
  if (process.env.DOGSC_REQUIRE_DXGI_SURFACE === "1" &&
      (!previewStatusA.includes("DXGI swapchain") || !previewStatusA.includes("D2D scene") || !previewStatusA.includes("vblank") ||
       await evaluateValue('document.querySelector("#project-preview-image").style.visibility!=="hidden"'))) {
    throw new Error(`工程 A 未进入原生 D2D/DXGI presentation 或缺少 frame statistics: ${previewStatusA}`);
  }
  if (process.env.DOGSC_REQUIRE_CAMERA === "1" && !previewStatusA.includes("camera")) {
    throw new Error(`工程 A 未产生摄像头 scene 层: ${previewStatusA}`);
  }
  if (process.env.DOGSC_REQUIRE_CURSOR === "1" && !previewStatusA.includes("cursor")) {
    throw new Error(`工程 A 未产生光标 scene 层: ${previewStatusA}`);
  }
  if (process.env.DOGSC_REQUIRE_CURSOR === "1" && !previewStatusA.includes("click")) {
    throw new Error(`工程 A 未产生光标点击效果 scene: ${previewStatusA}`);
  }
  if (process.env.DOGSC_REQUIRE_STICKERS === "1" && !previewStatusA.includes("stickers")) {
    throw new Error(`工程 A 未产生贴图 scene 层: ${previewStatusA}`);
  }
  if (process.env.DOGSC_REQUIRE_SPOTLIGHT === "1" && !previewStatusA.includes("spotlight")) {
    throw new Error(`工程 A 未产生 spotlight scene 层: ${previewStatusA}`);
  }
  if (process.env.DOGSC_REQUIRE_SCREEN_FRAME === "1" && !previewStatusA.includes("screen-frame")) {
    throw new Error(`工程 A 未产生 screen frame scene 层: ${previewStatusA}`);
  }
  if (process.env.DOGSC_REQUIRE_OPENING === "1" && !previewStatusA.includes("opening")) {
    throw new Error(`工程 A 未产生 opening scene 层: ${previewStatusA}`);
  }
  if (process.env.DOGSC_REQUIRE_SCREEN_MOTION === "1" && !previewStatusA.includes("screen-motion")) {
    throw new Error(`工程 A 未产生 screen motion scene 层: ${previewStatusA}`);
  }
  if (process.env.DOGSC_EXERCISE_WINDOW === "1") {
    const beforeWindow = windowCommand("get");
    const resizedWindow = windowCommand("move-resize");
    if (resizedWindow.width <= beforeWindow.width || resizedWindow.height <= beforeWindow.height) {
      throw new Error(`窗口 move/resize 未生效: before=${JSON.stringify(beforeWindow)}, after=${JSON.stringify(resizedWindow)}`);
    }
    await delay(700);
    await evaluateValue('window.scrollTo(0, document.body.scrollHeight); true');
    await delay(500);
    await revealPreview();
    await evaluateValue('document.querySelector("#project-preview-play").click(); true');
    await waitFor("滚动恢复并重新呈现 DXGI scene", async () => {
      const status = await evaluateValue('document.querySelector("#project-preview-status").textContent');
      return status.includes("D2D scene") && status.includes("DXGI swapchain") && status.includes("vblank");
    });
  }

  await openProject(projectB, titleB);
  await waitForStream(projectA, false);
  await revealPreview();
  await evaluateValue('document.querySelector("#edit-time").value="0";document.querySelector("#project-preview-play").click(); true');
  await waitForStream(projectB, true);
  await waitFor("工程 B 的帧呈现统计", async () => await evaluateValue('document.querySelector("#project-preview-status").textContent.includes("背压")'));
  const previewStatus = await evaluateValue('document.querySelector("#project-preview-status").textContent');
  if (process.env.DOGSC_REQUIRE_DXGI_SURFACE === "1" &&
      (!previewStatus.includes("DXGI swapchain") || !previewStatus.includes("D2D scene") || !previewStatus.includes("vblank") ||
       await evaluateValue('document.querySelector("#project-preview-image").style.visibility!=="hidden"'))) {
    throw new Error(`工程 B 未进入原生 D2D/DXGI presentation 或缺少 frame statistics: ${previewStatus}`);
  }
  if (process.env.DOGSC_EXERCISE_WINDOW === "1") {
    const beforeWindow = windowCommand("get");
    const resizedWindow = windowCommand("move-resize");
    if (resizedWindow.width <= beforeWindow.width || resizedWindow.height <= beforeWindow.height) {
      throw new Error(`工程 B 窗口 move/resize 未生效: before=${JSON.stringify(beforeWindow)}, after=${JSON.stringify(resizedWindow)}`);
    }
    await delay(700);
    await evaluateValue('window.scrollTo(0, document.body.scrollHeight); true');
    await delay(500);
    await revealPreview();
    await evaluateValue('document.querySelector("#project-preview-play").click(); true');
    await waitFor("工程 B 滚动恢复并重新呈现 DXGI scene", async () => {
      const status = await evaluateValue('document.querySelector("#project-preview-status").textContent');
      return status.includes("D2D scene") && status.includes("DXGI swapchain") && status.includes("vblank");
    });
  }
  if (!/帧 [1-9]\d*\/[1-9]\d*/.test(previewStatus) || !previewStatus.includes("丢帧")) {
    throw new Error(`没有取得已呈现帧统计: ${previewStatus}`);
  }
  const active = await streamProcesses();
  const activeProjectStreams = active.filter((item) => item.CommandLine.toLowerCase().includes("project stream"));
  if (activeProjectStreams.length !== 1 || !activeProjectStreams[0].CommandLine.toLowerCase().includes(projectB.toLowerCase())) {
    throw new Error(`切换后 stream child 状态不正确: ${JSON.stringify(activeProjectStreams)}`);
  }
  if (process.env.DOGSC_EXERCISE_EXPORT_CANCEL === "1") {
    await openProject(projectA, titleA);
    const exportOutput = join(exportCancelDirectory, "cancel.mp4");
    await evaluateValue(`(()=>{const p=document.querySelector("#project-export-path");p.value=${JSON.stringify(exportOutput)};p.dispatchEvent(new Event("input",{bubbles:true}));return true})()`);
    await waitFor("导出按钮启用", async () => await evaluateValue('document.querySelector("#project-export").disabled === false'));
    await evaluateValue('document.querySelector("#project-export").click(); true');
    await waitFor("导出取消按钮启用", async () => await evaluateValue('document.querySelector("#project-export-cancel").disabled === false'));
    await evaluateValue('document.querySelector("#project-export-cancel").click(); true');
    await waitFor("导出取消完成", async () => await evaluateValue('document.querySelector("#project-export-cancel").disabled === true && document.querySelector("#project-export").disabled === false'));
    if (existsSync(exportOutput) || existsSync(`${exportOutput}.partial`)) {
      throw new Error(`取消导出遗留输出文件: ${exportOutput}`);
    }
  }
  if (process.env.DOGSC_EXERCISE_M4A_EXPORT === "1") {
    await openProject(projectA, titleA);
    const exportOutput = join(exportCancelDirectory, "audio.m4a");
    await evaluateValue(`(()=>{document.querySelector("#project-export-format").value="m4a";const p=document.querySelector("#project-export-path");p.value=${JSON.stringify(exportOutput)};p.dispatchEvent(new Event("input",{bubbles:true}));document.querySelector("#project-export").click();return true})()`);
    await waitFor("M4A 导出完成", async () => await evaluateValue('document.querySelector("#project-export").disabled === false && document.querySelector("#project-console").textContent.includes("M4A 导出完成")'));
    if (!existsSync(exportOutput)) throw new Error(`M4A 导出文件不存在: ${exportOutput}`);
  }
  if (process.env.DOGSC_EXERCISE_M4A_CANCEL === "1") {
    await openProject(projectA, titleA);
    const exportOutput = join(exportCancelDirectory, "cancel.m4a");
    await evaluateValue(`(()=>{document.querySelector("#project-export-format").value="m4a";const p=document.querySelector("#project-export-path");p.value=${JSON.stringify(exportOutput)};p.dispatchEvent(new Event("input",{bubbles:true}));return true})()`);
    await waitFor("M4A export button enabled", async () => await evaluateValue('document.querySelector("#project-export").disabled === false'));
    await evaluateValue('document.querySelector("#project-export").click(); true');
    await waitFor("M4A cancel button enabled", async () => await evaluateValue('document.querySelector("#project-export-cancel").disabled === false'));
    await evaluateValue('document.querySelector("#project-export-cancel").click(); true');
    await waitFor("M4A cancel completed", async () => await evaluateValue('document.querySelector("#project-export-cancel").disabled === true && document.querySelector("#project-export").disabled === false'));
    if (existsSync(exportOutput) || existsSync(`${exportOutput}.partial`)) throw new Error(`M4A cancel left output: ${exportOutput}`);
  }
  console.log(`stream_child_lifecycle_smoke: A=${previewStatusA}; B=${previewStatus}; project A child exited before project B child started`);
} finally {
  socket?.close();
  if (app.exitCode === null) {
    try { execFileSync("taskkill.exe", ["/T", "/F", "/PID", String(app.pid)], { stdio: "ignore" }); } catch {}
  }
  if (temporaryProjectA) rmSync(temporaryProjectA, { recursive: true, force: true });
  if (exportCancelDirectory) rmSync(exportCancelDirectory, { recursive: true, force: true });
}
