#!/usr/bin/env python3
"""
Arayüz ↔ cihaz gidiş-dönüş testi: her ayar arayüzden (gerçek tıklama/yazma) değiştirilir, Kaydet'e basılır,
sonra cihazdan (API) geri okunup karşılaştırılır. Butonlar, alt sayfalar, onay pencereleri, giriş ekranı dahil.

  python3 code/esp_bin/gui_roundtrip.py http://192.168.176.50 OUT_DIR

Dokunmaz: birincil WiFi (bağlantı kopar), fabrika ayarı (ayrı, geliştirici WiFi'li derlemeyle test edilir).
Başta mevcut yapılandırma alınır, sonda gizli olmayan alanlar geri yüklenir.
"""
import json
import os
import subprocess
import sys
import tempfile

HELPERS = r"""
window.H = {
  sleep: ms => new Promise(r => setTimeout(r, ms)),
  $: id => document.getElementById(id),
  set(id, v) {
    const e = H.$(id);
    if (e.type === 'checkbox') { if (e.checked !== v) e.click(); return; }
    e.value = v;
    e.dispatchEvent(new Event('input', { bubbles: true }));
    e.dispatchEvent(new Event('change', { bubbles: true }));
  },
  click(id) { H.$(id).click(); },
  async api(m, p, b) {
    for (let i = 0; i < 8; i++) {
      try {
        const o = { method: m, headers: { 'X-LS-Request': '1' } };
        if (b !== undefined) { o.headers['Content-Type'] = 'application/json'; o.body = JSON.stringify(b); }
        const r = await fetch(p, o);
        return { status: r.status, ...(await r.json()) };
      } catch (e) { await H.sleep(2000); }
    }
    return { status: 0 };
  },
  async cfg(s) { return (await H.api('GET', '/api/config/' + s)).data; },
  cmp(f, label, got, exp) {
    for (const k in exp) if (JSON.stringify(got && got[k]) !== JSON.stringify(exp[k]))
      f.push(label + '.' + k + ': ' + JSON.stringify(got && got[k]) + ' != ' + JSON.stringify(exp[k]));
  },
  open() { return H.$('popup').classList.contains('show'); },
  async tab(i) {
    while (H.$('footerCancelBtn').textContent && H.open() && H.$('settingsCarousel').style.display === 'none') { H.click('footerCancelBtn'); await H.sleep(200); }
    if (!H.open()) H.click('settingsBtn');
    document.querySelector('[data-index="' + i + '"]').click();
    document.querySelectorAll('.accordion').forEach(a => a.classList.add('open'));   // görünürlük denetimi için
    await H.sleep(2500);
  },
  async idle() { for (let i = 0; i < 120 && H.$('footerSaveBtn').disabled; i++) await H.sleep(250); },
  async save() { await H.idle(); H.click('footerSaveBtn'); await H.sleep(600); await H.idle(); await H.sleep(1500); },
  async ok() { await H.sleep(400); H.click('askOk'); await H.sleep(1500); },
  toasts() { return [...document.querySelectorAll('.toast')].map(t => t.className + ':' + t.textContent); },
  clearToasts() { document.querySelectorAll('.toast').forEach(t => t.remove()); },
  errToast() { return document.querySelector('.toast.err'); },
  vis(id) {                                   // fixed konumlu öğede offsetParent hep null: hesaplanmış stile bak
    const e = H.$(id);
    if (!e || e.classList.contains('hidden')) return false;
    for (let n = e; n && n !== document.body; n = n.parentElement) {
      const cs = getComputedStyle(n);
      if (cs.display === 'none' || cs.visibility === 'hidden' || n.classList.contains('hidden')) return false;
    }
    return true;
  },
};
return 'yardımcılar hazır';
"""


def step(name, body, wait=1):
    return [name, "(async()=>{const f=[];" + body + ";return f;})()", wait]


