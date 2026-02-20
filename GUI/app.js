// ============================================================
// i18n - Cok dilli destek
// ============================================================
var currentLang = localStorage.getItem('ls_lang') || 'en';
var langData = {};

function t(key, params) {
  var text = langData[key] || key;
  if (params) {
    var keys = Object.keys(params);
    for (var i = 0; i < keys.length; i++) {
      text = text.replace('{' + keys[i] + '}', params[keys[i]]);
    }
  }
  return text;
}

function applyLanguage(lang) {
  fetch('/ext/lang/' + lang + '.json?v=' + Date.now())
    .then(function(r) { return r.json(); })
    .then(function(data) {
      langData = data;
      currentLang = lang;
      localStorage.setItem('ls_lang', lang);
      // data-i18n -> textContent
      var els = document.querySelectorAll('[data-i18n]');
      for (var i = 0; i < els.length; i++) {
        var key = els[i].getAttribute('data-i18n');
        if (langData[key]) els[i].textContent = langData[key];
      }
      // data-i18n-ph -> placeholder
      var phs = document.querySelectorAll('[data-i18n-ph]');
      for (var j = 0; j < phs.length; j++) {
        var pk = phs[j].getAttribute('data-i18n-ph');
        if (langData[pk]) phs[j].placeholder = langData[pk];
      }
      // Dinamik icerik guncelle
      updateFooterButtons();
      updateTimerStatus();
      updateAlarmInfo();
      updateVacationUI();
      updateRelayCycleInfo();
    })
    .catch(function() {});
}

// ============================================================
// Modal ac/kapa
// ============================================================
var overlay = document.getElementById('overlay');
var popup = document.getElementById('popup');
var settingsBtn = document.getElementById('settingsBtn');

function openPopup() {
  overlay.className = 'modal-overlay show';
  popup.className = 'modal show';
  vacationToggleDirty = false;
  fetchTimerState();
}

function closePopup() {
  // Subpage aciksa once kapat
  if (infoSubpageActive) {
    closeInfoSubpage();
  }
  overlay.className = 'modal-overlay';
  popup.className = 'modal';
}

if (settingsBtn) settingsBtn.addEventListener('click', openPopup);
if (overlay) overlay.addEventListener('click', closePopup);

// ============================================================
// Footer butonlari (Iptal / Kaydet)
// ============================================================
var footerCancelBtn = document.getElementById('footerCancelBtn');
var footerSaveBtn = document.getElementById('footerSaveBtn');
var actionSubpageActive = false;
var currentActionView = '';

// Footer buton metnini guncelle (subpage acikken Geri, ana sayfada Iptal)
function updateFooterButtons() {
  if (!footerCancelBtn) return;
  if (infoSubpageActive || systemSubpageActive || actionSubpageActive) {
    footerCancelBtn.textContent = t('footer.back');
  } else {
    footerCancelBtn.textContent = t('footer.cancel');
  }
}

if (footerCancelBtn) {
  footerCancelBtn.addEventListener('click', function() {
    if (infoSubpageActive) {
      closeInfoSubpage();
    } else if (systemSubpageActive) {
      closeSystemSubpage();
    } else if (actionSubpageActive) {
      // mailGroupDetailView aciksa once oraya don
      var detailView = document.getElementById('mailGroupDetailView');
      if (detailView && !detailView.classList.contains('hidden')) {
        closeMailGroupDetail();
      } else {
        closeActionSubpage();
      }
    } else {
      closePopup();
    }
  });
}

if (footerSaveBtn) {
  footerSaveBtn.addEventListener('click', function() {
    if (actionSubpageActive) {
      if (currentActionView === 'telegramConfigView') saveTelegramConfig();
      else if (currentActionView === 'earlyMailConfigView') saveEarlyMailConfig();
      else closeActionSubpage();
    } else {
      saveAllSettings();
    }
  });
}

// ============================================================
// Carousel Tab Navigasyonu
// ============================================================
var currentTab = 0;
var navItems = document.querySelectorAll('.nav-item');
var panels = document.querySelectorAll('.settings-panel');
var carouselItemsEl = document.getElementById('carouselItems');
var carouselPrev = document.getElementById('carouselPrev');
var carouselNext = document.getElementById('carouselNext');
var tabCount = navItems.length;

function updateCarousel() {
  // Nav item durumlarini guncelle
  for (var i = 0; i < navItems.length; i++) {
    if (i === currentTab) {
      navItems[i].className = 'nav-item active';
    } else {
      navItems[i].className = 'nav-item';
    }
  }

  // Panel goster/gizle
  for (var j = 0; j < panels.length; j++) {
    if (j === currentTab) {
      panels[j].className = 'settings-panel active';
    } else {
      panels[j].className = 'settings-panel';
    }
  }

  // Subpage aciksa kapat (baska sekmeye gecildi)
  if (infoSubpageActive) {
    closeInfoSubpage();
  }

  // Carousel kaydirma (aktif ortada olacak sekilde)
  if (carouselItemsEl) {
    var track = carouselItemsEl.parentElement;
    var trackWidth = track ? track.offsetWidth : 240;
    // Aktif elementin merkezini track merkezine hizala
    var activeEl = navItems[currentTab];
    if (activeEl) {
      var activeCenter = activeEl.offsetLeft + (activeEl.offsetWidth / 2);
      var offset = (trackWidth / 2) - activeCenter;
      carouselItemsEl.style.transform = 'translateX(' + offset + 'px)';
    }
  }
}

// Ok butonlari (sonsuz dongude)
if (carouselPrev) {
  carouselPrev.addEventListener('click', function() {
    currentTab = (currentTab - 1 + tabCount) % tabCount;
    updateCarousel();
  });
}

if (carouselNext) {
  carouselNext.addEventListener('click', function() {
    currentTab = (currentTab + 1) % tabCount;
    updateCarousel();
  });
}

// Nav item tiklandiginda
for (var ni = 0; ni < navItems.length; ni++) {
  navItems[ni].addEventListener('click', function() {
    var idx = parseInt(this.getAttribute('data-index'));
    currentTab = idx;
    updateCarousel();
  });
}

// Baslangicta ilk sekmeyi goster
updateCarousel();

// ============================================================
// Accordion aç/kapa
// ============================================================
var accordionHeaders = document.querySelectorAll('.accordion-header');
for (var ai = 0; ai < accordionHeaders.length; ai++) {
  accordionHeaders[ai].addEventListener('click', function() {
    var accordion = this.parentElement;
    if (accordion.className.indexOf('open') >= 0) {
      accordion.className = accordion.className.replace(' open', '');
    } else {
      accordion.className += ' open';
    }
  });
}

// ============================================================
// Statik IP toggle (WiFi + WiFi Yedek)
// ============================================================
var wifiStaticToggle = document.getElementById('wifiStaticIpEnabled');
if (wifiStaticToggle) {
  wifiStaticToggle.addEventListener('change', function() {
    var fields = document.getElementById('wifiStaticIpFields');
    if (fields) {
      fields.className = this.checked ? 'static-ip-fields' : 'static-ip-fields hidden';
    }
  });
}

var wifiBackupStaticToggle = document.getElementById('wifiBackupStaticIpEnabled');
if (wifiBackupStaticToggle) {
  wifiBackupStaticToggle.addEventListener('change', function() {
    var fields = document.getElementById('wifiBackupStaticIpFields');
    if (fields) {
      fields.className = this.checked ? 'static-ip-fields' : 'static-ip-fields hidden';
    }
  });
}

// ============================================================
// AP Mod toggle uyari
// ============================================================
var apModeToggle = document.getElementById('apModeEnabled');
if (apModeToggle) {
  apModeToggle.addEventListener('change', function() {
    var warning = document.getElementById('apModeWarning');
    if (warning) {
      warning.className = this.checked ? 'warning-banner hidden' : 'warning-banner';
    }
    // Akordiyon renk degisimi (beyaz = aktif, gri = pasif)
    var apAcc = document.getElementById('apModeAccordion');
    if (apAcc) {
      apAcc.className = this.checked ? 'accordion configured open' : 'accordion open';
    }
  });
}

// ============================================================
// Sistem bilgisi yukle
// ============================================================
function loadInfo() {
  fetch('/api/system/info')
    .then(function(r) { return r.json(); })
    .then(function(d) {
      // AP Mod bilgileri (accordion icinde)
      var apSsid = document.getElementById('apSsid');
      if (apSsid) apSsid.value = d.ap_ssid;
      var apMdns = document.getElementById('apMdns');
      if (apMdns) apMdns.value = d.mdns;

      // WiFi baglanti durumu
      var st = document.getElementById('st');
      if (st) {
        if (d.connected) {
          st.textContent = t('wifi.status_connected', {ssid: d.sta_ssid, ip: d.ip});
          st.className = 'status ok';
        } else {
          st.textContent = t('wifi.status_ap');
          st.className = 'status';
        }
      }

      // WiFi mDNS bilgileri
      var wifiMdns = document.getElementById('wifiMdns');
      if (wifiMdns) wifiMdns.textContent = d.mdns;
      var wifiBackupMdns = document.getElementById('wifiBackupMdns');
      if (wifiBackupMdns) wifiBackupMdns.textContent = d.mdns;

      // Info sekmesi - deviceInfoSubpage baglanti bilgileri
      var el;
      el = document.getElementById('sysDeviceId');
      if (el) el.textContent = d.device_name;
      el = document.getElementById('sysWifiStatus');
      if (el) {
        el.textContent = d.connected ? t('wifi.connected') : t('wifi.not_connected');
        el.className = d.connected ? 'system-value status-connected' : 'system-value status-disconnected';
      }
      el = document.getElementById('sysWifiSsid');
      if (el) el.textContent = d.connected ? d.sta_ssid : '-';
      el = document.getElementById('sysIp');
      if (el) el.textContent = d.ip;

      // Flash bilgileri
      if (d.ext_flash) {
        el = document.getElementById('sysSlotA');
        if (el) el.textContent = (d.slot_a_total - d.slot_a_free) + ' KB / ' + d.slot_a_total + ' KB';
        el = document.getElementById('sysUserData');
        if (el) el.textContent = t('device.free_of_total', {free: d.user_data_free, total: d.user_data_total});
      }

      // Sistem sekmesi
      var sysInfo = document.getElementById('sysInfo');
      if (sysInfo) {
        var s = '<div class="info-grid">';
        s += '<span class="lbl">' + t('device.id') + ':</span><span>' + d.device_name + '</span>';
        s += '<span class="lbl">' + t('device.wifi_status') + ':</span><span>' + (d.connected ? t('wifi.connected') + ' (' + d.sta_ssid + ')' : t('device.ap_mode')) + '</span>';
        s += '<span class="lbl">' + t('device.ip') + ':</span><span>' + d.ip + '</span>';
        s += '<span class="lbl">' + t('device.mdns') + ':</span><span>' + d.mdns + '</span>';
        s += '</div>';
        sysInfo.innerHTML = s;
      }
    })
    .catch(function() {});
}

