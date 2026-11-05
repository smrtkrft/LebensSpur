// Sayaç ayarı: birim, değer, uyarı sayısı, ilk uyarı (otomatik/elle); sunucunun planıyla önizleme.
import { api } from '../api.js';
import { t } from '../i18n.js';
import { $, errText, fmtDur, toast } from '../ui.js';

const UNIT_S = { min: 60, hour: 3600, day: 86400 };

// sunucu formülünün aynısı (önizleme için; kaydı sunucu doğrular)
function plan(T, n, first) {
  const F = Math.min(60, Math.max(15, Math.floor(T / 10)));
  const L = Math.max(Math.min(Math.floor(T / 24), 86400), F);
  if (n === 1) return [L];
  const W = first || Math.floor(T / 4);
  if (W >= T || W <= L || W < L + (n - 1) * F) return null;
  const o = [W];
  for (let k = 1; k < n - 1; k++) o.push(Math.round(W * Math.pow(L / W, k / (n - 1))));
  o.push(L);
  for (let k = n - 2; k >= 1; k--) o[k] = Math.max(o[k], o[k + 1] + F);
  if (n > 2 && o[1] + F > W) for (let k = 1; k < n - 1; k++) o[k] = W - Math.round((W - L) * k / (n - 1));
  return o;
}

function maxCount(T, first) {
  let m = 0;
  for (let n = 1; n <= 10; n++) if (plan(T, n, first)) m = n;
  return m;
}

function read() {
  const unit = $('#t-unit').value, value = +$('#t-value').value, n = +$('#t-warn').value;
  const auto = $('#t-first-auto').checked;
  const first = auto ? 0 : Math.round(+$('#t-first').value * UNIT_S[$('#t-first-unit').value]);
  return { unit, value, warn_count: n, warn_first_s: first };
}

function preview() {
  const f = read();
  const T = f.value * UNIT_S[f.unit];
  $('#t-warn-out').textContent = f.warn_count;
  $('#t-first-row').hidden = $('#t-first-auto').checked;
  const p = plan(T, f.warn_count, f.warn_first_s);
  $('#t-preview').textContent = p ? t('warn.preview', { list: p.map(x => fmtDur(x, 1)).join(' · ') })
    : t('warn.max', { n: maxCount(T, f.warn_first_s) });
  $('#t-preview').classList.toggle('bad', !p);
  $('#t-save').disabled = !p;
}

export async function initTimer() {
  const c = await api('GET', '/api/config/timer');
  $('#t-unit').value = c.unit;
  $('#t-value').value = c.value;
  $('#t-warn').value = c.warn_count;
  $('#t-first-auto').checked = !c.warn_first_s;
  if (c.warn_first_s) {
    const u = c.warn_first_s % 86400 === 0 ? 'day' : c.warn_first_s % 3600 === 0 ? 'hour' : 'min';
    $('#t-first-unit').value = u;
    $('#t-first').value = c.warn_first_s / UNIT_S[u];
  }
  document.querySelectorAll('#timer-form input, #timer-form select').forEach(el => { el.oninput = preview; });
  $('#timer-form').onsubmit = async ev => {
    ev.preventDefault();
    try { await api('PUT', '/api/timer', read()); toast(t('common.saved')); $('#settings').close(); }
    catch (e) { toast(errText(e), 'err'); }
  };
  preview();
}
