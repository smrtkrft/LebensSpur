/**
 * ═══════════════════════════════════════════════════════════════════════════
 * LebensSpur - Dead Man's Switch Controller
 * ═══════════════════════════════════════════════════════════════════════════
 */

// ─────────────────────────────────────────────────────────────────────────────
// State Management
// ─────────────────────────────────────────────────────────────────────────────
const state = {
    isAuthenticated: false,
    // Timer status (from ESP32 polling)
    timerState: 'DISABLED',  // DISABLED, RUNNING, WARNING, TRIGGERED, PAUSED, VACATION
    timeRemainingMs: 0,
    intervalMinutes: 1440,
    warningsSent: 0,
    resetCount: 0,
    triggerCount: 0,
    timerEnabled: false,
    
    // Theme & Language
    theme: localStorage.getItem('theme') || 'dark',
    language: localStorage.getItem('language') || 'tr',
    
    // Settings Carousel
    carouselIndex: 0,
    carouselTabs: ['timer', 'mail', 'actions', 'system', 'info'],
    isCarouselAnimating: false,
    
    // Navigation Stack for modal
    navStack: [], // [{id: 'root', title: 'Ayarlar', breadcrumb: null}, {id: 'telegram', title: 'Telegram Ayarları', breadcrumb: 'Aksiyonlar'}]
    
    // Timer Configuration
    timerConfig: {
        unit: 'hours',       // 'days', 'hours', 'minutes'
        value: 24,           // 1-60
        alarmCount: 3,       // 0-10 (max = value / 2)
    },
    
    // Vacation Mode
    vacationMode: {
        enabled: false,
        days: 7
    },
    
    // Auto Logout
    autoLogoutTime: 10, // dakika
    lastActivity: Date.now(),
    autoLogoutTimer: null,
    
    // Login Protection
    loginAttempts: 0,
    maxLoginAttempts: 5,
    lockoutTime: 15, // dakika
    lockoutUntil: null,
    
    // PWA
    deferredPrompt: null,
    
    // Audit & Device Logs
    auditLogs: [],
    deviceLogs: [],
    
    // Mail Groups (max 10) - loaded from ESP32 via /api/config/mail
    mailGroups: [],
    currentEditingGroup: null,
    isCreatingNewGroup: false,

    settings: {
        timer: {
            unit: 'hours',
            value: 24,
            alarmCount: 3,
            vacationEnabled: false,
            vacationDays: 7
        },
        notifications: {
            emailEnabled: true,
            telegramEnabled: false,
            telegramChatId: '',
            warningAt: 50,
            tieredEnabled: true,
            recipients: []
        },
        actions: {
            sendEmail: true,
            triggerRelay: true,
            callWebhook: false,
            webhookUrl: '',
            mqttEnabled: false,
            mqttBroker: '',
            mqttTopic: ''
        },
        security: {
            autoLogoutTime: 10,
            loginProtection: true,
            lockoutTime: 15,
            trustedContacts: []
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// DOM Elements
// ─────────────────────────────────────────────────────────────────────────────
const elements = {
    loginPage: null,
    appPage: null,
    settingsModal: null,
    timerRing: null,
    timerValue: null,
    timeDays: null,
    timeHours: null,
    timeMinutes: null,
    settingsNav: null,
    settingsPanels: null
};

// ─────────────────────────────────────────────────────────────────────────────
// Initialization
// ─────────────────────────────────────────────────────────────────────────────
document.addEventListener('DOMContentLoaded', () => {
    cacheElements();
    initTheme();
    initEventListeners();
    initAutoLogout();
    initPWA();
    initLogs();
    updateUI();
    startTimerUpdate();
    fetchDeviceInfo();
    loadSecuritySettings();
    // Initialize i18n language system
    if (window.I18n) {
        I18n.init().then(() => {
            const sel = document.getElementById('languageSelect');
            if (sel) sel.value = I18n.getLanguage();
        });
    }
});

function cacheElements() {
    elements.loginPage = document.getElementById('login-page');
    elements.appPage = document.getElementById('app-page');
    elements.settingsModal = document.getElementById('settingsModal');
    elements.modalTitle = document.getElementById('modalTitle');
    elements.modalBackBtn = document.getElementById('modalBackBtn');
    elements.modalBreadcrumb = document.getElementById('modalBreadcrumb');
    elements.settingsNavCarousel = document.getElementById('settingsNavCarousel');
    elements.timerRing = document.querySelector('.timer-ring');
    elements.timerValue = document.getElementById('timerValue');
    elements.timeDays = document.getElementById('timeDays');
    elements.timeHours = document.getElementById('timeHours');
    elements.timeMinutes = document.getElementById('timeMinutes');
    elements.settingsNav = document.querySelector('.settings-nav-carousel');
    elements.carouselItems = document.getElementById('carouselItems');
    elements.carouselPrev = document.getElementById('carouselPrev');
    elements.carouselNext = document.getElementById('carouselNext');
    elements.navItems = document.querySelectorAll('.nav-item');
    elements.settingsPanels = document.querySelectorAll('.settings-panel');
}

function initEventListeners() {
    // Login Form
    const loginForm = document.getElementById('login-form');
    if (loginForm) {
        loginForm.addEventListener('submit', handleLogin);
    }

    // Reset Button
    const resetBtn = document.getElementById('resetBtn');
    if (resetBtn) {
        resetBtn.addEventListener('click', handleReset);
    }

    // Pause/Play Button
    const pauseBtn = document.getElementById('pauseBtn');
    if (pauseBtn) {
        pauseBtn.addEventListener('click', handlePause);
    }

    // Settings Button
    const settingsBtn = document.getElementById('settingsBtn');
    if (settingsBtn) {
        settingsBtn.addEventListener('click', openSettings);
    }

    // Logout Button
    const logoutBtn = document.getElementById('logoutBtn');
    if (logoutBtn) {
        logoutBtn.addEventListener('click', handleLogout);
    }

    // Modal Close
    const modalClose = document.getElementById('closeSettings');
    if (modalClose) {
        modalClose.addEventListener('click', closeSettings);
    }

    // Cancel Settings (İptal butonu)
    const cancelSettingsBtn = document.getElementById('cancelSettings');
    if (cancelSettingsBtn) {
        cancelSettingsBtn.addEventListener('click', closeSettings);
    }

    // Save Settings (timer only)
    const saveSettingsBtn = document.getElementById('saveSettings');
    if (saveSettingsBtn) {
        saveSettingsBtn.addEventListener('click', handleSaveSettings);
    }

    // WiFi Save Buttons
    const saveWifiBtn = document.getElementById('saveWifiBtn');
    if (saveWifiBtn) {
        saveWifiBtn.addEventListener('click', () => saveWifiConfig('primary'));
    }
    const saveWifiBackupBtn = document.getElementById('saveWifiBackupBtn');
    if (saveWifiBackupBtn) {
        saveWifiBackupBtn.addEventListener('click', () => saveWifiConfig('backup'));
    }

    // SMTP Save Button
    const saveSmtpBtn = document.getElementById('saveSmtpBtn');
    if (saveSmtpBtn) {
        saveSmtpBtn.addEventListener('click', saveSmtpConfig);
    }

    // Settings Navigation - Carousel
    if (elements.carouselPrev) {
        elements.carouselPrev.addEventListener('click', () => navigateCarousel(-1));
    }
    if (elements.carouselNext) {
        elements.carouselNext.addEventListener('click', () => navigateCarousel(1));
    }
    if (elements.carouselItems) {
        elements.carouselItems.addEventListener('click', handleNavClick);
    }

    // Activity Toggle
    const activityToggle = document.querySelector('.section-toggle');
    if (activityToggle) {
        activityToggle.addEventListener('click', toggleActivity);
    }

    // Modal Overlay Click - Ayarlar modalı sadece X veya İptal ile kapanır
    // (Dışarı tıklama ile kapanma devre dışı)

    // Theme Selector (Select element)
    const themeSelect = document.getElementById('themeSelect');
    if (themeSelect) {
        themeSelect.addEventListener('change', () => {
            setTheme(themeSelect.value);
        });
    }

    // Accordion Toggle
    document.querySelectorAll('.accordion-header').forEach(header => {
        header.addEventListener('click', () => {
            const accordion = header.closest('.accordion');
            accordion.classList.toggle('open');
        });
    });

    // PWA Install Button
    const pwaInstallBtn = document.getElementById('pwaInstallBtn');
    if (pwaInstallBtn) {
        pwaInstallBtn.addEventListener('click', installPWA);
    }

    // Export Settings Button - Artık subpage açılıyor, eski modal kaldırıldı
    // const exportBtn = document.getElementById('exportSettingsBtn');

    // OTA Check Button (Firmware)
    const checkFirmwareUpdateBtn = document.getElementById('checkFirmwareUpdateBtn');
    if (checkFirmwareUpdateBtn) {
        checkFirmwareUpdateBtn.addEventListener('click', checkForUpdates);
    }

    // GUI Update Button
    const updateGuiBtn = document.getElementById('updateGuiBtn');
    if (updateGuiBtn) {
        updateGuiBtn.addEventListener('click', handleGuiUpdate);
    }

    // Log Filter Buttons
    document.querySelectorAll('[data-log-filter]').forEach(btn => {
        btn.addEventListener('click', (e) => {
            const filter = e.target.dataset.logFilter;
            filterLogs(filter, e.target.closest('.setting-section'));
        });
    });

    // Export Logs Button
    const exportAuditBtn = document.getElementById('exportAuditLogs');
    if (exportAuditBtn) {
        exportAuditBtn.addEventListener('click', () => exportLogs('audit'));
    }
    
    const exportDeviceBtn = document.getElementById('exportDeviceLogs');
    if (exportDeviceBtn) {
        exportDeviceBtn.addEventListener('click', () => exportLogs('device'));
    }

    // Activity tracking for auto logout
    ['click', 'keypress', 'mousemove', 'scroll', 'touchstart'].forEach(event => {
        document.addEventListener(event, resetAutoLogoutTimer, { passive: true });
    });

    // Timer Configuration Event Listeners
    initTimerConfig();
    
    // Action Cards Event Listeners
    initActionCards();
    
    // OTA Source Selectors
    initOtaSourceSelectors();
    
    // Connection Accordions Configuration Check
    initConnectionConfigCheck();
    
    // Password Change Button
    const changePasswordBtn = document.getElementById('changePasswordBtn');
    if (changePasswordBtn) {
        changePasswordBtn.addEventListener('click', handlePasswordChange);
    }
    
    // Export Button (subpage)
    const startExportBtn = document.getElementById('startExportBtn');
    if (startExportBtn) {
        startExportBtn.addEventListener('click', handleExport);
    }
    
    // Import Button (subpage)
    const startImportBtn = document.getElementById('startImportBtn');
    if (startImportBtn) {
        startImportBtn.addEventListener('click', handleImport);
    }
    
    // Import file area click → trigger file input
    const importFileArea = document.getElementById('importFileArea');
    const importFileInput = document.getElementById('importFileInput');
    if (importFileArea && importFileInput) {
        importFileArea.addEventListener('click', () => importFileInput.click());
        importFileArea.addEventListener('dragover', (e) => { e.preventDefault(); importFileArea.classList.add('dragover'); });
        importFileArea.addEventListener('dragleave', () => importFileArea.classList.remove('dragover'));
        importFileArea.addEventListener('drop', (e) => {
            e.preventDefault();
            importFileArea.classList.remove('dragover');
            if (e.dataTransfer.files.length) {
                importFileInput.files = e.dataTransfer.files;
                importFileInput.dispatchEvent(new Event('change'));
            }
        });
        importFileInput.addEventListener('change', () => {
            const file = importFileInput.files[0];
            if (file) {
                importFileArea.querySelector('p').textContent = file.name;
                document.getElementById('startImportBtn').disabled = false;
            }
        });
    }
    
    // Language Select
    const languageSelect = document.getElementById('languageSelect');
    if (languageSelect) {
        languageSelect.addEventListener('change', () => {
            if (window.I18n) {
                I18n.setLanguage(languageSelect.value);
            }
        });
    }
    
    // Reboot Button
    const rebootBtn = document.getElementById('rebootBtn');
    if (rebootBtn) {
        rebootBtn.addEventListener('click', handleReboot);
    }
    
    // Factory Reset Button
    const factoryResetBtn = document.getElementById('factoryResetBtn');
    if (factoryResetBtn) {
        factoryResetBtn.addEventListener('click', handleFactoryReset);
    }
    
    // Security: Auto Logout Time
    const autoLogoutInput = document.getElementById('autoLogoutTime');
    if (autoLogoutInput) {
        autoLogoutInput.addEventListener('change', () => {
            const val = parseInt(autoLogoutInput.value) || 10;
            state.autoLogoutTime = Math.max(1, Math.min(60, val));
            localStorage.setItem('autoLogoutTime', state.autoLogoutTime);
            autoLogoutInput.value = state.autoLogoutTime;
            showToast('Otomatik çıkış süresi kaydedildi', 'success');
        });
    }
    
    // Security: Login Protection Toggle
    const loginProtection = document.getElementById('loginProtection');
    if (loginProtection) {
        loginProtection.addEventListener('change', () => saveSecuritySettings());
    }
    
    // Security: Lockout Time
    const lockoutInput = document.getElementById('lockoutTime');
    if (lockoutInput) {
        lockoutInput.addEventListener('change', () => saveSecuritySettings());
    }
    
    // Security: Remote API Toggle
    const resetApiEnabled = document.getElementById('resetApiEnabled');
    if (resetApiEnabled) {
        resetApiEnabled.addEventListener('change', () => saveSecuritySettings());
    }
    
    // Copy API Key
    const copyApiKeyBtn = document.getElementById('copyApiKeyBtn');
    if (copyApiKeyBtn) {
        copyApiKeyBtn.addEventListener('click', () => {
            const key = document.getElementById('resetApiKey')?.value;
            if (key && key !== '-') {
                navigator.clipboard.writeText(key).then(() => showToast('API anahtarı kopyalandı', 'success'));
            }
        });
    }
    
    // Refresh API Key
    const refreshApiKeyBtn = document.getElementById('refreshApiKeyBtn');
    if (refreshApiKeyBtn) {
        refreshApiKeyBtn.addEventListener('click', handleRefreshApiKey);
    }

    // ── Enter Key Support ──────────────────────────────────────────────
    // Bind Enter key on input fields to trigger their associated save button
    initEnterKeyBindings();
}

// ─────────────────────────────────────────────────────────────────────────────
// Enter Key Bindings
// ─────────────────────────────────────────────────────────────────────────────
function initEnterKeyBindings() {
    /**
     * Helper: pressing Enter in inputId triggers click on buttonId
     */
    function bindEnter(inputId, buttonId) {
        const inp = document.getElementById(inputId);
        const btn = document.getElementById(buttonId);
        if (inp && btn) {
            inp.addEventListener('keydown', (e) => {
                if (e.key === 'Enter') { e.preventDefault(); btn.click(); }
            });
        }
    }

    // WiFi settings (primary)
    bindEnter('wifiPass', 'saveWifiBtn');

    // WiFi settings (backup)
    bindEnter('wifiBackupPass', 'saveWifiBackupBtn');

    // SMTP settings (last field)
    bindEnter('smtpApiKey', 'saveSmtpBtn');

    // Password change
    bindEnter('confirmPassword', 'changePasswordBtn');

    // Export modal password
    bindEnter('exportPasswordConfirm', 'confirmExport');

    // Import password
    bindEnter('importPassword', 'startImportBtn');
}

// ─────────────────────────────────────────────────────────────────────────────
// Password Change
// ─────────────────────────────────────────────────────────────────────────────
function handlePasswordChange() {
    const currentPassword = document.getElementById('currentPassword')?.value;
    const newPassword = document.getElementById('newPassword')?.value;
    const confirmPassword = document.getElementById('confirmPassword')?.value;
    
    // Validasyonlar
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
    
    // Send to backend
    fetch('/api/config/password', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ currentPassword, newPassword })
    })
    .then(res => res.json())
    .then(data => {
        if (data.success) {
            showToast('Şifre başarıyla değiştirildi', 'success');
            document.getElementById('currentPassword').value = '';
            document.getElementById('newPassword').value = '';
            document.getElementById('confirmPassword').value = '';
        } else {
            showToast(data.error || 'Şifre değiştirilemedi', 'error');
        }
    })
    .catch(() => showToast('Bağlantı hatası', 'error'));
}

