// Ana ekran (ALTE düzeni): sayaç çalışırken halka + kalan süre, değilse logo; tatil göstergesi;
// eylem çubuğu: ayarlar, durdur, buradayım (yeniden başlat), çıkış.
import { api } from '../api.js';
import { t } from '../i18n.js';
import { ask, byId, errText, fmtCountdown, toast } from '../ui.js';

const RING = 2 * Math.PI * 90;
let last = null;

export function initHome() {
  byId('progressRing').style.strokeDasharray = RING;
  byId('stopBtn').onclick = async () => {
    if (!last) return;
    const vac = last.timer.state === 'vacation';
    if (!vac && last.timer.state !== 'running') return;
    if (!vac && !(await ask(t('confirm.stop'), { ok: t('home.stop'), danger: true }))) return;
    try { await api(vac ? 'DELETE' : 'POST', vac ? '/api/timer/vacation' : '/api/timer/cancel'); }
    catch (e) { toast(errText(e), 'err'); }
  };
  byId('restartBtn').onclick = async () => {
    try { await api('POST', '/api/timer/checkin'); toast(t('home.checked_in')); }
    catch (e) { toast(errText(e), 'err'); }
  };
  byId('exitBtn').onclick = async () => {
    if (last && last.auth) { try { await api('POST', '/api/auth/logout'); } catch (e) { /* yine çık */ } }
    window.location.href = 'about:blank';
  };
}

export function renderHome(s) {
  last = s;
  const tm = s.timer, vac = tm.state === 'vacation', trig = tm.state === 'triggered';
  const showTimer = tm.state === 'running' || vac || trig;
  byId('logoImg').style.display = showTimer ? 'none' : 'block';
  byId('timerSection').style.display = showTimer ? 'flex' : 'none';
  byId('stopBtn').disabled = !(tm.state === 'running' || vac);
  byId('restartBtn').disabled = tm.state === 'idle';
  const vi = byId('vacationIndicator');
  vi.classList.toggle('active', vac);
  if (vac) byId('vacationIndicatorText').textContent = t('timer_display.vacation_left', { time: fmtCountdown(tm.vac_remaining).join(' ') });
  if (!showTimer) return;
  byId('timerRing').classList.toggle('vacation', vac);
  byId('timerLabel').textContent = t(trig ? 'state.triggered' : vac ? 'timer_display.waiting' : 'timer_display.remaining');
  const rem = trig ? 0 : tm.remaining;
  const p = rem > 0 ? fmtCountdown(rem) : [];
  const [d, h, m] = p.length >= 3 ? p : p.length === 2 ? ['', p[0], p[1]] : ['', p[0] || '', ''];
  byId('timeDays').textContent = d;
  byId('timeHours').textContent = h;
  byId('timeMinutes').textContent = m;
  byId('progressRing').style.strokeDashoffset = tm.total > 0 ? RING * (1 - rem / tm.total) : RING;
}
