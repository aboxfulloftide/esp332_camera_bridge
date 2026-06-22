'use strict';

// --- API helpers ---
async function apiFetch(method, url, body = null) {
  const opts = { method, headers: {} };
  if (body !== null) {
    opts.headers['Content-Type'] = 'application/json';
    opts.body = JSON.stringify(body);
  }
  const res = await fetch(url, opts);
  const ct = res.headers.get('content-type') || '';
  if (!ct.includes('json')) throw new Error(`HTTP ${res.status}`);
  return res.json();
}

const API = {
  get:  (url)       => apiFetch('GET',    url),
  post: (url, body) => apiFetch('POST',   url, body ?? {}),
  del:  (url)       => apiFetch('DELETE', url),
};

// --- Theme ---
const html = document.documentElement;
let dark = localStorage.getItem('hl-theme') !== 'light';

function applyTheme() {
  html.setAttribute('data-theme', dark ? '' : 'light');
  document.getElementById('theme-btn').textContent = dark ? '☀ Light' : '🌙 Dark';
}

applyTheme();
document.getElementById('theme-btn').addEventListener('click', () => {
  dark = !dark;
  localStorage.setItem('hl-theme', dark ? 'dark' : 'light');
  applyTheme();
});

// --- UI helpers ---
function setLog(el, text, type = '') {
  if (!el) return;
  el.textContent = text;
  el.className = 'log-line' + (type ? ' ' + type : '');
}

function setBusy(btn, busy, label = null) {
  if (!btn) return;
  if (busy) {
    btn.dataset.origText = btn.textContent;
    btn.disabled = true;
    btn.innerHTML = '<span class="spinner"></span> ' + (label || 'Working…');
  } else {
    btn.disabled = false;
    btn.textContent = btn.dataset.origText || btn.textContent;
  }
}

function badge(val, trueLabel, falseLabel) {
  const cls  = val ? 'badge-ok' : 'badge-err';
  const text = val ? trueLabel  : falseLabel;
  return `<span class="badge ${cls}">${text}</span>`;
}

function formatBytes(n) {
  if (!n) return '—';
  if (n < 1024)         return n + ' B';
  if (n < 1024 * 1024)  return (n / 1024).toFixed(1) + ' KB';
  return (n / 1024 / 1024).toFixed(1) + ' MB';
}

// --- Header status pill ---
const statusPill = document.getElementById('status-pill');

function updateHeaderStatus(data) {
  const conn = data?.camera_reachable;
  statusPill.className   = 'status-pill ' + (conn ? 'connected' : 'disconnected');
  statusPill.innerHTML   =
    `<span class="status-dot"></span>${conn ? 'connected' : 'offline'}`;
}

// --- Status grid ---
const statusGrid = document.getElementById('status-grid');

function renderStatusGrid(data) {
  if (!data || !data.wifi_connected && !data.halow_connected) {
    statusGrid.innerHTML =
      `<span class="status-label">bridge</span>` +
      `<span class="status-value">${badge(false, 'ok', 'unreachable')}</span>`;
    return;
  }
  statusGrid.innerHTML = [
    ['WiFi',     badge(data.wifi_connected, 'connected', 'offline')],
    ['HaLow',    badge(data.halow_connected, 'connected', 'offline')],
    ['Camera IP',data.camera_ip  || '—'],
    ['BLE Stage',data.ble_stage  || '—'],
    ['Tunnel',   badge(data.tunnel_connected, 'up', 'down')],
    ['Live View',badge(data.live_view_active, 'active', 'inactive')],
    ['Standby',  data.standby_requested
      ? '<span class="badge badge-warn">requested</span>'
      : '<span class="badge badge-muted">no</span>'],
    ['Last Msg', `<span style="color:var(--hl-text-muted)">${data.control_last_message || '—'}</span>`],
  ].map(([k, v]) =>
    `<span class="status-label">${k}</span><span class="status-value">${v}</span>`
  ).join('');
}