// ───────────────────────────────────────────────────────────────────────────────
// Export Settings
// ───────────────────────────────────────────────────────────────────────────────
function handleExport() {
    const authPassword = document.getElementById('exportAuthPassword')?.value;
    const exportPassword = document.getElementById('exportPassword')?.value;
    const exportPasswordConfirm = document.getElementById('exportPasswordConfirm')?.value;
    
    // Tarayıcı şifresi doğrulaması
    if (!authPassword) {
        showToast('Tarayıcı şifrenizi girin', 'error');
        return;
    }
    
    // Send export request to backend
    fetch('/api/config/export', {
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
        a.download = `lebensspur-backup-${new Date().toISOString().slice(0,10)}.lsb`;
        a.click();
        URL.revokeObjectURL(url);
        showToast('Ayarlar dışa aktarıldı', 'success');
    })
    .catch(err => showToast(err.message || 'Dışa aktarma başarısız', 'error'));
    
    // Clear fields
    document.getElementById('exportAuthPassword').value = '';
    document.getElementById('exportPassword').value = '';
    document.getElementById('exportPasswordConfirm').value = '';
}

// ─────────────────────────────────────────────────────────────────────────────
// Import Settings (from .lsbackup file via backend)
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
        const base64 = e.target.result.split(',')[1] || btoa(e.target.result);
        
        fetch('/api/config/import', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ data: base64, password: password || '' })
        })
        .then(res => res.json())
        .then(data => {
            if (data.success) {
                showToast('Ayarlar başarıyla içe aktarıldı. Cihaz yeniden başlatılacak.', 'success');
                setTimeout(() => location.reload(), 3000);
            } else {
                showToast(data.error || 'İçe aktarma başarısız', 'error');
            }
        })
        .catch(() => showToast('Bağlantı hatası', 'error'));
    };
    
    reader.readAsDataURL(file);
}

// ─────────────────────────────────────────────────────────────────────────────
// Reboot & Factory Reset
// ─────────────────────────────────────────────────────────────────────────────
function handleReboot() {
    if (!confirm('Cihaz yeniden başlatılacak. Devam edilsin mi?')) return;
    
    fetch('/api/reboot', { method: 'POST' })
        .then(res => res.json())
        .then(data => {
            if (data.success) {
                showToast('Cihaz yeniden başlatılıyor...', 'success');
                setTimeout(() => location.reload(), 5000);
            } else {
                showToast(data.error || 'Yeniden başlatma başarısız', 'error');
            }
        })
        .catch(() => showToast('Bağlantı hatası', 'error'));
}

function handleFactoryReset() {
    if (!confirm('TÜM ayarlar silinecek! Bu işlem geri alınamaz. Devam edilsin mi?')) return;
    if (!confirm('Bu işlem geri alınamaz! Emin misiniz?')) return;
    
    fetch('/api/factory-reset', { method: 'POST' })
        .then(res => res.json())
        .then(data => {
            if (data.success) {
                showToast('Fabrika ayarlarına sıfırlama yapılıyor...', 'success');
                localStorage.clear();
                setTimeout(() => location.reload(), 5000);
            } else {
                showToast(data.error || 'Fabrika sıfırlama başarısız', 'error');
            }
        })
        .catch(() => showToast('Bağlantı hatası', 'error'));
}

// ─────────────────────────────────────────────────────────────────────────────
// Security Settings (save/load via backend)
// ─────────────────────────────────────────────────────────────────────────────
function loadSecuritySettings() {
    // Load auto-logout from localStorage
    const autoLogout = parseInt(localStorage.getItem('autoLogoutTime')) || 10;
    state.autoLogoutTime = autoLogout;
    const autoLogoutInput = document.getElementById('autoLogoutTime');
    if (autoLogoutInput) autoLogoutInput.value = autoLogout;
    
    // Load security settings from backend
    fetch('/api/config/security')
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
    
    fetch('/api/config/security', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ loginProtection, lockoutTime, resetApiEnabled })
    })
    .then(res => res.json())
    .then(data => {
        if (data.success) {
            showToast('Güvenlik ayarları kaydedildi', 'success');
        } else {
            showToast(data.error || 'Güvenlik ayarları kaydedilemedi', 'error');
        }
    })
    .catch(() => showToast('Bağlantı hatası', 'error'));
}

function handleRefreshApiKey() {
    fetch('/api/config/security/api-key', { method: 'POST' })
        .then(res => res.json())
        .then(data => {
            if (data.apiKey) {
                document.getElementById('resetApiKey').value = data.apiKey;
                showToast('API anahtarı yenilendi', 'success');
            }
        })
        .catch(() => showToast('Bağlantı hatası', 'error'));
}

// ─────────────────────────────────────────────────────────────────────────────
// GUI OTA Update
// ─────────────────────────────────────────────────────────────────────────────
function handleGuiUpdate() {
    const btn = document.getElementById('updateGuiBtn');
    if (btn) { btn.disabled = true; btn.textContent = 'Güncelleniyor...'; }
    
    showToast('Web arayüzü güncelleniyor...', 'success');
    
    fetch('/api/gui/download', { method: 'POST' })
        .then(res => res.json())
        .then(data => {
            if (data.started || data.success) {
                // Poll status
                const pollInterval = setInterval(() => {
                    fetch('/api/gui/download/status')
                        .then(r => r.json())
                        .then(st => {
                            if (st.status === 'done' || st.status === 'idle') {
                                clearInterval(pollInterval);
                                if (btn) { btn.disabled = false; btn.textContent = 'Web Arayüzünü Güncelle'; }
                                if (st.downloaded > 0) {
                                    showToast(`GUI güncellendi! ${st.downloaded} dosya indirildi.`, 'success');
                                } else {
                                    showToast('GUI güncelleme tamamlandı', 'success');
                                }
                            } else if (st.status === 'error') {
                                clearInterval(pollInterval);
                                if (btn) { btn.disabled = false; btn.textContent = 'Web Arayüzünü Güncelle'; }
                                showToast('GUI güncelleme hatası: ' + (st.error || 'Bilinmeyen hata'), 'error');
                            }
                        })
                        .catch(() => {});
                }, 2000);
            } else {
                if (btn) { btn.disabled = false; btn.textContent = 'Web Arayüzünü Güncelle'; }
                showToast(data.error || 'GUI güncelleme başlatılamadı', 'error');
            }
        })
        .catch(() => {
            if (btn) { btn.disabled = false; btn.textContent = 'Web Arayüzünü Güncelle'; }
            showToast('Bağlantı hatası', 'error');
        });
}

// ─────────────────────────────────────────────────────────────────────────────
// Mail Group Helper Functions
// ─────────────────────────────────────────────────────────────────────────────
function saveMailGroup() {
    const name = document.getElementById('mailGroupName').value.trim();
    const subject = document.getElementById('mailGroupSubject').value.trim();
    const content = document.getElementById('mailGroupContent').value.trim();
    
    if (!name) {
        showToast('Grup adı gereklidir', 'error');
        return;
    }
    
    // Get recipients from DOM
    const recipientItems = document.querySelectorAll('#mailGroupRecipients .recipient-item');
    const recipients = Array.from(recipientItems).map(item => 
        item.querySelector('.recipient-email').textContent
    );
    
    // Get files from DOM (simulated)
    const fileItems = document.querySelectorAll('#mailGroupFileList .file-item');
    const files = Array.from(fileItems).map(item => 
        item.querySelector('.file-item-name').textContent
    );
    
    if (state.isCreatingNewGroup) {
        // Create new group
        const newId = Math.max(0, ...state.mailGroups.map(g => g.id)) + 1;
        const newGroup = { id: newId, name, subject, content, files, recipients };
        state.mailGroups.push(newGroup);
        showToast('Mail grubu oluşturuldu', 'success');
    } else {
        // Update existing group
        const group = state.mailGroups.find(g => g.id === state.currentEditingGroup);
        if (group) {
            group.name = name;
            group.subject = subject;
            group.content = content;
            group.files = files;
            group.recipients = recipients;
            showToast('Mail grubu güncellendi', 'success');
        }
    }
    
    // Persist mail groups to ESP32
    persistMailGroups();
    
    // Refresh list view
    renderMailGroupList();
    handleModalBack();
}

