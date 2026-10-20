import { useEffect, useRef, useState, type KeyboardEvent as ReactKeyboardEvent } from 'react';
import { Download, ExternalLink, FolderOpen, Minus } from 'lucide-react';

type PanoramaIconProps = {
  name: string;
  className?: string;
  alt?: string;
};

function PanoramaIcon({ name, className = '', alt = '' }: PanoramaIconProps) {
  return <img className={`panorama-icon ${className}`} src={`./panorama/icons/${name}`} alt={alt} draggable={false} />;
}

function PanoramaControlIcon({ name, className = '', alt = '' }: PanoramaIconProps) {
  return <img className={`panorama-control-icon ${className}`} src={`./panorama/control-icons/${name}`} alt={alt} draggable={false} />;
}

/**
 * Middle-truncated path. A launcher window is ~600px wide and these paths are long, so the tail
 * -- the filename, the thing you are actually checking -- is what must survive.
 */
function shortenPath(value: string, keep = 46) {
  if (value.length <= keep) return value;
  const tail = value.slice(-(keep - 12));
  const head = value.slice(0, 9);
  return `${head}…\\${tail.replace(/^[\\/]/, '')}`;
}

/**
 * The path a setting really resolves to, shown whether or not it was overridden. "Automatic" on
 * its own tells you nothing about which DLL is about to be injected.
 */
function ResolvedHint({ entry }: { entry?: ResolvedPath }) {
  if (!entry || !entry.path) return null;
  const state = entry.overridden ? 'SET' : 'AUTO';
  return (
    <span className={`resolved-hint ${entry.exists ? '' : 'is-missing'}`} title={entry.path}>
      <span className="resolved-tag">{state}</span>
      <span className="resolved-path">{shortenPath(entry.path)}</span>
      {!entry.exists && <span className="resolved-missing">NOT FOUND</span>}
    </span>
  );
}

function GoToButton({
  entry,
  label,
  onReveal,
}: { entry?: ResolvedPath; label: string; onReveal: (entry?: ResolvedPath) => void }) {
  const disabled = !entry || !entry.path;
  return (
    <button
      type="button"
      className="goto-button"
      disabled={disabled}
      onClick={() => onReveal(entry)}
      aria-label={`Open ${label} in Explorer`}
      title={disabled ? 'Nothing to open yet' : `Open ${label} in Explorer`}
    >
      <ExternalLink size={13} />
    </button>
  );
}

function formatBytes(bytes: number) {
  if (bytes < 1024) return `${bytes} B`;
  const units = ['KB', 'MB', 'GB', 'TB'];
  let value = bytes / 1024;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit += 1;
  }
  return `${value >= 100 ? value.toFixed(0) : value.toFixed(1)} ${units[unit]}`;
}

/** A path the launcher owns but exposes no editable field for. */
function LocationRow({
  label,
  entry,
  onReveal,
  detail,
  size,
}: {
  label: string;
  entry?: ResolvedPath;
  onReveal: (entry?: ResolvedPath) => void;
  detail?: string;
  /** Bytes on disk; undefined while it is still being measured. */
  size?: number;
}) {
  const measured = entry?.exists && size !== undefined;
  // Saved-data rows (the ones with a detail) are created on first use, so absence is not an error.
  const missing = entry && !entry.exists && !detail;
  return (
    <div className={`location-row ${missing ? 'is-missing' : ''}`}>
      <span className="location-label" title={detail}>{label}</span>
      <span className="location-path" title={detail ? `${detail}\n${entry?.path || ''}` : entry?.path || ''}>
        {entry?.path
          ? (entry.exists ? shortenPath(entry.path, 40) : `${shortenPath(entry.path, 28)}  (not created yet)`)
          : 'Not available yet'}
      </span>
      <span className="location-actions">
        {measured && <span className="location-size">{formatBytes(size)}</span>}
        <GoToButton entry={entry} label={label} onReveal={onReveal} />
      </span>
    </div>
  );
}

const emptySettings: LauncherSettings = {
  cs2Exe: '',
  runtimeRoot: '',
  hookDllPath: '',
  injectorPath: '',
  mmcfgEnabled: false,
  mmcfg: '',
  devMode: false,
  customLaunchOptions: '-console +sv_lan 1',
  ffmpegPath: '',
  dlss5Enabled: false,
  dlssPromptAnswered: false,
  autoUpdate: false,
};

const games = [
  { id: 'cs2', label: 'CS2', detail: 'SOURCE 2', available: true },
  { id: 'csgo', label: 'CS:GO', detail: 'LEGACY', available: false },
  { id: 'css', label: 'CS SOURCE', detail: 'SOURCE', available: false },
  { id: 'cs16', label: '1.6', detail: 'GOLDSRC', available: false },
];

const LETS_ROLL_SOUND = './sounds/mm_success_lets_roll.wav';

type IntroPhase = 'playing' | 'fading' | 'done';
type AdvancedTab = 'game' | 'runtime' | 'locations' | 'dlss' | 'updates';

function playLetsRoll(audio: HTMLAudioElement | null) {
  if (!audio) return;
  try {
    audio.currentTime = 0;
    // 0.36 = 40% of the old 0.9. The GO cue is a launch flourish, not an alert, and at
    // near-full volume it was startling on headphones.
    audio.volume = 0.36;
    void audio.play().catch(() => {
      // Audio failures should never block launch.
    });
  } catch {
    // Ignore restricted media contexts.
  }
}

