// Standalone app-icon generator for the SOURCE:MVM launcher.
//
// Produces build-resources/icon.png (1024x1024) and build-resources/icon.ico (multi-size) from
// the native CS2 "video clip" film-strip glyph on the launcher's own near-black plate with its
// gold sprocket accent.
//
// Both outputs are needed. electron-builder can convert a PNG for the app exe, but NSIS takes
// installerIcon/uninstallerIcon/installerHeaderIcon straight through and rejects a PNG with
// "invalid icon file", so the .ico is what package.json actually points at.
//
// This is deliberately its own script with its own entry point: it does not run as part of the
// renderer build, the host build, or anything in the CS2/CMake tree.
//
//   npm run build:icon
//
// Electron is used purely as a headless rasteriser -- it is already a devDependency, so this
// adds no new tooling.

const { app, BrowserWindow, nativeImage } = require('electron');
const fs = require('node:fs');
const path = require('node:path');

const SIZE = 1024;
const OUTPUT_DIR = path.resolve(__dirname, '..', 'build-resources');
const OUTPUT = path.join(OUTPUT_DIR, 'icon.png');
const OUTPUT_ICO = path.join(OUTPUT_DIR, 'icon.ico');

// Windows picks the nearest of these per surface: 16/32 for the title bar and Explorer lists,
// 48 for the desktop, 256 for large-icon views and the installer header.
const ICO_SIZES = [16, 24, 32, 48, 64, 128, 256];

// Palette lifted from src/styles.css so the icon and the launcher chrome cannot drift apart.
const BG = '#0c0e12';
const PLATE_TOP = '#1a1f26';
const PLATE_BOTTOM = '#0f1319';
const GOLD = '#f0b323';
const INK = '#eef2f6';

// The film-strip body is drawn as primitives rather than pasting the 32x25 native path, because
// the native glyph is tuned for a 32px HUD icon: at 1024px its 1px sprocket gutters vanish and
// its aspect ratio leaves the taskbar icon looking like a letterboxed bar. Same mark, redrawn at
// icon scale -- strip, sprocket columns, centred play triangle.
function filmStrip() {
  const stripX = 152;
  const stripY = 268;
  const stripW = SIZE - stripX * 2;
  const stripH = SIZE - stripY * 2;
  const railW = 116;
  const holeW = 74;
  const holeH = 74;
  const holeGap = 24;
  const holeCount = 5;

  const holes = [];
  const columnH = holeCount * holeH + (holeCount - 1) * holeGap;
  const firstHoleY = stripY + (stripH - columnH) / 2;
  for (let index = 0; index < holeCount; index += 1) {
    const y = firstHoleY + index * (holeH + holeGap);
    const leftX = stripX + (railW - holeW) / 2;
    const rightX = stripX + stripW - railW + (railW - holeW) / 2;
    holes.push(`<rect x="${leftX}" y="${y}" width="${holeW}" height="${holeH}" rx="10" />`);
    holes.push(`<rect x="${rightX}" y="${y}" width="${holeW}" height="${holeH}" rx="10" />`);
  }

  const centreX = stripX + stripW / 2;
  const centreY = stripY + stripH / 2;
  const triangleW = 150;
  const triangleH = 172;

  return `
    <rect x="${stripX}" y="${stripY}" width="${stripW}" height="${stripH}" rx="26" fill="${INK}" />
    <g fill="${BG}">${holes.join('')}</g>
    <path
      fill="${BG}"
      d="M ${centreX - triangleW / 2} ${centreY - triangleH / 2}
         L ${centreX + triangleW / 2} ${centreY}
         L ${centreX - triangleW / 2} ${centreY + triangleH / 2} Z" />
    <rect x="${stripX}" y="${stripY}" width="${stripW}" height="${stripH}" rx="26"
          fill="none" stroke="${GOLD}" stroke-width="10" />
  `;
}

function iconMarkup() {
  return `<!doctype html>
<html>
  <head>
    <meta charset="utf-8" />
    <style>
      html, body { margin: 0; padding: 0; background: transparent; }
      svg { display: block; }
    </style>
  </head>
  <body>
    <svg xmlns="http://www.w3.org/2000/svg" width="${SIZE}" height="${SIZE}" viewBox="0 0 ${SIZE} ${SIZE}">
      <defs>
        <linearGradient id="plate" x1="0" y1="0" x2="0" y2="1">
          <stop offset="0" stop-color="${PLATE_TOP}" />
          <stop offset="1" stop-color="${PLATE_BOTTOM}" />
        </linearGradient>
      </defs>
      <rect x="40" y="40" width="${SIZE - 80}" height="${SIZE - 80}" rx="180" fill="url(#plate)" />
      <rect x="40" y="40" width="${SIZE - 80}" height="${SIZE - 80}" rx="180"
            fill="none" stroke="${GOLD}" stroke-width="16" opacity="0.85" />
      ${filmStrip()}
    </svg>
  </body>
</html>`;
}

