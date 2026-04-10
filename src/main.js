import { getCurrentWindow } from "@tauri-apps/api/window";
import { invoke } from "@tauri-apps/api/core";
import { Command } from "@tauri-apps/plugin-shell";

const { PIXI } = window;
const appWindow = getCurrentWindow();

// ── Debug overlay ───────────────────────────────────────────────────────────
const debugEl = document.createElement("div");
debugEl.style.cssText =
  "position:fixed;top:0;left:0;right:0;background:rgba(0,0,0,0.8);color:#0f0;font:12px monospace;padding:8px;z-index:99999;max-height:40vh;overflow-y:auto;pointer-events:none;";
document.body.appendChild(debugEl);

function debugLog(msg) {
  console.log(msg);
  debugEl.textContent += msg + "\n";
}

window.addEventListener("error", (e) => debugLog(`[ERROR] ${e.message}`));
window.addEventListener("unhandledrejection", (e) =>
  debugLog(`[REJECT] ${e.reason}`)
);

debugLog(`PIXI: ${typeof PIXI !== "undefined" ? "loaded" : "MISSING"}`);
debugLog(
  `Live2D Core: ${typeof Live2DCubismCore !== "undefined" ? "loaded" : "MISSING"}`
);
debugLog(
  `PIXI.live2d: ${typeof PIXI !== "undefined" && PIXI.live2d ? "loaded" : "MISSING"}`
);

// ── PIXI Setup ──────────────────────────────────────────────────────────────
const canvas = document.getElementById("live2d-canvas");
const app = new PIXI.Application({
  view: canvas,
  backgroundAlpha: 0, // Default to transparent
  resizeTo: window,
  autoDensity: true,
  resolution: window.devicePixelRatio || 1,
  clearBeforeRender: true,
  preserveDrawingBuffer: false,
});

const modelContainer = new PIXI.Container();
app.stage.addChild(modelContainer);

// ── State ───────────────────────────────────────────────────────────────────
let currentModel = null;
let currentModelName = null;
let alwaysOnTop = true;

// ── Model Loading ───────────────────────────────────────────────────────────

// Base URL for the custom protocol that serves model files directly from disk
const MODEL_BASE = "model://localhost";

