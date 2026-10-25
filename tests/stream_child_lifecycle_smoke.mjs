import { execFileSync, spawn } from "node:child_process";
import { createServer } from "node:net";
import { setTimeout as delay } from "node:timers/promises";
import { existsSync, readFileSync } from "node:fs";
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
const projectA = projectPath(projectAArgument);
const projectB = projectPath(projectBArgument);
if (projectA.toLowerCase() === projectB.toLowerCase()) throw new Error("必须提供两个不同的工程文件");
const titleA = JSON.parse(readFileSync(projectA, "utf8")).title;
const titleB = JSON.parse(readFileSync(projectB, "utf8")).title;
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
    }, 10000);
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

async function waitForStream(path, present) {
  await waitFor(`${present ? "启动" : "退出"} ${path} 的 stream child`, async () => {
    const processes = await streamProcesses();
    const found = processes.some((item) => item.CommandLine.toLowerCase().includes(path.toLowerCase()));
    return present ? found : !found;
  });
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

  await openProject(projectA, titleA);
  await revealPreview();
  await evaluateValue('document.querySelector("#edit-time").value="0";document.querySelector("#project-preview-play").click(); true');
  await waitForStream(projectA, true);
  await waitFor("工程 A 的帧呈现统计", async () => {
    const status = await evaluateValue('document.querySelector("#project-preview-status").textContent');
    if (status.startsWith("预览失败") || status.startsWith("预览错误")) throw new Error(`工程 A 预览失败: ${status}`);
    return status.includes("丢帧");
  });
  const previewStatusA = await evaluateValue('document.querySelector("#project-preview-status").textContent');
  if (process.env.DOGSC_REQUIRE_DXGI_SURFACE === "1" &&
      (!previewStatusA.includes("DXGI swapchain") || !previewStatusA.includes("D2D scene") ||
       await evaluateValue('document.querySelector("#project-preview-image").style.visibility!=="hidden"'))) {
    throw new Error(`工程 A 未进入原生 D2D/DXGI presentation: ${previewStatusA}`);
  }

  await openProject(projectB, titleB);
  await waitForStream(projectA, false);
  await revealPreview();
  await evaluateValue('document.querySelector("#edit-time").value="0";document.querySelector("#project-preview-play").click(); true');
  await waitForStream(projectB, true);
  await waitFor("工程 B 的帧呈现统计", async () => await evaluateValue('document.querySelector("#project-preview-status").textContent.includes("背压")'));
  const previewStatus = await evaluateValue('document.querySelector("#project-preview-status").textContent');
  if (process.env.DOGSC_REQUIRE_DXGI_SURFACE === "1" &&
      (!previewStatus.includes("DXGI swapchain") || !previewStatus.includes("D2D scene") ||
       await evaluateValue('document.querySelector("#project-preview-image").style.visibility!=="hidden"'))) {
    throw new Error(`工程 B 未进入原生 D2D/DXGI presentation: ${previewStatus}`);
  }
  if (!/帧 [1-9]\d*\/[1-9]\d*/.test(previewStatus) || !previewStatus.includes("丢帧")) {
    throw new Error(`没有取得已呈现帧统计: ${previewStatus}`);
  }
  const active = await streamProcesses();
  const activeProjectStreams = active.filter((item) => item.CommandLine.toLowerCase().includes("project stream"));
  if (activeProjectStreams.length !== 1 || !activeProjectStreams[0].CommandLine.toLowerCase().includes(projectB.toLowerCase())) {
    throw new Error(`切换后 stream child 状态不正确: ${JSON.stringify(activeProjectStreams)}`);
  }
  console.log(`stream_child_lifecycle_smoke: A=${previewStatusA}; B=${previewStatus}; project A child exited before project B child started`);
} finally {
  socket?.close();
  if (app.exitCode === null) {
    try { execFileSync("taskkill.exe", ["/T", "/F", "/PID", String(app.pid)], { stdio: "ignore" }); } catch {}
  }
}
