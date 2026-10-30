const { contextBridge, ipcRenderer } = require("electron");
contextBridge.exposeInMainWorld("asterionInstaller", {
  inspect: () => ipcRenderer.invoke("installer:inspect"),
  run: (action, removeUserData) => ipcRenderer.invoke("installer:run", action, removeUserData),
  open: location => ipcRenderer.invoke("installer:open", location),
  onProgress: listener => {
    const receive = (_event, step) => listener(step);
    ipcRenderer.on("installer:progress", receive);
    return () => ipcRenderer.removeListener("installer:progress", receive);
  },
});