// --- Status polling ---
async function refreshStatus() {
  try {
    const res = await API.get('/api/status');
    if (res.ok) {
      updateHeaderStatus(res.data);
      renderStatusGrid(res.data);
      updateLiveUI(res.data.live_view_active);
    } else {
      updateHeaderStatus(null);
    }
  } catch {
    updateHeaderStatus(null);
  }
}

refreshStatus();
setInterval(refreshStatus, 5000);

// --- Connection ---
const connectBtn    = document.getElementById('connect-btn');
const disconnectBtn = document.getElementById('disconnect-btn');
const connectionLog = document.getElementById('connection-log');

connectBtn.addEventListener('click', async () => {
  setBusy(connectBtn, true, 'Connecting…');
  setLog(connectionLog, 'Opening session…');
  try {
    const res = await API.post('/api/session/open');
    if (res.ok) {
      setLog(connectionLog, `session_ready: ${res.data.session_ready}`, 'ok');
      refreshStatus();
    } else {
      setLog(connectionLog, res.error?.message || 'Failed', 'err');
    }
  } catch (e) {
    setLog(connectionLog, e.message, 'err');
  } finally {
    setBusy(connectBtn, false);
  }
});

disconnectBtn.addEventListener('click', async () => {
  setBusy(disconnectBtn, true, 'Disconnecting…');
  setLog(connectionLog, 'Closing session…');
  try {
    const res = await API.post('/api/session/close');
    if (res.ok) {
      setLog(connectionLog, `session_closed: ${res.data.session_closed}`, 'ok');
      refreshStatus();
    } else {
      setLog(connectionLog, res.error?.message || 'Failed', 'err');
    }
  } catch (e) {
    setLog(connectionLog, e.message, 'err');
  } finally {
    setBusy(disconnectBtn, false);
  }
});

// --- Actions ---
const takePicBtn  = document.getElementById('take-picture-btn');
const vidStartBtn = document.getElementById('video-start-btn');
const vidStopBtn  = document.getElementById('video-stop-btn');
const actionsLog  = document.getElementById('actions-log');

let videoRecording = false;

function setVideoUI(recording) {
  videoRecording = recording;
  vidStartBtn.disabled = recording;
  vidStopBtn.disabled  = !recording;
  vidStartBtn.className = recording ? 'btn btn-warning' : 'btn btn-warning';
  vidStopBtn.className  = recording ? 'btn btn-danger'  : 'btn btn-secondary';
}

takePicBtn.addEventListener('click', async () => {
  setBusy(takePicBtn, true, 'Capturing…');
  setLog(actionsLog, 'Taking photo…');
  try {
    const res = await API.post('/api/actions/take-picture');
    if (res.ok) {
      const d = res.data;
      const id = d.media?.id ?? '?';
      setLog(actionsLog,
        `captured: ${d.captured} · id: ${id} · ${d.elapsed_sec?.toFixed(1)}s`, 'ok');
      if (d.captured) refreshMedia();
    } else {
      setLog(actionsLog, res.error?.message || 'Failed', 'err');
    }
  } catch (e) {
    setLog(actionsLog, e.message, 'err');
  } finally {
    setBusy(takePicBtn, false);
  }
});

vidStartBtn.addEventListener('click', async () => {
  setBusy(vidStartBtn, true, 'Starting…');
  setLog(actionsLog, 'Starting video recording…');
  try {
    const res = await API.post('/api/actions/video/start');
    if (res.ok) {
      setVideoUI(true);
      setLog(actionsLog, `started: ${res.data.started}`, 'ok');
    } else {
      setLog(actionsLog, res.error?.message || 'Failed', 'err');
    }
  } catch (e) {
    setLog(actionsLog, e.message, 'err');
  } finally {
    setBusy(vidStartBtn, false);
  }
});

vidStopBtn.addEventListener('click', async () => {
  setBusy(vidStopBtn, true, 'Stopping…');
  setLog(actionsLog, 'Stopping video recording…');
  try {
    const res = await API.post('/api/actions/video/stop');
    if (res.ok) {
      setVideoUI(false);
      const d = res.data;
      setLog(actionsLog,
        `stopped: ${d.stopped} · new_video: ${d.new_video_observed} · ${d.elapsed_sec?.toFixed(1)}s`,
        'ok');
      if (d.new_video_observed) refreshMedia();
    } else {
      setLog(actionsLog, res.error?.message || 'Failed', 'err');
    }
  } catch (e) {
    setLog(actionsLog, e.message, 'err');
  } finally {
    setBusy(vidStopBtn, false);
  }
});