function deleteMailGroup() {
    if (!state.currentEditingGroup) return;
    
    const groupIndex = state.mailGroups.findIndex(g => g.id === state.currentEditingGroup);
    if (groupIndex > -1) {
        state.mailGroups.splice(groupIndex, 1);
        persistMailGroups();
        showToast('Mail grubu silindi', 'success');
        renderMailGroupList();
        handleModalBack();
    }
}

function persistMailGroups() {
    const groups = state.mailGroups.map(g => ({
        name: g.name,
        subject: g.subject,
        content: g.content,
        recipients: g.recipients
    }));
    fetch('/api/config/mail-groups', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ groups })
    }).catch(err => console.error('Mail group save failed:', err));
}

function loadMailGroups() {
    fetch('/api/config/mail-groups')
        .then(res => res.json())
        .then(data => {
            if (data.groups && Array.isArray(data.groups)) {
                state.mailGroups = data.groups.map((g, i) => ({
                    id: i + 1,
                    name: g.name || '',
                    subject: g.subject || '',
                    content: g.content || '',
                    files: [],
                    recipients: g.recipients || []
                }));
                renderMailGroupList();
            }
        })
        .catch(err => console.error('Mail groups load failed:', err));
}

// ─────────────────────────────────────────────────────────────────────────────
// WiFi Config Load/Save
// ─────────────────────────────────────────────────────────────────────────────
function loadWifiConfig() {
    fetch('/api/config/wifi')
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
    
    if (!payload.ssid) {
        showToast('SSID boş olamaz', 'error');
        return;
    }
    
    fetch('/api/config/wifi', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
    })
    .then(res => res.json())
    .then(data => {
        if (data.success) {
            showToast(isBackup ? 'Yedek WiFi kaydedildi' : 'WiFi kaydedildi', 'success');
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
    fetch('/api/config/smtp')
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
    
    fetch('/api/config/smtp', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
    })
    .then(res => res.json())
    .then(data => {
        if (data.success) {
            showToast('SMTP ayarları kaydedildi', 'success');
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
    fetch('/api/config/ap', {
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
            // Revert toggle on failure
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

function renderMailGroupList() {
    const listEl = document.getElementById('mailGroupList');
    if (!listEl) return;
    
    listEl.innerHTML = state.mailGroups.map(group => `
        <div class="mail-group-item" data-group-id="${group.id}">
            <div class="mail-group-info">
                <span class="mail-group-name">${group.name}</span>
                <span class="mail-group-count">${group.recipients.length} alıcı</span>
            </div>
            <button class="btn btn-ghost btn-sm" data-action="view-group">Görüntüle</button>
        </div>
    `).join('');
    
    // Re-attach event listeners
    listEl.querySelectorAll('[data-action="view-group"]').forEach(btn => {
        btn.addEventListener('click', (e) => {
            const groupItem = e.target.closest('.mail-group-item');
            const groupId = parseInt(groupItem.dataset.groupId);
            openMailGroupDetail(groupId);
        });
    });
}

function renderRecipients(recipients) {
    const listEl = document.getElementById('mailGroupRecipients');
    if (!listEl) return;
    
    listEl.innerHTML = recipients.map(email => `
        <div class="recipient-item">
            <div class="recipient-info">
                <span class="recipient-email">${email}</span>
            </div>
            <button class="btn btn-ghost btn-sm btn-delete" data-email="${email}">
                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="16" height="16">
                    <line x1="18" y1="6" x2="6" y2="18"></line>
                    <line x1="6" y1="6" x2="18" y2="18"></line>
                </svg>
            </button>
        </div>
    `).join('');
    
    // Attach delete listeners
    listEl.querySelectorAll('.btn-delete').forEach(btn => {
        btn.addEventListener('click', (e) => {
            const email = e.currentTarget.dataset.email;
            removeRecipient(email);
        });
    });
}

function addRecipient() {
    const input = document.getElementById('newRecipientEmail');
    const email = input.value.trim();
    
    if (!email || !email.includes('@')) {
        showToast('Geçerli bir email adresi girin', 'error');
        return;
    }
    
    // Get current recipients
    const recipientItems = document.querySelectorAll('#mailGroupRecipients .recipient-item');
    const currentRecipients = Array.from(recipientItems).map(item => 
        item.querySelector('.recipient-email').textContent
    );
    
    if (currentRecipients.includes(email)) {
        showToast('Bu email zaten ekli', 'error');
        return;
    }
    
    currentRecipients.push(email);
    renderRecipients(currentRecipients);
    input.value = '';
}

function removeRecipient(email) {
    const recipientItems = document.querySelectorAll('#mailGroupRecipients .recipient-item');
    const currentRecipients = Array.from(recipientItems)
        .map(item => item.querySelector('.recipient-email').textContent)
        .filter(e => e !== email);
    
    renderRecipients(currentRecipients);
}

function renderFiles(files) {
    const listEl = document.getElementById('mailGroupFileList');
    if (!listEl) return;
    
    listEl.innerHTML = files.map(file => `
        <div class="file-item">
            <div class="file-item-info">
                <svg class="file-item-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="16" height="16">
                    <path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"></path>
                    <polyline points="14 2 14 8 20 8"></polyline>
                </svg>
                <span class="file-item-name">${file}</span>
            </div>
            <button class="btn btn-ghost btn-sm btn-delete" data-file="${file}">
                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="16" height="16">
                    <line x1="18" y1="6" x2="6" y2="18"></line>
                    <line x1="6" y1="6" x2="18" y2="18"></line>
                </svg>
            </button>
        </div>
    `).join('');
    
    // Attach delete listeners
    listEl.querySelectorAll('.btn-delete').forEach(btn => {
        btn.addEventListener('click', (e) => {
            const filename = e.currentTarget.dataset.file;
            removeFile(filename);
        });
    });
}

function handleFileUpload(e) {
    const files = e.target.files;
    if (!files.length) return;
    
    // Get current files
    const fileItems = document.querySelectorAll('#mailGroupFileList .file-item');
    const currentFiles = Array.from(fileItems).map(item => 
        item.querySelector('.file-item-name').textContent
    );
    
    // Add new files
    Array.from(files).forEach(file => {
        if (!currentFiles.includes(file.name)) {
            currentFiles.push(file.name);
        }
    });
    
    renderFiles(currentFiles);
    e.target.value = ''; // Reset input
}

function removeFile(filename) {
    const fileItems = document.querySelectorAll('#mailGroupFileList .file-item');
    const currentFiles = Array.from(fileItems)
        .map(item => item.querySelector('.file-item-name').textContent)
        .filter(f => f !== filename);
    
    renderFiles(currentFiles);
}

// ─────────────────────────────────────────────────────────────────────────────
// OTA Source Selector
// ─────────────────────────────────────────────────────────────────────────────
function initOtaSourceSelectors() {
    // HW OTA Kaynak Seçici
    const hwOtaRadios = document.querySelectorAll('input[name="hwOtaSource"]');
    const hwOtaWarning = document.getElementById('hwOtaWarning');
    const hwOtaCustomSection = document.getElementById('hwOtaCustomSection');
    
    hwOtaRadios.forEach(radio => {
        radio.addEventListener('change', () => {
            const isCustom = radio.value === 'custom' && radio.checked;
            if (hwOtaWarning) hwOtaWarning.classList.toggle('hidden', !isCustom);
            if (hwOtaCustomSection) hwOtaCustomSection.classList.toggle('hidden', !isCustom);
        });
    });
    
    // GUI OTA Kaynak Seçici
    const guiOtaRadios = document.querySelectorAll('input[name="guiOtaSource"]');
    const guiOtaWarning = document.getElementById('guiOtaWarning');
    const guiOtaCustomSection = document.getElementById('guiOtaCustomSection');
    
    guiOtaRadios.forEach(radio => {
        radio.addEventListener('change', () => {
            const isCustom = radio.value === 'custom' && radio.checked;
            if (guiOtaWarning) guiOtaWarning.classList.toggle('hidden', !isCustom);
            if (guiOtaCustomSection) guiOtaCustomSection.classList.toggle('hidden', !isCustom);
        });
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// Connection Accordions Configuration Check
// ─────────────────────────────────────────────────────────────────────────────
function initConnectionConfigCheck() {
    // Akordiyonların yapılandırma durumunu kontrol eden mapping
    // Not: apModeAccordion readonly olduğu için burada yok, HTML'de .configured classı sabit
    const accordionConfigs = {
        'smtpAccordion': ['smtpServer', 'smtpUser', 'smtpApiKey'],
        'wifiAccordion': ['wifiSsid', 'wifiPass'],
        'wifiBackupAccordion': ['wifiBackupSsid', 'wifiBackupPass'],
        'publicWifiAccordion': ['publicWifiSsid']
    };
    
    // Her akordiyonu kontrol et ve configured durumunu ayarla
    function checkAccordionConfig(accordionId, inputIds) {
        const accordion = document.getElementById(accordionId);
        if (!accordion) return;
        
        // Input'lardan herhangi biri doluysa configured
        const isConfigured = inputIds.some(id => {
            const input = document.getElementById(id);
            return input && input.value.trim() !== '';
        });
        
        accordion.classList.toggle('configured', isConfigured);
    }
    
    // Başlangıçta tüm akordiyonları kontrol et
    Object.entries(accordionConfigs).forEach(([accordionId, inputIds]) => {
        checkAccordionConfig(accordionId, inputIds);
        
        // Her input değiştiğinde kontrol et
        inputIds.forEach(inputId => {
            const input = document.getElementById(inputId);
            if (input) {
                input.addEventListener('input', () => {
                    checkAccordionConfig(accordionId, inputIds);
                });
            }
        });
    });
    
    // Statik IP toggle'ları
    initStaticIpToggles();
    
    // AP Mode değerlerini ayarla
    initApModeValues();
}

// AP Mode değerlerini Device ID'den ayarla
function initApModeValues() {
    // Fetch real device ID from ESP32 and use for AP mode fields
    fetch('/api/device/info')
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
        })
        .catch(() => {}); // Ignore - will be populated on fetchDeviceInfo    
    
    // AP Mode toggle event listener - uyarıyı göster/gizle
    const apModeToggle = document.getElementById('apModeEnabled');
    const apModeWarning = document.getElementById('apModeWarning');
    
    if (apModeToggle && apModeWarning) {
        apModeToggle.addEventListener('change', () => {
            // Toggle kapalıysa uyarıyı göster
            apModeWarning.classList.toggle('hidden', apModeToggle.checked);
            // Save AP mode state to backend
            saveApModeConfig(apModeToggle.checked);
        });
    }
}

// Statik IP alanlarını göster/gizle
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
// Relay Config Save/Load
// ─────────────────────────────────────────────────────────────────────────────
function saveRelayConfig() {
    const inverted = document.getElementById('relayInverted');
    const pulseEnabled = document.getElementById('relayPulseEnabled');
    const pulseDuration = document.getElementById('relayPulseDuration');
    const durationValue = document.getElementById('relayDurationValue');
    const durationUnit = document.getElementById('relayDurationUnit');
    
    const durVal = parseInt(durationValue?.value) || 1;
    const durUnit = durationUnit?.value || 'seconds';
    const totalMs = (durUnit === 'minutes' ? durVal * 60 : durVal) * 1000;
    
    const payload = {
        inverted: inverted ? inverted.checked : false,
        pulseMode: pulseEnabled ? pulseEnabled.checked : false,
        pulseDurationMs: parseInt(pulseDuration?.value) * 1000 || 5000,
        pulseIntervalMs: parseInt(pulseDuration?.value) * 1000 || 5000,
        onDelayMs: totalMs,
        offDelayMs: 0
    };
    
    fetch('/api/config/relay', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
    })
    .then(res => res.json())
    .then(data => {
        if (data.success) {
            showToast('Röle ayarları kaydedildi', 'success');
            handleModalBack();
        } else {
            showToast(data.error || 'Kaydetme hatası', 'error');
        }
    })
    .catch(() => showToast('Bağlantı hatası', 'error'));
}

function loadRelayConfig() {
    fetch('/api/config/relay')
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
    
    // Toplam süreyi saniyeye çevir
    const value = parseInt(durationValue.value) || 1;
    const unit = durationUnit.value;
    const totalSeconds = unit === 'minutes' ? value * 60 : value;
    
    // Pulse süresi
    const pulseSeconds = parseInt(pulseDuration.value) || 5;
    
    // Döngü süresi (açık + kapalı)
    const cycleTime = pulseSeconds * 2;
    
    // Döngü sayısı
    const cycles = Math.floor(totalSeconds / cycleTime);
    const remaining = totalSeconds % cycleTime;
    
    // Toplam süre metni
    let totalText = '';
    if (totalSeconds >= 60) {
        const mins = Math.floor(totalSeconds / 60);
        const secs = totalSeconds % 60;
        totalText = secs > 0 ? `${mins} dk ${secs} sn` : `${mins} dakika`;
    } else {
        totalText = `${totalSeconds} saniye`;
    }
    
    // Sonuç metni
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
// Action Cards Management
// ─────────────────────────────────────────────────────────────────────────────
function initActionCards() {
    // Action card toggle (ana içerik alanına tıklanınca)
    document.querySelectorAll('.action-card-main').forEach(main => {
        main.addEventListener('click', () => {
            const card = main.closest('.action-card');
            const isActive = card.dataset.active === 'true';
            card.dataset.active = isActive ? 'false' : 'true';
        });
    });
    
    // Ayarsız kartlar için kart geneli toggle
    document.querySelectorAll('.action-card:not(.has-settings)').forEach(card => {
        card.addEventListener('click', () => {
            const isActive = card.dataset.active === 'true';
            card.dataset.active = isActive ? 'false' : 'true';
        });
    });
    
    // Action config buttons (yapılandırma)
    document.querySelectorAll('.action-card-settings').forEach(btn => {
        btn.addEventListener('click', (e) => {
            e.stopPropagation();
            const configType = btn.dataset.config;
            openActionConfig(configType);
        });
    });
    
    // Modal back button (merkezi geri butonu)
    const modalBackBtn = document.getElementById('modalBackBtn');
    if (modalBackBtn) {
        modalBackBtn.addEventListener('click', handleModalBack);
    }

    // Generic subpage handler (Sistem sekmesi için)
    document.querySelectorAll('[data-subpage]').forEach(btn => {
        btn.addEventListener('click', (e) => {
            const subpageId = btn.dataset.subpage;
            const subpage = document.getElementById(subpageId);
            if (!subpage) return;
            
            const title = subpage.dataset.title || 'Alt Sayfa';
            const breadcrumb = subpage.dataset.breadcrumb || null;
            
            openSystemSubpage(subpageId, title, breadcrumb);
        });
    });
    
    // API Config cancel/save buttons
    const apiCancelBtn = document.getElementById('apiConfigCancelBtn');
    const apiSaveBtn = document.getElementById('apiConfigSaveBtn');
    
    if (apiCancelBtn) apiCancelBtn.addEventListener('click', handleModalBack);
    if (apiSaveBtn) apiSaveBtn.addEventListener('click', () => {
        const webhookUrl = document.getElementById('webhookUrl')?.value || '';
        const webhookSecret = document.getElementById('webhookSecret')?.value || '';
        fetch('/api/config/webhook', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ webhookUrl, webhookSecret })
        })
        .then(res => res.json())
        .then(data => {
            if (data.success) {
                showToast('API ayarları kaydedildi', 'success');
                handleModalBack();
            } else showToast(data.error || 'Hata', 'error');
        })
        .catch(() => showToast('Bağlantı hatası', 'error'));
    });
    
    // Relay Config cancel/save buttons
    const relayCancelBtn = document.getElementById('relayConfigCancelBtn');
    const relaySaveBtn = document.getElementById('relayConfigSaveBtn');
    
    if (relayCancelBtn) relayCancelBtn.addEventListener('click', handleModalBack);
    if (relaySaveBtn) relaySaveBtn.addEventListener('click', () => {
        saveRelayConfig();
    });
    
    // Relay test button
    const testRelayBtn = document.getElementById('testRelayBtn');
    if (testRelayBtn) {
        testRelayBtn.addEventListener('click', () => {
            testRelayBtn.disabled = true;
            testRelayBtn.textContent = 'Test ediliyor...';
            fetch('/api/relay/test', { method: 'POST' })
                .then(res => res.json())
                .then(data => {
                    if (data.success) {
                        showToast('Röle test edildi', 'success');
                    } else {
                        showToast(data.error || 'Röle testi başarısız', 'error');
                    }
                })
                .catch(() => showToast('Bağlantı hatası', 'error'))
                .finally(() => {
                    testRelayBtn.disabled = false;
                    testRelayBtn.textContent = 'Röleyi Test Et';
                });
        });
    }
    
    // Relay invert toggle - enerji durumu açıklamasını güncelle
    const relayInvertedToggle = document.getElementById('relayInverted');
    const relayIdleState = document.getElementById('relayIdleState');
    const relayActiveState = document.getElementById('relayActiveState');
    
    if (relayInvertedToggle && relayIdleState && relayActiveState) {
        relayInvertedToggle.addEventListener('change', () => {
            if (relayInvertedToggle.checked) {
                // Ters çevrilmiş - normalde enerji var
                relayIdleState.textContent = 'Enerji VAR';
                relayIdleState.classList.add('relay-state-active');
                relayActiveState.textContent = 'Enerji YOK';
                relayActiveState.classList.remove('relay-state-active');
            } else {
                // Normal - normalde enerji yok
                relayIdleState.textContent = 'Enerji YOK';
                relayIdleState.classList.remove('relay-state-active');
                relayActiveState.textContent = 'Enerji VAR';
                relayActiveState.classList.add('relay-state-active');
            }
        });
    }
    
    // Relay duration validation
    const relayDurationValue = document.getElementById('relayDurationValue');
    const relayDurationUnit = document.getElementById('relayDurationUnit');
    
    if (relayDurationValue && relayDurationUnit) {
        const validateDuration = () => {
            const unit = relayDurationUnit.value;
            let value = parseInt(relayDurationValue.value) || 1;
            
            if (unit === 'seconds') {
                value = Math.min(59, Math.max(1, value));
            } else {
                value = Math.min(60, Math.max(1, value));
            }
            
            relayDurationValue.value = value;
            updateRelayCycleInfo();
        };
        
        relayDurationValue.addEventListener('change', validateDuration);
        relayDurationValue.addEventListener('input', validateDuration);
        relayDurationUnit.addEventListener('change', validateDuration);
    }
    
    // Relay pulse toggle - pulse alanlarını göster/gizle
    const relayPulseToggle = document.getElementById('relayPulseEnabled');
    const relayPulseFields = document.getElementById('relayPulseFields');
    
    if (relayPulseToggle && relayPulseFields) {
        relayPulseToggle.addEventListener('change', () => {
            relayPulseFields.classList.toggle('hidden', !relayPulseToggle.checked);
            updateRelayCycleInfo();
        });
    }
    
    // Relay pulse duration select
    const relayPulseDuration = document.getElementById('relayPulseDuration');
    if (relayPulseDuration) {
        relayPulseDuration.addEventListener('change', updateRelayCycleInfo);
    }
    
    // Telegram Config cancel/save buttons
    const telegramCancelBtn = document.getElementById('telegramConfigCancelBtn');
    const telegramSaveBtn = document.getElementById('telegramConfigSaveBtn');
    
    if (telegramCancelBtn) telegramCancelBtn.addEventListener('click', handleModalBack);
    if (telegramSaveBtn) telegramSaveBtn.addEventListener('click', () => {
        const botToken = document.getElementById('telegramBotToken')?.value || '';
        const chatId = document.getElementById('telegramChatId')?.value || '';
        fetch('/api/config/telegram', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ botToken, chatId })
        })
        .then(res => res.json())
        .then(data => {
            if (data.success) {
                showToast('Telegram ayarları kaydedildi', 'success');
                handleModalBack();
            } else showToast(data.error || 'Hata', 'error');
        })
        .catch(() => showToast('Bağlantı hatası', 'error'));
    });
    
    // Early Mail Config cancel/save buttons
    const earlyMailCancelBtn = document.getElementById('earlyMailConfigCancelBtn');
    const earlyMailSaveBtn = document.getElementById('earlyMailConfigSaveBtn');
    
    if (earlyMailCancelBtn) earlyMailCancelBtn.addEventListener('click', handleModalBack);
    if (earlyMailSaveBtn) earlyMailSaveBtn.addEventListener('click', () => {
        const earlyRecipient = document.getElementById('earlyWarningEmail')?.value || '';
        const earlySubject = document.getElementById('earlyReminderSubject')?.value || '';
        const earlyMessage = document.getElementById('earlyReminderMessage')?.value || '';
        fetch('/api/config/early-mail', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ recipient: earlyRecipient, subject: earlySubject, message: earlyMessage })
        })
        .then(res => res.json())
        .then(data => {
            if (data.success) {
                showToast('Erken uyarı mail ayarları kaydedildi', 'success');
                handleModalBack();
            } else showToast(data.error || 'Hata', 'error');
        })
        .catch(() => showToast('Bağlantı hatası', 'error'));
    });
    
    // Trigger Mail Config back button
    const triggerMailCancelBtn = document.getElementById('triggerMailConfigCancelBtn');
    if (triggerMailCancelBtn) triggerMailCancelBtn.addEventListener('click', handleModalBack);
    
    // Mail Group Detail buttons
    const mailGroupCancelBtn = document.getElementById('mailGroupCancelBtn');
    const mailGroupSaveBtn = document.getElementById('mailGroupSaveBtn');
    
    if (mailGroupCancelBtn) mailGroupCancelBtn.addEventListener('click', handleModalBack);
    if (mailGroupSaveBtn) mailGroupSaveBtn.addEventListener('click', () => {
        saveMailGroup();
    });
    
    // Mail Group list item clicks
    document.querySelectorAll('.mail-group-item [data-action="view-group"]').forEach(btn => {
        btn.addEventListener('click', (e) => {
            const groupItem = e.target.closest('.mail-group-item');
            const groupId = parseInt(groupItem.dataset.groupId);
            openMailGroupDetail(groupId);
        });
    });
    
    // Create Mail Group button
    const createMailGroupBtn = document.getElementById('createMailGroup');
    if (createMailGroupBtn) {
        createMailGroupBtn.addEventListener('click', () => {
            if (state.mailGroups.length >= 10) {
                showToast('Maksimum 10 mail grubu oluşturabilirsiniz', 'error');
                return;
            }
            openMailGroupDetail(null, true);
        });
    }
}