export default function App() {
  const [settings, setSettings] = useState(emptySettings);
  const [ready, setReady] = useState(false);
  const [advanced, setAdvanced] = useState(false);
  const [advancedTab, setAdvancedTab] = useState<AdvancedTab>('game');
  const [busy, setBusy] = useState(false);
  const [started, setStarted] = useState(false);
  const [message, setMessage] = useState('READY');
  const [messageTone, setMessageTone] = useState<'normal' | 'good' | 'error'>('normal');
  const [gameFound, setGameFound] = useState(false);
  const [tools, setTools] = useState<WorkshopToolsStatus | null>(null);
  const [introPhase, setIntroPhase] = useState<IntroPhase>('playing');
  const [introVideoUrl, setIntroVideoUrl] = useState('');
  const [updateBusy, setUpdateBusy] = useState(true);
  const [launchAllowed, setLaunchAllowed] = useState(false);
  const [updateState, setUpdateState] = useState<UpdateStatus['state']>('checking');
  // A newer launcher the last check found, offered once per version and never forced.
  const [pendingUpdate, setPendingUpdate] = useState<UpdateStatus['update'] | null>(null);
  const [updatePromptDismissed, setUpdatePromptDismissed] = useState('');
  const [payload, setPayload] = useState<PayloadLocalStatus | null>(null);
  // Footer progress bars. percent < 0 = working, no measurable progress (indeterminate).
  const [payloadProgress, setPayloadProgress] = useState<{ label: string; percent: number } | null>(null);
  const [updateProgress, setUpdateProgress] = useState<number | null>(null);
  const [paths, setPaths] = useState<ResolvedPaths | null>(null);
  const [sizes, setSizes] = useState<Record<string, number>>({});
  const [ffmpeg, setFfmpeg] = useState<FfmpegStatus | null>(null);
  const [ffmpegBusy, setFfmpegBusy] = useState(false);
  const [ffmpegPrompt, setFfmpegPrompt] = useState(false);
  const [ffmpegAddToPath, setFfmpegAddToPath] = useState(true);
  // Download percent while the ffmpeg install runs; null = no bar. -1 = indeterminate (unpack).
  const [ffmpegPercent, setFfmpegPercent] = useState<number | null>(null);
  const [dlss, setDlss] = useState<DlssStatus | null>(null);
  const [dlssBusy, setDlssBusy] = useState(false);
  const [dlssNote, setDlssNote] = useState('');
  const [dlssVariant, setDlssVariant] = useState('NVIDIA original');
  // First launch of a self-contained build: offer DLSS 5 once (yes installs it and turns it on).
  const [dlssPrompt, setDlssPrompt] = useState(false);
  const [dlssGpu, setDlssGpu] = useState<DlssDetect | null>(null);
  const introPhaseRef = useRef<IntroPhase>('playing');
  const letsRollAudioRef = useRef<HTMLAudioElement>(null);
  const drawerRef = useRef<HTMLElement>(null);
  const advancedButtonRef = useRef<HTMLButtonElement>(null);

  useEffect(() => {
    let active = true;
    Promise.all([
      window.sourceMvm.getSettings(),
      window.sourceMvm.getIntroVideoUrl(),
    ]).then(([saved, videoUrl]) => {
      if (!active) return;
      setSettings({ ...emptySettings, ...saved });
      setIntroVideoUrl(videoUrl);
      setReady(true);
    }).catch((error: Error) => {
      if (!active) return;
      setMessage(error.message);
      setMessageTone('error');
      setReady(true);
    });
    return () => {
      active = false;
    };
  }, []);

  // What the launcher already has on disk, so the status line can say "no runtime yet" before
  // the user presses anything. The main process separately starts the update check.
  useEffect(() => {
    let active = true;
    window.sourceMvm.getPayloadStatus()
      .then((status) => { if (active) setPayload(status); })
      .catch(() => { if (active) setPayload(null); });
    return () => { active = false; };
  }, []);

  // Progress from the runtime payload download/unpack.
  useEffect(() => window.sourceMvm.onPayloadStatus((status) => {
    const label = (status.label ?? 'RUNTIME').toUpperCase();
    if (status.state === 'downloading') {
      setMessage(`DOWNLOADING ${label}`);
      setMessageTone('normal');
      setPayloadProgress({ label, percent: status.percent ?? 0 });
    } else if (status.state === 'installing') {
      setMessage(`INSTALLING ${label}`);
      setMessageTone('normal');
      setPayloadProgress({ label, percent: status.percent ?? -1 });
    } else if (status.state === 'installed') {
      setPayloadProgress(null);
      // The app/runtime coordinator decides when GO is ready.
      window.sourceMvm.getPayloadStatus().then(setPayload).catch(() => {});
    } else if (status.state === 'error') {
      setPayloadProgress(null);
      setMessage(status.message ? status.message.toUpperCase() : 'RUNTIME INSTALL FAILED');
      setMessageTone('error');
    }
  }), []);

  // Subscribe before reading the snapshot: startup may finish before this renderer exists.
  useEffect(() => {
    let active = true, observed = false;
    const apply = (status: UpdateStatus) => {
      if (!active) return;
      setLaunchAllowed(status.launchAllowed === true);
      setUpdateState(status.state);
      setUpdateBusy(['checking', 'available', 'downloading', 'downloaded', 'installing'].includes(status.state));
      setUpdateProgress(status.state === 'downloading' ? status.percent ?? 0 : null);
      if (status.state === 'ready') setPendingUpdate(status.update ?? null);
      const messages: Partial<Record<UpdateStatus['state'], string>> = {
        checking: 'CHECKING FOR UPDATES',
        available: `DOWNLOADING UPDATE ${status.version ?? ''}`.trim(),
        downloading: `DOWNLOADING UPDATE ${Math.round(status.percent ?? 0)}%`,
        downloaded: 'UPDATE DOWNLOADED',
        installing: 'INSTALLING UPDATE / RESTARTING',
        ready: status.message ?? 'UP TO DATE',
        error: status.message?.toUpperCase() ?? 'UPDATE FAILED / RETRY UPDATES',
      };
      setMessage(messages[status.state] ?? status.message ?? 'CHECKING FOR UPDATES');
      // Ready because the update server was unreachable is still ready, but not a green "current".
      setMessageTone(status.state === 'error' ? 'error' : status.state === 'ready' && !status.skipped ? 'good' : 'normal');
      if (status.state === 'ready') void window.sourceMvm.getPayloadStatus().then(setPayload).catch(() => {});
    };
    const unsubscribe = window.sourceMvm.onUpdateStatus(status => { observed = true; apply(status); });
    window.sourceMvm.getUpdateStatus().then(status => { if (!observed) apply(status); })
      .catch(() => { if (!observed) apply({ state: 'error', launchAllowed: false, message: 'UPDATE STATUS UNAVAILABLE / RETRY UPDATES' }); });
    return () => { active = false; unsubscribe(); };
  }, []);

  const finishIntro = () => {
    if (introPhaseRef.current !== 'playing') return;
    introPhaseRef.current = 'fading';
    setIntroPhase('fading');
    window.setTimeout(() => {
      introPhaseRef.current = 'done';
      setIntroPhase('done');
    }, 420);
  };

  useEffect(() => {
    const fallback = window.setTimeout(finishIntro, 5000);
    return () => window.clearTimeout(fallback);
  }, []);

  useEffect(() => {
    let active = true;
    window.sourceMvm.validateCs2(settings.cs2Exe).then((valid) => {
      if (active) setGameFound(valid);
    });
    return () => {
      active = false;
    };
  }, [settings.cs2Exe]);

  useEffect(() => {
    if (!started) return;
    let active = true;
    const monitor = window.setInterval(() => {
      window.sourceMvm.isCs2Running().then((running) => {
        if (!active || running) return;
        setStarted(false);
        setMessage('READY');
        setMessageTone('normal');
      });
    }, 1000);
    return () => {
      active = false;
      window.clearInterval(monitor);
    };
  }, [started]);

  useEffect(() => {
    if (!advanced) return;
    const drawer = drawerRef.current;
    const focusable = drawer?.querySelectorAll<HTMLElement>(
      'button:not(:disabled), input:not(:disabled)',
    );
    focusable?.[0]?.focus();
    const closeOnEscape = (event: KeyboardEvent) => {
      if (event.key === 'Escape') setAdvanced(false);
    };
    window.addEventListener('keydown', closeOnEscape);
    return () => {
      window.removeEventListener('keydown', closeOnEscape);
      advancedButtonRef.current?.focus();
    };
  }, [advanced]);

  const trapDrawerFocus = (event: ReactKeyboardEvent<HTMLElement>) => {
    if (event.key !== 'Tab') return;
    const focusable = Array.from(
      event.currentTarget.querySelectorAll<HTMLElement>('button:not(:disabled), input:not(:disabled)'),
    );
    if (!focusable.length) return;
    const first = focusable[0];
    const last = focusable[focusable.length - 1];
    if (event.shiftKey && document.activeElement === first) {
      event.preventDefault();
      last.focus();
    } else if (!event.shiftKey && document.activeElement === last) {
      event.preventDefault();
      first.focus();
    }
  };

  const patch = (next: Partial<LauncherSettings>) => {
    setSettings((current) => ({ ...current, ...next }));
  };

  // DLSS 5 state: re-read when the game path changes or the drawer opens, so a Steam update or a
  // ReShade file someone dropped beside Steam's cs2.exe shows up without restarting the launcher.
  const refreshDlss = () => window.sourceMvm.getDlssStatus().then(setDlss).catch(() => setDlss(null));
  useEffect(() => { void refreshDlss(); }, [settings.cs2Exe, advanced]);
  useEffect(() => window.sourceMvm.onDlssStatus((status) => setDlssNote(status.message)), []);

  // CS2 died shortly after a launch that already reported CS2 STARTED.
  useEffect(() => window.sourceMvm.onCs2StartupExit((status) => {
    setStarted(false);
    setMessage(status.message);
    setMessageTone('error');
  }), []);

  // The GPU decides the runtime: preselect it, and ask the first-launch question once.
  useEffect(() => {
    if (!ready || !dlss || !dlss.bundled) return;
    let active = true;
    window.sourceMvm.detectDlss().then((found) => {
      if (!active) return;
      setDlssGpu(found);
      if (found.variant) setDlssVariant(found.variant);
      if (!settings.dlssPromptAnswered && !dlss.installed) setDlssPrompt(true);
    }).catch(() => {});
    return () => { active = false; };
  }, [ready, dlss?.bundled, dlss?.installed, settings.dlssPromptAnswered]);

  // A self-contained build: everything is in the installer, so no folders to pick.
  const installBundledDlss = async (fromPrompt: boolean) => {
    if (dlssBusy) return;
    setDlssBusy(true);
    setDlssNote('Installing DLSS 5 and ReShade...');
    try {
      const result = await window.sourceMvm.installDlss({ variant: fromPrompt ? '' : dlssVariant });
      if (result.error) { setDlssNote(result.error); return; }
      setDlss(result);
      patch({ dlss5Enabled: true, dlssPromptAnswered: true });
      setDlssPrompt(false);
      setDlssNote('DLSS 5 installed and switched on. It runs only when SOURCE:MVM starts CS2.');
    } catch (error) {
      setDlssNote(error instanceof Error ? error.message : 'DLSS 5 install failed.');
    } finally {
      setDlssBusy(false);
    }
  };

  const installDlss = async () => {
    if (dlssBusy) return;
    if (dlss?.bundled) { await installBundledDlss(false); return; }
    const payloadDir = await window.sourceMvm.chooseDlssPayload();
    if (!payloadDir) return;
    setDlssBusy(true);
    setDlssNote('Checking the DLSS 5 folder...');
    try {
      let result = await window.sourceMvm.installDlss({ payloadDir, variant: dlssVariant });
      if (result.code === 'NEED_RESHADE') {
        setDlssNote('Choose ReShade64.dll or the official ReShade setup with add-on support.');
        const reshadePath = await window.sourceMvm.chooseReShade();
        if (!reshadePath) { setDlssNote('Install cancelled: ReShade is needed.'); return; }
        result = await window.sourceMvm.installDlss({ payloadDir, reshadePath, variant: dlssVariant });
      }
      if (result.error) { setDlssNote(result.error); return; }
      setDlss(result);
      patch({ dlss5Enabled: true });
      setDlssNote('DLSS 5 installed. It runs only when SOURCE:MVM starts CS2.');
    } catch (error) {
      setDlssNote(error instanceof Error ? error.message : 'DLSS 5 install failed.');
    } finally {
      setDlssBusy(false);
    }
  };

  const uninstallDlss = async () => {
    if (dlssBusy) return;
    setDlssBusy(true);
    try {
      setDlss(await window.sourceMvm.uninstallDlss());
      patch({ dlss5Enabled: false });
      setDlssNote('DLSS 5 removed.');
    } catch (error) {
      setDlssNote(error instanceof Error ? error.message : 'Uninstall failed.');
    } finally {
      setDlssBusy(false);
    }
  };

  const cleanSteamFolder = async () => {
    const result = await window.sourceMvm.cleanSteamFolder();
    setDlss(result.status);
    setDlssNote(result.moved.length
      ? `Moved ${result.moved.length} file(s) out of Steam's CS2 folder to ${result.backup}`
      : "Steam's CS2 folder is already clean.");
  };
  const steamLeftovers = dlss?.steamLeftovers ?? [];

  // Persist settings as they change. Historically only GO saved them, so anything edited in the
  // drawer (paths and launch options) silently vanished if the launcher was closed without
  // launching. Debounced so typing does not write a file per keystroke; gated on
  // `ready` so the initial load never writes defaults over the saved file.
  useEffect(() => {
    if (!ready) return undefined;
    const timer = setTimeout(() => { void window.sourceMvm.saveSettings(settings); }, 400);
    return () => clearTimeout(timer);
  }, [ready, settings]);

  // Re-probe whenever the game path changes: the answer depends entirely on which install we are
  // pointed at, and the user can repoint it from LOCATE GAME at any time.
  useEffect(() => {
    let cancelled = false;
    window.sourceMvm.getWorkshopTools(settings.cs2Exe)
      .then((status) => { if (!cancelled) setTools(status); })
      .catch(() => { if (!cancelled) setTools(null); });
    return () => { cancelled = true; };
  }, [settings.cs2Exe]);

  // Re-resolve whenever a path setting changes or the drawer opens. Resolution depends on what is
  // actually on disk, so it cannot be derived in the renderer -- the main process has to answer.
  useEffect(() => {
    let cancelled = false;
    window.sourceMvm.getResolvedPaths(settings)
      .then((next) => { if (!cancelled) setPaths(next); })
      .catch(() => { if (!cancelled) setPaths(null); });
    return () => { cancelled = true; };
  }, [advanced, settings.cs2Exe, settings.runtimeRoot, settings.hookDllPath, settings.mmcfg, settings.mmcfgEnabled, payload]);

  // Disk usage is only measured while LOCATIONS is open: walking the demo and map-data folders
  // touches thousands of files, which the launch strip never needs.
  useEffect(() => {
    if (!advanced || advancedTab !== 'locations' || !paths) return undefined;
    let cancelled = false;
    const targets = [
      ...paths.data.filter((entry) => entry.exists).map((entry) => entry.path),
      paths.installRoot.path,
      paths.appData.path,
    ].filter(Boolean);
    window.sourceMvm.getPathSizes(targets)
      .then((next) => { if (!cancelled) setSizes(next); })
      .catch(() => { if (!cancelled) setSizes({}); });
    return () => { cancelled = true; };
  }, [advanced, advancedTab, paths]);

  // Grow the window while the drawer is open so the settings are not a scroll-fest in a 300px
  // tall strip. The main process owns the bounds and puts them back on close.
  useEffect(() => {
    window.sourceMvm.setAdvancedOpen(advanced);
  }, [advanced]);

  // FFmpeg is checked once at startup. It is not bundled -- it is a large third-party GPL binary
  // most people already have -- so the launcher looks before it ever offers to download.
  useEffect(() => {
    let cancelled = false;
    window.sourceMvm.detectFfmpeg()
      .then((status) => {
        if (cancelled) return;
        setFfmpeg(status);
        // Ask once, on the first run where it is genuinely missing. Nothing renders without it.
        if (!status.found) setFfmpegPrompt(true);
      })
      .catch(() => { if (!cancelled) setFfmpeg(null); });
    return () => { cancelled = true; };
  }, []);

  useEffect(() => window.sourceMvm.onFfmpegStatus((status) => {
    if (status.state === 'downloading') {
      setMessage(`DOWNLOADING FFMPEG ${Math.round(status.percent ?? 0)}%`);
      setMessageTone('normal');
      setFfmpegPercent(status.percent ?? 0);
    } else if (status.state === 'installing') {
      setMessage('INSTALLING FFMPEG');
      setMessageTone('normal');
      setFfmpegPercent(-1);
    } else if (status.state === 'unverified') {
      // Say so rather than implying the download was checked when it was not.
      setMessage('FFMPEG CHECKSUM UNAVAILABLE — VERIFYING BY RUNNING IT');
      setMessageTone('normal');
    } else if (status.state === 'error') {
      setMessage(status.message ? status.message.toUpperCase() : 'FFMPEG INSTALL FAILED');
      setMessageTone('error');
    }
  }), []);

  const installFfmpeg = async () => {
    if (ffmpegBusy) return;
    setFfmpegBusy(true);
    setFfmpegPercent(0);
    try {
      const result = await window.sourceMvm.installFfmpeg({ addToPath: ffmpegAddToPath });
      if (!result.ok) {
        setMessage(result.message ? result.message.toUpperCase() : 'FFMPEG INSTALL FAILED');
        setMessageTone('error');
        return;
      }
      setFfmpeg(await window.sourceMvm.detectFfmpeg());
      setFfmpegPrompt(false);
      setMessage(
        `FFMPEG ${result.version ?? ''} INSTALLED${result.pathAdded && !result.pathAlready ? ' — ADDED TO PATH' : ''}`.trim(),
      );
      setMessageTone('good');
    } catch (error) {
      setMessage(error instanceof Error ? error.message.toUpperCase() : 'FFMPEG INSTALL FAILED');
      setMessageTone('error');
    } finally {
      setFfmpegBusy(false);
      setFfmpegPercent(null);
    }
  };

  // Point the launcher at an ffmpeg.exe the user already has. The main process validates by
  // running the picked file, saves it in settings, and links the runtime at it.
  const pickFfmpeg = async () => {
    if (ffmpegBusy) return;
    setFfmpegBusy(true);
    try {
      const result = await window.sourceMvm.chooseFfmpeg();
      if (result.canceled) return;
      if (!result.ok) {
        setMessage(result.message ? result.message.toUpperCase() : 'THAT FILE DID NOT RUN AS FFMPEG');
        setMessageTone('error');
        return;
      }
      setSettings(await window.sourceMvm.getSettings());
      setFfmpeg(await window.sourceMvm.detectFfmpeg());
      setFfmpegPrompt(false);
      setMessage(`FFMPEG ${result.version ?? ''} SELECTED`.trim());
      setMessageTone('good');
    } finally {
      setFfmpegBusy(false);
    }
  };

  // "I'll install it myself" — dismiss, then re-check so their own install gets picked up and
  // linked without needing a restart.
  const recheckFfmpeg = async () => {
    setFfmpegBusy(true);
    try {
      const relinked = await window.sourceMvm.relinkFfmpeg();
      const status = await window.sourceMvm.detectFfmpeg();
      setFfmpeg(status);
      if (status.found) {
        setFfmpegPrompt(false);
        setMessage(`FFMPEG FOUND (${(relinked.source ?? status.source).toUpperCase()})`);
        setMessageTone('good');
      } else {
        setMessage('STILL NO FFMPEG FOUND');
        setMessageTone('error');
      }
    } finally {
      setFfmpegBusy(false);
    }
  };

  const reveal = async (entry?: ResolvedPath) => {
    if (!entry || !entry.path) return;
    const result = await window.sourceMvm.revealPath(entry.path);
    if (!result.ok) {
      setMessage(result.message ? result.message.toUpperCase() : 'COULD NOT OPEN THAT PATH');
      setMessageTone('error');
    } else if (result.fellBack) {
      setMessage('NOT CREATED YET — OPENED THE PARENT FOLDER');
      setMessageTone('normal');
    }
  };

  const toolsMissing = settings.devMode && tools !== null && !tools.installed && !tools.unknown;
  // No hook on disk yet: the very first thing a fresh install needs is the runtime download, so
  // the footer button relabels itself rather than hiding that behind a generic "check".
  const runtimeMissing = payload !== null && !payload.components.runtime?.present;

  const openWorkshopToolsInstall = async () => {
    try {
      await window.sourceMvm.installWorkshopTools();
      setMessage('OPENING STEAM');
      setMessageTone('normal');
    } catch {
      setMessage('COULD NOT OPEN STEAM');
      setMessageTone('error');
    }
  };

  const chooseGame = async () => {
    const selected = await window.sourceMvm.chooseFile('game');
    if (selected) patch({ cs2Exe: selected });
  };

  const chooseFolder = async (key: 'runtimeRoot' | 'mmcfg') => {
    const selected = await window.sourceMvm.chooseFolder();
    if (!selected) return;
    patch(key === 'mmcfg'
      ? { mmcfg: selected, mmcfgEnabled: true }
      : { runtimeRoot: selected });
  };

  const chooseHook = async () => {
    const selected = await window.sourceMvm.chooseFile('hook');
    if (selected) patch({ hookDllPath: selected });
  };

  const launch = async () => {
    if (busy || started || !launchAllowed) return;
    playLetsRoll(letsRollAudioRef.current);
    setBusy(true);
    setMessage('STARTING CS2');
    setMessageTone('normal');
    try {
      await window.sourceMvm.saveSettings(settings);
      await window.sourceMvm.launchCs2(settings);
      setStarted(true);
      setMessage('CS2 STARTED');
      setMessageTone('good');
    } catch (error) {
      setMessage(error instanceof Error ? error.message : 'LAUNCH FAILED');
      setMessageTone('error');
    } finally {
      setBusy(false);
    }
  };

  // UPDATES: a check, or the download once a check has found an update. Either way the status
  // line follows the main process; the renderer only locks GO until the first status arrives.
  const runUpdates = async (install: boolean) => {
    if (updateBusy || busy || started) return;
    setLaunchAllowed(false);
    setUpdateBusy(true);
    setMessage(install ? 'DOWNLOADING UPDATE' : 'CHECKING FOR UPDATES');
    setMessageTone('normal');
    try {
      if (install) await window.sourceMvm.downloadUpdate();
      else await window.sourceMvm.checkForUpdates();
    } catch (error) {
      setMessage(error instanceof Error ? error.message.toUpperCase() : 'UPDATE CHECK FAILED / RETRY UPDATES');
      setMessageTone('error');
      setUpdateBusy(false);
    }
  };
  const updateInstallable = Boolean(pendingUpdate?.installable);
  // RETRY UPDATES after an error re-checks; otherwise a pending update is downloaded.
  const checkForUpdates = () => runUpdates(updateInstallable && updateState !== 'error');
  const installUpdate = () => {
    setUpdatePromptDismissed(pendingUpdate?.version ?? '');
    void runUpdates(true);
  };
  // Shown once per version: "not now" hides it until a check finds a different version.
  const updatePrompt = updateInstallable && pendingUpdate && pendingUpdate.version !== updatePromptDismissed
    && !updateBusy && !busy && !started;

  const minimizeWindow = () => window.sourceMvm.minimizeWindow();
  const closeWindow = () => window.sourceMvm.closeWindow();

  if (!ready) return <main className="loading">INITIALIZING</main>;

  const introDone = introPhase === 'done';
  // One bar in the footer for whatever is installing or downloading right now.
  const railProgress: { label: string; percent: number } | null = payloadProgress
    ?? (updateProgress !== null ? { label: 'LAUNCHER', percent: updateProgress } : null)
    ?? (ffmpegPercent !== null ? { label: 'FFMPEG', percent: ffmpegPercent } : null)
    ?? (dlssBusy ? { label: 'DLSS 5', percent: -1 } : null)
    ?? (busy ? { label: 'STARTING CS2', percent: -1 } : null);

  return (
    <main className={`launcher ${introDone ? 'is-ready' : ''}`}>
      <section
        className={`intro ${introPhase === 'fading' ? 'is-fading' : ''} ${introDone ? 'is-hidden' : ''}`}
        aria-hidden={introDone}
      >
        <audio className="media-preload" ref={letsRollAudioRef} src={LETS_ROLL_SOUND} preload="auto" aria-hidden="true" />
        <video
          className="intro-video"
          src={introVideoUrl || './source-mvm-intro.mp4'}
          autoPlay
          muted
          playsInline
          preload="auto"
          onLoadedMetadata={(event) => {
            const video = event.currentTarget;
            video.playbackRate = 1.25;
            void video.play().catch(finishIntro);
          }}
          onEnded={finishIntro}
          onError={finishIntro}
        />
      </section>

      <header className="window-chrome mainmenu-top-navbar">
        <div className="window-identity" aria-label="SOURCE:MVM Filmmaker">
          <span className="window-mark"><PanoramaIcon name="film.svg" /></span>
          <span className="window-title">SOURCE:MVM</span>
          <span className="window-context">FILMMAKER</span>
        </div>
        <div className="window-actions">
          <button type="button" className="window-button" onClick={minimizeWindow} aria-label="Minimize launcher">
            <Minus size={13} strokeWidth={1.7} />
          </button>
          <button type="button" className="window-button window-button-close" onClick={closeWindow} aria-label="Close launcher">
            <PanoramaControlIcon name="close.png" />
          </button>
        </div>
      </header>

      <header className="game-nav content-navbar__tabs content-navbar__tabs--dark content-navbar__tabs--noflow">
        <nav className="game-tabs" aria-label="Game target">
          {games.map((game) => (
            <button
              key={game.id}
              className={`game-tab content-navbar__tabs__btn ${game.available ? 'is-active' : ''}`}
              type="button"
              disabled={!game.available}
              aria-current={game.available ? 'page' : undefined}
              title={game.available ? 'Counter-Strike 2' : `${game.label} is not available yet`}
            >
              <span>{game.label}</span>
              <small>{game.detail}</small>
            </button>
          ))}
        </nav>
      </header>

      <section className="launch-field mainmenu-content__container">
        <div className="session-copy SettingsBackground">
          <div className="session-code">
            <span className="session-code-rule" aria-hidden="true" />
            <span className="session-code-text">SOURCE // FILMMAKER SESSION</span>
            <span className={`readiness ${gameFound ? 'is-found' : 'is-missing'}`}>
              <span className="readiness-dot" aria-hidden="true" />
              <span>{gameFound ? 'GAME READY' : 'CS2 NOT FOUND'}</span>
              {!gameFound && <button type="button" onClick={chooseGame}>LOCATE GAME</button>}
            </span>
          </div>
          {/* The in-game workspace wordmark ("Gen X soft club", WorkspaceShellJs.h): a dim,
              soft-focus title with motion-blur echoes and an out-of-focus ghost behind it, on the
              same streak-in / rest / whip-out loop. Decoration, so one accessible name for all. */}
          <h1 className="session-wordmark" aria-label="SOURCE:MVM">
            <span className="wm-ghost" aria-hidden="true">SOURCE:MVM</span>
            <span className="wm-trail wm-t3" aria-hidden="true">SOURCE:MVM</span>
            <span className="wm-trail wm-t2" aria-hidden="true">SOURCE:MVM</span>
            <span className="wm-trail wm-t1" aria-hidden="true">SOURCE:MVM</span>
            <span className="wm-title" aria-hidden="true">SOURCE:MVM</span>
          </h1>
          {!toolsMissing && <p className="session-sub">COUNTER-STRIKE 2 // OFFLINE DEMO</p>}

          {/* Launch mode. Two buttons rather than a checkbox: the choice changes what CS2 IS for
              the session, so both options should be visible and named, not inferred from whether
              a box is ticked. */}
          <div className="mode-switch" role="group" aria-label="Launch mode">
            <button
              type="button"
              className={`mode-option ${settings.devMode ? '' : 'is-active'}`}
              aria-pressed={!settings.devMode}
              onClick={() => patch({ devMode: false })}
            >
              <span className="mode-option-name">NORMAL</span>
              <span className="mode-option-detail">Retail CS2</span>
            </button>
            <button
              type="button"
              className={`mode-option ${settings.devMode ? 'is-active' : ''} ${toolsMissing ? 'is-blocked' : ''}`}
              aria-pressed={settings.devMode}
              onClick={() => patch({ devMode: true })}
              title="Launches with -tools -dx11, the same mode as Steam's Counter-Strike 2 Workshop Tools."
            >
              <span className="mode-option-name">DEV</span>
              <span className="mode-option-detail">Workshop Tools</span>
            </button>
          </div>

          {/* Only shown when it is actually a problem: dev mode selected, a game path we could
              check, and the tool binaries absent. Names the DLC and offers the one-click install
              rather than leaving the user to find it in Steam's menus. */}
          {toolsMissing && (
            <div className="mode-warning" role="alert">
              <PanoramaIcon name="cancel.svg" />
              <div className="mode-warning-body">
                <strong>Workshop Tools not installed</strong>
                <span>Dev mode needs the free Counter-Strike 2 Workshop Tools DLC.</span>
              </div>
              <button type="button" onClick={openWorkshopToolsInstall}>INSTALL</button>
            </div>
          )}
        </div>

        <div className="play-menu-controls-actions-bar">
          <div className="action-meta">
            <span className="action-meta-brand">SOURCE:MVM</span>
            <span className="action-meta-state">
              {busy ? 'STARTING'
                : started ? 'STARTED'
                  : !launchAllowed ? updateState === 'error' ? 'UPDATE FAILED' : 'UPDATING'
                    : toolsMissing ? 'TOOLS MISSING'
                    : gameFound ? `READY${settings.devMode ? ' · DEV' : ''}${settings.dlss5Enabled && dlss?.installed ? ' · DLSS' : ''}`
                      : 'WAITING'}
            </span>
          </div>
          <button
            className="start-button play-menu__playbtn play-menu__playbtn--startsearch"
            type="button"
            disabled={!gameFound || busy || started || toolsMissing || !launchAllowed}
            onClick={launch}
            aria-label={busy
              ? 'Starting Counter-Strike 2'
              : started
                ? 'Counter-Strike 2 is already started'
                : toolsMissing
                  ? 'Cannot start: dev mode needs the Counter-Strike 2 Workshop Tools'
                  : !launchAllowed
                    ? updateState === 'error'
                      ? 'Updates failed. Select RETRY UPDATES before launching Counter-Strike 2.'
                      : 'Wait for updates to finish before launching Counter-Strike 2'
                    : settings.devMode
                    ? 'GO — launch Counter-Strike 2 in dev mode with SOURCE:MVM'
                    : 'GO — launch Counter-Strike 2 with SOURCE:MVM'}
          >
            <span className="play-menu__playbtn__bg" aria-hidden="true">
              <video
                className="play-linemap-video"
                src="./panorama/videos/linemap.webm"
                autoPlay
                loop
                muted
                playsInline
                preload="auto"
              />
            </span>
            <span className="group-text-with-steam-button">
              <span className="start-text">GO</span>
            </span>
            <span className="play-menu__playbtn__bar" aria-hidden="true" />
            <span className="play-menu__playbtn__bar play-menu__playbtn__bar--right" aria-hidden="true" />
          </button>
          <div className="action-caption">
            {toolsMissing
              ? 'INSTALL THE WORKSHOP TOOLS TO USE DEV MODE'
              : settings.devMode
                ? 'LAUNCH CS2 DEV MODE WITH SOURCE:MVM'
                : 'LAUNCH CS2 WITH SOURCE:MVM'}
          </div>
        </div>
      </section>

      {/* A newer launcher is offered, never forced: NOT NOW keeps this version launchable, and the
          footer button downloads it later. Same dialog styling as the FFmpeg and DLSS prompts. */}
      {updatePrompt && (
        <section className="ffmpeg-prompt" role="dialog" aria-labelledby="update-prompt-title">
          <div className="ffmpeg-prompt-body">
            <strong id="update-prompt-title">Hey, new update.</strong>
            <p>
              SOURCE:MVM {pendingUpdate?.version} is available. Do you want to update? It downloads,
              installs and restarts the launcher. If not, GO still works and the UPDATES button
              downloads it whenever you like.
            </p>
            <div className="ffmpeg-prompt-actions">
              <button type="button" className="utility-button is-primary" onClick={installUpdate}>
                <Download size={13} />
                <span>UPDATE</span>
              </button>
              <button type="button" className="utility-button" onClick={() => setUpdatePromptDismissed(pendingUpdate?.version ?? '')}>
                <span>NOT NOW</span>
              </button>
            </div>
          </div>
        </section>
      )}

      {/* FFmpeg is what encodes every render. Missing it does not break the launcher, so this
          asks rather than blocks -- but it asks up front, because the alternative is finding out
          at the end of a render that nothing could be written. */}
      {ffmpegPrompt && (
        <section className="ffmpeg-prompt" role="dialog" aria-labelledby="ffmpeg-prompt-title">
          <div className="ffmpeg-prompt-body">
            <strong id="ffmpeg-prompt-title">FFmpeg is not installed</strong>
            <p>
              SOURCE:MVM needs FFmpeg to encode renders. Without it the Render workspace runs but
              writes no video.
            </p>
            <label className="ffmpeg-checkbox">
              <input
                type="checkbox"
                checked={ffmpegAddToPath}
                onChange={(event) => setFfmpegAddToPath(event.target.checked)}
              />
              <span>Also add it to my PATH so I can use <code>ffmpeg</code> in a terminal</span>
            </label>
            <div className="ffmpeg-prompt-actions">
              <button type="button" className="utility-button is-primary" onClick={installFfmpeg} disabled={ffmpegBusy}>
                <Download size={13} />
                <span>
                  {ffmpegPercent !== null
                    ? (ffmpegPercent < 0 ? 'UNPACKING…' : `DOWNLOADING ${Math.round(ffmpegPercent)}%`)
                    : ffmpegBusy ? 'WORKING…' : 'INSTALL IT FOR ME'}
                </span>
              </button>
              <button type="button" className="utility-button" onClick={pickFfmpeg} disabled={ffmpegBusy}>
                <span>I HAVE IT — PICK ffmpeg.exe</span>
              </button>
              <button type="button" className="utility-button" onClick={recheckFfmpeg} disabled={ffmpegBusy}>
                <span>RE-CHECK</span>
              </button>
              <button type="button" className="utility-button" onClick={() => setFfmpegPrompt(false)} disabled={ffmpegBusy}>
                <span>LATER</span>
              </button>
            </div>
            {/* The ~150 MB download runs for minutes on a slow line; without a bar the dialog
                reads as hung and the install gets cancelled or force-closed mid-write. */}
            {ffmpegPercent !== null && (
              <div
                className={`ffmpeg-progress ${ffmpegPercent < 0 ? 'is-indeterminate' : ''}`}
                role="progressbar"
                aria-label="FFmpeg install progress"
                aria-valuemin={0}
                aria-valuemax={100}
                aria-valuenow={ffmpegPercent < 0 ? undefined : Math.round(ffmpegPercent)}
              >
                <span style={{ width: ffmpegPercent < 0 ? '100%' : `${Math.max(1, ffmpegPercent)}%` }} />
              </div>
            )}
            <span className="ffmpeg-prompt-note">
              Downloads the official Windows build from gyan.dev (~150&nbsp;MB), checks it against
              the published SHA-256, and installs it into the launcher&apos;s own folder. No admin
              rights, and uninstalling SOURCE:MVM removes it.
            </span>
          </div>
        </section>
      )}

      {/* First launch of a self-contained build: DLSS 5 is in the installer, so offer it once.
          Yes installs it (the runtime is picked from the GPU) and turns it on in ADVANCED. */}
      {dlssPrompt && !ffmpegPrompt && (
        <section className="ffmpeg-prompt" role="dialog" aria-labelledby="dlss-prompt-title">
          <div className="ffmpeg-prompt-body">
            <strong id="dlss-prompt-title">Do you want DLSS 5 (ReShade) installed?</strong>
            <p>
              {dlssGpu && dlssGpu.supported
                ? `Your ${dlssGpu.gpu} gets the ${dlssGpu.variant} runtime. `
                : `${dlssGpu?.reason ?? 'DLSS 5 needs an NVIDIA RTX 20-series or newer.'} `}
              DLSS 5 runs only when SOURCE:MVM starts CS2, from its own copy of the game&apos;s
              program files, so Steam&apos;s CS2 and online matches never load it. You can change this
              later in ADVANCED &gt; DLSS 5.
            </p>
            {dlssNote && dlssBusy && <span className="ffmpeg-prompt-note">{dlssNote}</span>}
            <div className="ffmpeg-prompt-actions">
              <button
                type="button"
                className="utility-button is-primary"
                onClick={() => void installBundledDlss(true)}
                disabled={dlssBusy || !(dlssGpu && dlssGpu.supported)}
              >
                <Download size={13} />
                <span>{dlssBusy ? 'INSTALLING…' : 'YES, INSTALL IT'}</span>
              </button>
              <button
                type="button"
                className="utility-button"
                onClick={() => { patch({ dlssPromptAnswered: true }); setDlssPrompt(false); }}
                disabled={dlssBusy}
              >
                <span>NO THANKS</span>
              </button>
            </div>
            {!dlssBusy && dlssNote && <span className="ffmpeg-prompt-note">{dlssNote}</span>}
          </div>
        </section>
      )}

      <footer className="utility-rail">
        {/* title carries the untruncated text: launch errors are multi-line and the one-line
            rail ellipsizes them into uselessness ("Created CS2 pro..."). */}
        <div className={`session-state is-${messageTone}`} title={message}>
          <span className="state-dot" />
          <span>SOURCE:MVM / {steamLeftovers.length && message === 'READY'
            ? "RESHADE IN STEAM'S CS2 FOLDER — SEE ADVANCED > DLSS 5"
            : runtimeMissing && message === 'READY' ? 'NO RUNTIME INSTALLED' : message}</span>
        </div>
        {railProgress && (
          <div
            className={`rail-progress ${railProgress.percent < 0 ? 'is-indeterminate' : ''}`}
            role="progressbar"
            aria-label={`${railProgress.label} progress`}
            aria-valuemin={0}
            aria-valuemax={100}
            aria-valuenow={railProgress.percent < 0 ? undefined : Math.round(railProgress.percent)}
          >
            <span className="rail-progress-label">{railProgress.label}</span>
            <span className="rail-progress-track">
              <span style={{ width: railProgress.percent < 0 ? '100%' : `${Math.max(1, railProgress.percent)}%` }} />
            </span>
            <span className="rail-progress-value">
              {railProgress.percent < 0 ? '' : `${Math.round(railProgress.percent)}%`}
            </span>
          </div>
        )}
        <div className="utility-actions">
          <button
            type="button"
            className="utility-button hc-tracker-button"
            onClick={() => { void window.sourceMvm.openHcTracker(); }}
            title="Open HC Tracker (hctracker.com) in your browser"
          >
            <img className="hc-tracker-eye" src="./hctracker/hctracker-eye.png" alt="" draggable={false} />
            <span>HC TRACKER</span>
          </button>
          <button
            type="button"
            className={`utility-button ${updateBusy ? 'is-active' : ''}`}
            onClick={checkForUpdates}
            disabled={updateBusy || busy || started}
            title={updateInstallable
              ? `Download and install SOURCE:MVM ${pendingUpdate?.version ?? ''}, then restart`
              : runtimeMissing
                ? 'Download the SOURCE:MVM runtime (hook + FX pack)'
                : settings.autoUpdate
                  ? 'Check for launcher and runtime updates'
                  : 'Check once for launcher and runtime updates (automatic checks are off)'}
          >
            <PanoramaControlIcon name="refresh.png" />
            <span>{updateState === 'error' ? 'RETRY UPDATES' : updateInstallable ? 'DOWNLOAD UPDATE' : runtimeMissing ? 'INSTALL RUNTIME' : 'UPDATES'}</span>
          </button>
          <button
            ref={advancedButtonRef}
            type="button"
            className={`utility-button ${advanced ? 'is-active' : ''}`}
            onClick={() => setAdvanced((open) => !open)}
            aria-expanded={advanced}
          >
            <PanoramaControlIcon name="gear.png" />
            <span>ADVANCED</span>
          </button>
        </div>
      </footer>

      <section
        ref={drawerRef}
        className={`advanced-drawer ${advanced ? 'is-open' : ''}`}
        role="dialog"
        aria-modal="true"
        aria-labelledby="advanced-title"
        aria-hidden={!advanced}
        onKeyDown={trapDrawerFocus}
      >
        <header className="drawer-header">
          <div>
            <span>LAUNCH CONFIGURATION</span>
            <strong id="advanced-title">Advanced Settings</strong>
          </div>
          <div className="drawer-header-actions">
            <span className="autosave-note">SAVES AUTOMATICALLY</span>
            <button type="button" className="icon-button" onClick={() => setAdvanced(false)} aria-label="Close advanced settings">
              <PanoramaControlIcon name="close.png" />
            </button>
          </div>
        </header>

        <nav className="advanced-tabs" role="tablist" aria-label="Advanced settings sections">
          <button id="advanced-tab-game" type="button" role="tab" aria-selected={advancedTab === 'game'} aria-controls="advanced-panel-game" className={advancedTab === 'game' ? 'is-active' : ''} onClick={() => setAdvancedTab('game')}>GAME</button>
          <button id="advanced-tab-runtime" type="button" role="tab" aria-selected={advancedTab === 'runtime'} aria-controls="advanced-panel-runtime" className={advancedTab === 'runtime' ? 'is-active' : ''} onClick={() => setAdvancedTab('runtime')}>RUNTIME</button>
          <button id="advanced-tab-locations" type="button" role="tab" aria-selected={advancedTab === 'locations'} aria-controls="advanced-panel-locations" className={advancedTab === 'locations' ? 'is-active' : ''} onClick={() => setAdvancedTab('locations')}>LOCATIONS</button>
          <button id="advanced-tab-dlss" type="button" role="tab" aria-selected={advancedTab === 'dlss'} aria-controls="advanced-panel-dlss" className={advancedTab === 'dlss' ? 'is-active' : ''} onClick={() => setAdvancedTab('dlss')}>DLSS 5</button>
          <button id="advanced-tab-updates" type="button" role="tab" aria-selected={advancedTab === 'updates'} aria-controls="advanced-panel-updates" className={advancedTab === 'updates' ? 'is-active' : ''} onClick={() => setAdvancedTab('updates')}>UPDATES</button>
        </nav>

        <div className="drawer-content">
          {advancedTab === 'game' && <div id="advanced-panel-game" className="field-group field-wide" role="tabpanel" aria-labelledby="advanced-tab-game">
            <h3 className="group-title">Game</h3>
            <label className="field">
              <span>CS2 EXECUTABLE</span>
              <span className="field-control">
                <input value={settings.cs2Exe} onChange={(event) => patch({ cs2Exe: event.target.value })} placeholder="Detected automatically" />
                <button type="button" onClick={chooseGame} aria-label="Choose CS2 executable"><FolderOpen size={14} /></button>
                <GoToButton entry={paths?.cs2Exe} label="CS2 executable" onReveal={reveal} />
              </span>
              <ResolvedHint entry={paths?.cs2Exe} />
            </label>
            <label className="field">
              <span>LAUNCH OPTIONS</span>
              <input value={settings.customLaunchOptions} onChange={(event) => patch({ customLaunchOptions: event.target.value })} />
            </label>
          </div>}

          {advancedTab === 'runtime' && <div id="advanced-panel-runtime" className="field-group field-wide" role="tabpanel" aria-labelledby="advanced-tab-runtime">
            <h3 className="group-title">Runtime</h3>
            <label className="field">
              <span>RUNTIME FOLDER</span>
              <span className="field-control">
                <input value={settings.runtimeRoot} onChange={(event) => patch({ runtimeRoot: event.target.value })} placeholder="Automatic" />
                <button type="button" onClick={() => chooseFolder('runtimeRoot')} aria-label="Choose runtime folder"><FolderOpen size={14} /></button>
                <GoToButton entry={paths?.runtimeRoot} label="runtime folder" onReveal={reveal} />
              </span>
              <ResolvedHint entry={paths?.runtimeRoot} />
            </label>
            <label className="field">
              <span>HOOK DLL OVERRIDE</span>
              <span className="field-control">
                <input value={settings.hookDllPath} onChange={(event) => patch({ hookDllPath: event.target.value })} placeholder="Automatic" />
                <button type="button" onClick={chooseHook} aria-label="Choose hook DLL"><FolderOpen size={14} /></button>
                <GoToButton entry={paths?.hookDll} label="hook DLL" onReveal={reveal} />
              </span>
              <ResolvedHint entry={paths?.hookDll} />
            </label>
          </div>}

          {/* Updates are opt-in. Off (the default), the launcher contacts nothing on its own and GO
              works without internet; the footer UPDATES button still checks once when pressed.
              Turning it on starts the usual check straight away (settings:save in main.cjs). */}
          {advancedTab === 'updates' && <div id="advanced-panel-updates" className="field-group field-wide setting-panel" role="tabpanel" aria-labelledby="advanced-tab-updates">
            <h3 className="group-title">Updates</h3>
            <label className="setting-toggle">
              <input
                type="checkbox"
                checked={settings.autoUpdate}
                onChange={(event) => patch({ autoUpdate: event.target.checked })}
              />
              <span>
                <strong>Check for updates automatically</strong>
                <small>On: every start looks for a new launcher and hook and asks before installing. Off: nothing is checked unless you press UPDATES.</small>
              </span>
            </label>
            <div className="setting-status">
              <span className={`setting-dot ${settings.autoUpdate ? 'is-on' : ''}`} aria-hidden="true" />
              <span>
                {pendingUpdate
                  ? `UPDATE ${pendingUpdate.version} AVAILABLE · ${pendingUpdate.installable ? 'PRESS DOWNLOAD UPDATE' : 'INSTALL THE NEW SETUP EXE'}`
                  : settings.autoUpdate ? 'AUTOMATIC · CHECKS ON EVERY START' : 'OFF · PRESS UPDATES TO CHECK ONCE'}
              </span>
            </div>
            <p className="setting-note">
              An update is never forced: you are asked first, and GO keeps working on this version.
              CS2 starts without internet either way. Steam still has to be open and signed in.
            </p>
          </div>}

          {/* Everything the launcher owns but has no field for. Without these there is simply no
              way to get to the downloaded runtime or the FX pack from the UI. */}
          {/* DLSS 5 (ReShade + RenoDX) lives in the launcher's own copy of CS2's program files and
              runs only when the launcher starts that copy. Steam's cs2.exe never has ReShade beside
              it, so online games cannot load it (see electron/dlss-tool.cjs). */}
          {advancedTab === 'dlss' && <div id="advanced-panel-dlss" className="field-group field-wide dlss-panel" role="tabpanel" aria-labelledby="advanced-tab-dlss">
            <h3 className="group-title">DLSS 5</h3>
            {steamLeftovers.length > 0 && (
              <div className="mode-warning" role="alert">
                <PanoramaIcon name="cancel.svg" />
                <div className="mode-warning-body">
                  <strong>ReShade files in Steam&apos;s CS2 folder</strong>
                  <span>{steamLeftovers.join(', ')} load in every CS2 session, online games too.</span>
                </div>
                <button type="button" onClick={cleanSteamFolder}>MOVE OUT</button>
              </div>
            )}
            <label className={`dlss-toggle ${dlss?.installed ? '' : 'is-disabled'}`}>
              <input
                type="checkbox"
                checked={Boolean(settings.dlss5Enabled && dlss?.installed)}
                disabled={!dlss?.installed}
                onChange={(event) => patch({ dlss5Enabled: event.target.checked })}
              />
              <span>
                <strong>Use DLSS 5 when SOURCE:MVM launches CS2</strong>
                <small>Only this launcher starts the DLSS copy. Steam&apos;s CS2 never loads it.</small>
              </span>
            </label>
            <div className="dlss-status">
              <span className={`dlss-dot ${dlss?.installed ? 'is-on' : ''}`} aria-hidden="true" />
              <span>
                {dlss?.installed
                  ? `INSTALLED · ${dlss.variant.toUpperCase()}${dlss.outOfDate ? ' · CS2 UPDATED, RESYNCS ON NEXT LAUNCH' : ''}`
                  : 'NOT INSTALLED'}
              </span>
            </div>
            <div className="dlss-actions">
              <label className="dlss-variant">
                <span>RUNTIME</span>
                <select value={dlssVariant} onChange={(event) => setDlssVariant(event.target.value)} disabled={dlssBusy}>
                  {(dlss?.variants ?? ['NVIDIA original', 'RTX 40, 30, 20 compatible']).map((v) => (
                    <option key={v} value={v}>{v === 'NVIDIA original' ? 'NVIDIA original (RTX 50)' : 'RTX 40 / 30 / 20 compatible'}</option>
                  ))}
                </select>
              </label>
              <button type="button" className="utility-button is-primary" onClick={installDlss} disabled={dlssBusy}>
                <Download size={13} />
                <span>{dlssBusy ? 'WORKING…' : dlss?.installed ? 'REINSTALL…' : 'INSTALL…'}</span>
              </button>
              {dlss?.installed && (
                <button type="button" className="utility-button" onClick={uninstallDlss} disabled={dlssBusy}>
                  <span>UNINSTALL</span>
                </button>
              )}
            </div>
            <p className="dlss-note" role="status">
              {dlssNote || 'INSTALL asks for your "dlss 5" folder, and for ReShade with add-on support if that folder has none. Nothing is downloaded.'}
            </p>
          </div>}

          {advancedTab === 'locations' && <div id="advanced-panel-locations" className="field-group field-wide" role="tabpanel" aria-labelledby="advanced-tab-locations">
            <h3 className="group-title">Program files</h3>
            <div className="location-list">
              <div className={`location-row ${ffmpeg && !ffmpeg.found ? 'is-missing' : ''}`}>
                <span className="location-label">FFmpeg</span>
                <span className="location-path" title={ffmpeg?.path || ''}>
                  {ffmpeg?.found
                    ? `${shortenPath(ffmpeg.path, 30)}  (${ffmpeg.source})`
                    : 'Not installed — renders cannot encode'}
                </span>
                <span className="location-actions">
                  <button
                    type="button"
                    className="location-select-button"
                    onClick={pickFfmpeg}
                    disabled={ffmpegBusy}
                    title="Pick an ffmpeg.exe you already have"
                    aria-label="Select an existing ffmpeg.exe"
                  >
                    SELECT…
                  </button>
                  {ffmpeg?.found
                    ? <GoToButton entry={{ path: ffmpeg.path, kind: 'file', overridden: settings.ffmpegPath === ffmpeg.path, exists: true }} label="FFmpeg" onReveal={reveal} />
                    : (
                      <button
                        type="button"
                        className="goto-button"
                        onClick={() => setFfmpegPrompt(true)}
                        title="Install FFmpeg"
                        aria-label="Install FFmpeg"
                      >
                        <Download size={13} />
                      </button>
                    )}
                </span>
              </div>
              <LocationRow label="Injector" entry={paths?.injector} onReveal={reveal} />
              <LocationRow label="FX pack" entry={paths?.fxPack} onReveal={reveal} />
              <LocationRow label="Installed runtime" entry={paths?.installRoot} onReveal={reveal} size={paths ? sizes[paths.installRoot.path] : undefined} />
              <LocationRow label="Launcher data" entry={paths?.appData} onReveal={reveal} size={paths ? sizes[paths.appData.path] : undefined} />
            </div>
            <h3 className="group-title location-group-title">Saved data</h3>
            <p className="location-note">What SOURCE:MVM saves while you work. Hover a name for details.</p>
            <div className="location-list">
              {(paths?.data ?? []).map((entry) => (
                <LocationRow
                  key={entry.id}
                  label={entry.label}
                  detail={entry.detail}
                  entry={entry}
                  onReveal={reveal}
                  size={sizes[entry.path]}
                />
              ))}
            </div>
          </div>}
        </div>
      </section>
    </main>
  );
}
