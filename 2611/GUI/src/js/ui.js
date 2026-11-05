// Küçük UI yardımcıları: seçici, toast, onay penceresi, süre biçimi.
import { t } from './i18n.js';

export const $ = (s, r = document) => r.querySelector(s);

export function toast(msg, kind = 'ok') {
  const el = document.createElement('div');
  el.className = 'toast ' + kind;
  el.setAttribute('role', kind === 'err' ? 'alert' : 'status');
  el.textContent = msg;
  document.body.appendChild(el);
  setTimeout(() => el.remove(), 3500);
}

export function confirmDialog(text) {
  return new Promise(resolve => {
    const d = $('#confirm');
    $('#confirm-text').textContent = text;
    const done = v => { d.close(); resolve(v); };
    $('#confirm-ok').onclick = () => done(true);
    $('#confirm-cancel').onclick = () => done(false);
    d.onclose = () => resolve(false);
    d.showModal();
  });
}

// saniye → "2 g 3 sa", "45 dk", "30 sn" (dil kısaltmalarıyla)
export function fmtDur(sec, parts = 2) {
  sec = Math.max(0, Math.round(sec));
  const u = [[86400, 'time.day_abbr'], [3600, 'time.hour_abbr'], [60, 'time.min_abbr'], [1, 'time.sec_abbr']];
  const out = [];
  for (const [s, k] of u) {
    if (sec >= s || (s === 1 && !out.length)) {
      const n = Math.floor(sec / s);
      sec -= n * s;
      out.push(n + ' ' + t(k));
      if (out.length === parts) break;
    }
  }
  return out.join(' ');
}

export function errText(e) {
  if (e.code === 'offline') return t('common.offline');
  return e.message || t('alert.error');
}