loadInfo();
setInterval(loadInfo, 5000);

// ============================================================
// SMTP Ayarlari
// ============================================================
function loadSmtpConfig() {
  fetch('/api/smtp/get')
    .then(function(r) { return r.json(); })
    .then(function(d) {
      if (d.configured) {
        var el;
        el = document.getElementById('smtpServer');
        if (el && d.server) el.value = d.server;
        el = document.getElementById('smtpPort');
        if (el && d.port) el.value = d.port;
        el = document.getElementById('smtpUser');
        if (el && d.user) el.value = d.user;
        var acc = document.getElementById('smtpAccordion');
        if (acc) acc.className = 'accordion configured';
      }
    })
    .catch(function() {});
}
loadSmtpConfig();

// ============================================================
// WiFi Config yukle (primary + backup SSID + akordiyon durumu)
// ============================================================
function loadWifiConfig() {
  fetch('/api/wifi/config')
    .then(function(r) { return r.json(); })
    .then(function(cfg) {
      // Primary WiFi SSID doldur (bos ise)
      var wifiAcc = document.getElementById('wifiAccordion');
      if (cfg.sta_ssid) {
        var el = document.getElementById('ssid');
        if (el && !el.value) el.value = cfg.sta_ssid;
        if (wifiAcc) wifiAcc.className = 'accordion configured';
      } else {
        if (wifiAcc) wifiAcc.className = 'accordion';
      }
      // Backup WiFi SSID doldur (bos ise)
      var backupAcc = document.getElementById('wifiBackupAccordion');
      if (cfg.backup_ssid) {
        var el2 = document.getElementById('wifiBackupSsid');
        if (el2 && !el2.value) el2.value = cfg.backup_ssid;
        if (backupAcc) backupAcc.className = 'accordion configured';
      } else {
        if (backupAcc) backupAcc.className = 'accordion';
      }
      // AP Mode durumu
      var apToggle = document.getElementById('apModeEnabled');
      var apAcc = document.getElementById('apModeAccordion');
      if (apToggle) {
        apToggle.checked = (cfg.ap_enabled !== false);
      }
      // AP akordiyon renk: aktifse configured (beyaz), pasifse normal (gri)
      if (apAcc) {
        apAcc.className = (cfg.ap_enabled !== false) ? 'accordion configured' : 'accordion';
      }
    })
    .catch(function() {});
}
loadWifiConfig();

// SMTP Test butonu (bagimsiz kalir)
var smtpTestBtn = document.getElementById('smtpTestBtn');
if (smtpTestBtn) {
  smtpTestBtn.addEventListener('click', function() {
    var statusEl = document.getElementById('smtpStatus');
    if (statusEl) {
      statusEl.textContent = t('status.sending');
      statusEl.className = 'smtp-status';
    }
    fetch('/api/smtp/test', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: '{}'
    })
      .then(function(r) { return r.json(); })
      .then(function(d) {
        if (statusEl) {
          statusEl.textContent = d.message;
          statusEl.className = d.status === 'ok' ? 'smtp-status ok' : 'smtp-status error';
        }
      })
      .catch(function() {
        if (statusEl) {
          statusEl.textContent = t('mail.test_fail');
          statusEl.className = 'smtp-status error';
        }
      });
  });
}

// ============================================================
// Zamanlayici (Countdown Timer)
// ============================================================

var timerRemaining = 0;
var timerTotal = 0;
var timerEnabled = false;
var timerInterval = null;
var selectedUnit = 'saat';

// Tatil modu
var vacationEnabled = false;
var vacationRemaining = 0;
var vacationTotal = 0;

// Alarm
var alarmCount = 3;

// SVG ring sabitleri
var RING_CIRCUMFERENCE = 565.48;

// ============================================================
// Birim secici
// ============================================================
var unitBtns = document.querySelectorAll('.timer-unit-selector .unit-btn');
for (var u = 0; u < unitBtns.length; u++) {
  unitBtns[u].addEventListener('click', function() {
    for (var v = 0; v < unitBtns.length; v++) {
      unitBtns[v].className = 'unit-btn';
    }
    this.className = 'unit-btn active';
    selectedUnit = this.getAttribute('data-unit');
    updateAlarmInfo();
  });
}

// ============================================================
// Timer +/- butonlari
// ============================================================
var timerInput = document.getElementById('timerValue');
var timerDecrease = document.getElementById('timerDecrease');
var timerIncrease = document.getElementById('timerIncrease');

if (timerDecrease) {
  timerDecrease.addEventListener('click', function() {
    var val = parseInt(timerInput.value) || 1;
    if (val > 1) timerInput.value = val - 1;
    updateAlarmInfo();
  });
}

if (timerIncrease) {
  timerIncrease.addEventListener('click', function() {
    var val = parseInt(timerInput.value) || 1;
    if (val < 60) timerInput.value = val + 1;
    updateAlarmInfo();
  });
}

// Klavye ile deger girildiginde alarm slider'i otomatik ayarla
if (timerInput) {
  timerInput.addEventListener('input', function() {
    var val = parseInt(this.value);
    if (isNaN(val) || val < 1) this.value = 1;
    else if (val > 60) this.value = 60;
    updateAlarmInfo();
  });
}

// ============================================================
// Timer display guncelle
// ============================================================
function updateTimerDisplay() {
  var daysEl = document.getElementById('timeDays');
  var hoursEl = document.getElementById('timeHours');
  var minsEl = document.getElementById('timeMinutes');
  var labelEl = document.getElementById('timerLabel');
  var ringEl = document.getElementById('timerRing');
  if (!daysEl || !hoursEl || !minsEl) return;

  // Ana ring her zaman kullanici timer'ini gosterir
  // Tatil gerisayimi alttaki turuncu indicator'da ayri gosterilir
  var rem = timerRemaining;
  var tot = timerTotal;

  if (labelEl) {
    if (vacationEnabled && vacationRemaining > 0) {
      labelEl.textContent = t('timer_display.waiting');
    } else {
      labelEl.textContent = t('timer_display.remaining');
    }
  }

  if (ringEl) {
    ringEl.className = (vacationEnabled && vacationRemaining > 0) ? 'timer-ring vacation' : 'timer-ring';
  }

  if (rem <= 0) {
    daysEl.textContent = '';
    hoursEl.textContent = '';
    minsEl.textContent = '';
    var progressEl = document.getElementById('progressRing');
    if (progressEl) progressEl.style.strokeDashoffset = RING_CIRCUMFERENCE;
    return;
  }

  var gun = Math.floor(rem / 86400);
  var saat = Math.floor((rem % 86400) / 3600);
  var dakika = Math.floor((rem % 3600) / 60);
  var saniye = rem % 60;
  var totalDakika = Math.floor(rem / 60);

  var parts = [];
  if (gun > 0) parts.push(gun + t('time.day_abbr'));
  if (saat > 0) parts.push(saat + t('time.hour_abbr'));
  if (dakika > 0) parts.push(dakika + t('time.min_abbr'));
  if (totalDakika < 60) parts.push(saniye + t('time.sec_abbr'));

  if (parts.length === 1) {
    daysEl.textContent = '';
    hoursEl.textContent = parts[0];
    minsEl.textContent = '';
  } else if (parts.length === 2) {
    daysEl.textContent = '';
    hoursEl.textContent = parts[0];
    minsEl.textContent = parts[1];
  } else if (parts.length >= 3) {
    daysEl.textContent = parts[0];
    hoursEl.textContent = parts[1];
    minsEl.textContent = parts[2];
  }

  if (tot > 0) {
    var percentage = rem / tot;
    var offset = RING_CIRCUMFERENCE * (1 - percentage);
    var progressEl = document.getElementById('progressRing');
    if (progressEl) {
      progressEl.style.strokeDashoffset = offset;
    }
  }
}

// ============================================================
// Countdown metin formati
// ============================================================
function formatCountdownText(totalSec) {
  if (totalSec <= 0) return '';
  var gun = Math.floor(totalSec / 86400);
  var saat = Math.floor((totalSec % 86400) / 3600);
  var dakika = Math.floor((totalSec % 3600) / 60);
  var saniye = totalSec % 60;
  var totalDakika = Math.floor(totalSec / 60);
  var parts = [];
  if (gun > 0) parts.push(gun + t('time.day_abbr'));
  if (saat > 0) parts.push(saat + t('time.hour_abbr'));
  if (dakika > 0) parts.push(dakika + t('time.min_abbr'));
  if (totalDakika < 60) parts.push(saniye + t('time.sec_abbr'));
  return parts.join(' ');
}

// ============================================================
// Action butonlari goster/gizle
// ============================================================
function updateActionButtons() {
  // 4 buton daima gorunur: ayarlar, stop, restart, cikis
}

// ============================================================
// Ana ekran guncelle (logo vs countdown)
// ============================================================
function updateMainDisplay() {
  var logoImg = document.getElementById('logoImg');
  var timerSection = document.getElementById('timerSection');
  var vacIndicator = document.getElementById('vacationIndicator');
  var vacRemText = document.getElementById('vacationRemaining');
  if (!logoImg || !timerSection) return;

  // Timer veya tatil aktifse timer section goster
  // Tatil aktifken de ana ring'de kullanici timer'i (beklemede) gosterilir
  var showTimer = (timerEnabled && timerRemaining > 0) || (vacationEnabled && vacationRemaining > 0);

  if (showTimer) {
    logoImg.style.display = 'none';
    timerSection.style.display = 'flex';
    updateTimerDisplay();
  } else {
    // Gerisayim durmus veya kapaliysa logo goster (temaya uygun)
    var currentTheme = localStorage.getItem('ls_theme') || 'dark';
    logoImg.src = (currentTheme === 'light') ? '/ext/pic/darklogo.png' : '/ext/pic/logo.png';
    logoImg.style.display = 'block';
    timerSection.style.display = 'none';
  }

  if (vacIndicator) {
    if (vacationEnabled && vacationRemaining > 0) {
      vacIndicator.className = 'vacation-indicator active';
      var vacIndText = document.getElementById('vacationIndicatorText');
      if (vacIndText) vacIndText.textContent = t('timer_display.vacation_left', {time: ''});
      if (vacRemText) {
        vacRemText.textContent = formatCountdownText(vacationRemaining);
      }
    } else {
      vacIndicator.className = 'vacation-indicator';
    }
  }

  updateActionButtons();
}

