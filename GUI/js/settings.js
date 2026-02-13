/**
 * LebensSpur - Settings (WiFi, SMTP, Relay, Export/Import, System)
 */

// ─────────────────────────────────────────────────────────────────────────────
// WiFi Config Load/Save
// ─────────────────────────────────────────────────────────────────────────────
function loadWifiConfig() {
    authFetch('/api/config/wifi')
        .then(res => res.json())
        .then(data => {
            // Primary WiFi
            if (data.primary) {
                const p = data.primary;
                const ssid = document.getElementById('wifiSsid');
                const pass = document.getElementById('wifiPass');
                const staticEn = document.getElementById('wifiStaticIpEnabled');
                const staticIp = document.getElementById('wifiStaticIp');
                const gw = document.getElementById('wifiGateway');
                const subnet = document.getElementById('wifiSubnet');
                const dns = document.getElementById('wifiDns');

                if (ssid) ssid.value = p.ssid || '';
                if (pass) pass.value = p.password || '';
                if (staticEn) {
                    staticEn.checked = p.staticIpEnabled || false;
                    const fields = document.getElementById('wifiStaticIpFields');
                    if (fields) fields.classList.toggle('hidden', !staticEn.checked);
                }
                if (staticIp) staticIp.value = p.staticIp || '';
                if (gw) gw.value = p.gateway || '';
                if (subnet) subnet.value = p.subnet || '';
                if (dns) dns.value = p.dns || '';
            }

            // mDNS hostname (device-level)
            const mdnsHost = data.hostname || '';
            const wifiMdns = document.getElementById('wifiMdnsHostname');
            const wifiBackupMdns = document.getElementById('wifiBackupMdnsHostname');
            if (wifiMdns) wifiMdns.value = mdnsHost;
            if (wifiBackupMdns) wifiBackupMdns.value = mdnsHost;

            // Backup WiFi
            if (data.backup) {
                const b = data.backup;
                const ssid = document.getElementById('wifiBackupSsid');
                const pass = document.getElementById('wifiBackupPass');
                const staticEn = document.getElementById('wifiBackupStaticIpEnabled');
                const staticIp = document.getElementById('wifiBackupStaticIp');
                const gw = document.getElementById('wifiBackupGateway');
                const subnet = document.getElementById('wifiBackupSubnet');
                const dns = document.getElementById('wifiBackupDns');

                if (ssid) ssid.value = b.ssid || '';
                if (pass) pass.value = b.password || '';
                if (staticEn) {
                    staticEn.checked = b.staticIpEnabled || false;
                    const fields = document.getElementById('wifiBackupStaticIpFields');
                    if (fields) fields.classList.toggle('hidden', !staticEn.checked);
                }
                if (staticIp) staticIp.value = b.staticIp || '';
                if (gw) gw.value = b.gateway || '';
                if (subnet) subnet.value = b.subnet || '';
                if (dns) dns.value = b.dns || '';
            }

            // Update accordion configured status
            initConnectionConfigCheck();
        })
        .catch(err => console.error('WiFi config load failed:', err));
}

function saveWifiConfig(type) {
    const isBackup = (type === 'backup');
    const prefix = isBackup ? 'wifiBackup' : 'wifi';

    const ssid = document.getElementById(prefix + 'Ssid');
    const pass = document.getElementById(prefix + 'Pass');
    const staticEn = document.getElementById(prefix + 'StaticIpEnabled');
    const staticIp = document.getElementById(prefix + 'StaticIp');
    const gw = document.getElementById(prefix + 'Gateway');
    const subnet = document.getElementById(prefix + 'Subnet');
    const dns = document.getElementById(prefix + 'Dns');

    const payload = {
        type: isBackup ? 'backup' : 'primary',
        ssid: ssid ? ssid.value.trim() : '',
        password: pass ? pass.value : '',
        staticIpEnabled: staticEn ? staticEn.checked : false,
        staticIp: staticIp ? staticIp.value.trim() : '',
        gateway: gw ? gw.value.trim() : '',
        subnet: subnet ? subnet.value.trim() : '',
        dns: dns ? dns.value.trim() : ''
    };

    // Include mDNS hostname (device-level, sent with primary wifi save)
    const mdnsInput = document.getElementById(prefix + 'MdnsHostname');
    if (mdnsInput && mdnsInput.value.trim()) {
        payload.mdnsHostname = mdnsInput.value.trim();
    }

    if (!payload.ssid) {
        showToast('SSID boş olamaz', 'error');
        return;
    }

    authFetch('/api/config/wifi', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
    })
    .then(res => res.json())
    .then(data => {
        if (data.success) {
            showToast(isBackup ? t('toast.backupWifiSaved') : t('toast.wifiSaved'), 'success');
            initConnectionConfigCheck();
        } else {
            showToast(data.error || 'Kaydetme hatası', 'error');
        }
    })
    .catch(() => showToast('Bağlantı hatası', 'error'));
}

