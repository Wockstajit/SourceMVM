import { mkdir, writeFile } from 'node:fs/promises';
import path from 'node:path';

const port = Number(process.argv[2] || 9333);
const outputDirectory = path.resolve(process.argv[3] || 'Automation/runs/launcher-advanced');
const tabs = ['GAME', 'RUNTIME', 'LOCATIONS'];

async function waitForPage() {
  for (let attempt = 0; attempt < 40; attempt += 1) {
    try {
      const response = await fetch(`http://127.0.0.1:${port}/json`);
      const pages = await response.json();
      const page = pages.find((entry) => entry.type === 'page' && entry.webSocketDebuggerUrl);
      if (page) return page;
    } catch {
      // The launcher is still starting.
    }
    await new Promise((resolve) => setTimeout(resolve, 250));
  }
  throw new Error(`Launcher debugging endpoint did not appear on port ${port}.`);
}

function createClient(url) {
  const socket = new WebSocket(url);
  let nextId = 1;
  const pending = new Map();

  socket.addEventListener('message', (event) => {
    const message = JSON.parse(event.data);
    if (!message.id || !pending.has(message.id)) return;
    const { resolve, reject } = pending.get(message.id);
    pending.delete(message.id);
    if (message.error) reject(new Error(message.error.message));
    else resolve(message.result);
  });

  const ready = new Promise((resolve, reject) => {
    socket.addEventListener('open', resolve, { once: true });
    socket.addEventListener('error', reject, { once: true });
  });

  return {
    ready,
    close: () => socket.close(),
    send(method, params = {}) {
      const id = nextId;
      nextId += 1;
      return new Promise((resolve, reject) => {
        pending.set(id, { resolve, reject });
        socket.send(JSON.stringify({ id, method, params }));
      });
    },
  };
}

const page = await waitForPage();
const client = createClient(page.webSocketDebuggerUrl);
await client.ready;
await mkdir(outputDirectory, { recursive: true });

await client.send('Runtime.evaluate', {
  expression: `Array.from(document.querySelectorAll('button')).find((button) => button.textContent.includes('ADVANCED'))?.click()`,
});
await new Promise((resolve) => setTimeout(resolve, 500));

for (const tab of tabs) {
  await client.send('Runtime.evaluate', {
    expression: `Array.from(document.querySelectorAll('[role="tab"]')).find((button) => button.textContent.trim() === ${JSON.stringify(tab)})?.click()`,
  });
  await new Promise((resolve) => setTimeout(resolve, 150));
  const screenshot = await client.send('Page.captureScreenshot', { format: 'png', fromSurface: true });
  const filename = `${tab.toLowerCase()}.png`;
  await writeFile(path.join(outputDirectory, filename), Buffer.from(screenshot.data, 'base64'));
  console.log(path.join(outputDirectory, filename));
}

client.close();