// --- Live view ---
const liveStartBtn   = document.getElementById('live-start-btn');
const liveStopBtn    = document.getElementById('live-stop-btn');
const livePlaceholder = document.getElementById('live-placeholder');
const liveLog        = document.getElementById('live-log');

function updateLiveUI(active) {
  liveStartBtn.disabled = active;
  liveStopBtn.disabled  = !active;
  livePlaceholder.textContent = active
    ? '▶ Stream active — connect a player to the bridge SDP endpoint'
    : 'Stream inactive';
  livePlaceholder.className = 'live-placeholder' + (active ? ' active' : '');
}

liveStartBtn.addEventListener('click', async () => {
  setBusy(liveStartBtn, true, 'Starting…');
  setLog(liveLog, 'Starting stream…');
  try {
    const res = await API.post('/api/live/start');
    if (res.ok) {
      updateLiveUI(res.data.stream_active);
      setLog(liveLog,
        `stream_active: ${res.data.stream_active} · tunnel: ${res.data.tunnel_connected}`, 'ok');
    } else {
      setLog(liveLog, res.error?.message || 'Failed', 'err');
    }
  } catch (e) {
    setLog(liveLog, e.message, 'err');
  } finally {
    setBusy(liveStartBtn, false);
  }
});

liveStopBtn.addEventListener('click', async () => {
  setBusy(liveStopBtn, true, 'Stopping…');
  setLog(liveLog, 'Stopping stream…');
  try {
    const res = await API.post('/api/live/stop');
    if (res.ok) {
      updateLiveUI(false);
      setLog(liveLog, `stopped: ${res.data.stopped}`, 'ok');
    } else {
      setLog(liveLog, res.error?.message || 'Failed', 'err');
    }
  } catch (e) {
    setLog(liveLog, e.message, 'err');
  } finally {
    setBusy(liveStopBtn, false);
  }
});

// --- Settings ---
const loadSettingsBtn = document.getElementById('load-settings-btn');
const saveSettingsBtn = document.getElementById('save-settings-btn');
const settingsFields  = document.getElementById('settings-fields');
const settingsLog     = document.getElementById('settings-log');

let schema = [];
let currentSettings = {};

async function loadSchema() {
  try {
    const res = await API.get('/api/settings/schema');
    if (res.ok) schema = res.data.fields || [];
  } catch { /* non-fatal */ }
}

function buildSettingsForm() {
  if (!schema.length) {
    settingsFields.innerHTML =
      '<p style="color:var(--hl-text-faint);font-size:12px;margin:0">Click Load to read settings from camera.</p>';
    return;
  }
  settingsFields.innerHTML = schema.map(field => {
    const val = currentSettings[field.key] ?? '';
    let input;

    if ((field.type === 'select' || field.type === 'boolean') && field.options) {
      const opts = field.options
        .map(o => `<option value="${o.value}"${String(o.value) === String(val) ? ' selected' : ''}>${o.label}</option>`)
        .join('');
      input = `<select class="field-input" name="${field.key}">${opts}</select>`;
    } else if (field.type === 'boolean') {
      input = `<select class="field-input" name="${field.key}">
        <option value="1"${val == 1 ? ' selected' : ''}>Yes</option>
        <option value="0"${val != 1 ? ' selected' : ''}>No</option>
      </select>`;
    } else {
      input = `<input class="field-input" type="text" name="${field.key}" value="${String(val).replace(/"/g, '&quot;')}">`;
    }

    return `<div class="field-row">
      <label class="field-label">${field.label}</label>
      ${input}
    </div>`;
  }).join('');
}