// ============================================================
// Popup timer durumu guncelle
// ============================================================
function updateTimerStatus() {
  var ts = document.getElementById('timerStatus');
  if (!ts) return;

  if (vacationEnabled && vacationRemaining > 0) {
    ts.textContent = t('timer.status_vacation', {time: formatCountdownText(vacationRemaining)});
    ts.className = 'timer-status active';
  } else if (timerEnabled && timerRemaining > 0) {
    ts.textContent = t('timer.status_active', {time: formatCountdownText(timerRemaining)});
    ts.className = 'timer-status active';
  } else {
    ts.textContent = t('timer.status_inactive');
    ts.className = 'timer-status';
  }
}

// ============================================================
// Tatil modu UI guncelle
// ============================================================
// Kullanici toggle'a dokundu mu? (SSE'nin ezme sorununu onlemek icin)
var vacationToggleDirty = false;

function updateVacationUI() {
  var toggle = document.getElementById('vacationMode');
  var daysGroup = document.getElementById('vacationDaysGroup');
  var activeStatus = document.getElementById('vacationActiveStatus');
  var statusText = document.getElementById('vacationStatusText');

  if (vacationEnabled && vacationRemaining > 0) {
    // Tatil aktif - kullanici toggle'a dokunmadiysa checked yap
    if (toggle && !vacationToggleDirty) toggle.checked = true;
    if (daysGroup) daysGroup.style.display = 'none';
    if (activeStatus) activeStatus.style.display = 'block';
    if (statusText) statusText.textContent = formatCountdownText(vacationRemaining);
    var vacActiveText = document.getElementById('vacationActiveText');
    if (vacActiveText) vacActiveText.textContent = t('vacation.active_text', {time: formatCountdownText(vacationRemaining)});
  } else {
    // Tatil pasif
    if (toggle && !vacationToggleDirty) toggle.checked = false;
    if (activeStatus) activeStatus.style.display = 'none';
    if (daysGroup && toggle && toggle.checked) {
      daysGroup.style.display = 'block';
    } else if (daysGroup) {
      daysGroup.style.display = 'none';
    }
  }
}

// ============================================================
// Her saniye geri say
// ============================================================
function startLocalCountdown() {
  if (timerInterval) clearInterval(timerInterval);
  timerInterval = setInterval(function() {
    var changed = false;

    if (vacationEnabled && vacationRemaining > 0) {
      vacationRemaining--;
      changed = true;
      if (vacationRemaining <= 0) {
        vacationEnabled = false;
        if (timerTotal > 0) {
          timerRemaining = timerTotal;
          timerEnabled = true;
        }
      }
    } else if (timerEnabled && timerRemaining > 0) {
      timerRemaining--;
      changed = true;
      if (timerRemaining <= 0) {
        timerEnabled = false;
      }
    }

    if (changed) {
      updateMainDisplay();
      updateTimerStatus();
      updateVacationUI();
    }

    if (!timerEnabled && !vacationEnabled) {
      clearInterval(timerInterval);
      timerInterval = null;
      updateMainDisplay();
      updateTimerStatus();
      updateVacationUI();
    }
  }, 1000);
}

// ============================================================
// Timer durumunu ESP32'den al
// ============================================================
// Popup acik mi? (SSE'nin ayar alanlarini ezmesini engellemek icin)
function isPopupOpen() {
  return popup && popup.className.indexOf('show') >= 0;
}

function applyTimerState(d) {
  timerEnabled = d.enabled;
  timerRemaining = d.remaining_seconds;
  timerTotal = d.total_seconds;
  vacationEnabled = d.vacation_enabled;
  vacationRemaining = d.vacation_remaining;
  vacationTotal = d.vacation_total;
  if (d.alarm_count !== undefined) alarmCount = d.alarm_count;

  updateMainDisplay();
  updateTimerStatus();

  // Popup acikken SSE'nin ayar alanlarini ezmesini engelle
  if (!isPopupOpen()) {
    updateVacationUI();

    // Alarm slider sync (sadece popup kapali iken)
    var slider = document.getElementById('alarmCount');
    var sliderVal = document.getElementById('alarmCountValue');
    if (slider) slider.value = alarmCount;
    if (sliderVal) sliderVal.textContent = alarmCount;
    updateAlarmInfo();
  } else {
    // Popup acikken sadece tatil durumu guncelle (dirty degilse)
    if (!vacationToggleDirty) {
      updateVacationUI();
    }
  }

  var anyActive = (timerEnabled && timerRemaining > 0) ||
                  (vacationEnabled && vacationRemaining > 0);

  if (anyActive && !timerInterval) {
    startLocalCountdown();
  }
  if (!anyActive && timerInterval) {
    clearInterval(timerInterval);
    timerInterval = null;
  }
}

function fetchTimerState() {
  fetch('/api/timer/get')
    .then(function(r) { return r.json(); })
    .then(function(d) { applyTimerState(d); })
    .catch(function() {});
}

fetchTimerState();
setInterval(fetchTimerState, 5000);

// ============================================================
// SSE - Anlik timer guncelleme (buton, expire, restart)
// ============================================================
function connectSSE() {
  var evtSource = new EventSource('/api/events');

  evtSource.onmessage = function(e) {
    if (e.data === 'connected') return;
    try {
      var d = JSON.parse(e.data);
      applyTimerState(d);
    } catch (ex) {}
  };

  evtSource.onerror = function() {
    evtSource.close();
    setTimeout(connectSSE, 3000);
  };
}

connectSSE();

// ============================================================
// Stop butonu (ana ekran)
// ============================================================
var stopBtn = document.getElementById('stopBtn');
if (stopBtn) {
  stopBtn.addEventListener('click', function() {
    var action = (vacationEnabled && vacationRemaining > 0) ? 'vacation_cancel' : 'cancel';
    fetch('/api/timer/set', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ action: action })
    })
      .then(function(r) { return r.json(); })
      .then(function(d) {
        if (d.status === 'ok') {
          // Hemen lokal state guncelle (SSE'yi bekleme)
          if (action === 'cancel') {
            timerEnabled = false;
            timerRemaining = 0;
          } else {
            vacationEnabled = false;
            vacationRemaining = 0;
            if (timerTotal > 0) {
              timerRemaining = timerTotal;
              timerEnabled = true;
            }
          }
          updateMainDisplay();
          updateTimerStatus();
          updateVacationUI();
          if (timerEnabled && timerRemaining > 0) {
            startLocalCountdown();
          }
        }
      })
      .catch(function() {});
  });
}

// ============================================================
// Restart butonu (ana ekran)
// ============================================================
var restartBtn = document.getElementById('restartBtn');
if (restartBtn) {
  restartBtn.addEventListener('click', function() {
    fetch('/api/timer/set', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ action: 'restart' })
    })
      .then(function(r) { return r.json(); })
      .then(function(d) {
        if (d.status === 'ok') {
          timerEnabled = true;
          timerRemaining = d.remaining_seconds;
          timerTotal = d.total_seconds;
          updateMainDisplay();
          updateTimerStatus();
          startLocalCountdown();
        }
      })
      .catch(function() {});
  });
}

// ============================================================
// Cikis butonu (ana ekran)
// ============================================================
var exitBtn = document.getElementById('exitBtn');
if (exitBtn) {
  exitBtn.addEventListener('click', function() {
    window.location.href = 'about:blank';
  });
}

// ============================================================
// Alarm Slider (sadece UI - kaydetme Kaydet butonuyla)
// ============================================================
var alarmSlider = document.getElementById('alarmCount');
var alarmValueEl = document.getElementById('alarmCountValue');

function getMaxAlarmCount() {
  var timerVal = parseInt(document.getElementById('timerValue').value) || 24;
  return Math.max(0, Math.floor(timerVal / 2));
}

function enforceAlarmMax() {
  if (!alarmSlider) return;
  var max = getMaxAlarmCount();
  alarmSlider.max = max;
  if (parseInt(alarmSlider.value) > max) {
    alarmSlider.value = max;
  }
  // Sag taraftaki rakami her zaman slider degeriyle senkronize et
  if (alarmValueEl) alarmValueEl.textContent = alarmSlider.value;
}

function updateAlarmInfo() {
  var infoEl = document.getElementById('alarmInfo');
  if (!infoEl) return;

  enforceAlarmMax();

  var count = parseInt(alarmSlider ? alarmSlider.value : 0);
  if (count === 0) {
    infoEl.textContent = t('alarm.disabled');
    return;
  }

  var timerVal = parseInt(document.getElementById('timerValue').value) || 24;
  var unit = selectedUnit;

  var maxAlarm = timerVal - 1;
  var effectiveCount = Math.min(count, maxAlarm);

  if (effectiveCount <= 0) {
    infoEl.textContent = t('alarm.too_short');
    return;
  }

  var times = [];
  for (var i = effectiveCount; i >= 1; i--) {
    var tv = timerVal - i;
    if (tv > 0) times.push(tv + '.');
  }

  if (times.length === 0) {
    infoEl.textContent = t('alarm.will_notify', {count: count});
    return;
  }

  var unitText = (unit === 'gun') ? t('alarm.unit_day') : (unit === 'saat') ? t('alarm.unit_hour') : t('alarm.unit_minute');
  infoEl.textContent = t('alarm.times_text', {times: times.join(' '), unit_text: unitText});
}

if (alarmSlider) {
  alarmSlider.addEventListener('input', function() {
    if (alarmValueEl) alarmValueEl.textContent = this.value;
    updateAlarmInfo();
  });
}