STEPS = [
    ["init", HELPERS, 0],
    step("baslangic", """
      window.ORIG = {};
      for (const s of ['timer','smtp','wifi_tx','device','telegram','early_mail','webhook','mqtt','relay','actions','auth','remote'])
        ORIG[s] = await H.cfg(s);
      const st = (await H.api('GET','/api/status')).data;
      if (!st) f.push('status okunamadı');
      if (H.$('timerSection').style.display === 'none' && H.$('logoImg').style.display === 'none') f.push('ana ekran boş');
    """),
    # ---------- ana ekran düğmeleri ----------
    step("buradayim", """
      H.click('restartBtn'); await H.sleep(2000);
      let s = (await H.api('GET','/api/status')).data;
      if (s.timer.state !== 'running') f.push('buradayım sonrası durum ' + s.timer.state);
      if (H.$('timerSection').style.display !== 'flex') f.push('halka görünmüyor');
      if (!H.$('timeHours').textContent) f.push('kalan süre yazmıyor');
      H.click('stopBtn'); await H.sleep(500);
      if (!H.$('askDlg').open) f.push('durdur onayı açılmadı');
      H.click('askCancel'); await H.sleep(1000);
      s = (await H.api('GET','/api/status')).data;
      if (s.timer.state !== 'running') f.push('iptal edilen onay yine durdurdu');
      H.click('stopBtn'); await H.ok();
      s = (await H.api('GET','/api/status')).data;
      if (s.timer.state !== 'stopped') f.push('durdur sonrası durum ' + s.timer.state);
      await H.sleep(500);
      if (H.$('logoImg').style.display !== 'block') f.push('durunca logo görünmüyor');
      if (!H.$('stopBtn').disabled) f.push('dururken durdur düğmesi etkin');
    """),
    # ---------- zamanlayıcı ----------
    step("zamanlayici", """
      await H.tab(0);
      if (H.$('timerValue').value != ORIG.timer.value) f.push('yüklenen süre ' + H.$('timerValue').value + ' != ' + ORIG.timer.value);
      document.querySelector('.unit-btn[data-unit="hour"]').click();
      H.set('timerValue', 5); H.set('alarmCount', 3); H.set('warnFirstAuto', false);
      if (!H.vis('warnFirstRow')) f.push('ilk uyarı satırı görünmüyor');
      H.set('warnFirstUnit', 'hour'); H.set('warnFirstValue', 2);
      const prev = H.$('alarmInfo').textContent;
      if (H.$('alarmInfo').classList.contains('bad')) f.push('geçerli plan kötü gösteriliyor: ' + prev);
      await H.save();
      H.cmp(f, 'timer', await H.cfg('timer'), { unit: 'hour', value: 5, warn_count: 3, warn_first_s: 7200 });
      const s = (await H.api('GET','/api/status')).data;
      H.cmp(f, 'plan', s.timer, { warn_plan: [7200, 2324, 750] });
      if (H.open()) f.push('kaydet sonrası pencere kapanmadı');
      await H.tab(0);
      H.set('timerValue', 1); document.querySelector('.unit-btn[data-unit="min"]').click(); H.set('alarmCount', 10);
      if (!H.$('alarmInfo').classList.contains('bad')) f.push('sığmayan plan uyarısı yok');
      H.clearToasts(); await H.save();
      if (!H.errToast()) f.push('geçersiz plan kaydı hata vermedi');
      H.click('footerCancelBtn'); await H.sleep(500);
      H.cmp(f, 'timer-korundu', await H.cfg('timer'), { unit: 'hour', value: 5 });
    """),
    step("tatil", """
      await H.tab(0);
      H.set('vacationMode', true);
      if (!H.vis('vacationDaysGroup')) f.push('tatil gün alanı görünmüyor');
      H.click('vacationIncrease'); H.set('vacationDays', 2);
      await H.save();
      let s = (await H.api('GET','/api/status')).data;
      H.cmp(f, 'tatil', s.timer, { state: 'vacation', vac_total: 172800 });
      if (!H.$('vacationIndicator').classList.contains('active')) f.push('tatil göstergesi yok');
      await H.tab(0);
      if (!H.vis('vacationActiveStatus')) f.push('tatil etkin metni yok');
      H.click('footerCancelBtn');
      H.click('stopBtn'); await H.sleep(1500);
      s = (await H.api('GET','/api/status')).data;
      if (s.timer.state === 'vacation') f.push('durdur tatili bitirmedi');
    """),
    # ---------- bağlantılar ----------
    step("smtp", """
      await H.tab(1);
      H.set('smtpServer', 'smtp.test.local'); H.set('smtpPort', 587); H.set('smtpUser', 'u@test.local');
      H.set('smtpApiKey', 'pw123456'); H.set('smtpTlsVerify', false);
      await H.save();
      H.cmp(f, 'smtp', await H.cfg('smtp'), { server: 'smtp.test.local', port: 587, user: 'u@test.local', pass: '***', tls_verify: false });
      await H.tab(1);
      if (H.$('smtpApiKey').placeholder.indexOf('***') >= 0) f.push('sır *** olarak görünüyor');
      const b = H.$('smtpApiKey').nextElementSibling;
      if (!b || !b.classList.contains('secret-clear') || b.hidden) f.push('Kaldır düğmesi yok');
      else b.click();
      await H.save();
      H.cmp(f, 'smtp-sil', await H.cfg('smtp'), { pass: '' });
    """, 1),
    step("wifi_birincil_okuma", """
      await H.tab(1);
      const p = await H.cfg('wifi_p');
      if (H.$('ssid').value !== p.ssid) f.push('birincil SSID gösterilmiyor');
      if (p.pass === '***' && H.$('pass').value !== '') f.push('birincil parola kutusu dolu');
      if (!H.$('st').textContent.includes(p.ssid)) f.push('WiFi durum satırı: ' + H.$('st').textContent);
      if (!H.$('wifiMdns').textContent.endsWith('.local')) f.push('mDNS yok');
      if (!H.$('apSsid').value.startsWith('LS-')) f.push('AP SSID yok');
      if (H.$('txPowerNow').textContent === '-') f.push('anlık TX yok');
      H.click('footerCancelBtn');
    """),
    step("wifi_yedek", """
      await H.tab(1);
      H.set('wifiBackupSsid', 'LS-Test-B'); H.set('wifiBackupPass', 'testpass9'); H.set('wifiBackupHidden', true);
      H.set('wifiBackupStaticIpEnabled', true);
      if (!H.vis('wifiBackupStaticIpFields')) f.push('statik IP alanları açılmadı');
      H.set('wifiBackupStaticIp', '192.168.176.250'); H.set('wifiBackupGateway', '192.168.176.1');
      H.set('wifiBackupSubnet', '255.255.255.0'); H.set('wifiBackupDns', '1.1.1.1');
      await H.save(); await H.sleep(12000);
      H.cmp(f, 'wifi_b', await H.cfg('wifi_b'), { ssid: 'LS-Test-B', pass: '***', hidden: true, ip: '192.168.176.250', gw: '192.168.176.1', mask: '255.255.255.0', dns: '1.1.1.1' });
      await H.tab(1);
      H.set('wifiBackupStaticIpEnabled', false); await H.save(); await H.sleep(12000);
      H.cmp(f, 'wifi_b-dhcp', await H.cfg('wifi_b'), { ip: '', gw: '', mask: '', dns: '' });
      const d = await H.api('DELETE', '/api/wifi/b'); await H.sleep(12000);
      H.cmp(f, 'wifi_b-sil', await H.cfg('wifi_b'), { ssid: '', pass: '' });
    """, 1),
    step("ap", """
      await H.tab(1);
      H.set('apPass', 'kisa'); H.clearToasts(); await H.save();
      if (!H.errToast()) f.push('kısa AP parolası reddedilmedi (arayüz)');
      H.click('footerCancelBtn'); await H.sleep(300);
      let r = await H.api('PUT', '/api/config/device', { ap_pass: 'kisa' });
      if (r.status !== 400) f.push('kısa AP parolası sunucuda kabul edildi');
      await H.tab(1);
      H.set('apPass', 'lstest1234'); await H.save(); await H.sleep(3000);
      H.cmp(f, 'ap', await H.cfg('device'), { ap_pass: '***' });
      await H.tab(1);
      H.$('apPass').nextElementSibling.click(); await H.save(); await H.sleep(3000);
      H.cmp(f, 'ap-sil', await H.cfg('device'), { ap_pass: '' });
      await H.tab(1);
      H.set('apModeEnabled', false);
      if (!H.vis('apModeWarning')) f.push('AP kapatma uyarısı görünmüyor');
      await H.save(); await H.sleep(4000);
      H.cmp(f, 'ap-kapali', await H.cfg('device'), { ap_enabled: false });
      if ((await H.api('GET','/api/status')).data.wifi.ap_on) f.push('AP hâlâ açık');
      await H.tab(1); H.set('apModeEnabled', true); await H.save(); await H.sleep(4000);
      H.cmp(f, 'ap-acik', await H.cfg('device'), { ap_enabled: true });
    """, 1),
    step("tx_gucu", """
      await H.tab(1);
      H.set('txPowerMax', 15);
      if (H.$('txPowerMaxValue').textContent !== '15') f.push('TX etiketi güncellenmedi');
      H.set('txPowerAdaptive', false); await H.save();
      H.cmp(f, 'wifi_tx', await H.cfg('wifi_tx'), { max_q: 60, adaptive: false });
      await H.api('PUT', '/api/config/wifi_tx', ORIG.wifi_tx);
    """),
    # ---------- eylemler ----------
    step("kartlar", """
      await H.tab(2);
      const card = document.querySelector('[data-action="early-telegram"]');
      const before = card.dataset.active;
      card.querySelector('.action-card-main').click(); await H.sleep(2000);
      let a = await H.cfg('actions');
      if (String(a.early_telegram) === before) f.push('telegram kartı kaydedilmedi');
      if (card.dataset.active === before) f.push('kart görünümü değişmedi');
      card.querySelector('.action-card-main').click(); await H.sleep(2000);
      a = await H.cfg('actions');
      if (String(a.early_telegram) !== before) f.push('telegram kartı geri alınamadı');
      for (const c of document.querySelectorAll('.action-card')) {
        const map = { 'early-telegram': ['early_telegram'], 'early-mail': ['early_mail'], 'early-api': ['early_webhook','early_mqtt'],
                      'trigger-mail': ['trig_mail'], 'trigger-api': ['trig_webhook','trig_mqtt'], 'trigger-relay': ['trig_relay'] }[c.dataset.action];
        if (String(map.some(k => a[k])) !== c.dataset.active) f.push('kart ' + c.dataset.action + ' cihazla uyuşmuyor');
      }
    """),
    step("telegram", """
      await H.tab(2);
      document.querySelector('[data-config="telegram"]').click(); await H.sleep(1500);
      if (!H.vis('telegramConfigView')) f.push('telegram görünümü açılmadı');
      H.set('telegramToken', '123:abc'); H.set('telegramChat', '-100'); H.set('telegramMessage', 'msg {status}');
      await H.save();
      H.cmp(f, 'telegram', await H.cfg('telegram'), { token: '***', chat_id: '-100', message: 'msg {status}' });
      if (H.vis('telegramConfigView')) f.push('kaydet sonrası alt sayfa kapanmadı');
    """),
    step("erken_posta", """
      document.querySelector('[data-config="early-mail"]').click(); await H.sleep(1500);
      H.set('earlyMailRecipient', 'a@b.cd'); H.set('earlyMailSubject', 'Konu'); H.set('earlyReminderMessage', 'Satır 1\\nSatır 2');
      await H.save();
      H.cmp(f, 'early_mail', await H.cfg('early_mail'), { email: 'a@b.cd', subject: 'Konu', body: 'Satır 1\\nSatır 2' });
    """),
    step("webhook_mqtt", """
      document.querySelector('[data-config="early-api"]').click(); await H.sleep(2000);
      if (H.$('webhookUrl').value !== ORIG.webhook.url) f.push('webhook URL yüklenmedi');
      H.set('webhookHeaders', '[1]'); H.clearToasts(); await H.save();
      if (!H.errToast()) f.push('geçersiz başlık reddedilmedi');
      let r = await H.api('PUT', '/api/config/webhook', { headers: 'x' });
      if (r.status !== 400) f.push('geçersiz başlık sunucuda kabul edildi');
      H.set('webhookHeaders', '{"X-T":"1"}'); H.set('webhookMethod', 'PUT'); H.set('webhookBody', '{"s":"{status}"}');
      H.set('mqttTopic', 'ls/test'); H.set('mqttPayload', '{"a":1}');
      await H.save();
      H.cmp(f, 'webhook', await H.cfg('webhook'), { headers: '{"X-T":"1"}', method: 'PUT', body: '{"s":"{status}"}' });
      H.cmp(f, 'mqtt', await H.cfg('mqtt'), { topic: 'ls/test', payload: '{"a":1}' });
    """),
    step("role", """
      document.querySelector('[data-config="relay"]').click(); await H.sleep(1500);
      H.set('relayInverted', true);
      if (!H.$('relayIdleState').textContent || H.$('relayIdleState').textContent === H.$('relayActiveState').textContent) f.push('röle durum metinleri');
      H.set('relayDelay', '2'); H.set('relayDurationUnit', 'seconds'); H.set('relayDurationValue', 30);
      H.set('relayPulseEnabled', true); H.set('relayPulseDuration', '5');
      if (!H.vis('relayCycleInfo') || !H.$('relayCycleText').textContent.includes('3')) f.push('döngü metni: ' + H.$('relayCycleText').textContent);
      await H.save();
      H.cmp(f, 'relay', await H.cfg('relay'), { inverted: true, delay_s: 2, duration_s: 30, pulse_s: 5 });
      document.querySelector('[data-config="relay"]').click(); await H.sleep(1500);
      if (H.$('relayDurationValue').value !== '30' || H.$('relayDurationUnit').value !== 'seconds') f.push('röle süresi geri yüklenmedi');
      H.set('relayDurationUnit', 'minutes'); H.set('relayDurationValue', 2); await H.save();
      H.cmp(f, 'relay-dk', await H.cfg('relay'), { duration_s: 120 });
      document.querySelector('[data-config="relay"]').click(); await H.sleep(1500);
      H.set('relayInverted', false); H.set('relayDelay', '0'); H.set('relayDurationUnit', 'none'); H.set('relayPulseEnabled', false);
      H.clearToasts(); H.click('testRelayBtn'); await H.sleep(1500);
      const s = (await H.api('GET','/api/status')).data;
      if (!s.relay.on) f.push('röle testi röleyi açmadı');
      if (!H.toasts().join().includes('ok:')) f.push('röle testi bildirimi yok');
      H.cmp(f, 'relay-geri', await H.cfg('relay'), { inverted: false, delay_s: 0, duration_s: 0, pulse_s: 0 });
      H.click('footerCancelBtn'); await H.sleep(6000);
      if ((await H.api('GET','/api/status')).data.relay.on) f.push('röle testi 5 sn sonra kapanmadı');
    """),
    step("posta_gruplari", """
      document.querySelector('[data-config="trigger-mail"]').click(); await H.sleep(2000);
      if (!H.vis('triggerMailConfigView')) f.push('grup listesi açılmadı');
      H.click('createMailGroup'); await H.sleep(800);
      H.set('mailGroupName', 'Test G'); H.set('mailGroupSubject', 'Konu G'); H.set('mailGroupContent', 'Gövde');
      H.set('newRecipientEmail', 'bad'); H.clearToasts(); H.click('addRecipientBtn');
      if (!H.errToast()) f.push('geçersiz alıcı reddedilmedi');
      H.set('newRecipientEmail', 'x@y.zz'); H.click('addRecipientBtn');
      H.set('newRecipientEmail', 'q@w.ee'); H.click('addRecipientBtn');
      if (H.$('mailGroupRecipients').children.length !== 2) f.push('alıcı listesi ' + H.$('mailGroupRecipients').children.length);
      H.$('mailGroupRecipients').children[1].querySelector('button').click();
      H.click('mailGroupSaveBtn'); await H.sleep(2500);
      const gs = (await H.api('GET', '/api/groups')).data;
      const g = gs.find(x => x.name === 'Test G');
      if (!g) { f.push('grup kaydedilmedi'); return f; }
      const gd = (await H.api('GET', '/api/groups/' + g.id)).data;
      H.cmp(f, 'grup', gd, { subject: 'Konu G', body: 'Gövde', recipients: 'x@y.zz' });
      const item = [...document.querySelectorAll('.mail-group-item')].find(i => i.textContent.includes('Test G'));
      if (!item) { f.push('grup listede yok'); return f; }
      item.querySelector('button').click(); await H.sleep(2000);
      const dt = new DataTransfer();
      dt.items.add(new File(['merhaba dünya'], 'not ğ.txt', { type: 'text/plain' }));
      H.$('mailGroupFile').files = dt.files; H.$('mailGroupFile').dispatchEvent(new Event('change'));
      await H.sleep(4000);
      const files = (await H.api('GET', '/api/groups/' + g.id + '/files')).data.files;
      if (files.length !== 1 || files[0].name !== 'not g.txt') f.push('ek: ' + JSON.stringify(files));
      if (H.$('mailGroupFileList').children.length !== 1) f.push('ek listesi arayüzde yok');
      else { H.$('mailGroupFileList').querySelector('.recipient-remove').click(); await H.sleep(2500); }
      if ((await H.api('GET', '/api/groups/' + g.id + '/files')).data.files.length) f.push('ek silinmedi');
      H.click('mailGroupDeleteBtn'); await H.ok(); await H.sleep(1000);
      if ((await H.api('GET', '/api/groups')).data.some(x => x.name === 'Test G')) f.push('grup silinmedi');
      if (!H.vis('triggerMailConfigView')) f.push('silince listeye dönmedi');
    """),
    # ---------- sistem ----------
    step("guvenlik", """
      await H.tab(3);
      H.click('securityCardBtn'); await H.sleep(1500);
      if (!H.$('securityStatusText').textContent || H.$('securityStatusText').textContent === '-') f.push('güvenlik durumu boş');
      H.set('autoLogoutTime', 15); H.set('loginProtection', false); H.set('lockoutTime', 20);
      await H.save();
      H.cmp(f, 'auth', await H.cfg('auth'), { ttl_min: 15, protect: false, lock_min: 20 });
      H.click('securityCardBtn'); await H.sleep(1500);
      H.set('newPassword', 'test1234'); H.set('confirmPassword', 'test1235'); H.clearToasts();
      H.click('changePasswordBtn'); await H.sleep(800);
      if (!H.errToast()) f.push('uyuşmayan parola kabul edildi');
      H.set('confirmPassword', 'test1234'); H.click('changePasswordBtn'); await H.sleep(2500);
      if (!(await H.api('GET','/api/auth')).data.enabled) f.push('parola ayarlanmadı');
      await H.sleep(2000);
      if (!H.vis('loginOverlay')) f.push('oturum düşünce giriş ekranı açılmadı');
      H.set('loginPassword', 'yanlis'); H.click('loginBtn'); await H.sleep(1500);
      if (!H.vis('loginError')) f.push('yanlış parola hatası görünmüyor');
      H.set('loginPassword', 'test1234'); H.click('loginBtn'); await H.sleep(2500);
      if (H.vis('loginOverlay')) f.push('doğru parolayla giriş olmadı');
      await H.tab(3); H.click('securityCardBtn'); await H.sleep(1500);
      H.set('currentPassword', 'test1234'); H.click('changePasswordBtn'); await H.ok(); await H.sleep(1500);
      if ((await H.api('GET','/api/auth')).data.enabled) f.push('parola kaldırılamadı');
      await H.api('PUT', '/api/config/auth', { ttl_min: ORIG.auth.ttl_min, protect: ORIG.auth.protect, lock_min: ORIG.auth.lock_min });
      if (H.vis('loginOverlay')) { H.set('loginPassword', ''); }
    """, 1),
    step("uzaktan", """
      await H.tab(3);
      H.click('resetApiCardBtn'); await H.sleep(1500);
      if (!H.vis('resetApiSubpage')) f.push('uzaktan sayfası açılmadı');
      H.set('resetApiEnabled', true); await H.sleep(1500);
      if (!(await H.cfg('remote')).enabled) f.push('uzaktan etkinleştirme kaydedilmedi');
      H.click('refreshApiKeyBtn'); await H.ok(); await H.sleep(1000);
      const key = H.$('resetApiKey').value;
      if (!/^[A-Z2-7]{26}$/.test(key)) f.push('anahtar biçimi: ' + key);
      if (!document.querySelector('#resetApiSubpage .code-block').textContent.includes(key)) f.push('örnek uçta anahtar yok');
      await H.api('POST', '/api/timer/cancel');
      let r = await fetch('/api/reset?key=' + key); let j = await r.json();
      if (!j.ok) f.push('doğru anahtar reddedildi ' + r.status);
      await H.sleep(1000);
      if ((await H.api('GET','/api/status')).data.timer.state !== 'running') f.push('uzaktan sıfırlama sayacı başlatmadı');
      r = await fetch('/api/reset?key=AAAAAAAAAAAAAAAAAAAAAAAAAA');
      if (r.status !== 403) f.push('yanlış anahtar ' + r.status);
      H.set('resetApiEnabled', false); await H.sleep(1500);
      r = await fetch('/api/reset?key=' + key);
      if (r.status !== 403) f.push('kapalıyken anahtar kabul edildi ' + r.status);
      await H.api('POST', '/api/timer/cancel');
    """),
    step("guncelleme_sayfasi", """
      await H.tab(3);
      H.click('otaFirmwareBtn'); await H.sleep(1500);
      if (!H.$('currentFirmwareVersion').textContent.startsWith('v1.')) f.push('sürüm: ' + H.$('currentFirmwareVersion').textContent);
      if (!H.$('runningPartition').textContent.startsWith('ota_')) f.push('bölüm: ' + H.$('runningPartition').textContent);
      document.querySelector('input[name="hwOtaSource"][value="custom"]').click(); await H.sleep(200);
      if (!H.vis('hwOtaCustomSection') || !H.vis('hwOtaWarning')) f.push('özel kaynak alanları açılmadı');
      document.querySelector('input[name="hwOtaSource"][value="official"]').click();
      H.clearToasts(); H.click('checkFirmwareUpdateBtn'); await H.sleep(500);
      if (!H.$('checkFirmwareUpdateBtn').disabled) f.push('denetim sürerken düğme kilitlenmedi');
      for (let i = 0; i < 60 && H.$('checkFirmwareUpdateBtn').disabled; i++) await H.sleep(500);
      const o = (await H.api('GET', '/api/ota')).data;
      if (!['error', 'no_update', 'available'].includes(o.state)) f.push('denetim sonucu: ' + o.state);
      if (!H.toasts().length && !o.available) f.push('denetim sonucu kullanıcıya bildirilmedi');
      if (o.available && !H.vis('fwStartSection')) f.push('yeni sürüm var ama Kur düğmesi yok');
      H.click('footerCancelBtn');
    """),
    step("tema_dil", """
      await H.tab(3);
      H.set('themeSelect', 'light');
      if (!document.body.classList.contains('light-theme')) f.push('açık tema uygulanmadı');
      H.set('themeSelect', 'dark');
      if (document.body.classList.contains('light-theme')) f.push('koyu temaya dönmedi');
      H.set('languageSelect', 'de'); await H.sleep(2500);
      if (document.documentElement.lang !== 'de' || H.$('footerCancelBtn').textContent === 'footer.cancel') f.push('dil değişmedi');
      const de = document.querySelector('[data-index="0"]').textContent;
      H.set('languageSelect', 'tr'); await H.sleep(2500);
      if (document.querySelector('[data-index="0"]').textContent === de) f.push('dil geri dönmedi');
      if (document.body.innerText.match(/\\b[a-z_]+\\.[a-z_]+\\b/g)?.filter(k => /^(common|alert|err|timer|warn|mail|wifi|device|logs|ota|security|relay|api|system|footer|home|state|vacation|factory|ap)\\./.test(k)).length)
        f.push('çevrilmemiş anahtar görünüyor');
      H.click('factoryResetBtn'); await H.sleep(600);
      if (document.querySelectorAll('#askChoices input').length !== 2) f.push('fabrika ayarı seçenekleri yok');
      H.click('askCancel'); await H.sleep(300);
      H.click('footerCancelBtn');
    """),
    # ---------- bilgi ----------
    step("cihaz_bilgisi", """
      await H.tab(4);
      document.querySelector('[data-subpage="deviceInfoSubpage"]').click(); await H.sleep(3000);
      for (const id of ['sysDeviceId','sysChip','sysCores','sysCpuFreq','sysFlashSize','sysRamTotal','sysRamFree','sysRamMinFree',
                        'ramUsagePercent','sysIntFlashTotal','sysIntFlashUsed','sysIntFlashFree','sysLogVol','sysUserData','sysWifiStatus',
                        'sysWifiSsid','sysRssi','sysIp','sysMdns','sysApStatus','sysApClients','sysTxPower','sysUptime','sysBootCount',
                        'sysResetReason','sysPrevUptime','sysFirmware','sysCoredump'])
        if (!H.$(id).textContent || H.$(id).textContent === '-' || /undefined|NaN|null/.test(H.$(id).textContent)) f.push(id + ' = ' + H.$(id).textContent);
      if (!H.$('sysExtFlashSize').textContent.includes('16')) f.push('harici flash boyutu: ' + H.$('sysExtFlashSize').textContent);
      return f.concat([]);
    """),
    step("gunlukler", """
      await H.tab(4);
      document.querySelector('[data-subpage="logsSubpage"]').click(); await H.sleep(3500);
      const n = H.$('logList').querySelectorAll('.log-item').length;
      if (n < 3) f.push('günlük satırı ' + n);
      for (const id of ['logBootCount','logLastCause','logShortBoots']) if (H.$(id).textContent === '-') f.push(id + ' boş');
      if (/undefined|NaN/.test(H.$('logList').textContent)) f.push('günlükte undefined/NaN');
      for (const b of document.querySelectorAll('.log-filter-btn')) { b.click(); await H.sleep(100); }
      document.querySelector('.log-filter-btn[data-filter="wifi"]').click();
      const w = [...H.$('logList').querySelectorAll('.log-item:not(.log-boot)')].filter(i => !i.className.includes('log-wifi'));
      if (w.length && H.$('logList').querySelectorAll('.log-wifi').length) f.push('wifi süzgeci başka kayıt gösteriyor');
      document.querySelector('.log-filter-btn[data-filter="all"]').click();
      H.click('logRefreshBtn'); await H.sleep(2500);
      H.click('logClearBtn'); await H.ok(); await H.sleep(2500);
      const after = H.$('logList').querySelectorAll('.log-item').length;
      if (after > 3) f.push('temizlik sonrası ' + after + ' satır');
      H.click('footerCancelBtn'); H.click('footerCancelBtn');
    """),
    # ---------- geri yükleme ----------
    step("geri_yukle", """
      for (const s of ['timer','smtp','telegram','early_mail','webhook','mqtt','relay','actions']) {
        const o = {}; for (const k in ORIG[s]) if (ORIG[s][k] !== '***') o[k] = ORIG[s][k];
        const r = await H.api('PUT', '/api/config/' + s, o);
        if (!r.ok) f.push('geri yükleme ' + s + ': ' + JSON.stringify(r.error));
      }
      await H.api('POST', '/api/timer/cancel');
    """),
    step("yeniden_baslat", """
      await H.tab(3);
      window.UP0 = (await H.api('GET','/api/status')).data.uptime;
      localStorage.setItem('rt_up0', UP0);
      H.click('rebootBtn'); await H.ok();
    """, 25),
    ["init2", HELPERS, 0],
    step("yeniden_baslat_dogrula", """
      const up0 = +localStorage.getItem('rt_up0');
      const s = (await H.api('GET','/api/status')).data;
      if (!s || s.uptime >= up0) f.push('yeniden başlamadı (uptime ' + (s && s.uptime) + ')');
      const d = (await H.api('GET','/api/device')).data;
      if (d.last_cause !== 'user_restart') f.push('açılış nedeni ' + d.last_cause);
      if (H.$('timerSection').style.display === 'none' && H.$('logoImg').style.display === 'none') f.push('yenilenen sayfa boş');
    """),
]


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as fh:
        json.dump(STEPS, fh, ensure_ascii=False)
    here = os.path.dirname(os.path.abspath(__file__))
    # bilinçli olumsuz denemeler (400/401/403/429) ve WiFi/AP değişiminde SSE akışının kopması beklenir;
    # işlevsel sonuçları adımlar ayrıca denetler. 5xx ve diğer ağ hataları sayılmaya devam eder.
    allow = r"status of 4\d\d |ERR_CONNECTION_RESET http://[^ ]+/api/events"
    r = subprocess.run([sys.executable, os.path.join(here, "gui_check.py"), sys.argv[1], sys.argv[2], "--steps", fh.name,
                        "--allow-net", allow])
    os.unlink(fh.name)
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
