// Sistem sekmesi: tema, dil, tarayıcı parolası + oturum, uzaktan sıfırlama, yazılım güncelleme,
// yeniden başlatma, fabrika ayarı (1: ayarlar, 2: ayarlar + harici flash tamamen silinir).
import { api } from '../api.js';
import { Section } from '../form.js';
import { LANGS, currentLang, loadLang, t } from '../i18n.js';
import { openSub, registerTab } from '../nav.js';
import { onStatus } from '../state.js';
import { ask, byId, errText, show, toast } from '../ui.js';

let st = null;

/* ---------- tema ---------- */

export function applyTheme(th) {
  document.body.classList.toggle('light-theme', th === 'light');
  localStorage.setItem('ls-theme', th);
}

/* ---------- güvenlik ---------- */

const auth = new Section('auth', { ttl_min: 'autoLogoutTime', protect: 'loginProtection', lock_min: 'lockoutTime' });

async function saveSecurity() {
  const cur = byId('currentPassword').value, nxt = byId('newPassword').value, cnf = byId('confirmPassword').value;
  let changed = await auth.save();
  if (nxt || cnf || cur) {
    if (nxt !== cnf) throw new Error(t('security.mismatch'));
    if (st && st.auth && !cur) throw new Error(t('security.current_required'));
    if (nxt && nxt.length < 4) throw new Error(t('security.too_short'));
    if (!nxt && !(await ask(t('security.remove_confirm'), { danger: true }))) return changed;
    await api('PUT', '/api/auth/password', { current: cur, next: nxt });
    toast(t(nxt ? 'security.changed' : 'security.removed'));
    if (nxt) window.dispatchEvent(new Event('ls-auth-required'));   // tüm oturumlar düştü: hemen yeni parolayla giriş
    changed = true;
  }
  return changed;
}

function clearPw() { ['currentPassword', 'newPassword', 'confirmPassword'].forEach(id => { byId(id).value = ''; }); }

/* ---------- uzaktan sıfırlama ---------- */

function showKey(key) {
  byId('resetApiKey').value = key || t('reset_api.not_generated');
  const host = location.host;
  const codes = byId('resetApiSubpage').querySelectorAll('.code-block');
  codes[0].textContent = 'GET /api/reset?key=' + (key || '…');
  codes[1].textContent = 'curl http://' + host + '/api/reset?key=' + (key || '…');
}

async function openRemote() {
  try {
    const r = await api('GET', '/api/config/remote');
    byId('resetApiEnabled').checked = r.enabled;
    showKey(r.key ? (await api('GET', '/api/remote/key')).key : '');
  } catch (e) { toast(errText(e), 'err'); return; }
  openSub('resetApiSubpage');
}

/* ---------- güncelleme (OTA) ---------- */

let otaSrc = 'official';

function otaBody() {
  return otaSrc === 'custom' ? { source: 'custom', url: byId('otaServerUrl').value.trim() } : { source: 'official' };
}

// iş bitene kadar izle (denetim ve indirme sys_worker'da); her adımda onTick(iş)
async function waitJob(id, onTick) {
  for (let i = 0; i < 900; i++) {
    await new Promise(r => setTimeout(r, 1000));
    const j = await api('GET', '/api/jobs/' + id);
    if (onTick) onTick(j);
    if (j.state === 'done') return j;
    if (j.state === 'failed') throw new Error(j.message);
  }
  throw new Error('timeout');
}

async function otaInfo() {
  const o = await api('GET', '/api/ota');
  byId('currentFirmwareVersion').textContent = 'v' + o.version;
  byId('runningPartition').textContent = o.partition + (o.pending_verify ? ' · ' + t('ota.pending_verify') : '');
  show(byId('fwRollbackSection'), !!o.prev_version && o.state !== 'ready' && o.state !== 'rollback_ready');
  if (o.prev_version) byId('fwRollbackInfo').textContent = t('ota.fw_rollback_info', { version: o.prev_version });
  show(byId('fwRestartSection'), o.state === 'ready' || o.state === 'rollback_ready');
  show(byId('fwRemoteVersionRow'), !!o.remote);
  byId('remoteFirmwareVersion').textContent = o.remote ? 'v' + o.remote.replace(/^v/, '') : '-';
  show(byId('fwStartSection'), o.available);
  return o;
}

async function otaCheck() {
  if (otaSrc === 'custom' && !otaBody().url) { toast(t('ota.custom_url_required'), 'err'); return; }
  const btn = byId('checkFirmwareUpdateBtn');
  btn.disabled = true;
  try {
    const { job_id } = await api('POST', '/api/ota/check', otaBody());
    await waitJob(job_id);
    const o = await otaInfo();
    if (!o.available) toast(t('ota.fw_already_latest'));
  } catch (e) {
    toast(t('ota.fw_update_error') + ': ' + errText(e), 'err');
  } finally {
    btn.disabled = false;
  }
}

async function otaStart() {
  if (!(await ask(t('ota.confirm_fw_update')))) return;
  show(byId('fwStartSection'), false);
  show(byId('fwOtaProgressSection'), true);
  const bar = byId('fwOtaProgressBar'), txt = byId('fwOtaProgressText');
  try {
    const { job_id } = await api('POST', '/api/ota/start', otaBody());
    await waitJob(job_id, j => {
      bar.style.width = j.pct + '%';
      txt.textContent = j.pct + '%' + (j.message ? ' · ' + j.message : '');
    });
    bar.style.width = '100%';
    toast(t('ota.fw_update_success'));
    await otaInfo();
  } catch (e) {
    txt.textContent = t('ota.fw_update_error') + ': ' + errText(e);
  }
}