async function loadModel(modelName) {
  try {
    debugLog(`Loading model: ${modelName}`);

    const models = await invoke("list_models");
    const model = models.find((m) => m.name === modelName);
    if (!model) throw new Error(`Model '${modelName}' not found`);

    debugLog(`Found model JSON: ${model.model_json}`);

    // Fetch model3.json via custom protocol
    const jsonUrl = `${MODEL_BASE}/${modelName}/${model.model_json}`;
    debugLog(`Fetching: ${jsonUrl}`);
    const resp = await fetch(jsonUrl);
    if (!resp.ok) throw new Error(`Failed to fetch model JSON: ${resp.status}`);
    const modelJson = await resp.json();
    debugLog(`Parsed model3.json OK`);

    // Rewrite all relative file paths to model:// URLs
    const refs = modelJson.FileReferences;
    if (refs) {
      const rewrite = (relPath) => {
        if (!relPath) return relPath;
        const clean = relPath.replace(/^\.\//, "");
        return `${MODEL_BASE}/${modelName}/${clean}`;
      };

      if (refs.Moc) refs.Moc = rewrite(refs.Moc);
      if (refs.Physics) refs.Physics = rewrite(refs.Physics);
      if (refs.Pose) refs.Pose = rewrite(refs.Pose);
      if (refs.DisplayInfo) refs.DisplayInfo = rewrite(refs.DisplayInfo);
      if (refs.UserData) refs.UserData = rewrite(refs.UserData);

      if (refs.Textures) {
        refs.Textures = refs.Textures.map(rewrite);
      }

      if (refs.Motions) {
        for (const group in refs.Motions) {
          for (const motion of refs.Motions[group]) {
            if (motion.File) motion.File = rewrite(motion.File);
            if (motion.Sound) motion.Sound = rewrite(motion.Sound);
          }
        }
      }

      if (refs.Expressions) {
        for (const exp of refs.Expressions) {
          if (exp.File) exp.File = rewrite(exp.File);
        }
      }
    }

    // Create a blob URL for the rewritten model JSON
    const modelJsonBlob = new Blob([JSON.stringify(modelJson)], {
      type: "application/json",
    });
    const modelJsonBlobUrl = URL.createObjectURL(modelJsonBlob);

    // Cleanup previous model
    if (currentModel) {
      modelContainer.removeChild(currentModel);
      currentModel.destroy();
      currentModel = null;
    }

    debugLog("Loading Live2D model...");
    const live2dModel = await PIXI.live2d.Live2DModel.from(modelJsonBlobUrl);
    currentModel = live2dModel;
    currentModelName = modelName;
    modelContainer.addChild(live2dModel);

    fitModelToWindow();
    debugLog(`Model '${modelName}' loaded successfully!`);
  } catch (e) {
    debugLog(`[LOAD ERROR] ${e.message}\n${e.stack}`);
  }
}

function fitModelToWindow() {
  if (!currentModel) return;

  currentModel.anchor.set(0);
  const bounds = currentModel.getLocalBounds();
  const w = bounds.width || currentModel.width;
  const h = bounds.height || currentModel.height;
  const bx = bounds.width ? bounds.x : 0;
  const by = bounds.height ? bounds.y : 0;

  currentModel.x = -bx - w / 2;
  currentModel.y = -by - h / 2;

  modelContainer.position.set(app.renderer.width / 2, app.renderer.height / 2);

  if (h > 0) {
    const scale = (app.renderer.height * 0.9) / h;
    modelContainer.scale.set(scale, scale);
  }
}

// ── Cursor Tracking (Hyprland IPC) ──────────────────────────────────────────

async function getCursorPos() {
  try {
    const cmd = Command.create("hyprctl-cursorpos");
    const output = await cmd.execute();
    if (output.code === 0) {
      const match = output.stdout.trim().match(/^(-?\d+),\s*(-?\d+)$/);
      if (match) {
        return { x: parseInt(match[1]), y: parseInt(match[2]) };
      }
    }
  } catch (e) {
    // Silently fail
  }
  return null;
}

// Cache total screen extent for cross-monitor eye tracking
let screenExtent = { width: 3840, height: 2160 }; // fallback

async function updateScreenExtent() {
  try {
    const cmd = Command.create("hyprctl-monitors");
    const output = await cmd.execute();
    if (output.code === 0) {
      const monitors = JSON.parse(output.stdout);
      let maxX = 0, maxY = 0;
      for (const m of monitors) {
        const right = m.x + m.width;
        const bottom = m.y + m.height;
        if (right > maxX) maxX = right;
        if (bottom > maxY) maxY = bottom;
      }
      if (maxX > 0 && maxY > 0) {
        screenExtent = { width: maxX, height: maxY };
      }
    }
  } catch (e) {
    // Use fallback
  }
}

updateScreenExtent();
// Refresh periodically in case monitors change
setInterval(updateScreenExtent, 30000);

let cachedWinPos = { x: 0, y: 0 };
let cachedWinSize = { width: 600, height: 800 };
let isInitializingWindow = true;

async function updateHyprlandWindow() {
  try {
    const cmd = Command.create("hyprctl-clients");
    const output = await cmd.execute();
    if (output.code === 0) {
      const clients = JSON.parse(output.stdout);
      const wnd = clients.find(c => c.class === "waifuland" || c.title === "Waifuland");
      if (wnd) {
        cachedWinPos = { x: wnd.at[0], y: wnd.at[1] };
        cachedWinSize = { width: wnd.size[0], height: wnd.size[1] };
        isInitializingWindow = false;
      }
    }
  } catch (e) {
  }
}
setInterval(updateHyprlandWindow, 200);

let customEyeOffsetNorm = null;

async function updateEyeTracking() {
  if (!currentModel) return;

  const cursorPos = await getCursorPos();
  if (!cursorPos) return;

  let winPos, winSize;
  if (!isInitializingWindow) {
    winPos = cachedWinPos;
    winSize = cachedWinSize;
  } else {
    winPos = await appWindow.outerPosition();
    winSize = await appWindow.outerSize();
  }

  let centerX, centerY;

  if (customEyeOffsetNorm) {
    const localCenterX = modelContainer.x + customEyeOffsetNorm.dx * modelContainer.scale.x;
    const localCenterY = modelContainer.y + customEyeOffsetNorm.dy * modelContainer.scale.y;
    centerX = winPos.x + localCenterX;
    centerY = winPos.y + localCenterY;
  } else {
    centerX = winPos.x + winSize.width / 2;
    centerY = winPos.y + winSize.height / 2;
  }

  const dx = cursorPos.x - centerX;
  const dy = cursorPos.y - centerY;

  // Use half of total screen diagonal as maxDist so tracking
  // remains responsive even when cursor is on a different monitor
  const maxDist = Math.sqrt(screenExtent.width ** 2 + screenExtent.height ** 2) / 2;

  const focusX = Math.max(-1, Math.min(1, dx / maxDist));
  const focusY = Math.max(-1, Math.min(1, dy / maxDist));

  currentModel.focus(
    app.renderer.width / 2 + focusX * app.renderer.width,
    app.renderer.height / 2 + focusY * app.renderer.height
  );
}

setInterval(updateEyeTracking, 33);

// ── Model Drag and Resize ───────────────────────────────────────────────────

let isDraggingModel = false;
let dragStartClientX = 0;
let dragStartClientY = 0;
let modelStartPosX = 0;
let modelStartPosY = 0;

canvas.addEventListener("mousedown", async (e) => {
  if (e.button === 0) {
    if (e.altKey || e.ctrlKey || e.shiftKey || e.metaKey) {
      // Allow window dragging with modifiers as fallback
      appWindow.startDragging();
    } else {
      isDraggingModel = true;
      dragStartClientX = e.clientX;
      dragStartClientY = e.clientY;
      modelStartPosX = modelContainer.x;
      modelStartPosY = modelContainer.y;
    }
  } else if (e.button === 1) { // Middle click
    if (currentModel) {
      customEyeOffsetNorm = {
        dx: (e.clientX - modelContainer.x) / modelContainer.scale.x,
        dy: (e.clientY - modelContainer.y) / modelContainer.scale.y
      };
    }
  }
});

window.addEventListener("mousemove", (e) => {
  if (isDraggingModel && currentModel) {
    const dx = e.clientX - dragStartClientX;
    const dy = e.clientY - dragStartClientY;
    modelContainer.x = modelStartPosX + dx;
    modelContainer.y = modelStartPosY + dy;
  }
});

window.addEventListener("mouseup", (e) => {
  if (e.button === 0) {
    isDraggingModel = false;
  }
});

canvas.addEventListener("wheel", (e) => {
  e.preventDefault();
  const factor = e.deltaY < 0 ? 1.08 : 1 / 1.08;
  
  if (e.altKey || e.ctrlKey || e.shiftKey || e.metaKey) {
    // Optional fallback to resize window with modifier
    appWindow.outerSize().then(currentSize => {
      const newWidth = Math.max(200, Math.min(2000, Math.round(currentSize.width * factor)));
      const newHeight = Math.max(200, Math.min(2000, Math.round(currentSize.height * factor)));
      import("@tauri-apps/api/dpi").then(({ LogicalSize }) => {
        appWindow.setSize(new LogicalSize(newWidth, newHeight));
      });
    });
  } else {
    // Model Resize around mouse cursor
    if (!currentModel) return;
    const localRect = app.renderer.view.getBoundingClientRect();
    const mouseX = e.clientX - localRect.left;
    const mouseY = e.clientY - localRect.top;
    
    const dx = mouseX - modelContainer.x;
    const dy = mouseY - modelContainer.y;
    
    modelContainer.x -= dx * (factor - 1);
    modelContainer.y -= dy * (factor - 1);
    
    modelContainer.scale.x *= factor;
    modelContainer.scale.y *= factor;
  }
});

// ── Window Resize Handler ───────────────────────────────────────────────────

window.addEventListener("resize", () => {
  app.renderer.resize(window.innerWidth, window.innerHeight);
  // Don't auto-fit model to allow custom manual dragged/scaled positions
});

// ── Context Menu (Right-click) ──────────────────────────────────────────────

const contextMenu = document.getElementById("context-menu");
const modelListEl = document.getElementById("model-list");
const btnToggleTopmost = document.getElementById("btn-toggle-topmost");
const btnQuit = document.getElementById("btn-quit");

canvas.addEventListener("contextmenu", async (e) => {
  e.preventDefault();

  const models = await invoke("list_models");
  modelListEl.innerHTML = '<div class="menu-header">Models</div>';
  for (const m of models) {
    const item = document.createElement("div");
    item.className = "menu-item";
    item.textContent = m.name;
    if (currentModelName === m.name) {
      item.classList.add("active");
    }
    item.addEventListener("click", () => {
      contextMenu.classList.add("hidden");
      loadModel(m.name);
    });
    modelListEl.appendChild(item);
  }

  contextMenu.style.left = `${e.clientX}px`;
  contextMenu.style.top = `${e.clientY}px`;
  contextMenu.classList.remove("hidden");
});

document.addEventListener("click", (e) => {
  if (!contextMenu.contains(e.target)) {
    contextMenu.classList.add("hidden");
  }
});

const chromaBtns = document.querySelectorAll(".chroma-btn");
chromaBtns.forEach(btn => {
  btn.addEventListener("click", (e) => {
    const colorStr = e.target.dataset.color;
    if (colorStr === "transparent") {
      app.renderer.backgroundAlpha = 0;
    } else {
      app.renderer.backgroundAlpha = 1;
      app.renderer.backgroundColor = parseInt(colorStr, 16);
    }
    contextMenu.classList.add("hidden");
  });
});

btnToggleTopmost.addEventListener("click", async () => {
  alwaysOnTop = !alwaysOnTop;
  await appWindow.setAlwaysOnTop(alwaysOnTop);
  btnToggleTopmost.textContent = `Always on Top: ${alwaysOnTop ? "ON" : "OFF"}`;
  contextMenu.classList.add("hidden");
});

btnQuit.addEventListener("click", async () => {
  const { exit } = await import("@tauri-apps/plugin-process");
  await exit(0);
});

// ── Init ────────────────────────────────────────────────────────────────────

async function init() {
  try {
    const models = await invoke("list_models");
    debugLog(`Found ${models.length} model(s): ${models.map((m) => m.name).join(", ")}`);
    if (models.length > 0) {
      await loadModel(models[0].name);
    } else {
      debugLog("No models found in models/ directory");
    }
  } catch (e) {
    debugLog(`[INIT ERROR] ${e.message}\n${e.stack}`);
  }
}

init();