// Config açma fonksiyonlarını güncelle
const configTitles = {
    'telegram': { title: 'Telegram Yapılandırması', breadcrumb: 'Aksiyonlar' },
    'early-mail': { title: 'Erken Uyarı Mail', breadcrumb: 'Aksiyonlar' },
    'early-api': { title: 'API Yapılandırması', breadcrumb: 'Aksiyonlar' },
    'trigger-mail': { title: 'Mail Grupları', breadcrumb: 'Aksiyonlar' },
    'trigger-api': { title: 'API Yapılandırması', breadcrumb: 'Aksiyonlar' },
    'relay': { title: 'Röle Yapılandırması', breadcrumb: 'Aksiyonlar' }
};

// Sistem sekmesi alt sayfaları için
function openSystemSubpage(subpageId, title, breadcrumb) {
    const subpage = document.getElementById(subpageId);
    if (!subpage) return;
    
    // Hangi panelde olduğumuzu bul
    const panel = subpage.closest('.settings-panel');
    if (!panel) return;
    
    // Navigation stack'e ekle
    pushView(subpageId, title, breadcrumb);
    
    // Ana içeriği gizle (system veya info)
    const systemMainContent = document.getElementById('systemMainContent');
    const infoMainContent = document.getElementById('infoMainContent');
    
    if (panel.id === 'panel-system' && systemMainContent) {
        systemMainContent.classList.add('hidden');
        // Tüm sistem alt sayfalarını gizle
        document.querySelectorAll('#panel-system .subpage').forEach(sp => {
            sp.classList.add('hidden');
        });
    } else if (panel.id === 'panel-info' && infoMainContent) {
        infoMainContent.classList.add('hidden');
        // Tüm info alt sayfalarını gizle
        document.querySelectorAll('#panel-info .subpage').forEach(sp => {
            sp.classList.add('hidden');
        });
    }
    
    // Hedef alt sayfayı göster
    subpage.classList.remove('hidden');
}