// ============================================================
// Tatil Modu (toggle UI)
// ============================================================
var vacationToggle = document.getElementById('vacationMode');
var vacationDaysGroup = document.getElementById('vacationDaysGroup');
var vacationDaysInput = document.getElementById('vacationDays');
var vacationDecrease = document.getElementById('vacationDecrease');
var vacationIncrease = document.getElementById('vacationIncrease');

if (vacationToggle) {
  vacationToggle.addEventListener('change', function() {
    vacationToggleDirty = true;
    if (this.checked && !(vacationEnabled && vacationRemaining > 0)) {
      if (vacationDaysGroup) vacationDaysGroup.style.display = 'block';
    } else if (!this.checked) {
      if (vacationDaysGroup) vacationDaysGroup.style.display = 'none';
    }
  });
}

if (vacationDecrease) {
  vacationDecrease.addEventListener('click', function() {
    var val = parseInt(vacationDaysInput.value) || 1;
    if (val > 1) vacationDaysInput.value = val - 1;
  });
}

if (vacationIncrease) {
  vacationIncrease.addEventListener('click', function() {
    var val = parseInt(vacationDaysInput.value) || 1;
    if (val < 60) vacationDaysInput.value = val + 1;
  });
}

// ============================================================
// Toplu Kaydet (Kaydet butonu)
// ============================================================
function saveAllSettings() {
  // Timer ve tatil sirayla, diger ayarlar paralel gonderilir
  var otherPromises = [];

  // Alarm sayisi
  var alarmEl = document.getElementById('alarmCount');
  if (alarmEl) {
    otherPromises.push(
      fetch('/api/timer/set', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ action: 'set_alarms', count: alarmEl.value })
      }).catch(function() {})
    );
  }

  // SMTP kaydet (alanlar doluysa)
  var smtpServer = document.getElementById('smtpServer');
  var smtpUser = document.getElementById('smtpUser');
  if (smtpServer && smtpUser && smtpServer.value && smtpUser.value) {
    var smtpData = {
      server: smtpServer.value,
      port: (document.getElementById('smtpPort') || {}).value || '465',
      user: smtpUser.value,
      api_key: (document.getElementById('smtpApiKey') || {}).value || ''
    };
    otherPromises.push(
      fetch('/api/smtp/save', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(smtpData)
      }).catch(function() {})
    );
  }

  // WiFi baglan veya temizle (ssid bossa NVS silinir)
  var ssidEl = document.getElementById('ssid');
  if (ssidEl) {
    var staticEnabled = document.getElementById('wifiStaticIpEnabled');
    var staticIp = '';
    if (staticEnabled && staticEnabled.checked) {
      staticIp = (document.getElementById('sip') || {}).value || '';
    }
    var wifiData = {
      ssid: ssidEl.value || '',
      password: (document.getElementById('pass') || {}).value || '',
      static_ip: staticIp
    };
    otherPromises.push(
      fetch('/api/wifi/connect', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(wifiData)
      }).catch(function() {})
    );
  }

  // WiFi Yedek kaydet veya temizle (ssid bossa NVS silinir)
  var backupSsidEl = document.getElementById('wifiBackupSsid');
  if (backupSsidEl) {
    var backupStaticEnabled = document.getElementById('wifiBackupStaticIpEnabled');
    var backupStaticIp = '';
    if (backupStaticEnabled && backupStaticEnabled.checked) {
      backupStaticIp = (document.getElementById('wifiBackupStaticIp') || {}).value || '';
    }
    var backupData = {
      ssid: backupSsidEl.value || '',
      password: (document.getElementById('wifiBackupPass') || {}).value || '',
      static_ip: backupStaticIp
    };
    otherPromises.push(
      fetch('/api/wifi/backup/save', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(backupData)
      }).catch(function() {})
    );
  }

  // AP Mode kaydet
  var apToggle = document.getElementById('apModeEnabled');
  if (apToggle) {
    otherPromises.push(
      fetch('/api/wifi/ap/set', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ enabled: apToggle.checked ? 'true' : 'false' })
      }).catch(function() {})
    );
  }

  // Relay ayarlarini kaydet
  var relayPulseToggleEl = document.getElementById('relayPulseEnabled');
  if (relayPulseToggleEl) {
    otherPromises.push(buildRelaySaveData().then(function(data) {
      return fetch('/api/relay/set', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(data)
      });
    }).catch(function() {}));
  }

  // Timer + Tatil sirayla gonderilir (siralama onemli)
  var timerChain = Promise.resolve();

  // 1. Once timer ayarla
  var timerVal = document.getElementById('timerValue').value;
  if (timerVal && parseInt(timerVal) >= 1) {
    timerChain = timerChain.then(function() {
      return fetch('/api/timer/set', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ unit: selectedUnit, value: timerVal })
      }).catch(function() {});
    });
  }

  // 2. Sonra tatil modu
  var vacToggle = document.getElementById('vacationMode');
  if (vacToggle) {
    if (vacToggle.checked && !(vacationEnabled && vacationRemaining > 0)) {
      var days = document.getElementById('vacationDays').value || '7';
      timerChain = timerChain.then(function() {
        return fetch('/api/timer/set', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ action: 'vacation_set', days: days })
        }).catch(function() {});
      });
    } else if (!vacToggle.checked && vacationEnabled && vacationRemaining > 0) {
      timerChain = timerChain.then(function() {
        return fetch('/api/timer/set', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ action: 'vacation_cancel' })
        }).catch(function() {});
      });
    }
  }

  // Tum istekler bitince guncelle ve kapat
  Promise.all([timerChain].concat(otherPromises)).then(function() {
    vacationToggleDirty = false;
    // Kisa delay: sunucudaki SSE tetiklenmesinin tamamlanmasini bekle
    setTimeout(function() {
      fetchTimerState();
      loadSmtpConfig();
      loadWifiConfig();
      closePopup();
    }, 300);
  });
}

// ============================================================
// Info Sekmesi - Subpage Navigasyonu
// ============================================================
var infoSubpageActive = false;

function openInfoSubpage(subpageId) {
  var mainContent = document.getElementById('infoMainContent');
  var subpage = document.getElementById(subpageId);
  if (!mainContent || !subpage) return;

  mainContent.className = 'info-main-content hidden';

  var allSubpages = document.querySelectorAll('#panel-info .subpage');
  for (var i = 0; i < allSubpages.length; i++) {
    allSubpages[i].className = 'subpage hidden';
  }

  subpage.className = 'subpage';
  infoSubpageActive = true;

  // Carousel'i gizle
  var carousel = document.getElementById('settingsCarousel');
  if (carousel) carousel.style.display = 'none';

  // Footer butonlarini guncelle
  updateFooterButtons();

  // Veri yukle
  if (subpageId === 'deviceInfoSubpage') {
    loadSystemDetail();
  } else if (subpageId === 'logsSubpage') {
    loadLogs();
  }
}

function closeInfoSubpage() {
  var mainContent = document.getElementById('infoMainContent');
  if (!mainContent) return;

  var allSubpages = document.querySelectorAll('#panel-info .subpage');
  for (var i = 0; i < allSubpages.length; i++) {
    allSubpages[i].className = 'subpage hidden';
  }

  mainContent.className = 'info-main-content';
  infoSubpageActive = false;

  // Carousel'i geri goster
  var carousel = document.getElementById('settingsCarousel');
  if (carousel) carousel.style.display = '';

  // Footer butonlarini guncelle
  updateFooterButtons();
}

// Info card tiklandiginda subpage ac
var infoCards = document.querySelectorAll('#panel-info .info-card');
for (var ic = 0; ic < infoCards.length; ic++) {
  infoCards[ic].addEventListener('click', function() {
    var subpageId = this.getAttribute('data-subpage');
    if (subpageId) openInfoSubpage(subpageId);
  });
}

// ============================================================
// Sistem Detay Bilgisi Yukle
// ============================================================
function formatUptime(seconds) {
  var gun = Math.floor(seconds / 86400);
  var saat = Math.floor((seconds % 86400) / 3600);
  var dakika = Math.floor((seconds % 3600) / 60);
  var parts = [];
  if (gun > 0) parts.push(gun + ' ' + (gun > 1 ? t('time.days') : t('time.day')));
  if (saat > 0) parts.push(saat + ' ' + (saat > 1 ? t('time.hours') : t('time.hour')));
  if (dakika > 0) parts.push(dakika + ' ' + t('time.min'));
  if (parts.length === 0) parts.push(t('time.less_than_1min'));
  return parts.join(' ');
}

function formatKB(bytes) {
  if (bytes >= 1048576) return (bytes / 1048576).toFixed(1) + ' MB';
  return Math.round(bytes / 1024) + ' KB';
}

function rssiLabel(rssi) {
  if (rssi >= -50) return rssi + ' dBm (' + t('rssi.excellent') + ')';
  if (rssi >= -60) return rssi + ' dBm (' + t('rssi.good') + ')';
  if (rssi >= -70) return rssi + ' dBm (' + t('rssi.fair') + ')';
  return rssi + ' dBm (' + t('rssi.weak') + ')';
}

