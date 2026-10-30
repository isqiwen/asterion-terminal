const { app, BrowserWindow, ipcMain, shell } = require("electron");
const path = require("node:path");
const fs = require("node:fs");
const { spawn } = require("node:child_process");
const { createInterface } = require("node:readline");
const { pathToFileURL } = require("node:url");
const mac = process.platform === "darwin";
if (!(mac && process.arch === "arm64") && !(process.platform === "linux" && process.arch === "x64"))
  throw new Error("Installer supports macOS Apple Silicon and Linux x86_64 only");
const root = path.resolve(__dirname, "../../../..");
const source = app.isPackaged
  ? path.join(process.resourcesPath, mac ? "terminal.app" : "terminal")
  : process.env.ASTERION_INSTALL_SOURCE;
const destination = mac
  ? path.join(app.getPath("home"), "Applications/Asterion Terminal.app")
  : path.join(app.getPath("home"), ".local/opt/asterion-terminal");
const native = directory =>
  path.join(directory, mac ? "Contents/Resources/native" : "resources/native");
const helper = source
  ? path.join(native(source), "asterion_maintenance")
  : path.join(root, "build/Debug/asterion_maintenance");
const html = app.isPackaged
  ? path.join(__dirname, "dist/installer.html")
  : path.join(root, "apps/clients/terminal/dist/installer.html");
let window,
  busy = false,
  view;
if (!app.requestSingleInstanceLock()) app.quit();
else {
  app.on("second-instance", () => {
    window?.show();
    window?.focus();
  });
  app.whenReady().then(() => {
    window = new BrowserWindow({
      width: 980,
      height: 720,
      minWidth: 840,
      minHeight: 640,
      backgroundColor: "#090e14",
      title: "Asterion Installer",
      autoHideMenuBar: true,
      webPreferences: {
        preload: path.join(__dirname, "preload.cjs"),
        sandbox: true,
        contextIsolation: true,
        nodeIntegration: false,
      },
    });
    window.webContents.setWindowOpenHandler(() => ({ action: "deny" }));
    window.webContents.on("will-navigate", event => event.preventDefault());
    window.on("close", event => {
      if (busy) event.preventDefault();
    });
    const trusted = event => {
      if (
        event.sender !== window.webContents ||
        event.senderFrame !== window.webContents.mainFrame ||
        event.senderFrame.url !== pathToFileURL(html).href
      )
        throw new Error("Untrusted installer request");
    };
    const execute = (action, removeUserData = false) =>
      new Promise((resolve, reject) => {
        const args = ["--operation", action, "--destination", destination];
        if (removeUserData) args.push("--remove-user-data");
        if (source && ["inspect", "install", "upgrade", "repair"].includes(action))
          args.push("--source", source);
        const environment = Object.fromEntries(
          Object.entries(process.env).filter(
            ([name]) => !name.startsWith("ASTERION_") && name !== "ELECTRON_RUN_AS_NODE",
          ),
        );
        environment.ASTERION_ENVIRONMENT = "production";
        const child = spawn(helper, args, { stdio: ["ignore", "pipe", "pipe"], env: environment });
        let result,
          failure,
          stderr = "";
        const lines = createInterface({ input: child.stdout });
        lines.on("line", line => {
          try {
            const message = JSON.parse(line);
            if (message.step) window.webContents.send("installer:progress", message.step);
            if (message.result) result = message.result;
            if (message.error) failure = message.error;
          } catch {
            failure = { code: "operation_failed", message: "Invalid installer executor response" };
          }
        });
        child.stderr.on("data", data => {
          stderr = (stderr + data.toString()).slice(-4096);
        });
        child.on("error", reject);
        child.on("close", code => {
          if (code !== 0 || failure || !result)
            reject(
              new Error(
                (failure?.code || "operation_failed") +
                  ": " +
                  (failure?.message || stderr || "Installation executor failed") +
                  (failure?.log_file ? "\nLog file: " + failure.log_file : ""),
              ),
            );
          else {
            view = result;
            resolve({ ...result, source_available: !!source });
          }
        });
      });
    ipcMain.handle("installer:inspect", async event => {
      trusted(event);
      if (busy) throw new Error("Installation operation is in progress");
      return execute("inspect");
    });
    ipcMain.handle("installer:run", async (event, action, removeUserData = false) => {
      trusted(event);
      if (!["install", "upgrade", "repair", "uninstall"].includes(action))
        throw new Error("Unknown installer action");
      if (typeof removeUserData !== "boolean" || (removeUserData && action !== "uninstall"))
        throw new Error("user data removal is only available during uninstall");
      if (busy) throw new Error("Installation operation is in progress");
      busy = true;
      try {
        await execute(action, removeUserData);
        return await execute("inspect");
      } finally {
        busy = false;
      }
    });
    ipcMain.handle("installer:open", async (event, location) => {
      trusted(event);
      if (busy || !view) throw new Error("Installer is not ready");
      if (location === "terminal") {
        const executable = path.join(
          destination,
          mac ? "Contents/MacOS/asterion_terminal" : "asterion_terminal",
        );
        if (!fs.existsSync(executable)) throw new Error("Installed Terminal executable is missing");
        const child = spawn(executable, [], { detached: true, stdio: "ignore" });
        await new Promise((resolve, reject) => {
          child.once("spawn", resolve);
          child.once("error", reject);
        });
        child.unref();
        return;
      }
      const directory =
        location === "logs" ? view.root : location === "data" ? view.root : undefined;
      if (!directory) throw new Error("Unknown installer location");
      const error = await shell.openPath(directory);
      if (error) throw new Error(error);
    });
    window.loadFile(html);
  });
  app.on("window-all-closed", () => app.quit());
}