function openActionConfig(type) {
    const config = configTitles[type];
    if (!config) return;
    
    const mainContent = document.getElementById('actionsMainContent');
    const apiView = document.getElementById('apiConfigView');
    const relayView = document.getElementById('relayConfigView');
    const telegramView = document.getElementById('telegramConfigView');
    const earlyMailView = document.getElementById('earlyMailConfigView');
    const triggerMailView = document.getElementById('triggerMailConfigView');
    
    if (!mainContent) return;
    
    // Navigation stack'e ekle
    pushView(type, config.title, config.breadcrumb);
    
    mainContent.classList.add('hidden');
    
    if ((type === 'early-api' || type === 'trigger-api') && apiView) {
        apiView.classList.remove('hidden');
    } else if (type === 'relay' && relayView) {
        relayView.classList.remove('hidden');
        loadRelayConfig();
    } else if (type === 'telegram' && telegramView) {
        telegramView.classList.remove('hidden');
    } else if (type === 'early-mail' && earlyMailView) {
        earlyMailView.classList.remove('hidden');
    } else if (type === 'trigger-mail' && triggerMailView) {
        triggerMailView.classList.remove('hidden');
    }
}

function openMailGroupDetail(groupIdOrName, isNew = false) {
    const triggerMailView = document.getElementById('triggerMailConfigView');
    const mailGroupDetailView = document.getElementById('mailGroupDetailView');
    const deleteBtn = document.getElementById('mailGroupDeleteBtn');
    
    if (!triggerMailView || !mailGroupDetailView) return;
    
    state.isCreatingNewGroup = isNew;
    
    // Grup bilgilerini al
    let groupName = 'Yeni Mail Grubu';
    if (groupIdOrName && typeof groupIdOrName === 'number') {
        const group = state.mailGroups.find(g => g.id === groupIdOrName);
        if (group) {
            groupName = group.name;
            state.currentEditingGroup = groupIdOrName;
            // Form alanlarını doldur
            document.getElementById('mailGroupName').value = group.name;
            document.getElementById('mailGroupSubject').value = group.subject;
            document.getElementById('mailGroupContent').value = group.content;
            renderFiles(group.files || []);
            renderRecipients(group.recipients || []);
        }
    } else if (isNew || groupIdOrName === null) {
        state.currentEditingGroup = null;
        state.isCreatingNewGroup = true;
        // Form alanlarını temizle
        document.getElementById('mailGroupName').value = '';
        document.getElementById('mailGroupSubject').value = '';
        document.getElementById('mailGroupContent').value = '';
        const fileListEl = document.getElementById('mailGroupFileList');
        if (fileListEl) fileListEl.innerHTML = '';
        renderRecipients([]);
    } else if (typeof groupIdOrName === 'string') {
        groupName = groupIdOrName;
    }
    
    // Show/hide delete button
    if (deleteBtn) {
        deleteBtn.style.display = isNew ? 'none' : 'flex';
    }
    
    // Navigation stack'e ekle
    const title = isNew ? 'Yeni Mail Grubu' : `${groupName} - Düzenle`;
    pushView('mail-group-detail', title, 'Mail Grupları');
    
    triggerMailView.classList.add('hidden');
    mailGroupDetailView.classList.remove('hidden');
}

function handleModalBack() {
    if (state.navStack.length <= 1) return;
    
    const currentView = state.navStack[state.navStack.length - 1];
    
    // Mevcut görünümü gizle
    hideCurrentView(currentView.id);
    
    // Stack'ten çıkar ve güncelle
    popView();
    
    // Önceki görünümü göster
    const prevView = state.navStack[state.navStack.length - 1];
    showPreviousView(prevView.id);
}

function hideCurrentView(viewId) {
    // Aksiyon alt sayfaları
    const views = {
        'telegram': 'telegramConfigView',
        'early-mail': 'earlyMailConfigView',
        'early-api': 'apiConfigView',
        'trigger-mail': 'triggerMailConfigView',
        'trigger-api': 'apiConfigView',
        'relay': 'relayConfigView',
        'mail-group-detail': 'mailGroupDetailView'
    };
    
    // Aksiyon görünümü mü?
    if (views[viewId]) {
        const viewElement = document.getElementById(views[viewId]);
        if (viewElement) viewElement.classList.add('hidden');
        return;
    }
    
    // Sistem alt sayfası mı? (subpage ID direkt olarak geliyor)
    const subpage = document.getElementById(viewId);
    if (subpage && subpage.classList.contains('subpage')) {
        subpage.classList.add('hidden');
    }
}

function showPreviousView(viewId) {
    if (viewId === 'root') {
        // Ana içeriği göster - hangi sekmede olduğumuza göre
        document.getElementById('actionsMainContent')?.classList.remove('hidden');
        document.getElementById('systemMainContent')?.classList.remove('hidden');
        document.getElementById('infoMainContent')?.classList.remove('hidden');
    } else if (viewId === 'trigger-mail') {
        // Mail grupları listesini göster
        document.getElementById('triggerMailConfigView')?.classList.remove('hidden');
    }
    // Sistem/Info alt sayfalarından bir üst sayfaya dönüş için
    // (şu an sadece root'a dönüş var, iç içe alt sayfalar eklenirse burası güncellenebilir)
}

// ─────────────────────────────────────────────────────────────────────────────
// Timer Configuration
// ─────────────────────────────────────────────────────────────────────────────
function initTimerConfig() {
    // Load timer config from ESP32
    loadTimerConfig();

    // Unit selector buttons (Gün/Saat/Dakika)
    document.querySelectorAll('.unit-btn').forEach(btn => {
        btn.addEventListener('click', () => {
            document.querySelectorAll('.unit-btn').forEach(b => b.classList.remove('active'));
            btn.classList.add('active');
            state.timerConfig.unit = btn.dataset.unit;
            updateAlarmConstraints();
            updateAlarmInfo();
        });
    });

    // Timer value input + buttons
    const timerInput = document.getElementById('timerDuration');
    const timerIncrease = document.getElementById('timerIncrease');
    const timerDecrease = document.getElementById('timerDecrease');
    
    if (timerInput) {
        timerInput.addEventListener('change', () => {
            let val = parseInt(timerInput.value) || 1;
            val = Math.max(1, Math.min(60, val));
            timerInput.value = val;
            state.timerConfig.value = val;
            updateAlarmConstraints();
            updateAlarmInfo();
        });
    }
    
    if (timerIncrease) {
        timerIncrease.addEventListener('click', () => {
            let val = parseInt(timerInput.value) || 1;
            val = Math.min(60, val + 1);
            timerInput.value = val;
            state.timerConfig.value = val;
            updateAlarmConstraints();
            updateAlarmInfo();
        });
    }
    
    if (timerDecrease) {
        timerDecrease.addEventListener('click', () => {
            let val = parseInt(timerInput.value) || 1;
            val = Math.max(1, val - 1);
            timerInput.value = val;
            state.timerConfig.value = val;
            updateAlarmConstraints();
            updateAlarmInfo();
        });
    }

    // Alarm slider
    const alarmSlider = document.getElementById('alarmCount');
    const alarmValue = document.getElementById('alarmCountValue');
    
    if (alarmSlider) {
        alarmSlider.addEventListener('input', () => {
            const val = parseInt(alarmSlider.value);
            state.timerConfig.alarmCount = val;
            if (alarmValue) alarmValue.textContent = val;
            updateAlarmInfo();
        });
    }

    // Vacation mode toggle
    const vacationToggle = document.getElementById('vacationMode');
    const vacationDaysGroup = document.getElementById('vacationDaysGroup');
    
    if (vacationToggle) {
        vacationToggle.addEventListener('change', () => {
            state.vacationMode.enabled = vacationToggle.checked;
            if (vacationDaysGroup) {
                vacationDaysGroup.classList.toggle('hidden', !vacationToggle.checked);
            }
        });
    }

    // Vacation days input + buttons
    const vacationInput = document.getElementById('vacationDays');
    const vacationIncrease = document.getElementById('vacationIncrease');
    const vacationDecrease = document.getElementById('vacationDecrease');
    
    if (vacationInput) {
        vacationInput.addEventListener('change', () => {
            let val = parseInt(vacationInput.value) || 1;
            val = Math.max(1, Math.min(60, val));
            vacationInput.value = val;
            state.vacationMode.days = val;
        });
    }
    
    if (vacationIncrease) {
        vacationIncrease.addEventListener('click', () => {
            let val = parseInt(vacationInput.value) || 1;
            val = Math.min(60, val + 1);
            vacationInput.value = val;
            state.vacationMode.days = val;
        });
    }
    
    if (vacationDecrease) {
        vacationDecrease.addEventListener('click', () => {
            let val = parseInt(vacationInput.value) || 1;
            val = Math.max(1, val - 1);
            vacationInput.value = val;
            state.vacationMode.days = val;
        });
    }
}

function loadTimerConfig() {
    fetch('/api/config/timer')
        .then(res => {
            if (!res.ok) throw new Error('Not authenticated');
            return res.json();
        })
        .then(data => {
            // Map intervalMinutes to unit + value
            const minutes = data.intervalMinutes || 1440;
            let unit = 'hours';
            let value = Math.round(minutes / 60);
            
            if (minutes >= 1440 && minutes % 1440 === 0) {
                unit = 'days';
                value = minutes / 1440;
            } else if (minutes < 60 || minutes % 60 !== 0) {
                unit = 'minutes';
                value = minutes;
            } else {
                unit = 'hours';
                value = minutes / 60;
            }
            
            state.timerConfig.unit = unit;
            state.timerConfig.value = value;
            state.timerConfig.alarmCount = data.alarmCount || 0;
            state.vacationMode.enabled = data.vacationEnabled || false;
            state.vacationMode.days = data.vacationDays || 7;
            
            // Update UI elements
            document.querySelectorAll('.unit-btn').forEach(b => {
                b.classList.toggle('active', b.dataset.unit === unit);
            });
            
            const timerInput = document.getElementById('timerDuration');
            if (timerInput) timerInput.value = value;
            
            const alarmSlider = document.getElementById('alarmCount');
            if (alarmSlider) alarmSlider.value = data.alarmCount || 0;
            
            const alarmValue = document.getElementById('alarmCountValue');
            if (alarmValue) alarmValue.textContent = data.alarmCount || 0;
            
            const vacationToggle = document.getElementById('vacationMode');
            if (vacationToggle) vacationToggle.checked = data.vacationEnabled || false;
            
            const vacationDaysGroup = document.getElementById('vacationDaysGroup');
            if (vacationDaysGroup) {
                vacationDaysGroup.classList.toggle('hidden', !data.vacationEnabled);
            }
            
            const vacationInput = document.getElementById('vacationDays');
            if (vacationInput) vacationInput.value = data.vacationDays || 7;
            
            updateAlarmConstraints();
            updateAlarmInfo();
            updateVacationIndicator();
        })
        .catch(() => {});
}

function updateAlarmConstraints() {
    const alarmSlider = document.getElementById('alarmCount');
    if (!alarmSlider) return;
    
    const maxAlarms = Math.min(10, Math.floor(state.timerConfig.value / 2));
    alarmSlider.max = maxAlarms;
    
    // Eğer mevcut değer maksimumu aşıyorsa ayarla
    if (state.timerConfig.alarmCount > maxAlarms) {
        state.timerConfig.alarmCount = maxAlarms;
        alarmSlider.value = maxAlarms;
        const alarmValue = document.getElementById('alarmCountValue');
        if (alarmValue) alarmValue.textContent = maxAlarms;
    }
}