function loadSystemDetail() {
  fetch('/api/system/detail')
    .then(function(r) { return r.json(); })
    .then(function(d) {
      var el;
      el = document.getElementById('sysChip');
      if (el) el.textContent = d.chip;
      el = document.getElementById('sysCores');
      if (el) el.textContent = d.cores;
      el = document.getElementById('sysCpuFreq');
      if (el) el.textContent = d.cpu_mhz + ' MHz';
      el = document.getElementById('sysFlashSize');
      if (el) el.textContent = Math.round(d.flash_kb / 1024) + ' MB';

      el = document.getElementById('sysRamTotal');
      if (el) el.textContent = formatKB(d.heap_total);
      el = document.getElementById('sysRamFree');
      if (el) el.textContent = formatKB(d.heap_free);
      el = document.getElementById('sysRamMinFree');
      if (el) el.textContent = formatKB(d.heap_min);

      var usedPercent = ((d.heap_total - d.heap_free) / d.heap_total * 100).toFixed(1);
      el = document.getElementById('ramBarFill');
      if (el) {
        el.style.width = usedPercent + '%';
        el.className = 'storage-bar-fill' + (usedPercent > 80 ? ' storage-bar-danger' : usedPercent > 60 ? ' storage-bar-warning' : '');
      }
      el = document.getElementById('ramUsagePercent');
      if (el) el.textContent = usedPercent + '%';

      // Dahili flash
      el = document.getElementById('sysIntFlashTotal');
      if (el) el.textContent = d.flash_kb + ' KB (' + Math.round(d.flash_kb / 1024) + ' MB)';
      el = document.getElementById('sysIntFlashUsed');
      if (el && d.flash_used_kb) el.textContent = d.flash_used_kb + ' KB';
      el = document.getElementById('sysIntFlashFree');
      if (el && d.flash_used_kb) el.textContent = (d.flash_kb - d.flash_used_kb) + ' KB';

      // Harici flash - slot a, slot b, user data
      el = document.getElementById('sysSlotA');
      if (el && d.slot_a_total) el.textContent = (d.slot_a_total - d.slot_a_free) + ' KB / ' + d.slot_a_total + ' KB';
      el = document.getElementById('sysSlotB');
      if (el) el.textContent = t('device.slot_b_text', {size: d.slot_b_kb});
      el = document.getElementById('sysUserData');
      if (el && d.user_total) el.textContent = t('device.free_of_total', {free: d.user_free, total: d.user_total});

      // mDNS
      el = document.getElementById('sysMdns');
      if (el && d.mdns) el.textContent = d.mdns;

      el = document.getElementById('sysRssi');
      if (el) el.textContent = d.rssi ? rssiLabel(d.rssi) : '-';

      el = document.getElementById('sysUptime');
      if (el) el.textContent = formatUptime(d.uptime);
      el = document.getElementById('sysResetReason');
      if (el) el.textContent = d.reset_reason;
      el = document.getElementById('sysFirmware');
      if (el) el.textContent = 'v' + d.fw_version;
    })
    .catch(function() {});
}

// ============================================================
// Log Yukle
// ============================================================
var logData = [];

function loadLogs() {
  var list = document.getElementById('logList');
  if (list) list.innerHTML = '<div class="log-item"><span class="log-text">' + t('logs.loading') + '</span></div>';

  fetch('/api/logs')
    .then(function(r) { return r.json(); })
    .then(function(d) {
      logData = d.logs || [];
      renderLogs();
    })
    .catch(function() {
      logData = [];
      if (list) list.innerHTML = '<div class="log-item"><span class="log-text">' + t('logs.error') + '</span></div>';
    });
}

function renderLogs() {
  var list = document.getElementById('logList');
  if (!list) return;

  if (logData.length === 0) {
    list.innerHTML = '<div class="log-item"><span class="log-text">' + t('logs.empty') + '</span></div>';
    return;
  }

  var h = '';
  for (var j = 0; j < logData.length; j++) {
    var log = logData[j];
    var typeClass = 'log-' + log.type;
    var typeBadgeClass = 'type-' + log.type;
    var uptime = formatUptime(log.ts);

    h += '<div class="log-item ' + typeClass + '">';
    h += '<div><span class="log-time">' + uptime + '</span> ';
    h += '<span class="log-type ' + typeBadgeClass + '">' + log.type + '</span></div>';
    h += '<span class="log-text">' + log.msg + '</span>';
    h += '</div>';
  }
  list.innerHTML = h;
}

// ============================================================
// Tema Secimi
// ============================================================
var themeSelect = document.getElementById('themeSelect');
var savedTheme = localStorage.getItem('ls_theme') || 'dark';

function updateLogoForTheme(theme) {
  var img = document.getElementById('logoImg');
  if (img) {
    img.src = (theme === 'light') ? '/ext/pic/darklogo.png' : '/ext/pic/logo.png';
  }
}

if (savedTheme === 'light') {
  document.body.classList.add('light-theme');
}
updateLogoForTheme(savedTheme);

if (themeSelect) {
  themeSelect.value = savedTheme;
  themeSelect.addEventListener('change', function() {
    if (this.value === 'light') {
      document.body.classList.add('light-theme');
    } else {
      document.body.classList.remove('light-theme');
    }
    updateLogoForTheme(this.value);
    localStorage.setItem('ls_theme', this.value);
  });
}

// ============================================================
// Dil Secimi
// ============================================================
var langSelect = document.getElementById('languageSelect');
if (langSelect) {
  langSelect.value = currentLang;
  langSelect.addEventListener('change', function() {
    applyLanguage(this.value);
  });
}

// ============================================================
// Sistem Sekmesi - Subpage Navigasyonu
// ============================================================
var systemSubpageActive = false;

function openSystemSubpage(subpageId) {
  var mainContent = document.getElementById('systemMainContent');
  var subpage = document.getElementById(subpageId);
  if (!mainContent || !subpage) return;

  mainContent.className = 'hidden';

  var allSubpages = document.querySelectorAll('#panel-system .subpage');
  for (var i = 0; i < allSubpages.length; i++) {
    allSubpages[i].className = 'subpage hidden';
  }

  subpage.className = 'subpage';
  systemSubpageActive = true;

  var carousel = document.getElementById('settingsCarousel');
  if (carousel) carousel.style.display = 'none';

  updateFooterButtons();

  // Surum bilgilerini doldur
  if (subpageId === 'hwOtaSubpage') {
    fetch('/api/system/detail')
      .then(function(r) { return r.json(); })
      .then(function(d) {
        var el = document.getElementById('currentFirmwareVersion');
        if (el) el.textContent = 'v' + d.fw_version;
      })
      .catch(function() {});
  } else if (subpageId === 'resetApiSubpage') {
    loadResetApiConfig();
  } else if (subpageId === 'guiOtaSubpage') {
    fetch('/ext/config.json')
      .then(function(r) { return r.json(); })
      .then(function(d) {
        var el = document.getElementById('currentGuiVersion');
        if (el) el.textContent = 'v' + d.version;
      })
      .catch(function() {});
    fetch('/api/system/info')
      .then(function(r) { return r.json(); })
      .then(function(d) {
        var el = document.getElementById('extFlashStatus');
        if (el) el.textContent = d.ext_flash ? t('ota.ext_flash_ready') : t('ota.ext_flash_unavailable');
      })
      .catch(function() {});
  }
}

function closeSystemSubpage() {
  var mainContent = document.getElementById('systemMainContent');
  if (!mainContent) return;

  var allSubpages = document.querySelectorAll('#panel-system .subpage');
  for (var i = 0; i < allSubpages.length; i++) {
    allSubpages[i].className = 'subpage hidden';
  }

  mainContent.className = '';
  systemSubpageActive = false;

  var carousel = document.getElementById('settingsCarousel');
  if (carousel) carousel.style.display = '';

  updateFooterButtons();
}

// OTA butonlari
var otaFirmwareBtn = document.getElementById('otaFirmwareBtn');
var otaGuiBtn = document.getElementById('otaGuiBtn');

if (otaFirmwareBtn) {
  otaFirmwareBtn.addEventListener('click', function() {
    openSystemSubpage('hwOtaSubpage');
  });
}

if (otaGuiBtn) {
  otaGuiBtn.addEventListener('click', function() {
    openSystemSubpage('guiOtaSubpage');
  });
}

var securityCardBtn = document.getElementById('securityCardBtn');
var resetApiCardBtn = document.getElementById('resetApiCardBtn');

if (securityCardBtn) {
  securityCardBtn.addEventListener('click', function() {
    openSystemSubpage('securitySubpage');
  });
}

if (resetApiCardBtn) {
  resetApiCardBtn.addEventListener('click', function() {
    openSystemSubpage('resetApiSubpage');
  });
}

// Carousel sekme degistiginde system subpage kapat
var origUpdateCarousel = updateCarousel;
updateCarousel = function() {
  if (systemSubpageActive) {
    closeSystemSubpage();
  }
  origUpdateCarousel();
};

// ============================================================
// OTA Kaynak Secici
// ============================================================
var hwOtaRadios = document.querySelectorAll('input[name="hwOtaSource"]');
for (var hr = 0; hr < hwOtaRadios.length; hr++) {
  hwOtaRadios[hr].addEventListener('change', function() {
    var warning = document.getElementById('hwOtaWarning');
    var custom = document.getElementById('hwOtaCustomSection');
    if (this.value === 'custom') {
      if (warning) warning.className = 'warning-banner';
      if (custom) custom.className = 'setting-section';
    } else {
      if (warning) warning.className = 'warning-banner hidden';
      if (custom) custom.className = 'setting-section hidden';
    }
  });
}

var guiOtaRadios = document.querySelectorAll('input[name="guiOtaSource"]');
for (var gr = 0; gr < guiOtaRadios.length; gr++) {
  guiOtaRadios[gr].addEventListener('change', function() {
    var warning = document.getElementById('guiOtaWarning');
    var custom = document.getElementById('guiOtaCustomSection');
    if (this.value === 'custom') {
      if (warning) warning.className = 'warning-banner';
      if (custom) custom.className = 'setting-section';
    } else {
      if (warning) warning.className = 'warning-banner hidden';
      if (custom) custom.className = 'setting-section hidden';
    }
  });
}

// OTA kontrol butonlari (simdilik placeholder)
var checkFwBtn = document.getElementById('checkFirmwareUpdateBtn');
if (checkFwBtn) {
  checkFwBtn.addEventListener('click', function() {
    alert(t('alert.fw_update_not_ready'));
  });
}

var updateGuiBtn2 = document.getElementById('updateGuiBtn');
if (updateGuiBtn2) {
  updateGuiBtn2.addEventListener('click', function() {
    alert(t('alert.gui_update_not_ready'));
  });
}

// ============================================================
// Restart ve Factory Reset
// ============================================================
var rebootBtn2 = document.getElementById('rebootBtn');
if (rebootBtn2) {
  rebootBtn2.addEventListener('click', function() {
    if (!confirm(t('confirm.restart'))) return;
    fetch('/api/device/restart', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: '{}'
    }).then(function() {
      alert(t('alert.restart_ok'));
      setTimeout(function() { location.reload(); }, 5000);
    }).catch(function() {
      alert(t('alert.restart_err'));
    });
  });
}

var factoryResetBtn2 = document.getElementById('factoryResetBtn');
if (factoryResetBtn2) {
  factoryResetBtn2.addEventListener('click', function() {
    if (!confirm(t('confirm.factory1'))) return;
    if (!confirm(t('confirm.factory2'))) return;
    fetch('/api/device/factory-reset', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: '{}'
    }).then(function() {
      alert(t('alert.factory_ok'));
      setTimeout(function() { location.reload(); }, 10000);
    }).catch(function() {
      alert(t('alert.factory_err'));
    });
  });
}