loadSettingsBtn.addEventListener('click', async () => {
  setBusy(loadSettingsBtn, true, 'Loading…');
  setLog(settingsLog, 'Reading settings from camera…');
  try {
    const res = await API.get('/api/settings');
    if (res.ok) {
      currentSettings = res.data.settings || {};
      buildSettingsForm();
      setLog(settingsLog, 'Settings loaded', 'ok');
    } else {
      setLog(settingsLog, res.error?.message || 'Failed', 'err');
    }
  } catch (e) {
    setLog(settingsLog, e.message, 'err');
  } finally {
    setBusy(loadSettingsBtn, false);
  }
});

saveSettingsBtn.addEventListener('click', async () => {
  const patch = {};
  settingsFields.querySelectorAll('[name]').forEach(el => {
    const field = schema.find(f => f.key === el.name);
    if (!field) return;
    const raw = el.value;
    if (field.type === 'integer') patch[el.name] = parseInt(raw, 10);
    else if (field.type === 'boolean') patch[el.name] = parseInt(raw, 10);
    else if (field.type === 'select') patch[el.name] = isNaN(Number(raw)) ? raw : Number(raw);
    else patch[el.name] = raw;
  });
  if (!Object.keys(patch).length) {
    setLog(settingsLog, 'No fields found — load settings first', '');
    return;
  }
  setBusy(saveSettingsBtn, true, 'Saving…');
  setLog(settingsLog, 'Applying patch…');
  try {
    const res = await API.post('/api/settings', { patch });
    if (res.ok) {
      const changed   = Object.keys(res.data.changed   || {});
      const unchanged = Object.keys(res.data.unchanged || {});
      const parts = [];
      if (changed.length)   parts.push(`changed: [${changed.join(', ')}]`);
      if (unchanged.length) parts.push(`unchanged: [${unchanged.join(', ')}]`);
      setLog(settingsLog, parts.join(' · ') || 'No changes', 'ok');
    } else {
      setLog(settingsLog, res.error?.message || 'Failed', 'err');
    }
  } catch (e) {
    setLog(settingsLog, e.message, 'err');
  } finally {
    setBusy(saveSettingsBtn, false);
  }
});

// --- Media gallery ---
const refreshMediaBtn = document.getElementById('refresh-media-btn');
const galleryBody     = document.getElementById('gallery-body');
const mediaLog        = document.getElementById('media-log');

function renderGallery(items) {
  if (!items || !items.length) {
    galleryBody.innerHTML = '<tr><td colspan="5" class="empty-state">No media found</td></tr>';
    return;
  }

  galleryBody.innerHTML = items.map(item => {
    const typeBadge = item.type === 'photo'
      ? '<span class="badge badge-info">photo</span>'
      : '<span class="badge badge-warn">video</span>';
    const statusBadge = item.status === 'pending'
      ? ' <span class="badge badge-warn">pending</span>'
      : '';

    return `<tr>
      <td>${item.id}</td>
      <td>${typeBadge}${statusBadge}</td>
      <td>${item.timestamp || '—'}</td>
      <td class="size-text">${formatBytes(item.size_bytes)}</td>
      <td class="col-actions">
        <a class="btn btn-sm btn-secondary"
           href="${item.thumbnail_path}" target="_blank"
           title="Open thumbnail in new tab">thumb</a>
        <a class="btn btn-sm btn-primary"
           href="${item.download_path}"
           title="Download file (may take a while for large videos)">↓ dl</a>
      </td>
    </tr>`;
  }).join('');
}

async function refreshMedia() {
  setBusy(refreshMediaBtn, true, 'Loading…');
  setLog(mediaLog, 'Fetching gallery…');
  try {
    const res = await API.get('/api/media');
    if (res.ok) {
      renderGallery(res.data.items);
      setLog(mediaLog, `${res.data.items.length} item(s) loaded`, 'ok');
    } else {
      setLog(mediaLog, res.error?.message || 'Failed', 'err');
    }
  } catch (e) {
    setLog(mediaLog, e.message, 'err');
  } finally {
    setBusy(refreshMediaBtn, false);
  }
}

refreshMediaBtn.addEventListener('click', refreshMedia);

// --- Init ---
loadSchema();
setVideoUI(false);
updateLiveUI(false);