// ─────────────────────────────────────────────────────────────────────────────
// SMTP Config Load/Save
// ─────────────────────────────────────────────────────────────────────────────
function loadSmtpConfig() {
    authFetch('/api/config/smtp')
        .then(res => res.json())
        .then(data => {
            const server = document.getElementById('smtpServer');
            const port = document.getElementById('smtpPort');
            const user = document.getElementById('smtpUser');
            const apiKey = document.getElementById('smtpApiKey');

            if (server) server.value = data.smtpServer || '';
            if (port) port.value = data.smtpPort || 465;
            if (user) user.value = data.smtpUsername || '';
            if (apiKey) apiKey.value = data.smtpPassword || '';

            // Update accordion configured status
            initConnectionConfigCheck();
        })
        .catch(err => console.error('SMTP config load failed:', err));
}

function saveSmtpConfig() {
    const server = document.getElementById('smtpServer');
    const port = document.getElementById('smtpPort');
    const user = document.getElementById('smtpUser');
    const apiKey = document.getElementById('smtpApiKey');

    const payload = {
        smtpServer: server ? server.value.trim() : '',
        smtpPort: port ? parseInt(port.value) || 465 : 465,
        smtpUsername: user ? user.value.trim() : '',
        smtpPassword: apiKey ? apiKey.value.trim() : ''
    };

    if (!payload.smtpServer) {
        showToast('SMTP sunucu boş olamaz', 'error');
        return;
    }

    authFetch('/api/config/smtp', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
    })
    .then(res => res.json())
    .then(data => {
        if (data.success) {
            showToast(t('toast.smtpSaved'), 'success');
            initConnectionConfigCheck();
        } else {
            showToast(data.error || 'Kaydetme hatası', 'error');
        }
    })
    .catch(() => showToast('Bağlantı hatası', 'error'));
}

