/* ── ESP32 Boot Manager — Web UI ────────────────────────────────────────────
 * All fetch calls go to relative paths so the page works from any IP.
 * ────────────────────────────────────────────────────────────────────────── */

'use strict';

/* ── Utilities ────────────────────────────────────────────────────────────── */

let toastTimer = null;

function showToast(msg, type = 'info', duration = 3500) {
  const t = document.getElementById('toast');
  t.textContent = msg;
  t.className = `toast visible ${type}`;
  if (toastTimer) clearTimeout(toastTimer);
  toastTimer = setTimeout(() => {
    t.classList.remove('visible');
  }, duration);
}

function showMsg(id, msg, type) {
  const el = document.getElementById(id);
  if (!el) return;
  el.textContent = msg;
  el.className = `msg ${type}`;
  el.classList.remove('hidden');
}

function hideMsg(id) {
  const el = document.getElementById(id);
  if (el) el.classList.add('hidden');
}

function setProgress(fillId, labelId, percent, label) {
  const fill  = document.getElementById(fillId);
  const lbl   = document.getElementById(labelId);
  if (fill) fill.style.width = `${Math.min(100, Math.max(0, percent))}%`;
  if (lbl)  lbl.textContent = label || `${Math.round(percent)}%`;
}

function fmtBytes(n) {
  if (n >= 1024 * 1024) return (n / 1024 / 1024).toFixed(1) + ' MB';
  if (n >= 1024)        return (n / 1024).toFixed(1) + ' KB';
  return n + ' B';
}

function fmtUptime(ms) {
  const s = Math.floor(ms / 1000);
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  return `${String(h).padStart(2,'0')}:${String(m).padStart(2,'0')}:${String(sec).padStart(2,'0')}`;
}

async function postJSON(url, body) {
  const resp = await fetch(url, {
    method:  'POST',
    headers: { 'Content-Type': 'application/json' },
    body:    JSON.stringify(body),
  });
  return resp.json();
}

/* ── Tab navigation ───────────────────────────────────────────────────────── */

document.querySelectorAll('.tab').forEach(btn => {
  btn.addEventListener('click', () => {
    document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
    document.querySelectorAll('.tab-content').forEach(t => t.classList.remove('active'));
    btn.classList.add('active');
    const target = document.getElementById('tab-' + btn.dataset.tab);
    if (target) target.classList.add('active');
  });
});

/* ── Status polling ───────────────────────────────────────────────────────── */

async function fetchStatus() {
  try {
    const r = await fetch('/api/status');
    if (!r.ok) return;
    const d = await r.json();
    applyStatus(d);
  } catch (e) {
    document.getElementById('sta-dot').className = 'status-dot failed';
  }
}

function applyStatus(d) {
  /* Header */
  const dot = document.getElementById('sta-dot');
  dot.className = 'status-dot ' + (d.sta_state || 'disconnected');
  dot.title = `STA: ${d.sta_state}${d.sta_ip ? ' · ' + d.sta_ip : ''}`;

  /* Dashboard — firmware */
  setText('fw-version', d.version);
  setText('fw-project', d.project_name);
  setText('fw-idf',     d.idf_version);
  setText('fw-built',   d.compile_date + ' ' + d.compile_time);

  /* Dashboard — partitions */
  setText('dash-running', d.running_partition);
  setText('dash-next',    d.next_partition);
  setText('dash-next-valid', d.next_partition_valid ? '✅ Yes' : '❌ No');
  setText('dash-fails',   d.fail_count);

  /* Dashboard — network */
  setText('net-sta',       d.sta_state);
  setText('net-sta-ip',    d.sta_ip   || '—');
  setText('net-ap-ip',     d.ap_ip    || '—');
  setText('net-ap-clients',d.ap_clients);

  /* Dashboard — system */
  setText('sys-heap',     fmtBytes(d.free_heap));
  setText('sys-min-heap', fmtBytes(d.min_heap));
  setText('sys-uptime',   fmtUptime(d.uptime_ms));
  setText('uptime-display', fmtUptime(d.uptime_ms));

  /* Partition tab */
  setText('part-running', d.running_partition);
  setText('part-next',    d.next_partition);
  setText('part-next-valid', d.next_partition_valid ? '✅ Yes' : '❌ No');

  const sw = document.getElementById('switch-warning');
  if (sw) sw.style.display = d.next_partition_valid ? 'none' : 'block';

  const btnSwitch = document.getElementById('btn-switch-partition');
  if (btnSwitch) btnSwitch.disabled = !d.next_partition_valid;

  /* Partition visual */
  updatePartitionVisual(d.running_partition, d.next_partition);
}

