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
const remoteCredentials = new Map();
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
    const execute = (action, removeUserData = false, remoteRequest) =>
      new Promise((resolve, reject) => {
        // Manager calls use its installed release. A same-release package supplies
        // that executor during repair even when installed program files are missing.
        const sameRelease = source && view?.version && view.version === view.candidate_version;
        const program =
          ["stop", "remote"].includes(action) && !sameRelease
            ? path.join(native(destination), "asterion_maintenance")
            : helper;
        const args = ["--operation", action, "--destination", destination];
        if (removeUserData) args.push("--remove-user-data");
        if (source && ["inspect", "install", "upgrade", "repair", "remote"].includes(action))
          args.push("--source", source);
        const environment = Object.fromEntries(
          Object.entries(process.env).filter(
            ([name]) => !name.startsWith("ASTERION_") && name !== "ELECTRON_RUN_AS_NODE",
          ),
        );
        environment.ASTERION_ENVIRONMENT = "production";
        const child = spawn(program, args, { stdio: ["pipe", "pipe", "pipe"], env: environment });
        if (remoteRequest?.maintenance)
          remoteRequest.maintenance.connections = [
            ...remoteCredentials.values(),
            ...(remoteRequest.maintenance.connections || []),
          ];
        child.stdin.end(remoteRequest ? JSON.stringify(remoteRequest) : "");
        let result,
          failure,
          stderr = "";
        const lines = createInterface({ input: child.stdout });
        lines.on("line", line => {
          try {
            const message = JSON.parse(line);
            if (message.remote_progress)
              window.webContents.send("installer:remote-progress", message.remote_progress);
            if (message.step) window.webContents.send("installer:progress", message.step);
            if (message.result) {
              result = message.result;
              if (action === "remote" && Array.isArray(result.nodes))
                window.webContents.send("installer:remote-progress", result);
            }
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
            if (action !== "remote") view = result;
            resolve({ ...result, source_available: !!source });
          }
        });
      });
    ipcMain.handle("installer:inspect", async event => {
      trusted(event);
      if (busy) throw new Error("Installation operation is in progress");
      return execute("inspect");
    });
    ipcMain.handle("installer:remote", async (event, request) => {
      trusted(event);
      if (busy) throw new Error("Installation operation is in progress");
      for (const entry of request.maintenance?.connections || []) {
        if (entry?.id) remoteCredentials.set(entry.id, entry);
      }
      busy = true;
      try {
        return await execute("remote", false, request);
      } finally {
        busy = false;
      }
    });
    ipcMain.handle(
      "installer:run",
      async (event, action, removeUserData = false, remoteAll = false) => {
        trusted(event);
        if (!["install", "upgrade", "repair", "uninstall", "stop"].includes(action))
          throw new Error("Unknown installer action");
        if (typeof removeUserData !== "boolean" || (removeUserData && action !== "uninstall"))
          throw new Error("user data removal is only available during uninstall");
        if (typeof remoteAll !== "boolean" || (remoteAll && action !== "uninstall"))
          throw new Error("Invalid remote uninstall scope");
        if (busy) throw new Error("Installation operation is in progress");
        busy = true;
        try {
          const before = await execute("inspect");
          if (remoteAll && !before.pending) {
            await execute("remote", false, {
              maintenance: { action: "UNINSTALL", removeData: removeUserData },
            });
            await execute("stop");
          }
          if (!before.pending && action === "upgrade") {
            window.webContents.send("installer:progress", "remotePreparing");
            await execute("remote", false, { maintenance: { action: "PREPARE" } });
            await execute("stop");
          }
          if (!before.pending && action === "repair" && before.busy?.length) await execute("stop");
          await execute(action, removeUserData);
          if (action === "upgrade") {
            window.webContents.send("installer:progress", "remoteFinishing");
            await execute("remote", false, { maintenance: { action: "FINISH" } });
          }
          return await execute("inspect");
        } finally {
          busy = false;
        }
      },
    );
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
