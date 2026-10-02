const { contextBridge, ipcRenderer } = require("electron");
contextBridge.exposeInMainWorld(
  "asterionDesktop",
  Object.freeze({
    platform: process.platform,
    environment: process.argv.includes("--asterion-environment=development")
      ? "development"
      : "production",
    onDevelopmentStopping: listener => {
      const handle = (_event, value) => listener(value === true);
      ipcRenderer.on("asterion:development-stopping", handle);
      return () => ipcRenderer.removeListener("asterion:development-stopping", handle);
    },
    request: request => ipcRenderer.invoke("asterion:request", request),
    openSettings: category => ipcRenderer.invoke("asterion:settings", category),
    open: options => ipcRenderer.invoke("asterion:open", options),
    save: options => ipcRenderer.invoke("asterion:save", options),
    setTitle: title => ipcRenderer.invoke("asterion:title", title),
    close: () => ipcRenderer.invoke("asterion:close"),
  }),
);
