// Bağlantılar sekmesi: SMTP, WiFi (birincil + yedek, gizli ağ, statik IP), AP, WiFi TX gücü.
// Kaydet yalnız değişen bölümleri gönderir; testler önce kaydeder.
import { api } from '../api.js';
import { Section, saveAll } from '../form.js';
import { t } from '../i18n.js';
import { registerTab } from '../nav.js';
import { onStatus } from '../state.js';
import { byId, runJob, show } from '../ui.js';

export const smtp = new Section('smtp', {
  server: 'smtpServer', port: 'smtpPort', user: 'smtpUser', pass: 'smtpApiKey', tls_verify: 'smtpTlsVerify',
});

// statik IP anahtarı: açıkken dört alan gönderilir, kapalıyken hepsi boş
function wifiSection(name, p) {
  const ids = { ip: p.ip, gw: p.gw, mask: p.mask, dns: p.dns };
  const fields = { ssid: p.ssid, pass: p.pass, hidden: p.hidden };
  for (const k in ids) {
    fields[k] = {
      get: () => (byId(p.toggle).checked ? byId(ids[k]).value.trim() : ''),
      set: v => { byId(ids[k]).value = v || ''; },
    };
  }
  const s = new Section(name, fields);
  const set = s.set.bind(s);
  s.set = d => {
    byId(p.toggle).checked = !!d.ip;
    show(byId(p.box), !!d.ip);
    set(d);
  };
  byId(p.toggle).onchange = () => show(byId(p.box), byId(p.toggle).checked);
  return s;
}

const wifiP = wifiSection('wifi_p', {
  ssid: 'ssid', pass: 'pass', hidden: 'wifiHidden', toggle: 'wifiStaticIpEnabled', box: 'wifiStaticIpFields',
  ip: 'sip', gw: 'wifiGateway', mask: 'wifiSubnet', dns: 'wifiDns',
});
const wifiB = wifiSection('wifi_b', {
  ssid: 'wifiBackupSsid', pass: 'wifiBackupPass', hidden: 'wifiBackupHidden', toggle: 'wifiBackupStaticIpEnabled',
  box: 'wifiBackupStaticIpFields', ip: 'wifiBackupStaticIp', gw: 'wifiBackupGateway', mask: 'wifiBackupSubnet',
  dns: 'wifiBackupDns',
});
const device = new Section('device', { ap_enabled: 'apModeEnabled', ap_pass: 'apPass' });
const tx = new Section('wifi_tx', {
  max_q: { get: () => +byId('txPowerMax').value * 4, set: v => { byId('txPowerMax').value = Math.round((v || 66) / 4); txLabel(); } },
  adaptive: 'txPowerAdaptive',
});

function txLabel() { byId('txPowerMaxValue').textContent = byId('txPowerMax').value; }

function accState(id, configured) { byId(id).classList.toggle('configured', configured); }

function renderStatus(s) {
  const w = s.wifi, st = byId('st');
  const up = w.state === 'connected';
  st.textContent = up ? t('wifi.status_connected', { ssid: w.ssid, ip: w.ip }) : w.ap_on ? t('wifi.status_ap') : t('wifi.not_connected');
  st.className = 'status' + (up ? ' ok' : '');
  const mdns = s.device.toLowerCase().slice(0, 15) + '.local';
  byId('wifiMdns').textContent = mdns;
  byId('wifiBackupMdns').textContent = mdns;
  byId('apSsid').value = s.device;
  byId('apMdns').value = mdns;
}

function apWarn() {
  show(byId('apModeWarning'), !byId('apModeEnabled').checked);
}

export function initConn() {
  byId('apModeEnabled').addEventListener('change', apWarn);
  byId('txPowerMax').addEventListener('input', txLabel);
  byId('smtpTestBtn').onclick = () => runJob(async () => {
    await smtp.save();
    return api('POST', '/api/test/smtp');
  }, byId('smtpStatus'), id => api('GET', '/api/jobs/' + id));
  onStatus(renderStatus);
  registerTab('panel-conn', {
    show: async () => {
      for (const s of [smtp, wifiP, wifiB, device, tx]) await s.load();
      const w = await api('GET', '/api/wifi');
      byId('txPowerNow').textContent = (w.tx_q / 4).toFixed(1) + ' dBm';
      accState('smtpAccordion', !!smtp.data.server);
      accState('wifiAccordion', !!wifiP.data.ssid);
      accState('wifiBackupAccordion', !!wifiB.data.ssid);
      apWarn();
    },
    // AP en son: WiFi bağlı değilse sunucu AP kapatmayı reddeder (wifi_not_connected)
    save: () => {
      const ap = byId('apPass').value;
      if (ap && ap.length < 8) throw new Error(t('err.ap_pass'));
      return saveAll([smtp, wifiP, wifiB, tx, device]);
    },
  });
}