// ─────────────────────────────────────────────────────────────────────────────
// AP Mode Toggle
// ─────────────────────────────────────────────────────────────────────────────
function saveApModeConfig(enabled) {
    authFetch('/api/config/ap', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ enabled })
    })
    .then(res => res.json())
    .then(data => {
        if (data.success) {
            showToast(enabled ? 'AP Mod açıldı' : 'AP Mod kapatıldı', 'success');
        } else {
            showToast(data.error || 'AP Mod hatası', 'error');
            const toggle = document.getElementById('apModeEnabled');
            if (toggle) toggle.checked = !enabled;
        }
    })
    .catch(() => {
        showToast('Bağlantı hatası', 'error');
        const toggle = document.getElementById('apModeEnabled');
        if (toggle) toggle.checked = !enabled;
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// Relay Config Save/Load
// ─────────────────────────────────────────────────────────────────────────────
function saveRelayConfig() {
    const inverted = document.getElementById('relayInverted');
    const pulseEnabled = document.getElementById('relayPulseEnabled');
    const pulseDuration = document.getElementById('relayPulseDuration');
    const durationValue = document.getElementById('relayDurationValue');
    const durationUnit = document.getElementById('relayDurationUnit');
    const relayDelay = document.getElementById('relayDelay');

    const durVal = parseInt(durationValue?.value) || 1;
    const durUnit = durationUnit?.value || 'seconds';
    const totalMs = (durUnit === 'minutes' ? durVal * 60 : durVal) * 1000;
    const delayMs = (parseInt(relayDelay?.value) || 0) * 1000;

    const payload = {
        inverted: inverted ? inverted.checked : false,
        pulseMode: pulseEnabled ? pulseEnabled.checked : false,
        pulseDurationMs: parseInt(pulseDuration?.value) * 1000 || 5000,
        pulseIntervalMs: parseInt(pulseDuration?.value) * 1000 || 5000,
        onDelayMs: delayMs,
        offDelayMs: totalMs
    };

    authFetch('/api/config/relay', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
    })
    .then(res => res.json())
    .then(data => {
        if (data.success) {
            showToast(t('toast.relaySaved'), 'success');
            handleModalBack();
        } else {
            showToast(data.error || 'Kaydetme hatası', 'error');
        }
    })
    .catch(() => showToast('Bağlantı hatası', 'error'));
}

function loadRelayConfig() {
    authFetch('/api/config/relay')
        .then(res => {
            if (!res.ok) throw new Error('err');
            return res.json();
        })
        .then(data => {
            const inverted = document.getElementById('relayInverted');
            if (inverted) inverted.checked = data.inverted || false;

            const pulseEnabled = document.getElementById('relayPulseEnabled');
            if (pulseEnabled) pulseEnabled.checked = data.pulseMode || false;

            const pulseFields = document.getElementById('relayPulseFields');
            if (pulseFields) pulseFields.classList.toggle('hidden', !data.pulseMode);

            const pulseDuration = document.getElementById('relayPulseDuration');
            if (pulseDuration && data.pulseDurationMs) {
                pulseDuration.value = Math.round(data.pulseDurationMs / 1000);
            }

            const relayDelay = document.getElementById('relayDelay');
            if (relayDelay && data.onDelayMs !== undefined) {
                relayDelay.value = Math.round(data.onDelayMs / 1000);
            }

            const durationValue = document.getElementById('relayDurationValue');
            const durationUnit = document.getElementById('relayDurationUnit');
            if (durationValue && durationUnit && data.offDelayMs) {
                const totalSec = Math.round(data.offDelayMs / 1000);
                if (totalSec >= 60 && totalSec % 60 === 0) {
                    durationUnit.value = 'minutes';
                    durationValue.value = totalSec / 60;
                } else {
                    durationUnit.value = 'seconds';
                    durationValue.value = totalSec || 1;
                }
            }

            // Update idle/active state labels
            const relayIdleState = document.getElementById('relayIdleState');
            const relayActiveState = document.getElementById('relayActiveState');
            if (relayIdleState && relayActiveState) {
                if (data.inverted) {
                    relayIdleState.textContent = 'Enerji VAR';
                    relayIdleState.classList.add('relay-state-active');
                    relayActiveState.textContent = 'Enerji YOK';
                    relayActiveState.classList.remove('relay-state-active');
                } else {
                    relayIdleState.textContent = 'Enerji YOK';
                    relayIdleState.classList.remove('relay-state-active');
                    relayActiveState.textContent = 'Enerji VAR';
                    relayActiveState.classList.add('relay-state-active');
                }
            }

            updateRelayCycleInfo();
        })
        .catch(() => {});
}

// ─────────────────────────────────────────────────────────────────────────────
// Relay Cycle Info Calculation
// ─────────────────────────────────────────────────────────────────────────────
function updateRelayCycleInfo() {
    const cycleText = document.getElementById('relayCycleText');
    const durationValue = document.getElementById('relayDurationValue');
    const durationUnit = document.getElementById('relayDurationUnit');
    const pulseDuration = document.getElementById('relayPulseDuration');

    if (!cycleText || !durationValue || !durationUnit || !pulseDuration) return;

    const value = parseInt(durationValue.value) || 1;
    const unit = durationUnit.value;
    const totalSeconds = unit === 'minutes' ? value * 60 : value;

    const pulseSeconds = parseInt(pulseDuration.value) || 5;
    const cycleTime = pulseSeconds * 2;
    const cycles = Math.floor(totalSeconds / cycleTime);
    const remaining = totalSeconds % cycleTime;

    let totalText = '';
    if (totalSeconds >= 60) {
        const mins = Math.floor(totalSeconds / 60);
        const secs = totalSeconds % 60;
        totalText = secs > 0 ? `${mins} dk ${secs} sn` : `${mins} dakika`;
    } else {
        totalText = `${totalSeconds} saniye`;
    }

    if (cycles > 0) {
        let text = `${pulseSeconds} sn açık → ${pulseSeconds} sn kapalı × ${cycles} döngü`;
        if (remaining > 0) {
            text += ` + ${remaining} sn`;
        }
        text += ` = ${totalText}`;
        cycleText.textContent = text;
    } else {
        cycleText.textContent = `Süre çok kısa! En az ${cycleTime} saniye gerekli.`;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Connection Accordions Configuration Check
// ─────────────────────────────────────────────────────────────────────────────
function initConnectionConfigCheck() {
    const accordionConfigs = {
        'smtpAccordion': ['smtpServer', 'smtpUser', 'smtpApiKey'],
        'wifiAccordion': ['wifiSsid', 'wifiPass'],
        'wifiBackupAccordion': ['wifiBackupSsid', 'wifiBackupPass'],
        'publicWifiAccordion': ['publicWifiSsid']
    };

    function checkAccordionConfig(accordionId, inputIds) {
        const accordion = document.getElementById(accordionId);
        if (!accordion) return;

        const isConfigured = inputIds.some(id => {
            const input = document.getElementById(id);
            return input && input.value.trim() !== '';
        });

        accordion.classList.toggle('configured', isConfigured);
    }

    Object.entries(accordionConfigs).forEach(([accordionId, inputIds]) => {
        checkAccordionConfig(accordionId, inputIds);

        inputIds.forEach(inputId => {
            const input = document.getElementById(inputId);
            if (input) {
                input.addEventListener('input', () => {
                    checkAccordionConfig(accordionId, inputIds);
                });
            }
        });
    });

    initStaticIpToggles();
    initApModeValues();
}

function initApModeValues() {
    authFetch('/api/device/info')
        .then(res => res.json())
        .then(data => {
            const deviceId = data.device_id || 'LS-UNKNOWN';
            const hostname = data.hostname || deviceId;

            const apSsidInput = document.getElementById('apSsid');
            const apPassInput = document.getElementById('apPass');
            const apMdnsInput = document.getElementById('apMdns');
            const wifiMdnsInput = document.getElementById('wifiMdnsHostname');
            const wifiBackupMdnsInput = document.getElementById('wifiBackupMdnsHostname');

            if (apSsidInput) apSsidInput.value = hostname;
            if (apPassInput) apPassInput.value = 'smartkraft';
            if (apMdnsInput) apMdnsInput.value = hostname + '.local';
            if (wifiMdnsInput) wifiMdnsInput.placeholder = hostname;
            if (wifiBackupMdnsInput) wifiBackupMdnsInput.placeholder = hostname;

            const apToggle = document.getElementById('apModeEnabled');
            if (apToggle && data.ap_active !== undefined) {
                apToggle.checked = data.ap_active;
                const apWarn = document.getElementById('apModeWarning');
                if (apWarn) apWarn.classList.toggle('hidden', apToggle.checked);
            }
        })
        .catch(() => {});

    const apModeToggle = document.getElementById('apModeEnabled');
    const apModeWarning = document.getElementById('apModeWarning');

    if (apModeToggle && apModeWarning) {
        apModeToggle.addEventListener('change', () => {
            apModeWarning.classList.toggle('hidden', apModeToggle.checked);
            saveApModeConfig(apModeToggle.checked);
        });
    }
}

function initStaticIpToggles() {
    const staticIpConfigs = [
        { toggle: 'wifiStaticIpEnabled', fields: 'wifiStaticIpFields' },
        { toggle: 'wifiBackupStaticIpEnabled', fields: 'wifiBackupStaticIpFields' }
    ];

    staticIpConfigs.forEach(({ toggle, fields }) => {
        const toggleEl = document.getElementById(toggle);
        const fieldsEl = document.getElementById(fields);

        if (toggleEl && fieldsEl) {
            toggleEl.addEventListener('change', () => {
                fieldsEl.classList.toggle('hidden', !toggleEl.checked);
            });
        }
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// Export Settings
// ─────────────────────────────────────────────────────────────────────────────
function handleExport() {
    const authPassword = document.getElementById('exportAuthPassword')?.value;
    const exportPassword = document.getElementById('exportPassword')?.value;
    const exportPasswordConfirm = document.getElementById('exportPasswordConfirm')?.value;

    if (!authPassword) {
        showToast('Tarayıcı şifrenizi girin', 'error');
        return;
    }

    if (exportPassword && exportPassword !== exportPasswordConfirm) {
        showToast('Şifreler eşleşmiyor', 'error');
        return;
    }

    authFetch('/api/config/export', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ authPassword, exportPassword })
    })
    .then(res => {
        if (!res.ok) return res.json().then(d => { throw new Error(d.error || 'Export failed'); });
        return res.blob();
    })
    .then(blob => {
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = `lebensspur-backup-${new Date().toISOString().slice(0,10)}.lsbackup`;
        a.click();
        URL.revokeObjectURL(url);
        showToast(t('importExport.exportSuccess'), 'success');
    })
    .catch(err => showToast(err.message || 'Dışa aktarma başarısız', 'error'));

    document.getElementById('exportAuthPassword').value = '';
    document.getElementById('exportPassword').value = '';
    document.getElementById('exportPasswordConfirm').value = '';
}

// ─────────────────────────────────────────────────────────────────────────────
// Import Settings
// ─────────────────────────────────────────────────────────────────────────────
function handleImport() {
    const fileInput = document.getElementById('importFileInput');
    const password = document.getElementById('importPassword')?.value;

    if (!fileInput?.files?.length) {
        showToast('Lütfen bir dosya seçin', 'error');
        return;
    }

    const file = fileInput.files[0];
    const reader = new FileReader();

    reader.onload = function(e) {
        const text = e.target.result;

        authFetch('/api/config/import', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ data: text, password: password || '' })
        })
        .then(res => res.json())
        .then(data => {
            if (data.success) {
                showToast(t('importExport.importSuccess'), 'success');
                setTimeout(() => location.reload(), 3000);
            } else {
                showToast(data.error || 'İçe aktarma başarısız', 'error');
            }
        })
        .catch(() => showToast('Bağlantı hatası', 'error'));
    };

    reader.readAsText(file);
}

