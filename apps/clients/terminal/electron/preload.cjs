const { contextBridge, ipcRenderer } = require("electron");
contextBridge.exposeInMainWorld(
  "asterionDesktop",
  Object.freeze({
    platform: process.platform,
    environment: process.argv.includes("--asterion-environment=development")
      ? "development"
      : "production",
    request: request => ipcRenderer.invoke("asterion:request", request),
    lab: payload => ipcRenderer.invoke("asterion:lab", payload),
    openSettings: category => ipcRenderer.invoke("asterion:settings", category),
    openLogs: () => ipcRenderer.invoke("asterion:logs"),
    open: options => ipcRenderer.invoke("asterion:open", options),
    save: options => ipcRenderer.invoke("asterion:save", options),
    setTitle: title => ipcRenderer.invoke("asterion:title", title),
    close: () => ipcRenderer.invoke("asterion:close"),
  }),
);
