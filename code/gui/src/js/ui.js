// UI yardımcıları: seçiciler, bildirim, onay penceresi (confirm/alert yerine), biçimler.
import { has, t } from './i18n.js';

export const byId = id => document.getElementById(id);
export const $$ = (s, r = document) => [...r.querySelectorAll(s)];
export const show = (el, on) => el.classList.toggle('hidden', !on);

export function toast(msg, kind = 'ok') {
  const el = document.createElement('div');
  el.className = 'toast ' + kind;
  if (kind === 'err') el.setAttribute('role', 'alert');
  el.textContent = msg;
  byId('toastBox').appendChild(el);
  setTimeout(() => el.remove(), kind === 'err' ? 5000 : 3000);
}

// Onay: true/false; choices verilirse seçilen değer (iptalde false).
export function ask(text, { ok, danger = false, choices = null, cancel = true } = {}) {
  const d = byId('askDlg'), box = byId('askChoices'), okBtn = byId('askOk');
  byId('askText').textContent = text;
  box.replaceChildren();
  (choices || []).forEach((c, i) => {
    const l = document.createElement('label'), r = document.createElement('input'), s = document.createElement('span');
    r.type = 'radio'; r.name = 'askChoice'; r.value = c.value; r.checked = i === 0;
    s.textContent = c.label;
    l.append(r, s);
    box.appendChild(l);
  });
  okBtn.textContent = ok || t('common.ok');
  okBtn.classList.toggle('btn-danger', danger);
  byId('askCancel').hidden = !cancel;
  return new Promise(resolve => {
    let v = false;
    okBtn.onclick = () => {
      const r = box.querySelector('input:checked');
      v = choices ? r && r.value : true;
      d.close();
    };
    byId('askCancel').onclick = () => d.close();
    d.onclose = () => resolve(v);
    d.showModal();
  });
}

// Sunucu hata kodu → dil anahtarı (err.<kod>), yoksa genel metin.
export function errText(e) {
  if (e && e.code && has('err.' + e.code)) return t('err.' + e.code) + (e.field ? ' (' + e.field + ')' : '');
  return t('alert.error') + (e && e.message ? ': ' + e.message : '');
}

// saniye → "2g 3sa 5dk"; seconds yalnız toplam < 1 saat (ALTE biçimi)
export function fmtCountdown(sec) {
  sec = Math.max(0, Math.floor(sec));
  const d = Math.floor(sec / 86400), h = Math.floor(sec % 86400 / 3600), m = Math.floor(sec % 3600 / 60);
  const p = [];
  if (d) p.push(d + t('time.day_abbr'));
  if (h) p.push(h + t('time.hour_abbr'));
  if (m) p.push(m + t('time.min_abbr'));
  if (sec < 3600) p.push(sec % 60 + t('time.sec_abbr'));
  return p;
}

// saniye → en büyük iki birim, tam birim adlarıyla ("2 gün 3 saat")
export function fmtDur(sec, parts = 2) {
  sec = Math.max(0, Math.round(sec));
  const u = [[86400, 'time.day', 'time.days'], [3600, 'time.hour', 'time.hours'], [60, 'time.min'], [1, 'time.sec']];
  const out = [];
  for (const [s, k, kp] of u) {
    if (sec >= s || (s === 1 && !out.length)) {
      const n = Math.floor(sec / s);
      sec -= n * s;
      out.push(n + ' ' + t(n !== 1 && kp ? kp : k));
      if (out.length === parts) break;
    }
  }
  return out.join(' ');
}

export const fmtKB = b => (b / 1024).toFixed(1) + ' KB';

// Test düğmesi ortak akışı: önce kaydet, sonra iş başlat, iş bitene kadar durum yaz.
export async function runJob(start, statusEl, poll) {
  statusEl.className = 'smtp-status';
  statusEl.textContent = t('common.working');
  try {
    const j = await start();
    if (!j || j.job_id === undefined) { statusEl.textContent = t('alert.success'); statusEl.classList.add('ok'); return; }
    for (let i = 0; i < 90; i++) {
      await new Promise(r => setTimeout(r, 1000));
      const s = await poll(j.job_id);
      if (s.state === 'done' || s.state === 'failed') {
        const ok = s.state === 'done';
        statusEl.textContent = (ok ? t('alert.success') : t('mail.test_fail')) + (s.message ? ': ' + s.message : '');
        statusEl.classList.add(ok ? 'ok' : 'error');
        return;
      }
    }
    statusEl.textContent = t('mail.test_fail');
    statusEl.classList.add('error');
  } catch (e) {
    statusEl.textContent = errText(e);
    statusEl.classList.add('error');
  }
}

// Küçük DOM kurucu (innerHTML yerine; kullanıcı verisi hep metin olarak girer)
export function h(tag, attrs, ...kids) {
  const el = document.createElement(tag);
  for (const k in attrs || {}) {
    if (k === 'class') el.className = attrs[k];
    else if (k.startsWith('on')) el[k] = attrs[k];
    else el.setAttribute(k, attrs[k]);
  }
  el.append(...kids.filter(x => x !== null && x !== undefined && x !== false));
  return el;
}

const X_ICON = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="14" height="14">'
  + '<line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/></svg>';

export function removeBtn(title, onclick) {
  const b = h('button', { class: 'recipient-remove', type: 'button', title, 'aria-label': title, onclick });
  b.innerHTML = X_ICON;
  return b;
}
