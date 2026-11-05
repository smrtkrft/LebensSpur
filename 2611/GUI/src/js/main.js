// LebensSpur 2611 — giriş noktası.
import { api, ApiError } from './api.js';
import { currentLang, loadLang, LANGS, t } from './i18n.js';
import { connect, disconnect, onStatus, refresh } from './state.js';
import { $, errText, toast } from './ui.js';
import { initHome, renderHome } from './pages/home.js';
import { initTimer } from './pages/timer.js';

function showLogin(on) {
  $('#login').hidden = !on;
  $('#app').hidden = on;
  if (on) { disconnect(); $('#login-pass').focus(); }
}

async function start() {
  try {
    await refresh();
    showLogin(false);
    connect();
  } catch (e) {
    if (e instanceof ApiError && e.status === 401) showLogin(true);
    else toast(errText(e), 'err');
  }
}

async function boot() {
  const theme = localStorage.getItem('ls-theme') || 'dark';
  document.documentElement.dataset.theme = theme;
  const sel = $('#lang');
  LANGS.forEach(l => sel.add(new Option(l.toUpperCase(), l)));
  sel.value = currentLang();
  sel.onchange = () => loadLang(sel.value).catch(() => {});
  /* dil yüklenemezse arayüz yine açılır (anahtarlar görünür), 5 sn sonra yeniden denenir */
  const tryLang = () => loadLang(sel.value).catch(() => setTimeout(tryLang, 5000));
  tryLang();

  $('#login-form').onsubmit = async ev => {
    ev.preventDefault();
    try {
      await api('POST', '/api/auth/login', { password: $('#login-pass').value });
      $('#login-pass').value = '';
      start();
    } catch (e) {
      $('#login-err').textContent = e.code === 'locked' ? t('auth.locked', { s: '' }) : t('auth.wrong');
    }
  };
  window.addEventListener('ls-auth-required', () => showLogin(true));
  $('#btn-settings').onclick = async () => {
    try { await initTimer(); $('#settings').showModal(); } catch (e) { toast(errText(e), 'err'); }
  };
  $('#settings-close').onclick = () => $('#settings').close();
  $('#theme').onclick = () => {
    const n = document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark';
    document.documentElement.dataset.theme = n;
    localStorage.setItem('ls-theme', n);
  };
  initHome();
  onStatus(renderHome);
  start();
}

boot();
