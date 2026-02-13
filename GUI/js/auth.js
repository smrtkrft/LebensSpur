/**
 * LebensSpur - Authentication & Security
 */

// ─────────────────────────────────────────────────────────────────────────────
// Auth Check on Page Load
// ─────────────────────────────────────────────────────────────────────────────
function checkAuthOnLoad() {
    // Token yoksa login sayfasi goster
    if (!state.authToken) {
        showPage('login');
        return;
    }

    // Token var, sunucudan dogrula
    fetch('/api/timer/status', {
        headers: { 'Authorization': 'Bearer ' + state.authToken }
    })
    .then(res => {
        if (res.status === 401) {
            // Token gecersiz, login sayfasi goster
            state.isAuthenticated = false;
            state.authToken = null;
            localStorage.removeItem('ls_token');
            showPage('login');
        } else {
            // Token gecerli, app sayfasini goster
            state.isAuthenticated = true;
            showPage('app');
            startAutoLogoutTimer();
            updateUI();
            startTimerUpdate();
            fetchDeviceInfo();
            loadSecuritySettings();
            // Stagger post-login requests
            setTimeout(() => {
                loadTimerConfig();
                loadMailGroups();
            }, 200);
            setTimeout(() => {
                loadWifiConfig();
                loadSmtpConfig();
            }, 400);
        }
    })
    .catch(() => {
        // Baglanti hatasi, login goster
        showPage('login');
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// Login / Logout
// ─────────────────────────────────────────────────────────────────────────────
function handleLogin(e) {
    e.preventDefault();

    const passwordInput = document.getElementById('password');
    const password = passwordInput.value;
    const loginBtn = document.querySelector('#login-form button[type="submit"]');

    if (!password) {
        showToast('Şifre gerekli', 'error');
        return;
    }

    // Disable button during request
    loginBtn.disabled = true;
    loginBtn.textContent = '...';

    authFetch('/api/login', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ password })
    })
    .then(res => res.json())
    .then(data => {
        if (data.success) {
            state.isAuthenticated = true;
            state.authToken = data.token || null;
            // Token'i localStorage'a kaydet
            if (data.token) {
                localStorage.setItem('ls_token', data.token);
            }
            state.loginAttempts = 0;
            state.lockoutUntil = null;
            showPage('app');
            showToast(t('toast.loginSuccess'), 'success');
            startAutoLogoutTimer();
            // Stagger post-login requests to avoid overwhelming ESP32
            pollTimerStatus();
            loadTimerConfig();
            setTimeout(() => {
                loadMailGroups();
                loadWifiConfig();
            }, 300);
            setTimeout(() => {
                loadSmtpConfig();
                loadSecuritySettings();
            }, 600);
            setTimeout(() => {
                fetchLogs();
                fetchDeviceInfo();
            }, 900);
        } else if (data.lockoutSeconds) {
            const mins = Math.ceil(data.lockoutSeconds / 60);
            state.lockoutUntil = Date.now() + (data.lockoutSeconds * 1000);
            showToast(`Hesap kilitli. ${mins} dakika bekleyin.`, 'error');
            state.loginAttempts = 0;
        } else {
            state.loginAttempts++;
            const remaining = data.remainingAttempts || (state.maxLoginAttempts - state.loginAttempts);
            showToast(`Geçersiz şifre. ${remaining} deneme kaldı.`, 'error');
            passwordInput.classList.add('shake');
            setTimeout(() => passwordInput.classList.remove('shake'), 500);
        }
    })
    .catch(() => {
        showToast('Sunucuya bağlanılamıyor', 'error');
    })
    .finally(() => {
        loginBtn.disabled = false;
        loginBtn.textContent = 'Giriş';
    });
}

function handleLogout() {
    authFetch('/api/logout', { method: 'POST' }).catch(() => {});
    state.isAuthenticated = false;
    state.authToken = null;
    localStorage.removeItem('ls_token');
    stopAutoLogoutTimer();
    if (timerPollInterval) { clearInterval(timerPollInterval); timerPollInterval = null; }
    if (timerCountdownInterval) { clearInterval(timerCountdownInterval); timerCountdownInterval = null; }
    showPage('login');
    showToast(t('toast.loggedOut'), 'info');
}

