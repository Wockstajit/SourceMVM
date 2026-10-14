const API = 'https://api.github.com/repos/Wockstajit/SourceMVM/releases';

export function countReleaseDownloads(releases) {
  if (!Array.isArray(releases)) throw new Error('Invalid release list');
  let total = 0;
  for (const release of releases) {
    if (release.draft) continue;
    for (const asset of release.assets || []) {
      if (asset.state !== 'uploaded' || !/\.(exe|zip)$/i.test(asset.name || '')) continue;
      if (!Number.isSafeInteger(asset.download_count) || asset.download_count < 0) {
        throw new Error('Invalid download count');
      }
      total += asset.download_count;
      if (!Number.isSafeInteger(total)) throw new Error('Download count overflow');
    }
  }
  return total;
}

export async function fetchTotalDownloads(fetchPage = fetch) {
  let next = `${API}?per_page=100`;
  let total = 0;
  const visited = new Set();
  while (next) {
    const url = new URL(next);
    if (url.origin !== 'https://api.github.com' || url.pathname !== new URL(API).pathname
      || visited.has(url.href)) throw new Error('Invalid release pagination');
    visited.add(url.href);
    const response = await fetchPage(url.href, {
      headers: { Accept: 'application/vnd.github+json' },
      signal: AbortSignal.timeout(15000),
    });
    if (!response.ok) throw new Error(`Download count unavailable (${response.status})`);
    total += countReleaseDownloads(await response.json());
    if (!Number.isSafeInteger(total)) throw new Error('Download count overflow');
    next = response.headers.get('link')?.match(/<([^>]+)>;\s*rel="next"/)?.[1] || '';
  }
  return total;
}

if (typeof document !== 'undefined') {
  const counter = document.getElementById('download-total');
  if (counter) {
    fetchTotalDownloads().then(total => {
      counter.textContent = `${total.toLocaleString()} total downloads`;
      counter.title = `Installer and hotfix downloads. Last checked ${new Date().toLocaleDateString()}.`;
    }).catch(() => { /* Keep the last complete total instead of showing a partial count. */ });
  }
}
