/// <reference types="vite/client" />

interface LauncherSettings {
  cs2Exe: string;
  runtimeRoot: string;
  hookDllPath: string;
  injectorPath: string;
  mmcfgEnabled: boolean;
  mmcfg: string;
  /** Launch with the Workshop Tools (-tools -dx11), the way Steam's tools entry does. */
  devMode: boolean;
  customLaunchOptions: string;
  /** Manually chosen ffmpeg.exe; empty means auto-detect. */
  ffmpegPath: string;
  /** Start the launcher's private CS2 copy, which carries ReShade + DLSS 5. */
  dlss5Enabled: boolean;
  /** The first-launch "install DLSS 5?" question was answered, yes or no. */
  dlssPromptAnswered: boolean;
}

/** The GPU and the DLSS runtime it gets (RTX 50 -> NVIDIA original, RTX 20/30/40 -> compatible). */
interface DlssDetect {
  gpu: string;
  variant: string;
  supported: boolean;
  reason?: string;
}

/** DLSS 5 install state (electron/dlss-tool.cjs). */
interface DlssStatus {
  installed: boolean;
  /** Steam's CS2 changed since the private copy was synced; the next DLSS launch re-syncs it. */
  outOfDate: boolean;
  variant: string;
  installedAt: string;
  privateRoot: string;
  privateExe: string;
  /** ReShade / DLSS files found beside STEAM's cs2.exe: they load in online matches. */
  steamLeftovers: string[];
  variants: string[];
  /** This launcher carries the whole DLSS 5 payload, ReShade included: nothing to choose. */
  bundled?: boolean;
}

/** Whether the Counter-Strike 2 Workshop Tools DLC content is present on disk. */
interface WorkshopToolsStatus {
  installed: boolean;
  /** True when no cs2.exe is configured yet, so nothing was actually checked. */
  unknown: boolean;
  missing: string[];
  appId: number;
}

interface UpdateStatus {
  state: 'checking' | 'available' | 'current' | 'downloading' | 'downloaded' | 'error' | 'dev'
    | 'portable' | 'bundled' | 'ready' | 'installing';
  launchAllowed?: boolean;
  version?: string;
  percent?: number;
  message?: string;
}

/** One downloadable piece of the game-side runtime. */
interface PayloadComponentStatus {
  label: string;
  present: boolean;
  version: string;
  sha256: string;
}

interface PayloadLocalStatus {
  root: string;
  components: Record<string, PayloadComponentStatus>;
}

/** Progress events emitted while the runtime payload downloads and unpacks. */
interface PayloadProgress {
  state: 'downloading' | 'installing' | 'installed' | 'error';
  component?: string;
  label?: string;
  percent?: number;
  received?: number;
  total?: number;
  version?: string;
  message?: string;
}

interface PayloadCheck {
  state: 'current' | 'outdated' | 'none-published' | 'installed' | 'error';
  version?: string;
  prerelease?: boolean;
  outdated?: string[];
  message?: string;
  local: PayloadLocalStatus;
}

/** FFmpeg on this machine. Without it the render pipeline cannot encode anything. */
interface FfmpegStatus {
  found: boolean;
  path: string;
  version: string;
  /** Human label: "on PATH", "winget", "installed by SourceMovieMaker", ... */
  source: string;
  /** True when this is the copy the launcher installed and therefore manages. */
  managed: boolean;
  onPath: boolean;
}

interface FfmpegProgress {
  state: 'checking' | 'unverified' | 'downloading' | 'installing' | 'installed' | 'error';
  percent?: number;
  received?: number;
  total?: number;
  version?: string;
  message?: string;
}

interface FfmpegInstallResult {
  ok: boolean;
  path?: string;
  version?: string;
  message?: string;
  pathAdded?: boolean;
  pathAlready?: boolean;
}

/** Where a setting actually resolves to, whether or not the user overrode it. */
interface ResolvedPath {
  path: string;
  kind: 'file' | 'directory';
  /** True when the user set this explicitly rather than letting the launcher pick. */
  overridden: boolean;
  exists: boolean;
}

