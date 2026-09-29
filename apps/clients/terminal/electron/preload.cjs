const { contextBridge, ipcRenderer } = require("electron");
contextBridge.exposeInMainWorld(
  "asterionDesktop",
  Object.freeze({
    platform: process.platform,
    request: request => ipcRenderer.invoke("asterion:request", request),
    openSettings: category => ipcRenderer.invoke("asterion:settings", category),
    open: options => ipcRenderer.invoke("asterion:open", options),
    save: options => ipcRenderer.invoke("asterion:save", options),
    setTitle: title => ipcRenderer.invoke("asterion:title", title),
    close: () => ipcRenderer.invoke("asterion:close"),
  }),
);