// ============================================================
// Aksiyonlar - Subpage Navigasyonu
// ============================================================

function openActionSubpage(viewId) {
  var mainContent = document.getElementById('actionsMainContent');
  var view = document.getElementById(viewId);
  if (!mainContent || !view) return;

  mainContent.className = 'hidden';

  var allViews = document.querySelectorAll('#panel-actions .action-detail-view');
  for (var i = 0; i < allViews.length; i++) {
    allViews[i].className = 'action-detail-view hidden';
  }

  view.className = 'action-detail-view';
  actionSubpageActive = true;
  currentActionView = viewId;

  var carousel = document.getElementById('settingsCarousel');
  if (carousel) carousel.style.display = 'none';

  updateFooterButtons();
}

function closeActionSubpage() {
  var mainContent = document.getElementById('actionsMainContent');
  if (!mainContent) return;

  var allViews = document.querySelectorAll('#panel-actions .action-detail-view');
  for (var i = 0; i < allViews.length; i++) {
    allViews[i].className = 'action-detail-view hidden';
  }

  mainContent.className = '';
  actionSubpageActive = false;
  currentActionView = '';

  var carousel = document.getElementById('settingsCarousel');
  if (carousel) carousel.style.display = '';

  updateFooterButtons();
}

// Action card ayar butonlari
var actionSettingsBtns = document.querySelectorAll('.action-card-settings');
for (var asb = 0; asb < actionSettingsBtns.length; asb++) {
  actionSettingsBtns[asb].addEventListener('click', function(e) {
    e.stopPropagation();
    var config = this.getAttribute('data-config');
    if (config === 'relay') openActionSubpage('relayConfigView');
    else if (config === 'telegram') { openActionSubpage('telegramConfigView'); loadTelegramConfig(); }
    else if (config === 'early-mail') { openActionSubpage('earlyMailConfigView'); loadEarlyMailConfig(); }
    else if (config === 'early-api' || config === 'trigger-api') openActionSubpage('apiConfigView');
    else if (config === 'trigger-mail') { openActionSubpage('triggerMailConfigView'); loadTriggerMailGroups(); }
  });
}

// Action card ana alana tiklaninca aktif/pasif toggle + backend kaydet
var actionCardMains = document.querySelectorAll('.action-card-main');
for (var acm = 0; acm < actionCardMains.length; acm++) {
  actionCardMains[acm].addEventListener('click', function() {
    var card = this.parentElement;
    var current = card.getAttribute('data-active');
    card.setAttribute('data-active', current === 'true' ? 'false' : 'true');
    saveActionStates();
  });
}

// Alt sekme Geri/Iptal artik footer butonu ile yonetiliyor

// Carousel sekme degistiginde action subpage kapat
var origUpdateCarousel2 = updateCarousel;
updateCarousel = function() {
  if (actionSubpageActive) {
    closeActionSubpage();
  }
  origUpdateCarousel2();
};

// ============================================================
// Röle (Relay) Yapilandirmasi - Tam Fonksiyonel
// ============================================================
var relayInvertedToggle = document.getElementById('relayInverted');
var relayIdleStateEl = document.getElementById('relayIdleState');
var relayActiveStateEl = document.getElementById('relayActiveState');
var relayPulseToggle = document.getElementById('relayPulseEnabled');
var relayPulseFields = document.getElementById('relayPulseFields');
var relayPulseDuration = document.getElementById('relayPulseDuration');
var relayDurationValue = document.getElementById('relayDurationValue');
var relayDurationUnit = document.getElementById('relayDurationUnit');
var relayCycleText = document.getElementById('relayCycleText');

// Relay invert toggle -> enerji durumu guncelle
if (relayInvertedToggle) {
  relayInvertedToggle.addEventListener('change', function() {
    if (this.checked) {
      if (relayIdleStateEl) relayIdleStateEl.textContent = t('relay.power_on');
      if (relayActiveStateEl) relayActiveStateEl.textContent = t('relay.power_off');
    } else {
      if (relayIdleStateEl) relayIdleStateEl.textContent = t('relay.power_off');
      if (relayActiveStateEl) relayActiveStateEl.textContent = t('relay.power_on');
    }
  });
}

// Pulse toggle -> pulse alanlari goster/gizle
if (relayPulseToggle) {
  relayPulseToggle.addEventListener('change', function() {
    if (relayPulseFields) {
      relayPulseFields.className = this.checked ? 'pulse-duration-fields' : 'pulse-duration-fields hidden';
    }
    updateRelayCycleInfo();
  });
}

// Dongu bilgisini guncelle
function updateRelayCycleInfo() {
  if (!relayCycleText || !relayDurationValue || !relayDurationUnit || !relayPulseDuration) return;

  var totalSec;
  var dVal = parseInt(relayDurationValue.value) || 1;
  if (relayDurationUnit.value === 'minutes') {
    totalSec = dVal * 60;
  } else {
    totalSec = dVal;
  }

  var pulseSec = parseInt(relayPulseDuration.value) || 5;
  var cycleDuration = pulseSec * 2; // acik + kapali
  var cycleCount = Math.floor(totalSec / cycleDuration);
  if (cycleCount < 1) cycleCount = 1;

  relayCycleText.textContent = t('relay.cycle_text', {pulse: pulseSec, cycles: cycleCount, total: totalSec});
}

if (relayPulseDuration) {
  relayPulseDuration.addEventListener('change', updateRelayCycleInfo);
}
if (relayDurationValue) {
  relayDurationValue.addEventListener('input', updateRelayCycleInfo);
}
if (relayDurationUnit) {
  relayDurationUnit.addEventListener('change', updateRelayCycleInfo);
}

// Relay form verilerinden kaydetme objesi olustur
function buildRelaySaveData() {
  return Promise.resolve().then(function() {
    var inv = relayInvertedToggle ? relayInvertedToggle.checked : false;
    var delayEl = document.getElementById('relayDelay');
    var delay_v = delayEl ? parseInt(delayEl.value) || 0 : 0;
    var pulse = 0;
    var duration = 0;

    if (relayPulseToggle && relayPulseToggle.checked) {
      pulse = relayPulseDuration ? parseInt(relayPulseDuration.value) || 5 : 5;
      // duration: toplam calisma suresi (saniye)
      var dVal = relayDurationValue ? parseInt(relayDurationValue.value) || 1 : 1;
      if (relayDurationUnit && relayDurationUnit.value === 'minutes') {
        duration = dVal * 60;
      } else {
        duration = dVal;
      }
    }
    // pulse kapali ise duration=0 (suresiz/sustained)

    return {
      duration: duration,
      pulse: pulse,
      delay: delay_v,
      inverted: inv ? 'true' : 'false'
    };
  });
}

// Relay mevcut ayarlari yukle
function loadRelayConfig() {
  fetch('/api/relay/get')
    .then(function(r) { return r.json(); })
    .then(function(d) {
      if (relayInvertedToggle) relayInvertedToggle.checked = !!d.inverted;
      if (d.inverted) {
        if (relayIdleStateEl) relayIdleStateEl.textContent = t('relay.power_on');
        if (relayActiveStateEl) relayActiveStateEl.textContent = t('relay.power_off');
      }

      var delayEl = document.getElementById('relayDelay');
      if (delayEl) delayEl.value = d.delay || 0;

      var isPulse = (d.pulse > 0);
      if (relayPulseToggle) relayPulseToggle.checked = isPulse;
      if (relayPulseFields) {
        relayPulseFields.className = isPulse ? 'pulse-duration-fields' : 'pulse-duration-fields hidden';
      }

      if (isPulse) {
        if (relayPulseDuration) relayPulseDuration.value = d.pulse;
        // duration -> durationValue + durationUnit
        var dur = d.duration || 0;
        if (dur > 0 && dur >= 60 && (dur % 60 === 0)) {
          if (relayDurationValue) relayDurationValue.value = dur / 60;
          if (relayDurationUnit) relayDurationUnit.value = 'minutes';
        } else {
          if (relayDurationValue) relayDurationValue.value = dur || 1;
          if (relayDurationUnit) relayDurationUnit.value = 'seconds';
        }
      }

      updateRelayCycleInfo();
    })
    .catch(function() {});
}

// Relay kaydet
var relayConfigSaveBtn = document.getElementById('relayConfigSaveBtn');
if (relayConfigSaveBtn) {
  relayConfigSaveBtn.addEventListener('click', function() {
    buildRelaySaveData().then(function(data) {
      return fetch('/api/relay/set', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(data)
      });
    })
      .then(function(r) { return r.json(); })
      .then(function(d) {
        if (d.status === 'ok') {
          closeActionSubpage();
        }
      })
      .catch(function() {
        alert(t('alert.relay_save_err'));
      });
  });
}

// Relay test: once kaydet, sonra test et
var testRelayBtn = document.getElementById('testRelayBtn');
if (testRelayBtn) {
  testRelayBtn.addEventListener('click', function() {
    testRelayBtn.disabled = true;
    testRelayBtn.textContent = t('status.saving');

    buildRelaySaveData().then(function(data) {
      return fetch('/api/relay/set', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(data)
      });
    })
      .then(function(r) { return r.json(); })
      .then(function(d) {
        if (d.status !== 'ok') {
          alert(t('alert.save_error'));
          return;
        }
        testRelayBtn.textContent = t('status.testing');
        return fetch('/api/relay/test', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: '{}'
        })
          .then(function() {
            alert(t('alert.relay_test_ok'));
          });
      })
      .catch(function() {
        alert(t('alert.relay_test_err'));
      })
      .finally(function() {
        testRelayBtn.disabled = false;
        testRelayBtn.textContent = t('relay.test_btn');
      });
  });
}

// Placeholder kaydet butonlari (henuz yapilandirilmamis aksiyonlar)
var placeholderSaveBtns2 = ['apiConfigSaveBtn'];
for (var psb = 0; psb < placeholderSaveBtns2.length; psb++) {
  var pBtn = document.getElementById(placeholderSaveBtns2[psb]);
  if (pBtn) {
    pBtn.addEventListener('click', function() {
      alert(t('alert.not_configured'));
    });
  }
}

