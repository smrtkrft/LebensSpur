// Durum deposu: SSE (/api/events) olayda gelir; kalan süre istemcide yerel saatle ilerler.
// SSE yoksa 30 sn'de bir /api/status. Sekme arka planda → bağlantı kapanır (güç kısıtı).
import { api } from './api.js';

let status = null, at = 0, es = null, poll = null;
const subs = new Set();

export function onStatus(fn) { subs.add(fn); if (status) fn(view()); }

function view() {
  const el = (Date.now() - at) / 1000;
  const s = structuredClone(status);
  if (s.timer.state === 'running') s.timer.remaining = Math.max(0, s.timer.remaining - el);
  if (s.timer.state === 'vacation') s.timer.vac_remaining = Math.max(0, s.timer.vac_remaining - el);
  return s;
}

function set(s) { status = s; at = Date.now(); subs.forEach(f => f(view())); }

export async function refresh() { set(await api('GET', '/api/status')); }

function startPoll() { if (!poll) poll = setInterval(() => refresh().catch(() => {}), 30000); }
function stopPoll() { clearInterval(poll); poll = null; }

export function connect() {
  disconnect();
  es = new EventSource('/api/events');
  es.onmessage = e => { stopPoll(); set(JSON.parse(e.data)); };
  es.onerror = () => { es.close(); es = null; startPoll(); };
}

export function disconnect() { if (es) { es.close(); es = null; } stopPoll(); }

setInterval(() => { if (status && !document.hidden) subs.forEach(f => f(view())); }, 1000);

document.addEventListener('visibilitychange', () => {
  if (document.hidden) disconnect();
  else { refresh().catch(() => {}); connect(); }
});