// ─────────────────────────────────────────────────────────────────────────────
// Password Change
// ─────────────────────────────────────────────────────────────────────────────
function handlePasswordChange() {
    const currentPassword = document.getElementById('currentPassword')?.value;
    const newPassword = document.getElementById('newPassword')?.value;
    const confirmPassword = document.getElementById('confirmPassword')?.value;

    if (!currentPassword) {
        showToast('Mevcut şifrenizi girin', 'error');
        return;
    }

    if (!newPassword) {
        showToast('Yeni şifre girin', 'error');
        return;
    }

    if (newPassword.length < 4) {
        showToast('Şifre en az 4 karakter olmalı', 'error');
        return;
    }

    if (newPassword !== confirmPassword) {
        showToast('Yeni şifreler eşleşmiyor', 'error');
        return;
    }

    if (currentPassword === newPassword) {
        showToast('Yeni şifre mevcut şifreden farklı olmalı', 'error');
        return;
    }

    authFetch('/api/config/password', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ currentPassword, newPassword })
    })
    .then(res => res.json())
    .then(data => {
        if (data.success) {
            showToast(t('toast.passwordChanged'), 'success');
            document.getElementById('currentPassword').value = '';
            document.getElementById('newPassword').value = '';
            document.getElementById('confirmPassword').value = '';
        } else {
            showToast(data.error || 'Şifre değiştirilemedi', 'error');
        }
    })
    .catch(() => showToast(t('toast.connectionError'), 'error'));
}

// ─────────────────────────────────────────────────────────────────────────────
// Security Settings
// ─────────────────────────────────────────────────────────────────────────────
function loadSecuritySettings() {
    // Load auto-logout from localStorage
    const autoLogout = parseInt(localStorage.getItem('autoLogoutTime')) || 10;
    state.autoLogoutTime = autoLogout;
    const autoLogoutInput = document.getElementById('autoLogoutTime');
    if (autoLogoutInput) autoLogoutInput.value = autoLogout;

    // Load security settings from backend
    authFetch('/api/config/security')
        .then(res => res.json())
        .then(data => {
            const loginProt = document.getElementById('loginProtection');
            const lockoutInput = document.getElementById('lockoutTime');
            const apiEnabled = document.getElementById('resetApiEnabled');
            const apiKeyInput = document.getElementById('resetApiKey');

            if (loginProt && data.loginProtection !== undefined) loginProt.checked = data.loginProtection;
            if (lockoutInput && data.lockoutTime) lockoutInput.value = data.lockoutTime;
            if (apiEnabled && data.resetApiEnabled !== undefined) apiEnabled.checked = data.resetApiEnabled;
            if (apiKeyInput && data.apiKey) apiKeyInput.value = data.apiKey;
        })
        .catch(() => {}); // Silently fail on load
}

function saveSecuritySettings() {
    const loginProtection = document.getElementById('loginProtection')?.checked ?? true;
    const lockoutTime = parseInt(document.getElementById('lockoutTime')?.value) || 15;
    const resetApiEnabled = document.getElementById('resetApiEnabled')?.checked ?? false;

    authFetch('/api/config/security', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ loginProtection, lockoutTime, resetApiEnabled })
    })
    .then(res => res.json())
    .then(data => {
        if (data.success) {
            showToast(t('toast.securitySaved'), 'success');
        } else {
            showToast(data.error || 'Güvenlik ayarları kaydedilemedi', 'error');
        }
    })
    .catch(() => showToast('Bağlantı hatası', 'error'));
}

function handleRefreshApiKey() {
    authFetch('/api/config/security/api-key', { method: 'POST' })
        .then(res => res.json())
        .then(data => {
            if (data.apiKey) {
                document.getElementById('resetApiKey').value = data.apiKey;
                showToast(t('toast.apiKeyRefreshed'), 'success');
            }
        })
        .catch(() => showToast('Bağlantı hatası', 'error'));
}