function updateAlarmInfo() {
    const alarmInfo = document.getElementById('alarmInfo');
    if (!alarmInfo) return;
    
    const { unit, value, alarmCount } = state.timerConfig;
    
    if (alarmCount === 0) {
        alarmInfo.textContent = 'Uyarı alarmı yok';
        return;
    }
    
    const unitNames = {
        days: { singular: 'gün', plural: 'günler', suffix: '.' },
        hours: { singular: 'saat', plural: 'saatler', suffix: '.' },
        minutes: { singular: 'dakika', plural: 'dakikalar', suffix: '.' }
    };
    
    const unitInfo = unitNames[unit];
    const alarmTimes = [];
    
    for (let i = 1; i <= alarmCount; i++) {
        alarmTimes.push(value - i);
    }
    
    const timeStr = alarmTimes.reverse().join(', ');
    alarmInfo.textContent = `${timeStr}${unitInfo.suffix} ${unitInfo.singular}de alarm çalar`;
}

function updateVacationIndicator() {
    const indicator = document.getElementById('vacationIndicator');
    const remaining = document.getElementById('vacationRemaining');
    
    if (!indicator) return;
    
    if (state.timerState === 'VACATION' || state.vacationMode.enabled) {
        indicator.classList.add('active');
        if (remaining) {
            remaining.textContent = `${state.vacationMode.days} gün`;
        }
    } else {
        indicator.classList.remove('active');
    }
}

