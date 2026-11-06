// Bilgi sekmesi: cihaz bilgisi (GET /api/device + durum) ve günlükler (GET/DELETE /api/logs).
import { api } from '../api.js';
import { has, t } from '../i18n.js';
import { openSub, registerTab } from '../nav.js';
import { ask, byId, errText, fmtDur, fmtKB, h, show, toast } from '../ui.js';

const set = (id, v) => { byId(id).textContent = v === undefined || v === null || v === '' ? '-' : v; };
const tr = (k, fb) => (has(k) ? t(k) : fb);

function rssiLabel(r) {
  if (!r) return '-';
  const q = r >= -55 ? 'rssi.excellent' : r >= -67 ? 'rssi.good' : r >= -75 ? 'rssi.fair' : 'rssi.weak';
  return r + ' dBm (' + tr(q, '') + ')';
}

function vol(v) {
  if (!v || v.state !== 'mounted') return tr('storage.' + (v ? v.state : 'absent'), v ? v.state : '-');
  return t('device.free_of_total', { free: Math.round((v.total - v.used) / 1024), total: Math.round(v.total / 1024) });
}

const causeLabel = (c, cat) => tr('cause.' + c, c) + (cat ? ' (' + tr('cat.' + cat, cat) + ')' : '');

async function loadDevice() {
  const [d, s] = await Promise.all([api('GET', '/api/device'), api('GET', '/api/status')]);
  set('sysDeviceId', s.device);
  set('sysChip', d.chip + ' v' + Math.floor(d.rev / 100) + '.' + (d.rev % 100));
  set('sysCores', d.cores);
  set('sysCpuFreq', d.cpu_mhz + ' MHz');
  set('sysFlashSize', Math.round(d.flash_kb / 1024) + ' MB');
  set('sysRamTotal', fmtKB(d.heap_total));
  set('sysRamFree', fmtKB(d.heap_free));
  set('sysRamMinFree', fmtKB(d.heap_min));
  const used = (d.heap_total - d.heap_free) / d.heap_total * 100;
  const bar = byId('ramBarFill');
  bar.style.width = used.toFixed(1) + '%';
  bar.className = 'storage-bar-fill' + (used > 80 ? ' storage-bar-danger' : used > 60 ? ' storage-bar-warning' : '');
  set('ramUsagePercent', used.toFixed(1) + '%');
  set('sysIntFlashTotal', d.flash_kb + ' KB');
  set('sysIntFlashUsed', d.app_kb + ' KB / ' + d.slot_kb + ' KB');
  set('sysIntFlashFree', (d.slot_kb - d.app_kb) + ' KB');
  byId('sysExtFlashSize').textContent = d.ext_mb ? '(' + d.ext_mb + ' MB)' : '(' + t('ota.ext_flash_unavailable') + ')';
  set('sysLogVol', vol(d.log));
  set('sysUserData', vol(d.data));
  const w = s.wifi, mdns = s.device.toLowerCase().slice(0, 15) + '.local';
  set('sysWifiStatus', t(w.state === 'connected' ? 'wifi.connected' : 'wifi.not_connected'));
  set('sysWifiSsid', w.ssid);
  set('sysRssi', w.state === 'connected' ? rssiLabel(w.rssi) : '-');
  set('sysIp', w.ip);
  set('sysMdns', mdns);
  const ap = byId('sysApStatus');
  ap.textContent = t(w.ap_on ? 'device.active' : 'device.inactive');
  ap.className = 'system-value' + (w.ap_on ? ' status-active' : '');
  set('sysApIp', w.ap_on ? '192.168.4.1' : '-');
  set('sysApClients', w.ap_clients);
  set('sysTxPower', (d.tx_q / 4).toFixed(1) + ' dBm (max ' + (d.tx_max_q / 4).toFixed(1) + ' dBm)');
  set('sysUptime', fmtDur(s.uptime, 3));
  set('sysBootCount', '#' + d.boot_count);
  set('sysResetReason', causeLabel(d.last_cause, d.last_cat));
  set('sysPrevUptime', d.prev_uptime ? fmtDur(d.prev_uptime, 3) : '-');
  set('sysFirmware', 'v' + s.version);
  set('sysCoredump', d.coredump ? fmtKB(d.coredump) : t('device.coredump_none'));
  show(byId('coredumpActions'), !!d.coredump);
}

/* ---------- günlükler ---------- */

let logs = [];
let filter = 'all';

function when(r) {
  if (r.ts > 1700000000) {
    const d = new Date(r.ts * 1000), p = n => String(n).padStart(2, '0');
    return p(d.getDate()) + '.' + p(d.getMonth() + 1) + '.' + d.getFullYear() + ' ' + p(d.getHours()) + ':' + p(d.getMinutes());
  }
  return '+' + fmtDur(r.up || 0, 3);
}

function matches(r) {
  if (filter === 'all') return true;
  if (filter === 'boot') return r.type === 'boot';
  if (r.type === 'boot') return filter === 'err' && (r.cat === 'power' || r.cat === 'software');
  if (filter === 'err') return r.lvl === 'ERR' || r.lvl === 'WARN';
  if (filter === 'msg') return ['mail', 'tg', 'api'].includes(r.cat);
  if (filter === 'timer') return r.cat === 'timer' || r.cat === 'relay';
  return r.cat === filter;
}

