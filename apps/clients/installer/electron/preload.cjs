const { contextBridge, ipcRenderer } = require("electron");
contextBridge.exposeInMainWorld("asterionInstaller", {
  remote: request => ipcRenderer.invoke("installer:remote", request),
  inspect: () => ipcRenderer.invoke("installer:inspect"),
  run: (action, removeUserData, remoteAll) =>
    ipcRenderer.invoke("installer:run", action, removeUserData, remoteAll),
  open: location => ipcRenderer.invoke("installer:open", location),
  onRemoteProgress: listener => {
    const receive = (_event, view) => listener(view);
    ipcRenderer.on("installer:remote-progress", receive);
    return () => ipcRenderer.removeListener("installer:remote-progress", receive);
  },
  onProgress: listener => {
    const receive = (_event, step) => listener(step);
    ipcRenderer.on("installer:progress", receive);
    return () => ipcRenderer.removeListener("installer:progress", receive);
  },
});
