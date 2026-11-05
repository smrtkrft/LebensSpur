// Ana ekran: halka, kalan süre, durum, erteleme / durdurma, teslimat özeti.
import { api } from '../api.js';
import { t } from '../i18n.js';
import { $, confirmDialog, errText, fmtDur, toast } from '../ui.js';

const RING = 2 * Math.PI * 54;

export function initHome() {
  $('#btn-checkin').onclick = async () => {
    try { await api('POST', '/api/timer/checkin'); toast(t('common.saved')); }
    catch (e) { toast(errText(e), 'err'); }
  };
  $('#btn-stop').onclick = async () => {
    if (!(await confirmDialog(t('confirm.stop')))) return;
    try { await api('POST', '/api/timer/cancel'); } catch (e) { toast(errText(e), 'err'); }
  };
}

export function renderHome(s) {
  const tm = s.timer;
  const running = tm.state === 'running', vac = tm.state === 'vacation';
  const frac = tm.total ? tm.remaining / tm.total : 0;
  $('#ring-fg').style.strokeDasharray = RING;
  $('#ring-fg').style.strokeDashoffset = RING * (1 - (running ? frac : tm.state === 'stopped' ? frac : 0));
  $('#home').dataset.state = tm.state;
  $('#remain').textContent = running || tm.state === 'stopped' ? fmtDur(tm.remaining) : vac ? fmtDur(tm.vac_remaining) : '—';
  $('#state').textContent = t('state.' + tm.state);
  const next = (tm.warn_plan || [])[tm.warn_fired];
  $('#next-warn').textContent = running && next !== undefined && next < tm.remaining
    ? t('home.next_warn', { time: fmtDur(next) }) : '';
  const d = s.delivery;
  $('#delivery').textContent = tm.state === 'triggered' || d.pending
    ? t('state.delivery', { done: d.done, pending: d.pending, failed: d.failed }) : '';
  $('#btn-checkin').disabled = tm.state === 'idle';
  $('#btn-stop').hidden = !running;
  $('#device').textContent = s.device;
  $('#wifi-dot').dataset.on = s.wifi.state === 'connected';
  $('#auth-warn').hidden = s.auth;
}