function updatePartitionVisual(running, next) {
  const ota0 = document.getElementById('part-ota0');
  const ota1 = document.getElementById('part-ota1');
  const ota0s = document.getElementById('part-ota0-state');
  const ota1s = document.getElementById('part-ota1-state');
  if (!ota0 || !ota1) return;

  ota0.className = 'part-slot';
  ota1.className = 'part-slot';

  if (running === 'ota_0') {
    ota0.classList.add('active');
    ota1.classList.add('standby');
    if (ota0s) ota0s.textContent = '▶ Running';
    if (ota1s) ota1s.textContent = 'Standby';
  } else if (running === 'ota_1') {
    ota1.classList.add('active');
    ota0.classList.add('standby');
    if (ota1s) ota1s.textContent = '▶ Running';
    if (ota0s) ota0s.textContent = 'Standby';
  } else {
    if (ota0s) ota0s.textContent = '—';
    if (ota1s) ota1s.textContent = '—';
  }
}

function setText(id, val) {
  const el = document.getElementById(id);
  if (el) el.textContent = (val !== undefined && val !== null) ? String(val) : '—';
}

/* Poll every 5 seconds */
fetchStatus();
setInterval(fetchStatus, 5000);

/* ── WiFi status tab ──────────────────────────────────────────────────────── */

async function fetchWifiStatus() {
  try {
    const r = await fetch('/api/wifi/status');
    if (!r.ok) return;
    const d = await r.json();
    setText('wifi-sta-status', d.sta_state);
    setText('wifi-sta-ip',     d.sta_ip   || '—');
    setText('wifi-ap-ip',      d.ap_ip    || '192.168.4.1');
    setText('wifi-saved-ssid', d.saved_ssid || '(none)');
  } catch(e) {}
}
fetchWifiStatus();

/* ── Reboot ───────────────────────────────────────────────────────────────── */

document.getElementById('btn-reboot').addEventListener('click', async () => {
  if (!confirm('Reboot the ESP32 now?')) return;
  try {
    await postJSON('/api/reboot', {});
    showToast('Rebooting… page will reload in 10s', 'info', 10000);
    setTimeout(() => location.reload(), 10000);
  } catch(e) {
    showToast('Reboot request sent', 'info');
  }
});

/* ── OTA from URL ─────────────────────────────────────────────────────────── */

document.getElementById('btn-ota-url').addEventListener('click', async () => {
  const url = document.getElementById('ota-url').value.trim();
  if (!url) { showToast('Enter a firmware URL', 'error'); return; }

  if (!url.startsWith('http://') && !url.startsWith('https://')) {
    showToast('URL must start with http:// or https://', 'error');
    return;
  }

  const btn   = document.getElementById('btn-ota-url');
  const wrap  = document.getElementById('url-progress-wrap');
  const fill  = document.getElementById('url-progress-fill');
  const label = document.getElementById('url-progress-label');

  btn.disabled = true;
  wrap.classList.remove('hidden');
  fill.style.width = '5%';
  label.textContent = 'Sending request…';

  try {
    const resp = await postJSON('/api/ota/url', { url });
    if (resp.status === 'started') {
      label.textContent = 'Flashing firmware… device will reboot automatically.';
      fill.style.width  = '100%';
      showToast('OTA started. ESP32 will reboot when done.', 'success', 10000);
      /* Reload page after estimated flash time */
      setTimeout(() => location.reload(), 60000);
    } else {
      fill.style.width = '0%';
      label.textContent = resp.message || 'Error';
      showToast(resp.message || 'OTA failed', 'error');
      btn.disabled = false;
    }
  } catch(e) {
    fill.style.width = '0%';
    label.textContent = 'Connection error';
    showToast('Connection error: ' + e.message, 'error');
    btn.disabled = false;
  }
});

/* ── OTA file upload ──────────────────────────────────────────────────────── */

let selectedFile = null;
const dropZone   = document.getElementById('drop-zone');
const fileInput  = document.getElementById('file-input');
const btnUpload  = document.getElementById('btn-upload');
const dropName   = document.getElementById('drop-filename');

function selectFile(file) {
  if (!file) return;
  if (!file.name.endsWith('.bin')) {
    showToast('Please select a .bin firmware file', 'error');
    return;
  }
  selectedFile = file;
  dropName.textContent = `${file.name} (${fmtBytes(file.size)})`;
  btnUpload.disabled = false;
}

dropZone.addEventListener('click', (e) => {
  if (e.target.classList.contains('link') || e.target === dropZone ||
      e.target.classList.contains('drop-text') ||
      e.target.classList.contains('drop-icon')) {
    fileInput.click();
  }
});