// ─────────────────────────────────────────────────────────────────────────────
// Reboot & Factory Reset
// ─────────────────────────────────────────────────────────────────────────────
function handleReboot() {
    if (!confirm(t('settingsSys.confirmReboot'))) return;

    authFetch('/api/reboot', { method: 'POST' })
        .then(res => res.json())
        .then(data => {
            if (data.success) {
                showToast(t('toast.rebootStarted'), 'info');
                setTimeout(() => location.reload(), 5000);
            } else {
                showToast(data.error || 'Yeniden başlatma başarısız', 'error');
            }
        })
        .catch(() => showToast('Bağlantı hatası', 'error'));
}

function handleFactoryReset() {
    if (!confirm(t('settingsSys.confirmFactoryReset'))) return;
    if (!confirm('Bu işlem geri alınamaz! Emin misiniz?')) return;

    authFetch('/api/factory-reset', { method: 'POST' })
        .then(res => res.json())
        .then(data => {
            if (data.success) {
                showToast(t('toast.factoryResetStarted'), 'info');
                localStorage.clear();
                setTimeout(() => location.reload(), 5000);
            } else {
                showToast(data.error || 'Fabrika sıfırlama başarısız', 'error');
            }
        })
        .catch(() => showToast('Bağlantı hatası', 'error'));
}
