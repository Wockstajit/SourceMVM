const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('sourceMvm', {
  minimizeWindow: () => ipcRenderer.send('window:minimize'),
  closeWindow: () => ipcRenderer.send('window:close'),
  setAdvancedOpen: (open) => ipcRenderer.send('window:advanced', open),
  getResolvedPaths: (settings) => ipcRenderer.invoke('paths:resolved', settings),
  getPathSizes: (targets) => ipcRenderer.invoke('paths:sizes', targets),
  revealPath: (target) => ipcRenderer.invoke('path:reveal', target),
  openHcTracker: () => ipcRenderer.invoke('link:hctracker'),
  getSettings: () => ipcRenderer.invoke('settings:get'),
  saveSettings: (settings) => ipcRenderer.invoke('settings:save', settings),
  detectCs2: () => ipcRenderer.invoke('cs2:detect'),
  isCs2Running: () => ipcRenderer.invoke('cs2:running'),
  validateCs2: (candidate) => ipcRenderer.invoke('cs2:validate', candidate),
  getWorkshopTools: (cs2Exe) => ipcRenderer.invoke('cs2:workshop-tools', cs2Exe),
  installWorkshopTools: () => ipcRenderer.invoke('cs2:install-workshop-tools'),
  chooseFile: (kind) => ipcRenderer.invoke('file:choose', kind),
  chooseFolder: () => ipcRenderer.invoke('folder:choose'),
  launchCs2: (settings) => ipcRenderer.invoke('cs2:launch', settings),
  onCs2StartupExit: (callback) => {
    const listener = (_event, payload) => callback(payload);
    ipcRenderer.on('cs2-startup-exit', listener);
    return () => ipcRenderer.removeListener('cs2-startup-exit', listener);
  },
  getIntroVideoUrl: () => ipcRenderer.invoke('intro:video-url'),
  getUpdateStatus: () => ipcRenderer.invoke('update:status'),
  checkForUpdates: () => ipcRenderer.invoke('update:check'),
  downloadUpdate: () => ipcRenderer.invoke('update:download'),
  installUpdate: () => ipcRenderer.invoke('update:install'),
  onUpdateStatus: (callback) => {
    const listener = (_event, payload) => callback(payload);
    ipcRenderer.on('update-status', listener);
    return () => ipcRenderer.removeListener('update-status', listener);
  },
  detectFfmpeg: () => ipcRenderer.invoke('ffmpeg:detect'),
  chooseFfmpeg: () => ipcRenderer.invoke('ffmpeg:choose'),
  installFfmpeg: (options) => ipcRenderer.invoke('ffmpeg:install', options),
  relinkFfmpeg: () => ipcRenderer.invoke('ffmpeg:relink'),
  onFfmpegStatus: (callback) => {
    const listener = (_event, payload) => callback(payload);
    ipcRenderer.on('ffmpeg-status', listener);
    return () => ipcRenderer.removeListener('ffmpeg-status', listener);
  },
  getDlssStatus: () => ipcRenderer.invoke('dlss:status'),
  detectDlss: () => ipcRenderer.invoke('dlss:detect'),
  chooseDlssPayload: () => ipcRenderer.invoke('dlss:choose-payload'),
  chooseReShade: () => ipcRenderer.invoke('dlss:choose-reshade'),
  installDlss: (options) => ipcRenderer.invoke('dlss:install', options),
  uninstallDlss: () => ipcRenderer.invoke('dlss:uninstall'),
  cleanSteamFolder: () => ipcRenderer.invoke('dlss:clean-steam'),
  onDlssStatus: (callback) => {
    const listener = (_event, payload) => callback(payload);
    ipcRenderer.on('dlss-status', listener);
    return () => ipcRenderer.removeListener('dlss-status', listener);
  },
  getPayloadStatus: () => ipcRenderer.invoke('payload:status'),
  checkPayload: () => ipcRenderer.invoke('payload:check'),
  installPayload: (only) => ipcRenderer.invoke('payload:install', only),
  onPayloadStatus: (callback) => {
    const listener = (_event, payload) => callback(payload);
    ipcRenderer.on('payload-status', listener);
    return () => ipcRenderer.removeListener('payload-status', listener);
  },
});
