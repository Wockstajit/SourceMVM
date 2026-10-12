// ---- Download ------------------------------------------------------------------------------------
// Public release assets only. The source repository remains private.
const RELEASES_URL = 'https://api.github.com/repos/Wockstajit/SourceMVM/releases?per_page=20';
const RELEASES_PAGE = 'https://github.com/Wockstajit/SourceMVM/releases';
const download = document.getElementById('download');
const downloadVersion = document.getElementById('download-version');
document.getElementById('download-label').textContent = 'Download';

async function refreshDownload() {
  try {
    const response = await fetch(RELEASES_URL, { signal: AbortSignal.timeout(15000), headers: {
      Accept: 'application/vnd.github+json',
    } });
    if (!response.ok) throw new Error(`Release check failed (${response.status})`);
    const releases = await response.json();
    if (!Array.isArray(releases)) throw new Error('Invalid release list');
    for (const release of releases) {
      if (release.draft) continue;
      const installer = (release.assets || []).find(asset => asset.state === 'uploaded'
        && /^SourceMovieMaker-Complete-Setup-.*\.exe$/i.test(asset.name));
      if (!installer) continue;
      const url = new URL(installer.browser_download_url);
      if (url.origin !== 'https://github.com'
        || !url.pathname.startsWith('/Wockstajit/SourceMVM/releases/download/')) continue;
      download.href = url.href;
      download.removeAttribute('aria-disabled');
      downloadVersion.textContent = release.name || release.tag_name;
      return;
    }
    downloadVersion.textContent = 'Installer being prepared';
  } catch {
    // The release page is a useful fallback if GitHub's public API is unavailable or rate limited.
    download.href = RELEASES_PAGE;
    download.removeAttribute('aria-disabled');
    downloadVersion.textContent = 'View downloads on GitHub';
  }
}
void refreshDownload();

// ---- Logo ----------------------------------------------------------------------------------------
// The intro plays once and the page holds its last frame. The still is the same frame with real
// alpha: it replaces the video at the end (so the hold survives the browser dropping decoded
// frames), and whenever the video should not or cannot play (returning visit, reduced motion,
// autoplay blocked, load error).
const video = document.querySelector('.logo__video');
const still = document.querySelector('.logo__still');

// The hero's corner readout counts the intro's frames (24 fps) and parks on the held one.
const FPS = 24;
const LAST_FRAME = 94;
const timecode = document.getElementById('timecode');
const frameLabel = document.getElementById('frame');

function showFrame(frame, held) {
  const f = Math.min(Math.max(frame, 0), LAST_FRAME);
  const pad = (n, width) => String(n).padStart(width, '0');
  timecode.textContent = `TC 00:00:${pad(Math.floor(f / FPS), 2)}:${pad(f % FPS, 2)}${held ? ' · HOLD' : ''}`;
  frameLabel.textContent = `FRAME ${pad(f, 3)} / ${pad(LAST_FRAME, 3)}`;
}

function trackFrames() {
  if (video.hidden) return;
  showFrame(Math.round(video.currentTime * FPS), false);
  requestAnimationFrame(trackFrames);
}

function holdStill() {
  still.hidden = false;
  video.hidden = true;
  video.pause();
  showFrame(LAST_FRAME, true);
}

const reducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
const returning = document.documentElement.getAttribute('data-visit') === 'returning';
if (returning || reducedMotion) {
  // Stop the autoplay and the download the markup already started.
  video.removeAttribute('autoplay');
  video.removeAttribute('src');
  video.load();
  holdStill();
} else {
  video.addEventListener('ended', holdStill);
  video.addEventListener('error', holdStill);
  video.addEventListener('playing', () => requestAnimationFrame(trackFrames), { once: true });
  startIntro();
}

function startIntro() {
  const playing = video.play();
  if (!playing) return;
  playing.catch(() => {
    // Chrome will not start silent video in a background tab (a link opened with a middle-click).
    // Play the intro when the tab is shown instead of skipping straight to the still.
    if (document.hidden) document.addEventListener('visibilitychange', startIntro, { once: true });
    else holdStill();
  });
}

// ---- "I need beta testers." types itself out -----------------------------------------------------
// Screen readers get the whole heading at once; the letters are visual only. They start hidden but
// keep their space, so the page never shifts while they appear.
const typed = document.querySelector('[data-typewriter]');
if (typed) {
  const text = typed.textContent.trim();
  const letters = [];
  const visual = document.createElement('span');
  visual.setAttribute('aria-hidden', 'true');
  text.split(' ').forEach((word, i) => {
    if (i) visual.append(' ');
    const wordSpan = document.createElement('span');
    wordSpan.className = 'tw-word';
    for (const ch of word) {
      const letter = document.createElement('span');
      letter.className = 'tw-ch';
      letter.textContent = ch;
      wordSpan.append(letter);
      letters.push(letter);
    }
    visual.append(wordSpan);
  });
  const spoken = document.createElement('span');
  spoken.className = 'sr-only';
  spoken.textContent = text;
  typed.replaceChildren(spoken, visual);

  let next = 0;
  const typeLetter = () => {
    letters[0].classList.remove('is-start');
    if (next > 0) letters[next - 1].classList.remove('is-caret');
    letters[next].classList.add('is-on', 'is-caret');
    next += 1;
    if (next === letters.length) {
      typed.classList.add('is-typed');
      return;
    }
    const newWord = !letters[next].previousElementSibling;
    setTimeout(typeLetter, 55 + Math.random() * 60 + (newWord ? 120 : 0));
  };

  if (reducedMotion) {
    letters.forEach((letter) => letter.classList.add('is-on'));
    letters[letters.length - 1].classList.add('is-caret');
    typed.classList.add('is-typed');
  } else {
    // Wait until the heading is on screen, blink the caret a couple of times, then type.
    const begin = () => {
      letters[0].classList.add('is-caret', 'is-start');
      setTimeout(typeLetter, 700);
    };
    if ('IntersectionObserver' in window) {
      const watch = new IntersectionObserver(([entry]) => {
        if (!entry.isIntersecting) return;
        watch.disconnect();
        begin();
      }, { threshold: 0.6 });
      watch.observe(typed);
    } else {
      begin();
    }
  }
}

// ---- REPORT A BUG: every click files another (pretend) bug ---------------------------------------
const bugButton = document.getElementById('bug-button');
const bugStatus = document.getElementById('bug-status');
let bugsReported = 0;

bugButton.addEventListener('click', () => {
  bugsReported += 1;
  const pop = document.createElement('span');
  pop.className = 'bug-pop';
  pop.setAttribute('aria-hidden', 'true');
  pop.textContent = `Bug #${bugsReported} reported`;
  pop.style.setProperty('--x', `${Math.round(Math.random() * 40 - 20)}px`);
  pop.addEventListener('animationend', () => pop.remove());
  bugButton.after(pop);
  bugStatus.textContent = `Bug ${bugsReported} reported`;
});
