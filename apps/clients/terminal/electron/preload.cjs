const { contextBridge, ipcRenderer } = require("electron");
contextBridge.exposeInMainWorld(
  "asterionDesktop",
  Object.freeze({
    platform: process.platform,
    request: request => ipcRenderer.invoke("asterion:request", request),
    lab: payload => ipcRenderer.invoke("asterion:lab", payload),
    openSettings: category => ipcRenderer.invoke("asterion:settings", category),
    openLogs: () => ipcRenderer.invoke("asterion:logs"),
    setTitle: title => ipcRenderer.invoke("asterion:title", title),
    close: () => ipcRenderer.invoke("asterion:close"),
  }),
);