/* ---------- yeniden başlatma / fabrika ayarı ---------- */

// cihaz yeniden açılınca sayfayı tazele (en çok ~2 dk dener)
function reloadWhenBack(delay) {
  setTimeout(async function poll(n = 0) {
    try { const r = await fetch('/api/auth', { cache: 'no-store' }); if (r.ok) { location.reload(); return; } } catch (e) { /* kapalı */ }
    if (n < 60) setTimeout(() => poll(n + 1), 2000);
  }, delay);
}

async function restart(confirmKey) {
  if (!(await ask(t(confirmKey), { ok: t('system.restart_btn') }))) return;
  try { await api('POST', '/api/device/restart'); toast(t('alert.restart_ok')); reloadWhenBack(5000); }
  catch (e) { toast(t('alert.restart_err') + ': ' + errText(e), 'err'); }
}

async function factory() {
  const level = await ask(t('confirm.factory1'), {
    ok: t('system.factory_btn'), danger: true,
    choices: [{ value: '1', label: t('factory.level1') }, { value: '2', label: t('factory.level2') }],
  });
  if (!level) return;
  if (!(await ask(t('confirm.factory2'), { ok: t('system.factory_btn'), danger: true }))) return;
  try {
    const { job_id } = await api('POST', '/api/device/factory-reset', { level: +level });
    toast(t('alert.factory_ok'));
    // kademe 2'de /data silinirken cihaz yanıt vermeye devam eder: iş bitene kadar bekle
    for (let i = 0; i < 600; i++) {
      await new Promise(r => setTimeout(r, 1000));
      const j = await api('GET', '/api/jobs/' + job_id).catch(() => null);
      if (!j || j.state === 'done' || j.state === 'failed') break;
      byId('askText').textContent = t('alert.factory_ok') + ' ' + j.pct + '%';
    }
    reloadWhenBack(6000);
  } catch (e) { toast(t('alert.factory_err') + ': ' + errText(e), 'err'); }
}

export function initSystem() {
  const th = byId('themeSelect');
  th.value = localStorage.getItem('ls-theme') || 'dark';
  th.onchange = () => applyTheme(th.value);
  const ls = byId('languageSelect');
  ls.value = currentLang();
  ls.onchange = () => { if (LANGS.includes(ls.value)) loadLang(ls.value).catch(e => toast(errText(e), 'err')); };

  byId('securityCardBtn').onclick = async () => {
    try { await auth.load(); } catch (e) { toast(errText(e), 'err'); return; }
    clearPw();
    openSub('securitySubpage', { save: saveSecurity, back: clearPw });
  };
  byId('changePasswordBtn').onclick = async () => {
    try { if (await saveSecurity()) clearPw(); } catch (e) { toast(errText(e), 'err'); }
  };
  byId('logoutBtn').onclick = async () => {
    try { await api('POST', '/api/auth/logout'); } catch (e) { /* yine yenile */ }
    location.reload();
  };

  byId('resetApiCardBtn').onclick = openRemote;
  byId('resetApiEnabled').onchange = async e => {
    try { await api('PUT', '/api/config/remote', { enabled: e.target.checked }); toast(t('common.saved')); }
    catch (err) { e.target.checked = !e.target.checked; toast(errText(err), 'err'); }
  };
  byId('copyApiKeyBtn').onclick = async () => {
    try { await navigator.clipboard.writeText(byId('resetApiKey').value); toast(t('common.copied')); }
    catch (e) { byId('resetApiKey').select(); }
  };
  byId('refreshApiKeyBtn').onclick = async () => {
    if (!(await ask(t('confirm.new_key')))) return;
    try { showKey((await api('POST', '/api/remote/key')).key); toast(t('common.saved')); }
    catch (e) { toast(errText(e), 'err'); }
  };

  byId('otaFirmwareBtn').onclick = async () => {
    try { await otaInfo(); } catch (e) { toast(errText(e), 'err'); return; }
    openSub('hwOtaSubpage');
  };
  document.querySelectorAll("input[name='hwOtaSource']").forEach(r => {
    r.onchange = () => {
      otaSrc = r.value;
      show(byId('hwOtaWarning'), otaSrc === 'custom');
      show(byId('hwOtaCustomSection'), otaSrc === 'custom');
      show(byId('fwStartSection'), false);
    };
  });
  byId('checkFirmwareUpdateBtn').onclick = otaCheck;
  byId('startFirmwareUpdateBtn').onclick = otaStart;
  byId('fwOtaRestartBtn').onclick = () => restart('ota.confirm_restart');
  byId('fwRollbackBtn').onclick = async () => {
    if (!(await ask(t('ota.confirm_fw_rollback'), { danger: true }))) return;
    try { await api('POST', '/api/ota/rollback'); toast(t('ota.fw_rollback_success')); show(byId('fwRestartSection'), true); }
    catch (e) { toast(errText(e), 'err'); }
  };

  byId('rebootBtn').onclick = () => restart('confirm.restart');
  byId('factoryResetBtn').onclick = factory;

  onStatus(s => {
    st = s;
    byId('securityStatusText').textContent = t(s.auth ? 'security.status_on' : 'security.status_off');
    byId('securityStatusBox').classList.toggle('warning', !s.auth);
  });
  registerTab('panel-system', {});
}