function initToggles() {

    // Quiet Hours Toggle
    const quietToggle = document.getElementById('quiet-toggle');
    if (quietToggle) {
        quietToggle.addEventListener('change', (e) => {
            state.settings.timer.quietHoursEnabled = e.target.checked;
        });
    }

    // Telegram Toggle
    const telegramToggle = document.getElementById('telegram-toggle');
    if (telegramToggle) {
        telegramToggle.addEventListener('change', (e) => {
            state.settings.notifications.telegramEnabled = e.target.checked;
        });
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Authentication
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
    
    fetch('/api/login', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ password })
    })
    .then(res => res.json())
    .then(data => {
        if (data.success) {
            state.isAuthenticated = true;
            state.loginAttempts = 0;
            state.lockoutUntil = null;
            showPage('app');
            showToast('Giriş başarılı', 'success');
            startAutoLogoutTimer();
            pollTimerStatus();
            loadTimerConfig();
            loadMailGroups();
            loadWifiConfig();
            loadSmtpConfig();
            fetchLogs();
            fetchDeviceInfo();
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
    fetch('/api/logout', { method: 'POST' }).catch(() => {});
    state.isAuthenticated = false;
    stopAutoLogoutTimer();
    showPage('login');
    showToast('Çıkış yapıldı', 'success');
}

function fetchDeviceInfo() {
    fetch('/api/device/info')
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
                const spiffsTotal = data.ext_spiffs_total || 0;
                const spiffsUsed = data.ext_spiffs_used || 0;
                const spiffsFree = spiffsTotal > spiffsUsed ? spiffsTotal - spiffsUsed : 0;
                setTxt('extFlashStatus', fmtBytes(spiffsFree) + ' boş / ' + fmtBytes(spiffsTotal));
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

            // External Flash
            if (data.ext_flash_total) {
                const spiffsTotal = data.ext_spiffs_total || 0;
                const spiffsUsed = data.ext_spiffs_used || 0;
                const spiffsFree = spiffsTotal > spiffsUsed ? spiffsTotal - spiffsUsed : 0;
                const extPct = spiffsTotal ? (spiffsUsed / spiffsTotal * 100).toFixed(1) : '0';
                setTxt('sysExtFlashTotal', fmtBytes(data.ext_flash_total));
                setTxt('sysExtFlashSpiffs', fmtBytes(spiffsTotal));
                setTxt('sysExtFlashUsed', fmtBytes(spiffsUsed));
                setTxt('sysExtFlashFree', fmtBytes(spiffsFree));
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
function showPage(page) {
    if (page === 'login') {
        elements.loginPage.classList.remove('hidden');
        elements.appPage.classList.add('hidden');
    } else {
        elements.loginPage.classList.add('hidden');
        elements.appPage.classList.remove('hidden');
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Timer Logic - Real API Integration
// ─────────────────────────────────────────────────────────────────────────────
function handleReset() {
    if (state.timerState === 'TRIGGERED') {
        // Acknowledge triggered timer
        fetch('/api/timer/acknowledge', { method: 'POST' })
            .then(res => res.json())
            .then(data => {
                if (data.success) {
                    showToast('Tetikleme onaylandı, zamanlayıcı yeniden başlatıldı', 'success');
                    pollTimerStatus();
                } else {
                    showToast(data.error || 'Hata', 'error');
                }
            })
            .catch(() => showToast('Bağlantı hatası', 'error'));
    } else if (state.timerState === 'DISABLED') {
        // Enable timer
        fetch('/api/timer/enable', { method: 'POST' })
            .then(res => res.json())
            .then(data => {
                if (data.success) {
                    showToast('Zamanlayıcı etkinleştirildi', 'success');
                    pollTimerStatus();
                }
            })
            .catch(() => showToast('Bağlantı hatası', 'error'));
    } else {
        // Normal reset
        fetch('/api/timer/reset', { method: 'POST' })
            .then(res => res.json())
            .then(data => {
                if (data.success) {
                    showToast('Zamanlayıcı sıfırlandı', 'success');
                    pollTimerStatus();
                } else {
                    showToast(data.error || 'Sıfırlama başarısız', 'error');
                }
            })
            .catch(() => showToast('Bağlantı hatası', 'error'));
    }
}

function handlePause() {
    if (state.timerState === 'DISABLED') {
        fetch('/api/timer/enable', { method: 'POST' })
            .then(res => res.json())
            .then(data => {
                if (data.success) {
                    showToast('Geri sayım başlatıldı', 'success');
                    pollTimerStatus();
                }
            })
            .catch(() => showToast('Bağlantı hatası', 'error'));
    } else {
        fetch('/api/timer/disable', { method: 'POST' })
            .then(res => res.json())
            .then(data => {
                if (data.success) {
                    showToast('Geri sayım durduruldu', 'warning');
                    pollTimerStatus();
                }
            })
            .catch(() => showToast('Bağlantı hatası', 'error'));
    }
}

function updatePauseButton() {
    const pauseBtn = document.getElementById('pauseBtn');
    if (!pauseBtn) return;
    
    const pauseIcon = pauseBtn.querySelector('.pause-icon');
    const playIcon = pauseBtn.querySelector('.play-icon');
    
    const isRunning = state.timerState === 'RUNNING' || state.timerState === 'WARNING';
    
    if (isRunning) {
        pauseBtn.classList.remove('paused');
        pauseBtn.title = 'Durdur';
        if (pauseIcon) pauseIcon.classList.remove('hidden');
        if (playIcon) playIcon.classList.add('hidden');
    } else {
        pauseBtn.classList.add('paused');
        pauseBtn.title = 'Başlat';
        if (pauseIcon) pauseIcon.classList.add('hidden');
        if (playIcon) playIcon.classList.remove('hidden');
    }
}

let timerPollInterval = null;

function startTimerUpdate() {
    // Poll timer status from ESP32 every 5 seconds
    pollTimerStatus();
    timerPollInterval = setInterval(pollTimerStatus, 5000);
}

function pollTimerStatus() {
    fetch('/api/timer/status')
        .then(res => {
            if (!res.ok) throw new Error('Not authenticated');
            return res.json();
        })
        .then(data => {
            state.timerState = data.state || 'DISABLED';
            state.timeRemainingMs = data.timeRemainingMs || 0;
            state.intervalMinutes = data.intervalMinutes || 1440;
            state.warningsSent = data.warningsSent || 0;
            state.resetCount = data.resetCount || 0;
            state.triggerCount = data.triggerCount || 0;
            state.timerEnabled = data.enabled || false;
            
            // Update vacation state
            if (data.vacationEnabled) {
                state.vacationMode.enabled = true;
                state.vacationMode.days = data.vacationDays || 0;
            }
            
            updateTimerDisplay();
            updatePauseButton();
            updateVacationIndicator();
        })
        .catch(() => {
            // Silent fail during polling
        });
}

function updateTimerDisplay() {
    const totalMs = state.timeRemainingMs;
    const totalSeconds = Math.floor(totalMs / 1000);
    const days = Math.floor(totalSeconds / 86400);
    const hours = Math.floor((totalSeconds % 86400) / 3600);
    const minutes = Math.floor((totalSeconds % 3600) / 60);
    
    // Days display
    if (elements.timeDays) {
        if (days > 0) {
            elements.timeDays.textContent = `${days}g`;
            elements.timeDays.classList.remove('hidden');
        } else {
            elements.timeDays.classList.add('hidden');
        }
    }
    
    // Hours display
    if (elements.timeHours) {
        if (days > 0 || hours > 0) {
            elements.timeHours.textContent = `${hours}s`;
            elements.timeHours.classList.remove('hidden');
        } else {
            elements.timeHours.classList.add('hidden');
        }
    }
    
    // Minutes display (always visible)
    if (elements.timeMinutes) {
        elements.timeMinutes.textContent = `${minutes}d`;
    }
    
    // Timer label based on state
    const timerLabel = document.querySelector('.timer-label');
    if (timerLabel) {
        const labels = {
            'DISABLED': 'DEVRE DIŞI',
            'RUNNING': 'KALAN SÜRE',
            'WARNING': '⚠ UYARI',
            'TRIGGERED': '🔴 TETİKLENDİ',
            'PAUSED': 'DURAKLATILDI',
            'VACATION': '🏖 TATİL MODU'
        };
        timerLabel.textContent = labels[state.timerState] || 'KALAN SÜRE';
    }
    
    // Ring progress
    const intervalMs = state.intervalMinutes * 60 * 1000;
    const percentage = intervalMs > 0 ? Math.min(1, totalMs / intervalMs) : 0;
    const circumference = 2 * Math.PI * 90;
    const offset = circumference * (1 - percentage);
    
    const ringProgress = document.querySelector('.ring-progress');
    if (ringProgress) {
        ringProgress.style.strokeDashoffset = offset;
    }
    
    // Ring color
    if (elements.timerRing) {
        elements.timerRing.classList.remove('warning', 'danger');
        if (state.timerState === 'TRIGGERED') {
            elements.timerRing.classList.add('danger');
        } else if (state.timerState === 'WARNING') {
            elements.timerRing.classList.add('warning');
        } else if (percentage <= 0.25 && state.timerState === 'RUNNING') {
            elements.timerRing.classList.add('danger');
        } else if (percentage <= 0.5 && state.timerState === 'RUNNING') {
            elements.timerRing.classList.add('warning');
        }
    }
}

function updateStatusPill() {
    // Removed - no longer used
}

// ─────────────────────────────────────────────────────────────────────────────
// UI Updates
// ─────────────────────────────────────────────────────────────────────────────
function updateUI() {
    updateTimerDisplay();
}

function updateInfoCards() {
    // Removed - data comes from polling
}

// ─────────────────────────────────────────────────────────────────────────────
// Settings Modal
// ─────────────────────────────────────────────────────────────────────────────
function openSettings() {
    elements.settingsModal.classList.remove('hidden');
    document.body.style.overflow = 'hidden';
    // Reset navigation stack
    state.navStack = [{id: 'root', title: 'Ayarlar', breadcrumb: null}];
    updateModalHeader();
    // Carousel'ı başlat
    initCarousel();
    // Tüm alt görünümleri kapat
    closeAllSubViews();
}

function closeSettings() {
    elements.settingsModal.classList.add('hidden');
    document.body.style.overflow = '';
    // Reset nav stack
    state.navStack = [];
}

// ─────────────────────────────────────────────────────────────────────────────
// Navigation Stack Management
// ─────────────────────────────────────────────────────────────────────────────
function pushView(viewId, title, breadcrumb) {
    state.navStack.push({id: viewId, title: title, breadcrumb: breadcrumb});
    updateModalHeader();
}

function popView() {
    if (state.navStack.length > 1) {
        state.navStack.pop();
        updateModalHeader();
        return true;
    }
    return false;
}

function updateModalHeader() {
    const currentView = state.navStack[state.navStack.length - 1];
    const isRoot = state.navStack.length === 1;
    
    // Title güncelle
    if (elements.modalTitle) {
        elements.modalTitle.textContent = currentView.title;
    }
    
    // Back button ve breadcrumb
    if (elements.modalBackBtn) {
        if (isRoot) {
            elements.modalBackBtn.classList.add('hidden');
        } else {
            elements.modalBackBtn.classList.remove('hidden');
        }
    }
    
    if (elements.modalBreadcrumb) {
        if (isRoot || !currentView.breadcrumb) {
            elements.modalBreadcrumb.classList.add('hidden');
        } else {
            elements.modalBreadcrumb.classList.remove('hidden');
            elements.modalBreadcrumb.textContent = currentView.breadcrumb;
        }
    }
    
    // Carousel'ı ve başlığı gizle/göster
    if (elements.settingsNavCarousel) {
        if (isRoot) {
            elements.settingsNavCarousel.classList.remove('hidden');
            if (elements.modalTitle) elements.modalTitle.classList.add('hidden');
        } else {
            elements.settingsNavCarousel.classList.add('hidden');
            if (elements.modalTitle) elements.modalTitle.classList.remove('hidden');
        }
    }
}

function closeAllSubViews() {
    // Actions panel sub views
    const actionsMainContent = document.getElementById('actionsMainContent');
    const apiView = document.getElementById('apiConfigView');
    const relayView = document.getElementById('relayConfigView');
    const telegramView = document.getElementById('telegramConfigView');
    const earlyMailView = document.getElementById('earlyMailConfigView');
    const triggerMailView = document.getElementById('triggerMailConfigView');
    const mailGroupDetailView = document.getElementById('mailGroupDetailView');
    
    if (actionsMainContent) actionsMainContent.classList.remove('hidden');
    if (apiView) apiView.classList.add('hidden');
    if (relayView) relayView.classList.add('hidden');
    if (telegramView) telegramView.classList.add('hidden');
    if (earlyMailView) earlyMailView.classList.add('hidden');
    if (triggerMailView) triggerMailView.classList.add('hidden');
    if (mailGroupDetailView) mailGroupDetailView.classList.add('hidden');
    
    // System panel sub views
    const systemMainContent = document.getElementById('systemMainContent');
    if (systemMainContent) systemMainContent.classList.remove('hidden');
    document.querySelectorAll('#panel-system .subpage').forEach(sp => {
        sp.classList.add('hidden');
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// Settings Carousel Navigation
// ─────────────────────────────────────────────────────────────────────────────
function initCarousel() {
    state.carouselIndex = 0;
    state.isCarouselAnimating = false;
    
    // İlk sekmeye active class ekle
    if (elements.navItems.length > 0) {
        elements.navItems.forEach(item => item.classList.remove('active'));
        elements.navItems[0].classList.add('active');
    }
    
    // İlk paneli aktif yap, diğerlerini kapat
    const targetTab = state.carouselTabs[0];
    elements.settingsPanels.forEach(panel => {
        panel.classList.remove('active');
        if (panel.id === `panel-${targetTab}`) {
            panel.classList.add('active');
        }
    });
    
    updateCarouselPosition(true);
}

function navigateCarousel(direction) {
    if (state.isCarouselAnimating) return;
    
    const totalTabs = state.carouselTabs.length;
    
    // Sonsuz döngü
    let newIndex = state.carouselIndex + direction;
    if (newIndex < 0) {
        newIndex = totalTabs - 1;
    } else if (newIndex >= totalTabs) {
        newIndex = 0;
    }
    
    state.carouselIndex = newIndex;
    updateCarouselView();
}

function handleNavClick(e) {
    const navItem = e.target.closest('.nav-item');
    if (!navItem) return;
    if (state.isCarouselAnimating) return;
    
    const clickedIndex = parseInt(navItem.dataset.index);
    if (clickedIndex === state.carouselIndex) return;
    
    state.carouselIndex = clickedIndex;
    updateCarouselView();
}

function updateCarouselView() {
    const currentIndex = state.carouselIndex;
    
    // Animasyon başlat
    state.isCarouselAnimating = true;
    
    // Tüm nav itemları güncelle - sadece active class
    elements.navItems.forEach((item, index) => {
        item.classList.remove('active');
        if (index === currentIndex) {
            item.classList.add('active');
        }
    });
    
    // Transform ile kaydır
    updateCarouselPosition(false);
    
    // Panel değiştir
    const targetTab = state.carouselTabs[currentIndex];
    elements.settingsPanels.forEach(panel => {
        panel.classList.remove('active');
        if (panel.id === `panel-${targetTab}`) {
            panel.classList.add('active');
        }
    });
    
    // Animasyon bitişi
    setTimeout(() => {
        state.isCarouselAnimating = false;
    }, 400);
}

function updateCarouselPosition(instant = false) {
    const container = elements.carouselItems;
    const track = document.querySelector('.carousel-track');
    if (!container || !track) return;
    
    const items = Array.from(elements.navItems);
    if (items.length === 0) return;
    
    const currentIndex = state.carouselIndex;
    const trackWidth = track.offsetWidth;
    
    // Aktif sekmenin konumunu hesapla
    let offsetX = 0;
    for (let i = 0; i < currentIndex; i++) {
        offsetX += items[i].offsetWidth + 24; // 24px = gap (var --space-6)
    }
    
    // Aktif sekmeyi ortala
    const activeItemWidth = items[currentIndex].offsetWidth;
    const centerOffset = (trackWidth / 2) - (activeItemWidth / 2);
    const translateX = centerOffset - offsetX;
    
    if (instant) {
        container.style.transition = 'none';
        container.style.transform = `translateX(${translateX}px)`;
        // Force reflow
        container.offsetHeight;
        container.style.transition = '';
    } else {
        container.style.transform = `translateX(${translateX}px)`;
    }
}

function handleSaveSettings() {
    // Convert unit+value to intervalMinutes
    const { unit, value, alarmCount } = state.timerConfig;
    let intervalMinutes = value;
    if (unit === 'days') intervalMinutes = value * 24 * 60;
    else if (unit === 'hours') intervalMinutes = value * 60;
    // minutes stays as-is
    
    // Calculate warningMinutes: spread alarms evenly over the interval
    let warningMinutes = 60; // default 60 min before deadline
    if (alarmCount > 0) {
        if (unit === 'hours') warningMinutes = alarmCount * 60;
        else if (unit === 'days') warningMinutes = alarmCount * 24 * 60;
        else if (unit === 'minutes') warningMinutes = alarmCount;
    }
    
    const timerPayload = {
        enabled: state.timerEnabled || state.timerState !== 'DISABLED',
        intervalMinutes: intervalMinutes,
        warningMinutes: warningMinutes,
        alarmCount: alarmCount
    };
    
    // Save timer config
    fetch('/api/config/timer', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(timerPayload)
    })
    .then(res => res.json())
    .then(data => {
        if (data.success) {
            // Handle vacation mode change
            const vacAction = state.vacationMode.enabled
                ? fetch('/api/timer/vacation', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ enabled: true, days: state.vacationMode.days })
                  })
                : fetch('/api/timer/vacation', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ enabled: false })
                  });
            
            return vacAction.then(() => {
                showToast('Ayarlar kaydedildi', 'success');
                pollTimerStatus();
                closeSettings();
            });
        } else {
            showToast(data.error || 'Kaydetme hatası', 'error');
        }
    })
    .catch(() => showToast('Bağlantı hatası', 'error'));
}

// ─────────────────────────────────────────────────────────────────────────────
// Activity Timeline
// ─────────────────────────────────────────────────────────────────────────────
function toggleActivity() {
    const toggle = document.querySelector('.section-toggle');
    const list = document.querySelector('.activity-list');
    
    toggle.classList.toggle('active');
    list.classList.toggle('hidden');
}

function addActivityItem(text) {
    // Activity logged via addAuditLog instead
}

// ─────────────────────────────────────────────────────────────────────────────
// Toast Notifications
// ─────────────────────────────────────────────────────────────────────────────
function showToast(message, type = 'success') {
    const container = document.getElementById('toastContainer');
    if (!container) return;
    
    const icons = {
        success: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
            <path d="M20 6L9 17l-5-5"/>
        </svg>`,
        warning: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
            <path d="M12 9v4m0 4h.01M12 2L2 20h20L12 2z"/>
        </svg>`,
        error: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
            <circle cx="12" cy="12" r="10"/>
            <path d="M15 9l-6 6m0-6l6 6"/>
        </svg>`
    };
    
    const toast = document.createElement('div');
    toast.className = `toast ${type}`;
    toast.innerHTML = `
        <span class="toast-icon">${icons[type]}</span>
        <span class="toast-message">${message}</span>
    `;
    
    container.appendChild(toast);
    
    // 3 saniye sonra kaldır
    setTimeout(() => {
        toast.style.opacity = '0';
        toast.style.transform = 'translateX(100%)';
        setTimeout(() => toast.remove(), 300);
    }, 3000);
}

// ─────────────────────────────────────────────────────────────────────────────
// Utility Functions
// ─────────────────────────────────────────────────────────────────────────────
function formatTimeAgo(date) {
    const seconds = Math.floor((Date.now() - date.getTime()) / 1000);
    
    if (seconds < 60) return 'Az önce';
    if (seconds < 3600) return `${Math.floor(seconds / 60)} dk önce`;
    if (seconds < 86400) return `${Math.floor(seconds / 3600)} saat önce`;
    return `${Math.floor(seconds / 86400)} gün önce`;
}

function formatTime(date) {
    return date.toLocaleTimeString('tr-TR', { 
        hour: '2-digit', 
        minute: '2-digit' 
    });
}

function formatDate(date) {
    return date.toLocaleDateString('tr-TR', { 
        day: '2-digit', 
        month: '2-digit', 
        year: 'numeric',
        hour: '2-digit',
        minute: '2-digit'
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// Keyboard Shortcuts
// ─────────────────────────────────────────────────────────────────────────────
document.addEventListener('keydown', (e) => {
    // ESC - Modal kapat
    if (e.key === 'Escape' && !elements.settingsModal.classList.contains('hidden')) {
        closeSettings();
    }
    
    // R - Sıfırla (sadece app sayfasında)
    if (e.key === 'r' && state.isAuthenticated && !e.ctrlKey && !e.metaKey) {
        const target = e.target.tagName.toLowerCase();
        if (target !== 'input' && target !== 'textarea') {
            handleReset();
        }
    }
});

// ─────────────────────────────────────────────────────────────────────────────
// Theme Management
// ─────────────────────────────────────────────────────────────────────────────
function initTheme() {
    const savedTheme = localStorage.getItem('theme') || 'dark';
    setTheme(savedTheme, false);
    
    // Sistem tema değişikliklerini dinle (auto mod için)
    if (window.matchMedia) {
        window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', (e) => {
            if (state.theme === 'auto') {
                applyTheme(e.matches ? 'dark' : 'light');
            }
        });
    }
}

function getSystemTheme() {
    if (window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches) {
        return 'dark';
    }
    return 'light';
}

function applyTheme(actualTheme) {
    document.documentElement.setAttribute('data-theme', actualTheme);
    
    // Logo: koyu temada beyaz logo, açık temada siyah logo
    const brandLogo = document.getElementById('brandLogo');
    if (brandLogo) {
        brandLogo.src = actualTheme === 'dark' ? 'logo.png' : 'darklogo.png';
    }
    
    // Meta theme-color güncelle
    const metaThemeColor = document.querySelector('meta[name="theme-color"]');
    if (metaThemeColor) {
        metaThemeColor.setAttribute('content', actualTheme === 'dark' ? '#0a0a0a' : '#ffffff');
    }
}

function setTheme(theme, save = true) {
    state.theme = theme;
    
    // Auto ise sistem temasını kullan, değilse seçilen temayı
    const actualTheme = theme === 'auto' ? getSystemTheme() : theme;
    applyTheme(actualTheme);
    
    if (save) {
        localStorage.setItem('theme', theme);
        const themeNames = { 'dark': 'Koyu', 'light': 'Açık', 'auto': 'Otomatik' };
        addAuditLog('settings', `Tema değiştirildi: ${themeNames[theme] || theme}`);
    }
    
    // Update theme selector UI
    const themeSelect = document.getElementById('themeSelect');
    if (themeSelect) {
        themeSelect.value = theme;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Auto Logout
// ─────────────────────────────────────────────────────────────────────────────
function initAutoLogout() {
    state.autoLogoutTime = parseInt(localStorage.getItem('autoLogoutTime')) || 10;
}

function startAutoLogoutTimer() {
    state.lastActivity = Date.now();
    
    if (state.autoLogoutTimer) {
        clearInterval(state.autoLogoutTimer);
    }
    
    state.autoLogoutTimer = setInterval(() => {
        if (!state.isAuthenticated) return;
        
        const elapsed = (Date.now() - state.lastActivity) / 60000; // dakika
        
        if (elapsed >= state.autoLogoutTime) {
            handleAutoLogout();
        }
    }, 10000); // Her 10 saniyede kontrol et
}

function stopAutoLogoutTimer() {
    if (state.autoLogoutTimer) {
        clearInterval(state.autoLogoutTimer);
        state.autoLogoutTimer = null;
    }
}

function resetAutoLogoutTimer() {
    if (state.isAuthenticated) {
        state.lastActivity = Date.now();
    }
}

function handleAutoLogout() {
    showToast('İşlem yapılmadığı için çıkış yapıldı', 'warning');
    addAuditLog('auto_logout', `${state.autoLogoutTime} dakika işlem yapılmadı`);
    handleLogout();
}

// ─────────────────────────────────────────────────────────────────────────────
// PWA Installation
// ─────────────────────────────────────────────────────────────────────────────
function initPWA() {
    // Register Service Worker
    if ('serviceWorker' in navigator) {
        navigator.serviceWorker.register('sw.js')
            .then(registration => {
                console.log('SW registered:', registration);
            })
            .catch(error => {
                console.log('SW registration failed:', error);
            });
    }
    
    window.addEventListener('beforeinstallprompt', (e) => {
        e.preventDefault();
        state.deferredPrompt = e;
        
        // Show install button
        const pwaSection = document.querySelector('.pwa-install');
        if (pwaSection) {
            pwaSection.style.display = 'flex';
        }
    });
    
    window.addEventListener('appinstalled', () => {
        state.deferredPrompt = null;
        showToast('Uygulama yüklendi!', 'success');
        addAuditLog('pwa', 'PWA yüklendi');
    });
}

async function installPWA() {
    if (!state.deferredPrompt) {
        showToast('PWA kurulumu kullanılamıyor', 'warning');
        return;
    }
    
    state.deferredPrompt.prompt();
    const { outcome } = await state.deferredPrompt.userChoice;
    
    if (outcome === 'accepted') {
        showToast('PWA yükleniyor...', 'success');
    }
    
    state.deferredPrompt = null;
}

// ─────────────────────────────────────────────────────────────────────────────
// Encryption helpers using Web Crypto API (AES-GCM)
// ─────────────────────────────────────────────────────────────────────────────
async function encryptData(data, password) {
    const enc = new TextEncoder();
    const keyMaterial = await crypto.subtle.importKey('raw', enc.encode(password), 'PBKDF2', false, ['deriveKey']);
    const salt = crypto.getRandomValues(new Uint8Array(16));
    const key = await crypto.subtle.deriveKey(
        { name: 'PBKDF2', salt, iterations: 100000, hash: 'SHA-256' },
        keyMaterial, { name: 'AES-GCM', length: 256 }, false, ['encrypt']
    );
    const iv = crypto.getRandomValues(new Uint8Array(12));
    const encrypted = await crypto.subtle.encrypt({ name: 'AES-GCM', iv }, key, enc.encode(data));
    // Combine salt + iv + ciphertext and base64 encode
    const combined = new Uint8Array(salt.length + iv.length + encrypted.byteLength);
    combined.set(salt, 0);
    combined.set(iv, salt.length);
    combined.set(new Uint8Array(encrypted), salt.length + iv.length);
    return btoa(String.fromCharCode(...combined));
}

async function decryptData(data, password) {
    try {
        const enc = new TextEncoder();
        const raw = Uint8Array.from(atob(data), c => c.charCodeAt(0));
        const salt = raw.slice(0, 16);
        const iv = raw.slice(16, 28);
        const ciphertext = raw.slice(28);
        const keyMaterial = await crypto.subtle.importKey('raw', enc.encode(password), 'PBKDF2', false, ['deriveKey']);
        const key = await crypto.subtle.deriveKey(
            { name: 'PBKDF2', salt, iterations: 100000, hash: 'SHA-256' },
            keyMaterial, { name: 'AES-GCM', length: 256 }, false, ['decrypt']
        );
        const decrypted = await crypto.subtle.decrypt({ name: 'AES-GCM', iv }, key, ciphertext);
        return new TextDecoder().decode(decrypted);
    } catch {
        throw new Error('Decryption failed');
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// OTA Updates
// ─────────────────────────────────────────────────────────────────────────────
async function checkForUpdates() {
    showToast('Güncelleme kontrol ediliyor...', 'success');
    
    // Update last check time
    const now = new Date();
    const lastCheckEl = document.getElementById('lastOtaCheck');
    if (lastCheckEl) lastCheckEl.textContent = now.toLocaleString();
    
    const progressDiv = document.querySelector('.ota-progress');
    if (progressDiv) {
        progressDiv.classList.add('active');
        const fill = progressDiv.querySelector('.ota-progress-fill');
        const text = progressDiv.querySelector('.ota-progress-text');
        
        if (fill) fill.style.width = '50%';
        if (text) text.textContent = 'Sunucu kontrol ediliyor...';
        
        try {
            const res = await fetch('/api/ota/check');
            const data = await res.json();
            
            if (fill) fill.style.width = '100%';
            
            if (data.updateAvailable) {
                if (text) text.textContent = `Yeni sürüm mevcut: ${data.version}`;
                if (fill) fill.style.background = 'var(--accent-warning)';
            } else {
                if (text) text.textContent = 'Güncelleme mevcut değil, sistem güncel.';
                if (fill) fill.style.background = 'var(--accent-success)';
            }
            
            // Show firmware version from response
            if (data.currentVersion) {
                const fwEl = document.getElementById('currentFirmwareVersion');
                if (fwEl && fwEl.textContent === '-') fwEl.textContent = 'v' + data.currentVersion;
            }
            
            addAuditLog('ota', 'Güncelleme kontrolü yapıldı');
        } catch (err) {
            if (fill) fill.style.width = '100%';
            if (fill) fill.style.background = 'var(--accent-error)';
            if (text) text.textContent = 'Güncelleme kontrolü başarısız';
        }
        
        setTimeout(() => {
            progressDiv.classList.remove('active');
            if (fill) { fill.style.width = '0%'; fill.style.background = ''; }
        }, 3000);
    } else {
        // No progress div, just make the API call
        try {
            const res = await fetch('/api/ota/check');
            const data = await res.json();
            if (data.updateAvailable) {
                showToast(`Yeni sürüm mevcut: ${data.version}`, 'warning');
            } else {
                showToast('Sistem güncel', 'success');
            }
        } catch (err) {
            showToast('Güncelleme kontrolü başarısız', 'error');
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Audit & Device Logs
// ─────────────────────────────────────────────────────────────────────────────
function initLogs() {
    // Fetch real logs from ESP32
    state.auditLogs = [];
    state.deviceLogs = [];
    fetchLogs();
}

function fetchLogs() {
    fetch('/api/logs')
        .then(res => res.json())
        .then(data => {
            if (data.entries && Array.isArray(data.entries)) {
                state.auditLogs = [];
                state.deviceLogs = [];
                data.entries.forEach(entry => {
                    const log = {
                        time: new Date(entry.timestamp * 1000),
                        type: entry.category || 'info',
                        text: entry.message || '',
                        detail: ''
                    };
                    // Separate audit (auth, config, timer) from device (system, wifi, error)
                    const auditTypes = ['auth', 'config', 'login', 'settings', 'backup', 'timer'];
                    if (auditTypes.includes(log.type)) {
                        state.auditLogs.push(log);
                    } else {
                        state.deviceLogs.push(log);
                    }
                });
                renderAuditLogs();
                renderDeviceLogs();
            }
        })
        .catch(err => console.error('Log fetch failed:', err));
}

function addAuditLog(type, text, detail = '') {
    const log = {
        time: new Date(),
        type: type,
        text: text,
        detail: detail
    };
    
    state.auditLogs.unshift(log);
    
    // Keep max 100 logs
    if (state.auditLogs.length > 100) {
        state.auditLogs = state.auditLogs.slice(0, 100);
    }
    
    renderAuditLogs();
}

function addDeviceLog(type, text, detail = '') {
    const log = {
        time: new Date(),
        type: type,
        text: text,
        detail: detail
    };
    
    state.deviceLogs.unshift(log);
    
    // Keep max 100 logs
    if (state.deviceLogs.length > 100) {
        state.deviceLogs = state.deviceLogs.slice(0, 100);
    }
    
    renderDeviceLogs();
}

function renderAuditLogs(filter = 'all') {
    const list = document.getElementById('auditLogList');
    if (!list) return;
    
    const logs = filter === 'all' 
        ? state.auditLogs 
        : state.auditLogs.filter(log => log.type === filter);
    
    list.innerHTML = logs.map(log => `
        <div class="log-item log-info">
            <span class="log-time">${formatDate(log.time)}</span>
            <div class="log-content">
                <span class="log-text">${log.text}</span>
            </div>
            ${log.detail ? `<span class="log-detail">${log.detail}</span>` : ''}
        </div>
    `).join('');
}

function renderDeviceLogs(filter = 'all') {
    const list = document.getElementById('deviceLogList');
    if (!list) return;
    
    const logs = filter === 'all' 
        ? state.deviceLogs 
        : state.deviceLogs.filter(log => log.type === filter);
    
    list.innerHTML = logs.map(log => `
        <div class="log-item log-${log.type}">
            <span class="log-time">${formatDate(log.time)}</span>
            <div class="log-content">
                <span class="log-text">${log.text}</span>
            </div>
            ${log.detail ? `<span class="log-detail">${log.detail}</span>` : ''}
        </div>
    `).join('');
}

function filterLogs(filter, section) {
    // Update button states
    section.querySelectorAll('[data-log-filter]').forEach(btn => {
        btn.classList.remove('active');
    });
    section.querySelector(`[data-log-filter="${filter}"]`)?.classList.add('active');
    
    // Determine which log list
    const isAudit = section.querySelector('#auditLogList');
    
    if (isAudit) {
        renderAuditLogs(filter);
    } else {
        renderDeviceLogs(filter);
    }
}

function exportLogs(type) {
    const logs = type === 'audit' ? state.auditLogs : state.deviceLogs;
    const content = logs.map(log => 
        `${formatDate(log.time)} | ${log.type} | ${log.text}${log.detail ? ' | ' + log.detail : ''}`
    ).join('\n');
    
    const blob = new Blob([content], { type: 'text/plain' });
    const url = URL.createObjectURL(blob);
    
    const a = document.createElement('a');
    a.href = url;
    a.download = `lebensspur-${type}-logs-${formatDateForFile(new Date())}.txt`;
    a.click();
    
    URL.revokeObjectURL(url);
    showToast(`${type === 'audit' ? 'İşlem' : 'Cihaz'} logları indirildi`, 'success');
}

// ─────────────────────────────────────────────────────────────────────────────
// Additional Utility Functions
// ─────────────────────────────────────────────────────────────────────────────
function sleep(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
}

function formatDateForFile(date) {
    return date.toISOString().slice(0, 10);
}