fileInput.addEventListener('change', () => {
  if (fileInput.files.length > 0) selectFile(fileInput.files[0]);
});

dropZone.addEventListener('dragover', (e) => {
  e.preventDefault();
  dropZone.classList.add('drag-over');
});
dropZone.addEventListener('dragleave', () => dropZone.classList.remove('drag-over'));
dropZone.addEventListener('drop', (e) => {
  e.preventDefault();
  dropZone.classList.remove('drag-over');
  if (e.dataTransfer.files.length > 0) selectFile(e.dataTransfer.files[0]);
});

btnUpload.addEventListener('click', async () => {
  if (!selectedFile) { showToast('No file selected', 'error'); return; }

  btnUpload.disabled = true;
  const wrap  = document.getElementById('upload-progress-wrap');
  const fill  = document.getElementById('upload-progress-fill');
  const label = document.getElementById('upload-progress-label');

  wrap.classList.remove('hidden');

  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/api/ota/upload');
  xhr.setRequestHeader('Content-Type', 'application/octet-stream');

  xhr.upload.onprogress = (e) => {
    if (e.lengthComputable) {
      const pct = (e.loaded / e.total) * 100;
      fill.style.width  = pct + '%';
      label.textContent = `${Math.round(pct)}% — ${fmtBytes(e.loaded)} / ${fmtBytes(e.total)}`;
    }
  };

  xhr.onload = () => {
    try {
      const resp = JSON.parse(xhr.responseText);
      if (resp.status === 'success') {
        fill.style.width  = '100%';
        label.textContent = 'Flash complete! Rebooting…';
        showToast('Firmware flashed! Device is rebooting.', 'success', 10000);
        setTimeout(() => location.reload(), 15000);
      } else {
        fill.style.width  = '0%';
        label.textContent = resp.message || 'Upload failed';
        showToast(resp.message || 'Upload failed', 'error');
        btnUpload.disabled = false;
      }
    } catch(e) {
      label.textContent = 'Parse error';
      showToast('Unexpected response', 'error');
      btnUpload.disabled = false;
    }
  };

  xhr.onerror = () => {
    fill.style.width  = '0%';
    label.textContent = 'Upload error';
    showToast('Upload failed: network error', 'error');
    btnUpload.disabled = false;
  };

  xhr.send(selectedFile);
});

/* ── WiFi config form ─────────────────────────────────────────────────────── */

document.getElementById('btn-show-pass').addEventListener('click', () => {
  const inp = document.getElementById('wifi-pass');
  inp.type = inp.type === 'password' ? 'text' : 'password';
});

document.getElementById('btn-wifi-save').addEventListener('click', async () => {
  const ssid = document.getElementById('wifi-ssid').value.trim();
  const pass = document.getElementById('wifi-pass').value;

  if (!ssid) { showMsg('wifi-msg', 'SSID cannot be empty', 'error'); return; }

  const btn = document.getElementById('btn-wifi-save');
  btn.disabled = true;
  hideMsg('wifi-msg');

  try {
    const resp = await postJSON('/api/wifi/config', { ssid, password: pass });
    if (resp.status === 'success') {
      showMsg('wifi-msg', `Saved! Connecting to "${ssid}"…`, 'success');
      showToast(`WiFi credentials saved for "${ssid}"`, 'success');
      fetchWifiStatus();
    } else {
      showMsg('wifi-msg', resp.message || 'Failed to save', 'error');
    }
  } catch(e) {
    showMsg('wifi-msg', 'Request failed: ' + e.message, 'error');
  } finally {
    btn.disabled = false;
  }
});

/* ── Partition switch ─────────────────────────────────────────────────────── */

document.getElementById('btn-switch-partition').addEventListener('click', async () => {
  const running = document.getElementById('part-running').textContent;
  const next    = document.getElementById('part-next').textContent;

  if (!confirm(`Switch boot partition from ${running} → ${next}?\n\nThe device will reboot.`)) return;

  hideMsg('partition-msg');
  const btn = document.getElementById('btn-switch-partition');
  btn.disabled = true;

  try {
    const resp = await postJSON('/api/partition/switch', {});
    if (resp.status === 'success') {
      showMsg('partition-msg', `Switching to ${resp.target_partition}. Rebooting…`, 'success');
      showToast(`Rebooting into ${resp.target_partition}`, 'success', 15000);
      setTimeout(() => location.reload(), 15000);
    } else {
      showMsg('partition-msg', resp.message || 'Switch failed', 'error');
      btn.disabled = false;
    }
  } catch(e) {
    showMsg('partition-msg', 'Request failed: ' + e.message, 'error');
    btn.disabled = false;
  }
});