// ---- ICO encoding ----------------------------------------------------------------------------
//
// Entries are written as classic 32-bit DIBs rather than embedded PNGs. Both are legal in an
// .ico, but NSIS 3's icon loader is unhappy with PNG-compressed entries, and NSIS is the whole
// reason this file exists.

/** One BITMAPINFOHEADER + bottom-up BGRA bitmap + (unused, but mandatory) 1bpp AND mask. */
function encodeDib(bgra, size) {
  const header = Buffer.alloc(40);
  header.writeUInt32LE(40, 0); // biSize
  header.writeInt32LE(size, 4); // biWidth
  // biHeight is doubled: the ICO DIB stores the colour bitmap and the AND mask stacked.
  header.writeInt32LE(size * 2, 8);
  header.writeUInt16LE(1, 12); // biPlanes
  header.writeUInt16LE(32, 14); // biBitCount

  // Rows run bottom-up in a DIB, so the source is flipped here rather than at capture time.
  const pixels = Buffer.alloc(size * size * 4);
  for (let y = 0; y < size; y += 1) {
    const source = (size - 1 - y) * size * 4;
    bgra.copy(pixels, y * size * 4, source, source + size * 4);
  }

  // 1 bit per pixel, each row padded to a 4-byte boundary. Left zeroed: with a 32bpp entry
  // Windows uses the alpha channel, but the mask must still be present and correctly sized.
  const maskStride = Math.ceil(size / 32) * 4;
  const mask = Buffer.alloc(maskStride * size);

  header.writeUInt32LE(pixels.length + mask.length, 20); // biSizeImage
  return Buffer.concat([header, pixels, mask]);
}

function encodeIco(image) {
  const entries = ICO_SIZES.map((size) => ({
    size,
    data: encodeDib(image.resize({ width: size, height: size, quality: 'best' }).toBitmap(), size),
  }));

  const directory = Buffer.alloc(6 + entries.length * 16);
  directory.writeUInt16LE(0, 0); // reserved
  directory.writeUInt16LE(1, 2); // type: icon
  directory.writeUInt16LE(entries.length, 4);

  let offset = directory.length;
  entries.forEach((entry, index) => {
    const at = 6 + index * 16;
    // 256 is stored as 0 -- the field is a single byte.
    directory.writeUInt8(entry.size === 256 ? 0 : entry.size, at);
    directory.writeUInt8(entry.size === 256 ? 0 : entry.size, at + 1);
    directory.writeUInt8(0, at + 2); // palette size: none
    directory.writeUInt8(0, at + 3); // reserved
    directory.writeUInt16LE(1, at + 4); // colour planes
    directory.writeUInt16LE(32, at + 6); // bits per pixel
    directory.writeUInt32LE(entry.data.length, at + 8);
    directory.writeUInt32LE(offset, at + 12);
    offset += entry.data.length;
  });

  return Buffer.concat([directory, ...entries.map((entry) => entry.data)]);
}

app.disableHardwareAcceleration();

app.whenReady().then(async () => {
  const window = new BrowserWindow({
    width: SIZE,
    height: SIZE,
    show: false,
    frame: false,
    transparent: true,
    backgroundColor: '#00000000',
    webPreferences: { offscreen: true, sandbox: true },
  });

  await window.loadURL(`data:text/html;charset=utf-8,${encodeURIComponent(iconMarkup())}`);
  // One frame of settle time: offscreen rendering can capture before the first paint lands.
  await new Promise((resolve) => setTimeout(resolve, 400));

  const image = await window.webContents.capturePage({ x: 0, y: 0, width: SIZE, height: SIZE });
  if (image.isEmpty()) {
    process.stderr.write('Icon capture produced an empty image.\n');
    app.exit(1);
    return;
  }

  fs.mkdirSync(OUTPUT_DIR, { recursive: true });
  fs.writeFileSync(OUTPUT, image.toPNG());
  process.stdout.write(`Wrote ${OUTPUT} (${SIZE}x${SIZE})\n`);

  const ico = encodeIco(nativeImage.createFromBuffer(image.toPNG()));
  fs.writeFileSync(OUTPUT_ICO, ico);
  process.stdout.write(`Wrote ${OUTPUT_ICO} (${ICO_SIZES.join(', ')})\n`);
  app.exit(0);
});