// ─────────────────────────────────────────────────────────────────────────────
// Device Info
// ─────────────────────────────────────────────────────────────────────────────
function fetchDeviceInfo() {
    authFetch('/api/device/info')
        .then(res => res.json())
        .then(data => {
            const setTxt = (id, val) => {
                const el = document.getElementById(id);
                if (el) el.textContent = val;
            };
            const setBar = (id, pct) => {
                const el = document.getElementById(id);
                if (el) el.style.width = pct + '%';
            };
            const fmtBytes = (b) => {
                if (b >= 1048576) return (b / 1048576).toFixed(1) + ' MB';
                if (b >= 1024) return Math.round(b / 1024) + ' KB';
                return b + ' B';
            };

            // Firmware version
            const fw = data.firmware ? ('v' + data.firmware) : '';
            if (fw) {
                setTxt('loginVersion', fw);
                setTxt('sysFirmware', fw);
                setTxt('currentFirmwareVersion', fw);
            }

            // GUI version for OTA subpage
            setTxt('currentGuiVersion', fw || '-');

            // External flash status for GUI OTA subpage
            if (data.ext_flash_total) {
                const fsTotal = (data.fs_cfg_total || 0) + (data.fs_gui_total || 0) + (data.fs_data_total || 0);
                const fsUsed = (data.fs_cfg_used || 0) + (data.fs_gui_used || 0) + (data.fs_data_used || 0);
                const fsFree = fsTotal > fsUsed ? fsTotal - fsUsed : 0;
                setTxt('extFlashStatus', fmtBytes(fsFree) + ' boş / ' + fmtBytes(fsTotal));
            } else {
                setTxt('extFlashStatus', 'Algılanmadı');
            }

            // Device identity
            setTxt('sysDeviceId', data.device_id || '-');
            setTxt('sysMac', data.mac || '-');
            setTxt('sysChip', data.chip_model || '-');
            setTxt('sysCores', data.chip_cores != null ? data.chip_cores : '-');
            setTxt('sysCpuFreq', data.cpu_freq_mhz ? (data.cpu_freq_mhz + ' MHz') : '-');
            setTxt('sysFlashSize', data.int_flash_total ? fmtBytes(data.int_flash_total) : '-');

            // RAM / Heap
            if (data.heap_total) {
                const heapUsed = data.heap_total - (data.heap_free || 0);
                const heapPct = (heapUsed / data.heap_total * 100).toFixed(1);
                setTxt('sysRamTotal', fmtBytes(data.heap_total));
                setTxt('sysRamUsed', fmtBytes(heapUsed));
                setTxt('sysRamFree', fmtBytes(data.heap_free || 0));
                setTxt('sysRamMinFree', fmtBytes(data.heap_min_free || 0));
                setBar('ramBarFill', heapPct);
                setTxt('ramUsagePercent', heapPct + '%');
            }

            // Internal Flash
            if (data.int_flash_total) {
                const appSz = data.app_size || 0;
                const otaSz = data.ota_size || 0;
                const nvsSz = data.nvs_size || 0;
                const usedSz = appSz + otaSz + nvsSz;
                const freeSz = data.int_flash_total > usedSz ? data.int_flash_total - usedSz : 0;
                const intPct = (usedSz / data.int_flash_total * 100).toFixed(0);
                setTxt('sysIntFlashTotal', fmtBytes(data.int_flash_total));
                setTxt('sysIntFlashFirmware', fmtBytes(appSz));
                setTxt('sysIntFlashNvs', fmtBytes(nvsSz));
                setTxt('sysIntFlashOta', fmtBytes(otaSz));
                setTxt('sysIntFlashFree', fmtBytes(freeSz));
                setBar('intFlashBarFill', intPct);
                setTxt('intFlashUsagePercent', intPct + '%');
            }

            // External Flash - 3 LittleFS partitions
            if (data.ext_flash_total) {
                const cfgTotal = data.fs_cfg_total || 0;
                const cfgUsed = data.fs_cfg_used || 0;
                const guiTotal = data.fs_gui_total || 0;
                const guiUsed = data.fs_gui_used || 0;
                const dataTotal = data.fs_data_total || 0;
                const dataUsed = data.fs_data_used || 0;

                const fsTotal = cfgTotal + guiTotal + dataTotal;
                const fsUsed = cfgUsed + guiUsed + dataUsed;
                const fsFree = fsTotal > fsUsed ? fsTotal - fsUsed : 0;
                const extPct = fsTotal ? (fsUsed / fsTotal * 100).toFixed(1) : '0';

                setTxt('sysExtFlashTotal', fmtBytes(data.ext_flash_total));

                // Per-partition info
                const cfgPct = cfgTotal ? (cfgUsed / cfgTotal * 100).toFixed(1) : '0';
                setTxt('sysFsCfgInfo', fmtBytes(cfgUsed) + ' / ' + fmtBytes(cfgTotal));
                setBar('fsCfgBarFill', cfgPct);

                const guiPct = guiTotal ? (guiUsed / guiTotal * 100).toFixed(1) : '0';
                setTxt('sysFsGuiInfo', fmtBytes(guiUsed) + ' / ' + fmtBytes(guiTotal));
                setBar('fsGuiBarFill', guiPct);

                const dataPct = dataTotal ? (dataUsed / dataTotal * 100).toFixed(1) : '0';
                setTxt('sysFsDataInfo', fmtBytes(dataUsed) + ' / ' + fmtBytes(dataTotal));
                setBar('fsDataBarFill', dataPct);

                // Totals
                setTxt('sysExtFlashUsed', fmtBytes(fsUsed));
                setTxt('sysExtFlashFree', fmtBytes(fsFree));
                setBar('extFlashBarFill', extPct);
                setTxt('extFlashUsagePercent', extPct + '%');
            }

            // WiFi status
            if (data.sta_connected) {
                setTxt('sysWifiStatus', 'Bağlı');
                const el = document.getElementById('sysWifiStatus');
                if (el) el.className = 'system-value status-connected';
            } else {
                setTxt('sysWifiStatus', 'Bağlı Değil');
                const el = document.getElementById('sysWifiStatus');
                if (el) el.className = 'system-value status-disconnected';
            }
            setTxt('sysWifiSsid', data.sta_ssid || '-');
            if (data.sta_rssi != null && data.sta_connected) {
                const rssi = data.sta_rssi;
                let quality = 'Çok Zayıf';
                if (rssi >= -50) quality = 'Mükemmel';
                else if (rssi >= -60) quality = 'Çok İyi';
                else if (rssi >= -70) quality = 'İyi';
                else if (rssi >= -80) quality = 'Zayıf';
                setTxt('sysRssi', rssi + ' dBm (' + quality + ')');
            } else {
                setTxt('sysRssi', '-');
            }
            setTxt('sysIp', data.sta_ip || '-');
            if (data.ap_active) {
                setTxt('sysApStatus', 'Aktif');
                const el = document.getElementById('sysApStatus');
                if (el) el.className = 'system-value status-active';
            } else {
                setTxt('sysApStatus', 'Kapalı');
                const el = document.getElementById('sysApStatus');
                if (el) el.className = 'system-value';
            }
            setTxt('sysApIp', data.ap_ip || '-');

            // Uptime
            if (data.uptime_s != null) {
                const s = data.uptime_s;
                const days = Math.floor(s / 86400);
                const hours = Math.floor((s % 86400) / 3600);
                const mins = Math.floor((s % 3600) / 60);
                let upStr = '';
                if (days > 0) upStr += days + ' gün ';
                if (hours > 0 || days > 0) upStr += hours + ' saat ';
                upStr += mins + ' dk';
                setTxt('sysUptime', upStr.trim());
            }

            // Reset reason
            const resetReasons = {
                1: 'Güç açıldı', 2: 'Harici reset', 3: 'Yazılım reset',
                4: 'Panik / İstisna', 5: 'Watchdog (INT_WDT)',
                6: 'Watchdog (TASK_WDT)', 7: 'Watchdog (Diğer)',
                8: 'Derin uyku', 9: 'Brownout', 10: 'SDIO'
            };
            setTxt('sysResetReason', resetReasons[data.reset_reason] || ('Kod: ' + data.reset_reason));

            // NTP
            setTxt('sysNtpStatus', data.ntp_synced ? 'Senkronize' : 'Senkronize Değil');
            const ntpEl = document.getElementById('sysNtpStatus');
            if (ntpEl) {
                ntpEl.className = data.ntp_synced
                    ? 'system-value status-connected'
                    : 'system-value status-disconnected';
            }

            // System time
            setTxt('sysTime', data.time || '-');

            // GUI version (from sw.js cache version)
            setTxt('sysGuiVersion', data.firmware ? 'v' + data.firmware : '-');
        })
        .catch(err => {
            console.error('Device info fetch failed:', err);
        });
}