interface ResolvedPaths {
  cs2Exe: ResolvedPath;
  runtimeRoot: ResolvedPath;
  hookDll: ResolvedPath;
  injector: ResolvedPath;
  fxPack: ResolvedPath;
  installRoot: ResolvedPath;
  appData: ResolvedPath;
  /** Everything SOURCE:MVM saves in game, under %APPDATA%\HLAE. */
  data: DataLocation[];
}

/** One place the in-game tools save data. */
interface DataLocation extends ResolvedPath {
  id: string;
  label: string;
  /** What is stored there, in a few words. */
  detail: string;
}

interface Window {
  sourceMvm: {
    minimizeWindow: () => void;
    closeWindow: () => void;
    setAdvancedOpen: (open: boolean) => void;
    /** Opens hctracker.com in the default browser. */
    openHcTracker: () => Promise<void>;
    getResolvedPaths: (settings?: LauncherSettings) => Promise<ResolvedPaths>;
    /** Bytes on disk per path; slow folders are measured off the UI path. */
    getPathSizes: (targets: string[]) => Promise<Record<string, number>>;
    revealPath: (target: string) => Promise<{ ok: boolean; message?: string; fellBack?: boolean }>;
    getSettings: () => Promise<LauncherSettings>;
    saveSettings: (settings: LauncherSettings) => Promise<LauncherSettings>;
    detectCs2: () => Promise<string>;
    isCs2Running: () => Promise<boolean>;
    validateCs2: (candidate: string) => Promise<boolean>;
    getWorkshopTools: (cs2Exe?: string) => Promise<WorkshopToolsStatus>;
    installWorkshopTools: () => Promise<{ ok: boolean; appId: number }>;
    chooseFile: (kind?: 'game' | 'hook') => Promise<string>;
    chooseFolder: () => Promise<string>;
    launchCs2: (settings: LauncherSettings) => Promise<{ ok: boolean; output?: string; processId?: number }>;
    onCs2StartupExit: (callback: (status: { message: string }) => void) => () => void;
    getIntroVideoUrl: () => Promise<string>;
    getUpdateStatus: () => Promise<UpdateStatus>;
    checkForUpdates: () => Promise<UpdateStatus>;
    downloadUpdate: () => Promise<unknown>;
    installUpdate: () => Promise<UpdateStatus>;
    onUpdateStatus: (callback: (status: UpdateStatus) => void) => () => void;
    detectFfmpeg: () => Promise<FfmpegStatus>;
    chooseFfmpeg: () => Promise<{ ok: boolean; canceled?: boolean; path?: string; version?: string; message?: string }>;
    installFfmpeg: (options?: { addToPath?: boolean }) => Promise<FfmpegInstallResult>;
    relinkFfmpeg: () => Promise<{ ok: boolean; path?: string; version?: string; source?: string; message?: string }>;
    onFfmpegStatus: (callback: (status: FfmpegProgress) => void) => () => void;
    getPayloadStatus: () => Promise<PayloadLocalStatus>;
    checkPayload: () => Promise<PayloadCheck>;
    installPayload: (only?: string[]) => Promise<PayloadCheck>;
    getDlssStatus: () => Promise<DlssStatus>;
    chooseDlssPayload: () => Promise<string>;
    chooseReShade: () => Promise<string>;
    installDlss: (options: { payloadDir?: string; reshadePath?: string; variant?: string }) => Promise<DlssStatus & { error?: string; code?: string }>;
    detectDlss: () => Promise<DlssDetect>;
    uninstallDlss: () => Promise<DlssStatus>;
    cleanSteamFolder: () => Promise<{ ok: boolean; moved: string[]; backup: string; status: DlssStatus }>;
    onDlssStatus: (callback: (status: { message: string }) => void) => () => void;
    onPayloadStatus: (callback: (status: PayloadProgress) => void) => () => void;
  };
}