function bootItem(r) {
  const cat = r.cat || 'unknown';
  const hint = r.hint && has('hint.' + r.hint) ? t('hint.' + r.hint) : '';
  const meta = [h('span', { class: 'log-meta' }, t('logs.ran_for', { time: fmtDur(r.up || 0, 3) }) + (r.approx ? ' (' + t('logs.approx') + ')' : ''))];
  if (r.ev && r.ev !== 'NONE') {
    meta.push(h('span', { class: 'log-meta' }, t('logs.last_event') + ': ' + tr('ev.' + r.ev, r.ev)
      + (r.ev_dt >= 0 ? ' (' + t('logs.secs_before', { n: r.ev_dt }) + ')' : '')));
  }
  if (r.wifi && r.wifi !== '?') {
    meta.push(h('span', { class: 'log-meta' }, 'WiFi ' + r.wifi + ' · ' + t('logs.clients', { n: r.cl || 0 })
      + (r.rssi ? ' · RSSI ' + r.rssi + ' dBm' : '') + (r.tx ? ' · TX ' + r.tx + ' dBm' : '')
      + ' · heap ' + fmtKB(r.heap_free || 0) + ' / min ' + fmtKB(r.heap_min || 0)));
  }
  if (r.short > 0) meta.push(h('span', { class: 'log-meta log-warn' }, t('logs.short_boots_note', { n: r.short })));
  return h('div', { class: 'log-item log-boot cat-' + cat },
    h('div', { class: 'log-boot-top' },
      h('span', { class: 'log-time' }, when(r)),
      h('span', { class: 'log-type type-boot' }, t('logs.boot') + ' #' + r.n),
      h('span', { class: 'log-type cat-' + cat }, tr('cat.' + cat, cat))),
    h('span', { class: 'log-text log-cause' }, tr('cause.' + r.cause, r.cause)),
    hint && h('span', { class: 'log-hint' }, hint),
    ...meta);
}

function eventItem(r) {
  const cat = r.cat || 'sys', lvl = r.lvl || 'INFO';
  return h('div', { class: 'log-item log-' + cat + ' lvl-' + lvl },
    h('div', {},
      h('span', { class: 'log-time' }, when(r)), ' ',
      lvl !== 'INFO' && h('span', { class: 'log-type lvl-' + lvl }, lvl), ' ',
      h('span', { class: 'log-type type-' + cat }, tr('logcat.' + cat, cat))),
    h('span', { class: 'log-text' }, r.msg));
}

function renderLogs() {
  const out = [];
  let lastBoot = -1;
  for (const r of logs) {
    if (!matches(r)) continue;
    const bootNo = r.type === 'boot' ? r.n + 1 : r.n;
    if (r.type !== 'boot' && bootNo !== undefined && bootNo !== lastBoot) {
      if (out.length) out.push(h('div', { class: 'log-boot-divider' }));
      out.push(h('div', { class: 'log-boot-header' }, t('logs.boot') + ' #' + bootNo));
      lastBoot = bootNo;
    }
    if (r.type === 'boot') { out.push(bootItem(r)); lastBoot = r.n; } else out.push(eventItem(r));
  }
  const list = byId('logList');
  list.replaceChildren(...(out.length ? out : [h('div', { class: 'log-item' }, h('span', { class: 'log-text' }, t('logs.empty')))]));
}

async function loadLogs() {
  const list = byId('logList');
  list.replaceChildren(h('div', { class: 'log-item' }, h('span', { class: 'log-text' }, t('logs.loading'))));
  try {
    const d = await api('GET', '/api/logs');
    logs = d.logs || [];
    set('logBootCount', '#' + d.boot_count);
    set('logLastCause', d.last_cause ? causeLabel(d.last_cause, d.last_cat) : '-');
    set('logPrevUptime', d.prev_uptime ? fmtDur(d.prev_uptime, 3) : '-');
    set('logShortBoots', String(d.short_boots));
    renderLogs();
  } catch (e) {
    logs = [];
    list.replaceChildren(h('div', { class: 'log-item' }, h('span', { class: 'log-text' }, t('logs.error') + ' (' + errText(e) + ')')));
  }
}

export function initInfo() {
  document.querySelectorAll('#panel-info .info-card').forEach(c => {
    c.onclick = async () => {
      const id = c.dataset.subpage;
      openSub(id);
      if (id === 'deviceInfoSubpage') loadDevice().catch(e => toast(errText(e), 'err'));
      else loadLogs();
    };
  });
  document.querySelectorAll('.log-filter-btn').forEach(b => {
    b.onclick = () => {
      filter = b.dataset.filter;
      document.querySelectorAll('.log-filter-btn').forEach(x => x.classList.toggle('active', x === b));
      renderLogs();
    };
  });
  byId('logRefreshBtn').onclick = loadLogs;
  byId('coredumpDelete').onclick = async () => {
    if (!(await ask(t('device.coredump_delete_confirm'), { ok: t('device.coredump_delete'), danger: true }))) return;
    try { await api('DELETE', '/api/coredump'); loadDevice(); } catch (e) { toast(errText(e), 'err'); }
  };
  byId('logClearBtn').onclick = async () => {
    if (!(await ask(t('logs.clear_confirm'), { ok: t('logs.clear'), danger: true }))) return;
    try { await api('DELETE', '/api/logs'); loadLogs(); } catch (e) { toast(errText(e), 'err'); }
  };
  registerTab('panel-info', {});
}
