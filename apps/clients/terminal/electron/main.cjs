const {
  app,
  crashReporter,
  BrowserWindow,
  ipcMain,
  dialog,
  Menu,
  protocol,
  net,
  session,
  screen,
  shell,
} = require("electron");
const path = require("node:path");
const fs = require("node:fs");
const os = require("node:os");
const { diagnostics } = require("./diagnostics.cjs");
const { pathToFileURL } = require("node:url");
const { labChannel } = require("./lab-channel.cjs");
const root = path.resolve(__dirname, "../../../..");
const devUrl = !app.isPackaged ? process.env.ASTERION_DEV_URL : undefined;
if (devUrl && !/^http:\/\/127\.0\.0\.1:\d+$/.test(devUrl))
  throw new Error("Invalid desktop development URL");
const origin = devUrl || "asterion://app";
const renderer = app.isPackaged
  ? path.join(__dirname, "dist")
  : path.join(root, "apps/clients/terminal/dist");
const resources = app.isPackaged
  ? process.resourcesPath
  : path.join(root, "build/electron-resources");
// The native programs, libraries and configuration defaults the Terminal's
// local services run from.
process.env.ASTERION_APPLICATION_DIRECTORY = path.join(resources, "native");
// The built-in strategies and factors: research/ of the project, or the copy
// the package ships. They are copied read only into the strategy directory.
process.env.ASTERION_BUILTIN_RESEARCH = app.isPackaged
  ? path.join(process.resourcesPath, "research")
  : path.join(root, "research");
// Development runs on the runtime `pnpm prepare:python` made; an installed
// application installs its own at the first launch.
if (!app.isPackaged) process.env.ASTERION_PYTHON = path.join(resources, "python", "bin", "python3");
protocol.registerSchemesAsPrivileged([
  { scheme: "asterion", privileges: { standard: true, secure: true, supportFetchAPI: true } },
]);
app.setName("Asterion Terminal");
// Run from source or installed, the Terminal keeps one set of settings.
if (!app.commandLine.hasSwitch("user-data-dir"))
  app.setPath("userData", path.join(app.getPath("appData"), "me.asterion.terminal"));
const dataRoot =
  process.env.ASTERION_ROOT ||
  (process.platform === "darwin"
    ? path.join(os.homedir(), "Library", "Application Support", "Asterion")
    : path.join(
        process.env.XDG_DATA_HOME || path.join(os.homedir(), ".local", "share"),
        "asterion",
      ));
if (!path.isAbsolute(dataRoot)) throw new Error("Invalid Asterion root directory");
const product = app.isPackaged
  ? { version: app.getVersion(), commit: require("./package.json").buildCommit }
  : JSON.parse(
      fs.readFileSync(
        path.join(
          process.env.ASTERION_CPP_BUILD || path.join(root, "build/Release"),
          "product.json",
        ),
        "utf8",
      ),
    );