// Placeholder test butonlari
var placeholderTestBtns = ['testWebhookBtn', 'testMqttBtn'];
for (var ptb = 0; ptb < placeholderTestBtns.length; ptb++) {
  var tBtn = document.getElementById(placeholderTestBtns[ptb]);
  if (tBtn) {
    tBtn.addEventListener('click', function() {
      alert(t('alert.not_configured'));
    });
  }
}

// ============================================================
// Telegram Yapilandirmasi
// ============================================================

function loadTelegramConfig() {
  fetch('/api/telegram/get')
    .then(function(r) { return r.json(); })
    .then(function(d) {
      var el;
      el = document.getElementById('telegramToken');
      if (el) {
        el.value = '';
        el.placeholder = d.configured ? t('telegram.token_configured') : t('telegram.token_placeholder');
      }
      el = document.getElementById('telegramChat');
      if (el) el.value = d.chat_id || '';
      el = document.getElementById('telegramMessage');
      if (el) el.value = d.message || '';
    })
    .catch(function() {});
}

function saveTelegramConfig() {
  var token = (document.getElementById('telegramToken') || {}).value || '';
  var chat_id = (document.getElementById('telegramChat') || {}).value || '';
  var message = (document.getElementById('telegramMessage') || {}).value || '';

  fetch('/api/telegram/save', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({token: token, chat_id: chat_id, message: message})
  })
    .then(function(r) { return r.json(); })
    .then(function(d) {
      if (d.status === 'ok') {
        closeActionSubpage();
      } else {
        alert(d.message || t('alert.save_error'));
      }
    })
    .catch(function() { alert(t('alert.connection_error')); });
}


var testTelegramBtn = document.getElementById('testTelegramBtn');
if (testTelegramBtn) {
  testTelegramBtn.addEventListener('click', function() {
    var token = (document.getElementById('telegramToken') || {}).value || '';
    var chat_id = (document.getElementById('telegramChat') || {}).value || '';
    var message = (document.getElementById('telegramMessage') || {}).value || '';

    testTelegramBtn.disabled = true;
    testTelegramBtn.textContent = t('status.saving');

    // Once kaydet, sonra test et
    fetch('/api/telegram/save', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({token: token, chat_id: chat_id, message: message})
    })
      .then(function(r) { return r.json(); })
      .then(function(d) {
        if (d.status !== 'ok') {
          alert(d.message || t('alert.save_error'));
          return;
        }
        testTelegramBtn.textContent = t('status.sending');
        return fetch('/api/telegram/test', {method: 'POST'})
          .then(function(r) { return r.json(); })
          .then(function(d) {
            alert(d.message || (d.status === 'ok' ? t('alert.success') : t('alert.error')));
          });
      })
      .catch(function() { alert(t('alert.connection_error')); })
      .finally(function() {
        testTelegramBtn.disabled = false;
        testTelegramBtn.textContent = t('telegram.test_msg_btn');
      });
  });
}

// ============================================================
// Erken Uyari Mail Yapilandirmasi
// ============================================================

function loadEarlyMailConfig() {
  fetch('/api/early-mail/get')
    .then(function(r) { return r.json(); })
    .then(function(d) {
      var el;
      el = document.getElementById('earlyMailRecipient');
      if (el && d.email) el.value = d.email;
      el = document.getElementById('earlyMailSubject');
      if (el && d.subject) el.value = d.subject;
      el = document.getElementById('earlyReminderMessage');
      if (el && d.body) el.value = d.body;
    })
    .catch(function() {});
}

function saveEarlyMailConfig() {
  var email = (document.getElementById('earlyMailRecipient') || {}).value || '';
  var subject = (document.getElementById('earlyMailSubject') || {}).value || '';
  var body = (document.getElementById('earlyReminderMessage') || {}).value || '';

  fetch('/api/early-mail/save', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ email: email, subject: subject, body: body })
  })
    .then(function(r) { return r.json(); })
    .then(function(d) {
      if (d.status === 'ok') {
        closeActionSubpage();
      } else {
        alert(t('alert.save_error') + ': ' + (d.message || ''));
      }
    })
    .catch(function() {
      alert(t('alert.early_mail_save_err'));
    });
}


// ============================================================
// Tetikleme Mail Gruplari
// ============================================================

var mailGroupData = []; // Yuklenmis grup listesi
var currentMailGroupId = -1; // Duzenlenen grup ID
var currentRecipients = []; // Gecici alici listesi

function loadTriggerMailGroups() {
  fetch('/api/trigger-mail/get')
    .then(function(r) { return r.json(); })
    .then(function(d) {
      mailGroupData = d.groups || [];
      renderMailGroupList();
    })
    .catch(function() {});
}

function renderMailGroupList() {
  var container = document.getElementById('mailGroupList');
  if (!container) return;
  container.innerHTML = '';

  for (var i = 0; i < mailGroupData.length; i++) {
    var g = mailGroupData[i];
    var rcptCount = 0;
    if (g.recipients && g.recipients.length > 0) {
      rcptCount = g.recipients.split(',').length;
    }

    var item = document.createElement('div');
    item.className = 'mail-group-item';
    item.setAttribute('data-group-id', g.id);
    item.innerHTML =
      '<div class="mail-group-info">' +
        '<span class="mail-group-name">' + (g.name || t('mail_group.unnamed')) + '</span>' +
        '<span class="mail-group-count">' + t('mail_group.recipient_count', {count: rcptCount}) + '</span>' +
      '</div>' +
      '<button class="btn-cancel" style="width:auto;margin-top:0;padding:6px 12px" data-action="view-group" data-gid="' + g.id + '">' + t('mail_group.view') + '</button>';

    container.appendChild(item);
  }

  // Goruntule butonlarina event listener ekle
  var viewBtns = container.querySelectorAll('[data-action="view-group"]');
  for (var v = 0; v < viewBtns.length; v++) {
    viewBtns[v].addEventListener('click', function() {
      var gid = parseInt(this.getAttribute('data-gid'));
      openMailGroupDetail(gid);
    });
  }
}

function openMailGroupDetail(groupId) {
  currentMailGroupId = groupId;

  // triggerMailConfigView'i gizle, mailGroupDetailView'i goster
  var trigView = document.getElementById('triggerMailConfigView');
  var detailView = document.getElementById('mailGroupDetailView');
  if (trigView) trigView.className = 'action-detail-view hidden';
  if (detailView) detailView.className = 'action-detail-view';

  // Form alanlari
  var nameEl = document.getElementById('mailGroupName');
  var subjEl = document.getElementById('mailGroupSubject');
  var bodyEl = document.getElementById('mailGroupContent');
  var deleteBtn = document.getElementById('mailGroupDeleteBtn');

  if (groupId >= 0 && groupId < mailGroupData.length) {
    // Mevcut grubu duzenle
    var g = mailGroupData[groupId];
    if (nameEl) nameEl.value = g.name || '';
    if (subjEl) subjEl.value = g.subject || '';
    if (bodyEl) bodyEl.value = g.body || '';
    currentRecipients = (g.recipients && g.recipients.length > 0) ? g.recipients.split(',') : [];
    if (deleteBtn) deleteBtn.style.display = '';
  } else {
    // Yeni grup olustur
    if (nameEl) nameEl.value = '';
    if (subjEl) subjEl.value = '';
    if (bodyEl) bodyEl.value = '';
    currentRecipients = [];
    if (deleteBtn) deleteBtn.style.display = 'none';
  }

  renderRecipients();
  loadGroupFiles(groupId);
}

function closeMailGroupDetail() {
  // mailGroupDetailView'i gizle, triggerMailConfigView'i goster
  var trigView = document.getElementById('triggerMailConfigView');
  var detailView = document.getElementById('mailGroupDetailView');
  if (detailView) detailView.className = 'action-detail-view hidden';
  if (trigView) trigView.className = 'action-detail-view';
  currentMailGroupId = -1;
  currentRecipients = [];
}

// Alici yonetimi
function renderRecipients() {
  var container = document.getElementById('mailGroupRecipients');
  if (!container) return;
  container.innerHTML = '';

  for (var i = 0; i < currentRecipients.length; i++) {
    var email = currentRecipients[i].trim();
    if (!email) continue;
    var row = document.createElement('div');
    row.className = 'recipient-item';
    row.innerHTML =
      '<span class="recipient-email">' + email + '</span>' +
      '<button class="recipient-remove" data-idx="' + i + '" title="Kaldir">' +
        '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="14" height="14">' +
          '<line x1="18" y1="6" x2="6" y2="18"></line>' +
          '<line x1="6" y1="6" x2="18" y2="18"></line>' +
        '</svg>' +
      '</button>';
    container.appendChild(row);
  }

  // Sil butonlarina event listener
  var removeBtns = container.querySelectorAll('.recipient-remove');
  for (var r = 0; r < removeBtns.length; r++) {
    removeBtns[r].addEventListener('click', function() {
      var idx = parseInt(this.getAttribute('data-idx'));
      currentRecipients.splice(idx, 1);
      renderRecipients();
    });
  }
}

// Alici ekle butonu
var addRecipientBtn = document.getElementById('addRecipientBtn');
if (addRecipientBtn) {
  addRecipientBtn.addEventListener('click', function() {
    var input = document.getElementById('newRecipientEmail');
    if (!input || !input.value.trim()) return;
    var email = input.value.trim();
    // Email dogrulama: local@domain.tld (.-_+ gibi karakterler desteklenir)
    var emailRegex = /^[a-zA-Z0-9.!#$%&'*+\/=?^_`{|}~\-]+@[a-zA-Z0-9](?:[a-zA-Z0-9\-]{0,61}[a-zA-Z0-9])?(?:\.[a-zA-Z0-9](?:[a-zA-Z0-9\-]{0,61}[a-zA-Z0-9])?)*$/;
    if (!emailRegex.test(email)) {
      alert(t('alert.email_invalid'));
      return;
    }
    currentRecipients.push(email);
    input.value = '';
    renderRecipients();
  });
}

// Yeni grup olustur butonu
var createMailGroupBtn = document.getElementById('createMailGroup');
if (createMailGroupBtn) {
  createMailGroupBtn.addEventListener('click', function() {
    openMailGroupDetail(-1);
  });
}

// Grup kaydet butonu
var mailGroupSaveBtn = document.getElementById('mailGroupSaveBtn');
if (mailGroupSaveBtn) {
  mailGroupSaveBtn.addEventListener('click', function() {
    var name = (document.getElementById('mailGroupName') || {}).value || '';
    var subject = (document.getElementById('mailGroupSubject') || {}).value || '';
    var body = (document.getElementById('mailGroupContent') || {}).value || '';
    var recipients = currentRecipients.join(',');

    if (!name) {
      alert(t('alert.group_name_required'));
      return;
    }

    var data = {
      id: String(currentMailGroupId),
      name: name,
      subject: subject,
      body: body,
      recipients: recipients
    };

    fetch('/api/trigger-mail/save', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(data)
    })
      .then(function(r) { return r.json(); })
      .then(function(d) {
        if (d.status === 'ok') {
          closeMailGroupDetail();
          loadTriggerMailGroups();
        } else {
          alert(t('alert.save_error') + ': ' + (d.message || ''));
        }
      })
      .catch(function() {
        alert(t('alert.group_save_err'));
      });
  });
}

