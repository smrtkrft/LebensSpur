// LebensSpur — giriş noktası (ALTE arayüz düzeni, yeni API).
import { ApiError, api } from './api.js';
import { currentLang, loadLang, t } from './i18n.js';
import { closePopup, initNav } from './nav.js';
import { connect, disconnect, onStatus, refresh } from './state.js';
import { byId, errText, show, toast } from './ui.js';
import { initActions } from './pages/actions.js';
import { initConn } from './pages/conn.js';
import { initGroups } from './pages/groups.js';
import { initHome, renderHome } from './pages/home.js';
import { initInfo } from './pages/info.js';
import { applyTheme, initSystem } from './pages/system.js';
import { initTimer } from './pages/timer.js';

function showLogin(on) {
  show(byId('loginOverlay'), on);
  if (on) {
    closePopup();
    disconnect();
    byId('loginPassword').focus();
  }
}

async function start() {
  try {
    await refresh();
    showLogin(false);
    connect();
  } catch (e) {
    if (e instanceof ApiError && e.status === 401) showLogin(true);
    else { toast(errText(e), 'err'); setTimeout(start, 5000); }
  }
}

async function login() {
  const err = byId('loginError');
  try {
    await api('POST', '/api/auth/login', { password: byId('loginPassword').value });
    byId('loginPassword').value = '';
    show(err, false);
    start();
  } catch (e) {
    err.textContent = e.code === 'locked' ? t('auth.locked', { s: '' }) : e.code === 'offline' ? t('common.offline') : t('auth.wrong');
    show(err, true);
  }
}

function boot() {
  applyTheme(localStorage.getItem('ls-theme') || 'dark');
  /* dil yüklenemezse arayüz yine açılır (HTML'deki İngilizce metin), 5 sn sonra yeniden denenir */
  const tryLang = () => loadLang(currentLang()).catch(() => setTimeout(tryLang, 5000));
  tryLang();
  initNav();
  initHome();
  initTimer();
  initConn();
  initActions();
  initGroups();
  initSystem();
  initInfo();
  onStatus(renderHome);
  byId('loginBtn').onclick = login;
  byId('loginPassword').onkeydown = e => { if (e.key === 'Enter') login(); };
  window.addEventListener('ls-auth-required', () => showLogin(true));
  start();
}

boot();