const logs = path.join(dataRoot, "terminal", "logs");
const report = diagnostics(logs, product);
const dumps = path.join(logs, "crashes");
fs.mkdirSync(dumps, { recursive: true, mode: 0o700 });
app.setPath("crashDumps", dumps);
crashReporter.start({
  uploadToServer: false,
  globalExtra: { version: product.version, commit: product.commit },
});
process.on("uncaughtException", error => {
  report("uncaughtException", error);
  app.exit(1);
});
process.on("unhandledRejection", error => {
  report("unhandledRejection", error);
  app.exit(1);
});
app.on("child-process-gone", (_event, details) =>
  report("child-process-gone", JSON.stringify(details)),
);
let main, settings, native;
let quitting = false;
let pending = 0;
// Quitting ends the Terminal's process; every local service watches it and
// stops by itself.
if (!app.isPackaged) {
  process.on("SIGINT", () => app.quit());
  process.on("SIGTERM", () => app.quit());
  process.on("message", message => {
    if (message?.type === "asterion:development-quit") app.quit();
  });
}
const owned = new Set();
function authorized(event) {
  const win = BrowserWindow.fromWebContents(event.sender);
  const frame = event.senderFrame;
  if (!win || !owned.has(win) || frame !== event.sender.mainFrame || !trusted(frame.url))
    throw new Error("Untrusted desktop request");
  return win;
}
function trusted(value) {
  try {
    const url = new URL(value);
    return `${url.protocol}//${url.host}` === origin;
  } catch {
    return false;
  }
}
function configure(win) {
  owned.add(win);
  win.on("closed", () => owned.delete(win));
  win.webContents.setWindowOpenHandler(({ url }) => {
    // Lightweight Charts' attribution opens its publisher in the system browser.
    if (url === "https://www.tradingview.com/")
      void shell.openExternal("https://www.tradingview.com/");
    return { action: "deny" };
  });
  win.webContents.on("will-navigate", (event, url) => {
    if (!trusted(url)) event.preventDefault();
  });
  win.webContents.on("will-attach-webview", event => event.preventDefault());
  win.webContents.on("console-message", details => {
    if (details.level === "error") report("renderer-error", details.message);
  });
  win.webContents.on("render-process-gone", (_event, details) => {
    report("render-process-gone", JSON.stringify(details));
    if (win === main) app.quit();
  });
}
// The page draws the title bar. macOS keeps the main window's buttons inset in
// it. Linux's own title bar follows the desktop's theme, not the Terminal's, so
// there every window drops it and has its buttons drawn over the page's right
// edge, in the theme's --canvas and --text colours and one pixel short of the
// bar's height so that its bottom rule stays whole.
function chrome(primary) {
  if (process.platform === "darwin")
    return primary ? { titleBarStyle: "hiddenInset", trafficLightPosition: { x: 12, y: 10 } } : {};
  return {
    titleBarStyle: "hidden",
    titleBarOverlay: { color: "#06080b", symbolColor: "#c5ccd6", height: 33 },
  };
}
function preferences() {
  return {
    preload: path.join(__dirname, "preload.cjs"),
    contextIsolation: true,
    nodeIntegration: false,
    sandbox: true,
    webSecurity: true,
  };
}
function fit(bounds, area) {
  const width = Math.min(bounds.width, area.width),
    height = Math.min(bounds.height, area.height);
  return {
    width,
    height,
    x: Math.max(area.x, Math.min(bounds.x, area.x + area.width - width)),
    y: Math.max(area.y, Math.min(bounds.y, area.y + area.height - height)),
  };
}
function positionSettings(first) {
  const current = settings.getBounds();
  if (
    !first &&
    screen
      .getAllDisplays()
      .some(({ workArea }) =>
        Object.entries(fit(current, workArea)).every(([key, value]) => current[key] === value),
      )
  )
    return;
  const owner = main.getBounds(),
    area = screen.getDisplayMatching(owner).workArea;
  settings.setMinimumSize(Math.min(640, area.width), Math.min(440, area.height));
  settings.setBounds(
    fit(
      {
        ...current,
        x: Math.round(owner.x + (owner.width - current.width) / 2),
        y: Math.round(owner.y + (owner.height - current.height) / 2),
      },
      area,
    ),
  );
}
async function openSettings(category) {
  if (!["preferences", "market", "trading", "about"].includes(category))
    throw new Error("Invalid settings category");
  if (settings && !settings.isDestroyed()) {
    if (settings.isMinimized()) settings.restore();
    positionSettings(false);
    settings.show();
    settings.focus();
    return;
  }
  settings = new BrowserWindow({
    ...chrome(false),
    parent: main,
    modal: false,
    // A dialog-sized window over the main one, even when that is full screen.
    fullscreenable: false,
    maximizable: false,
    width: 820,
    height: 540,
    minWidth: 640,
    minHeight: 440,
    show: false,
    backgroundColor: "#090a09",
    title: "Asterion Terminal — Settings",
    autoHideMenuBar: true,
    webPreferences: preferences(),
  });
  const win = settings;
  configure(win);
  win.on("close", event => {
    if (!quitting) {
      event.preventDefault();
      win.hide();
      main?.focus();
    }
  });
  win.on("closed", () => {
    if (settings === win) settings = undefined;
  });
  positionSettings(true);
  await win.loadURL(`${origin}/index.html?screen=settings&category=${category}`);
  if (!win.isDestroyed()) {
    win.show();
    win.focus();
  }
}
function options(input, save = false) {
  if (!input || typeof input !== "object" || Array.isArray(input))
    throw new Error("Invalid dialog options");
  const result = {};
  for (const key of ["title", "defaultPath"])
    if (input[key] !== undefined) {
      if (typeof input[key] !== "string" || input[key].length > 4096)
        throw new Error("Invalid dialog text");
      result[key] = input[key];
    }
  if (input.filters !== undefined) {
    if (!Array.isArray(input.filters) || input.filters.length > 10)
      throw new Error("Invalid file filters");
    result.filters = input.filters.map(item => {
      if (
        typeof item.name !== "string" ||
        !Array.isArray(item.extensions) ||
        !item.extensions.every(e => typeof e === "string" && /^[a-zA-Z0-9*]+$/.test(e))
      )
        throw new Error("Invalid file filter");
      return { name: item.name, extensions: item.extensions };
    });
  }
  if (!save)
    result.properties = [
      input.directory === true ? "openDirectory" : "openFile",
      ...(input.multiple === true ? ["multiSelections"] : []),
    ];
  return result;
}
app.on("before-quit", () => {
  quitting = true;
});
app.on("window-all-closed", () => app.quit());
app
  .whenReady()
  .then(async () => {
    // Only writing to the clipboard, for "copy diagnostic"; reading it stays denied.
    const allowed = permission => permission === "clipboard-sanitized-write";
    session.defaultSession.setPermissionRequestHandler((_web, permission, callback) =>
      callback(allowed(permission)),
    );
    session.defaultSession.setPermissionCheckHandler((_web, permission) => allowed(permission));
    protocol.handle("asterion", request => {
      const url = new URL(request.url);
      if (url.host !== "app" || !["GET", "HEAD"].includes(request.method))
        return new Response("Forbidden", { status: 403 });
      let file;
      try {
        file = path.resolve(renderer, "." + decodeURIComponent(url.pathname));
      } catch {
        return new Response("Invalid path", { status: 400 });
      }
      if (!file.startsWith(renderer + path.sep)) return new Response("Forbidden", { status: 403 });
      return net.fetch(pathToFileURL(file).href);
    });
    session.defaultSession.webRequest.onHeadersReceived((details, callback) => {
      const csp = devUrl
        ? "default-src 'self'; connect-src 'self' ws://127.0.0.1:*; style-src 'self' 'unsafe-inline'; img-src 'self' data:; script-src 'self' 'unsafe-inline'"
        : "default-src 'self'; connect-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; object-src 'none'; base-uri 'none'; frame-src 'none'";
      callback({
        responseHeaders: { ...details.responseHeaders, "Content-Security-Policy": [csp] },
      });
    });
    native = require(path.join(resources, "native/asterion_terminal.node"));
    ipcMain.handle("asterion:request", async (event, body) => {
      authorized(event);
      if (typeof body !== "string" || Buffer.byteLength(body) > 65536 || body.includes("\0"))
        throw new Error("Invalid native request");
      // Bound queued native work. The C++ facade rejects concurrent mutations.
      if (pending >= 32) throw new Error("Too many pending native requests");
      pending++;
      try {
        return await native.request(body);
      } finally {
        pending--;
      }
    });
    const lab = labChannel(async () => {
      const reply = JSON.parse(
        await native.request(JSON.stringify({ version: 1, method: "lab.endpoint", params: {} })),
      );
      if (reply.error) throw new Error(reply.error.message);
      return reply.result.lab_endpoint.endpoint;
    });
    ipcMain.handle("asterion:lab", async (event, payload) => {
      authorized(event);
      if (!(payload instanceof Uint8Array)) throw new Error("Invalid lab request");
      return new Uint8Array(await lab(payload));
    });
    ipcMain.handle("asterion:settings", async (event, category) => {
      authorized(event);
      return openSettings(category);
    });
    ipcMain.handle("asterion:logs", async event => {
      authorized(event);
      // Native owns the environment's path. The renderer cannot supply a path
      // or substitute a remote node's directory.
      const reply = JSON.parse(
        await native.request(
          JSON.stringify({ version: 1, method: "runtime.snapshot", params: {} }),
        ),
      );
      if (reply.error) throw new Error(reply.error.message);
      const failure = await shell.openPath(reply.result.diagnostics.log_directory);
      if (failure) throw new Error(failure);
    });
    ipcMain.handle("asterion:close", event => authorized(event).close());
    ipcMain.handle("asterion:title", (event, title) => {
      const win = authorized(event);
      if (typeof title !== "string" || title.length > 256) throw new Error("Invalid window title");
      win.setTitle(title);
    });
    ipcMain.handle("asterion:open", async (event, input) => {
      const win = authorized(event);
      const result = await dialog.showOpenDialog(win, options(input));
      return result.canceled
        ? null
        : input.multiple
          ? result.filePaths
          : (result.filePaths[0] ?? null);
    });
    ipcMain.handle("asterion:save", async (event, input) => {
      const win = authorized(event);
      const result = await dialog.showSaveDialog(win, options(input, true));
      return result.canceled ? null : (result.filePath ?? null);
    });
    Menu.setApplicationMenu(
      Menu.buildFromTemplate([
        ...(process.platform === "darwin" ? [{ role: "appMenu" }] : []),
        { role: "editMenu" },
        { label: "Window", submenu: [{ role: "minimize" }, { role: "zoom" }, { role: "close" }] },
      ]),
    );
    main = new BrowserWindow({
      ...chrome(true),
      width: 1440,
      height: 940,
      minWidth: 800,
      minHeight: 600,
      show: false,
      backgroundColor: "#090a09",
      title: "Asterion Terminal",
      autoHideMenuBar: true,
      webPreferences: preferences(),
    });
    configure(main);
    main.on("close", () => {
      quitting = true;
      settings?.destroy();
    });
    await main.loadURL(`${origin}/index.html`);
    main.show();
  })
  .catch(error => {
    report("startup.failed", error);
    app.exit(1);
  });