// Grup sil butonu
var mailGroupDeleteBtn = document.getElementById('mailGroupDeleteBtn');
if (mailGroupDeleteBtn) {
  mailGroupDeleteBtn.addEventListener('click', function() {
    if (currentMailGroupId < 0) return;
    if (!confirm(t('confirm.delete_group'))) return;

    fetch('/api/trigger-mail/delete', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ id: String(currentMailGroupId) })
    })
      .then(function(r) { return r.json(); })
      .then(function(d) {
        if (d.status === 'ok') {
          closeMailGroupDetail();
          loadTriggerMailGroups();
        } else {
          alert(t('alert.delete_error', {msg: d.message || ''}));
        }
      })
      .catch(function() {
        alert(t('alert.group_delete_err'));
      });
  });
}

// ============================================================
// Dosya Ek Yonetimi (Tetikleme Mail Gruplari)
// ============================================================

var currentGroupFiles = [];

function loadGroupFiles(groupId) {
  var container = document.getElementById('mailGroupFileList');
  if (!container) return;
  if (groupId < 0) {
    container.innerHTML = '';
    currentGroupFiles = [];
    return;
  }
  fetch('/api/trigger-mail/files?group=' + groupId)
    .then(function(r) { return r.json(); })
    .then(function(d) {
      currentGroupFiles = d.files || [];
      renderGroupFiles();
    })
    .catch(function() {
      currentGroupFiles = [];
      container.innerHTML = '';
    });
}

function formatFileSize(bytes) {
  if (bytes >= 1048576) return (bytes / 1048576).toFixed(1) + ' MB';
  if (bytes >= 1024) return Math.round(bytes / 1024) + ' KB';
  return bytes + ' B';
}

function renderGroupFiles() {
  var container = document.getElementById('mailGroupFileList');
  if (!container) return;
  container.innerHTML = '';

  for (var i = 0; i < currentGroupFiles.length; i++) {
    var f = currentGroupFiles[i];
    var row = document.createElement('div');
    row.className = 'recipient-item';
    row.innerHTML =
      '<span class="recipient-email" style="flex:1">' + f.name + '</span>' +
      '<span style="color:#888;font-size:12px;margin-right:8px">' + formatFileSize(f.size) + '</span>' +
      '<button class="recipient-remove" data-fname="' + f.name + '" title="Sil">' +
        '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="14" height="14">' +
          '<line x1="18" y1="6" x2="6" y2="18"></line>' +
          '<line x1="6" y1="6" x2="18" y2="18"></line>' +
        '</svg>' +
      '</button>';
    container.appendChild(row);
  }

  // Sil butonlarina event listener
  var delBtns = container.querySelectorAll('.recipient-remove');
  for (var d = 0; d < delBtns.length; d++) {
    delBtns[d].addEventListener('click', function() {
      var fname = this.getAttribute('data-fname');
      deleteGroupFile(fname);
    });
  }
}

function uploadGroupFile(file) {
  if (currentMailGroupId < 0) {
    alert(t('alert.save_first'));
    return;
  }
  if (file.size > 2 * 1024 * 1024) {
    alert(t('alert.file_too_large', {name: file.name}));
    return;
  }
  fetch('/api/trigger-mail/upload', {
    method: 'POST',
    headers: {
      'X-Group-Id': String(currentMailGroupId),
      'X-Filename': file.name
    },
    body: file
  })
    .then(function(r) { return r.json(); })
    .then(function(d) {
      if (d.status === 'ok') {
        loadGroupFiles(currentMailGroupId);
      } else {
        alert(t('alert.file_upload_fail', {msg: d.message || ''}));
      }
    })
    .catch(function() {
      alert(t('alert.file_upload_err'));
    });
}

function deleteGroupFile(filename) {
  if (currentMailGroupId < 0) return;
  fetch('/api/trigger-mail/file-delete', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ group: String(currentMailGroupId), name: filename })
  })
    .then(function(r) { return r.json(); })
    .then(function(d) {
      if (d.status === 'ok') {
        loadGroupFiles(currentMailGroupId);
      }
    })
    .catch(function() {});
}

// Dosya secme butonu ve input
var mailGroupFileBtn = document.getElementById('mailGroupFileBtn');
var mailGroupFileInput = document.getElementById('mailGroupFile');

if (mailGroupFileBtn && mailGroupFileInput) {
  mailGroupFileBtn.addEventListener('click', function() {
    mailGroupFileInput.click();
  });
  mailGroupFileInput.addEventListener('change', function() {
    for (var i = 0; i < this.files.length; i++) {
      uploadGroupFile(this.files[i]);
    }
    this.value = '';
  });
}

// Mail grup iptal butonu - grup detay'dan grup listesine don
var mailGroupCancelBtn = document.getElementById('mailGroupCancelBtn');
if (mailGroupCancelBtn) {
  mailGroupCancelBtn.addEventListener('click', function() {
    // Eger mailGroupDetailView aciksa, triggerMailConfigView'a don
    var detailView = document.getElementById('mailGroupDetailView');
    if (detailView && !detailView.classList.contains('hidden')) {
      closeMailGroupDetail();
    } else {
      closeActionSubpage();
    }
  });
}

// ============================================================
// Action Card Aktif Durumlari (NVS persist)
// ============================================================

function loadActionStates() {
  fetch('/api/actions/get')
    .then(function(r) { return r.json(); })
    .then(function(d) {
      var mapping = {
        'early-telegram': d.early_telegram,
        'early-mail': d.early_mail,
        'early-api': d.early_api,
        'trigger-mail': d.trigger_mail,
        'trigger-api': d.trigger_api,
        'trigger-relay': d.trigger_relay
      };
      var cards = document.querySelectorAll('.action-card');
      for (var i = 0; i < cards.length; i++) {
        var action = cards[i].getAttribute('data-action');
        if (action && mapping[action] !== undefined) {
          cards[i].setAttribute('data-active', mapping[action] ? 'true' : 'false');
        }
      }
    })
    .catch(function() {});
}

function saveActionStates() {
  var data = {};
  var cards = document.querySelectorAll('.action-card');
  for (var i = 0; i < cards.length; i++) {
    var action = cards[i].getAttribute('data-action');
    var active = cards[i].getAttribute('data-active') === 'true';
    if (action) {
      // data-action -> JSON key donusumu
      var key = action.replace(/-/g, '_');
      data[key] = active ? 'true' : 'false';
    }
  }

  fetch('/api/actions/save', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(data)
  }).catch(function() {});
}

// ============================================================
// Uzaktan Sifirlama API
// ============================================================
function loadResetApiConfig() {
  fetch('/api/reset/config')
    .then(function(r) { return r.json(); })
    .then(function(d) {
      var toggle = document.getElementById('resetApiEnabled');
      if (toggle) toggle.checked = d.enabled;
      var keyEl = document.getElementById('resetApiKey');
      if (keyEl) keyEl.value = d.has_key ? d.masked_key : t('reset_api.not_generated');

      // Ornek komutlari guncelle
      updateResetApiExamples(d.has_key ? d.masked_key : 'YOUR_API_KEY');
    })
    .catch(function() {});
}

function updateResetApiExamples(keyText) {
  // Hostname: mDNS veya IP
  var hostname = window.location.host || 'cihaz.local';
  var blocks = document.querySelectorAll('#resetApiSubpage .code-block');
  if (blocks.length >= 1) blocks[0].textContent = 'GET /api/reset?key=' + keyText;
  if (blocks.length >= 2) blocks[1].textContent = 'curl http://' + hostname + '/api/reset?key=' + keyText;
}

// Toggle degisince kaydet
var resetApiToggle = document.getElementById('resetApiEnabled');
if (resetApiToggle) {
  resetApiToggle.addEventListener('change', function() {
    fetch('/api/reset/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ enabled: this.checked ? 'true' : 'false' })
    }).catch(function() {});
  });
}

// Kopyala butonu
var copyApiKeyBtn = document.getElementById('copyApiKeyBtn');
if (copyApiKeyBtn) {
  copyApiKeyBtn.addEventListener('click', function() {
    var keyEl = document.getElementById('resetApiKey');
    if (!keyEl || !keyEl.value) return;
    if (navigator.clipboard) {
      navigator.clipboard.writeText(keyEl.value);
    } else {
      keyEl.select();
      document.execCommand('copy');
    }
  });
}

// Yeni key uret butonu
var refreshApiKeyBtn = document.getElementById('refreshApiKeyBtn');
if (refreshApiKeyBtn) {
  refreshApiKeyBtn.addEventListener('click', function() {
    if (!confirm(t('confirm.new_key'))) return;
    fetch('/api/reset/generate', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: '{}'
    })
      .then(function(r) { return r.json(); })
      .then(function(d) {
        if (d.status === 'ok' && d.api_key) {
          var keyEl = document.getElementById('resetApiKey');
          if (keyEl) keyEl.value = d.api_key;
          // Toggle'i aktif yap
          var toggle = document.getElementById('resetApiEnabled');
          if (toggle) toggle.checked = true;
          // Otomatik kopyala
          if (navigator.clipboard) {
            navigator.clipboard.writeText(d.api_key);
          }
          updateResetApiExamples(d.api_key);
          alert(t('alert.key_copied', {key: d.api_key}));
        }
      })
      .catch(function() {});
  });
}

// Baslangic verileri yukle
loadActionStates();
loadTriggerMailGroups();

// Dil yukle (en son - tum DOM hazir olduktan sonra)
applyLanguage(currentLang);
