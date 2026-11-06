// Zamanlayıcı sekmesi: süre (birim + değer), uyarı sayısı 1..10, ilk uyarı (otomatik/elle), önizleme, tatil.
// Önizleme sunucu formülünün aynısıdır (warn_sched.c); kaydı yine sunucu doğrular.
import { api } from '../api.js';
import { Section } from '../form.js';
import { t } from '../i18n.js';
import { registerTab } from '../nav.js';
import { onStatus } from '../state.js';
import { $$, byId, fmtCountdown, fmtDur, show } from '../ui.js';

const UNIT_S = { min: 60, hour: 3600, day: 86400 };
let unit = 'day';
let st = null;                     // son durum (SSE)
let vacTouched = false;            // kullanıcı tatil anahtarına dokundu mu (SSE ezmesin)

export function plan(T, n, first) {
  const F = Math.min(60, Math.max(15, Math.floor(T / 10)));
  const L = Math.max(Math.min(Math.floor(T / 24), 86400), F);
  if (L >= T) return null;
  if (n === 1) return [L];
  const W = first || Math.floor(T / 4);
  if (W >= T || W <= L || W < L + (n - 1) * F) return null;
  const o = [W];
  for (let k = 1; k < n - 1; k++) o.push(Math.round(W * Math.pow(L / W, k / (n - 1))));
  o.push(L);
  for (let k = n - 2; k >= 1; k--) o[k] = Math.max(o[k], o[k + 1] + F);
  if (n > 2 && o[1] + F > W) for (let k = 1; k < n - 1; k++) o[k] = W - Math.round((W - L) * k / (n - 1));
  for (let k = 1; k < n; k++) if (o[k - 1] < o[k] + F) return null;
  return o;
}

const maxCount = (T, first) => {
  let m = 0;
  for (let n = 1; n <= 10; n++) if (plan(T, n, first)) m = n;
  return m;
};

const firstS = () => (byId('warnFirstAuto').checked ? 0
  : Math.max(1, +byId('warnFirstValue').value || 1) * UNIT_S[byId('warnFirstUnit').value]);

const sec = new Section('timer', {
  unit: { get: () => unit, set: v => setUnit(v || 'day') },
  value: 'timerValue',
  warn_count: 'alarmCount',
  warn_first_s: {
    get: firstS,
    set: v => {
      byId('warnFirstAuto').checked = !v;
      if (v) {
        const u = v % 86400 === 0 ? 'day' : v % 3600 === 0 ? 'hour' : 'min';
        byId('warnFirstUnit').value = u;
        byId('warnFirstValue').value = Math.round(v / UNIT_S[u]);
      }
    },
  },
});

function setUnit(u) {
  unit = u;
  $$('.unit-btn').forEach(b => b.classList.toggle('active', b.dataset.unit === u));
  preview();
}

function clampValue() {
  const el = byId('timerValue');
  el.value = Math.min(60, Math.max(1, Math.round(+el.value || 1)));
}

function preview() {
  const T = (+byId('timerValue').value || 1) * UNIT_S[unit];
  const first = firstS();
  show(byId('warnFirstRow'), !byId('warnFirstAuto').checked);
  const sl = byId('alarmCount');
  byId('alarmCountValue').textContent = sl.value;
  const p = plan(T, +sl.value, first);
  const info = byId('alarmInfo');
  info.textContent = p ? t('warn.preview', { list: p.map(x => fmtDur(x, 1)).join(' · ') })
    : t('warn.max', { n: maxCount(T, first) });
  info.classList.toggle('bad', !p);
  return p;
}

function renderStatus() {
  if (!st) return;
  const tm = st.timer, el = byId('timerStatus');
  const k = { running: 'timer.status_active', vacation: 'timer.status_vacation', triggered: 'state.triggered' }[tm.state];
  el.textContent = k ? t(k, { time: fmtCountdown(tm.state === 'vacation' ? tm.vac_remaining : tm.remaining).join(' ') })
    : t('timer.status_inactive');
  el.classList.toggle('active', tm.state === 'running');
  const vac = tm.state === 'vacation';
  if (!vacTouched) byId('vacationMode').checked = vac;
  show(byId('vacationDaysGroup'), byId('vacationMode').checked && !vac);
  byId('vacationDaysGroup').style.display = '';
  show(byId('vacationActiveStatus'), vac);
  byId('vacationActiveStatus').style.display = '';
  if (vac) byId('vacationActiveText').textContent = t('vacation.active_text', { time: fmtCountdown(tm.vac_remaining).join(' ') });
}

async function save() {
  if (!preview()) throw new Error(byId('alarmInfo').textContent);
  let changed = false;
  if (sec.dirty()) { await sec.save(); changed = true; }
  const on = byId('vacationMode').checked, vac = st && st.timer.state === 'vacation';
  if (on && !vac) {
    const days = Math.min(60, Math.max(1, +byId('vacationDays').value || 1));
    await api('POST', '/api/timer/vacation', { days });
    changed = true;
  } else if (!on && vac) {
    await api('DELETE', '/api/timer/vacation');
    changed = true;
  }
  vacTouched = false;
  return changed;
}

export function initTimer() {
  $$('.unit-btn').forEach(b => { b.onclick = () => setUnit(b.dataset.unit); });
  const step = d => () => { byId('timerValue').value = (+byId('timerValue').value || 1) + d; clampValue(); preview(); };
  byId('timerDecrease').onclick = step(-1);
  byId('timerIncrease').onclick = step(1);
  byId('timerValue').onchange = () => { clampValue(); preview(); };
  ['timerValue', 'alarmCount', 'warnFirstAuto', 'warnFirstValue', 'warnFirstUnit'].forEach(id => {
    byId(id).addEventListener('input', preview);
  });
  const vstep = d => () => { const el = byId('vacationDays'); el.value = Math.min(60, Math.max(1, (+el.value || 1) + d)); };
  byId('vacationDecrease').onclick = vstep(-1);
  byId('vacationIncrease').onclick = vstep(1);
  byId('vacationMode').onchange = () => { vacTouched = true; renderStatus(); };
  // elle seçime geçince otomatik değerden (T/4) başla
  byId('warnFirstAuto').addEventListener('change', () => {
    if (byId('warnFirstAuto').checked) return;
    const W = Math.floor((+byId('timerValue').value || 1) * UNIT_S[unit] / 4);
    const u = W >= 86400 && W % 86400 === 0 ? 'day' : W >= 3600 && W % 3600 === 0 ? 'hour' : 'min';
    byId('warnFirstUnit').value = u;
    byId('warnFirstValue').value = Math.max(1, Math.round(W / UNIT_S[u]));
    preview();
  });
  onStatus(s => { st = s; renderStatus(); });
  window.addEventListener('ls-lang', () => { preview(); renderStatus(); });
  registerTab('panel-timer', {
    show: async () => { vacTouched = false; await sec.load(); preview(); renderStatus(); },
    save,
  });
}
