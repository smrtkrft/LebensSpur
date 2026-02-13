/**
 * ═══════════════════════════════════════════════════════════════════════════
 * LebensSpur - Dead Man's Switch Controller
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Ana init dosyasi. Tum modul dosyalari (state, utils, ui, auth, timer,
 * settings, actions, mailGroups, logs, ota, theme) bu dosyadan once yuklenir.
 * Global scope paylasimi ile senkron calisirlar.
 */

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

    // Sayfa yuklendiginde auth kontrol et
    checkAuthOnLoad();

    // Initialize i18n language system
    if (window.I18n) {
        I18n.init().then(() => {
            const sel = document.getElementById('languageSelect');
            if (sel) sel.value = I18n.getLanguage();
        });
        // Re-render dynamic content when language changes
        window.addEventListener('languageChanged', () => {
            if (typeof updateTimerDisplay === 'function') updateTimerDisplay();
        });
    }
});

// ─────────────────────────────────────────────────────────────────────────────
// Event Listeners
// ─────────────────────────────────────────────────────────────────────────────
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

    // Cancel Settings
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

    // Theme Selector
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

    // Log Filter Selects
    const auditLogFilter = document.getElementById('auditLogFilter');
    if (auditLogFilter) {
        auditLogFilter.addEventListener('change', () => {
            filterLogs(auditLogFilter.value, auditLogFilter.closest('.setting-section'));
        });
    }
    const deviceLogFilter = document.getElementById('deviceLogFilter');
    if (deviceLogFilter) {
        deviceLogFilter.addEventListener('change', () => {
            filterLogs(deviceLogFilter.value, deviceLogFilter.closest('.setting-section'));
        });
    }

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

    // Import file area click -> trigger file input
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
            showToast(t('toast.autoLogoutSaved'), 'success');
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
                navigator.clipboard.writeText(key).then(() => showToast(t('toast.apiKeyCopied'), 'success'));
            }
        });
    }

    // Refresh API Key
    const refreshApiKeyBtn = document.getElementById('refreshApiKeyBtn');
    if (refreshApiKeyBtn) {
        refreshApiKeyBtn.addEventListener('click', handleRefreshApiKey);
    }

    // Mail Group: Delete Button
    const mailGroupDeleteBtn = document.getElementById('mailGroupDeleteBtn');
    if (mailGroupDeleteBtn) {
        mailGroupDeleteBtn.addEventListener('click', deleteMailGroup);
    }

    // Mail Group: Add Recipient Button
    const addRecipientBtn = document.getElementById('addRecipientBtn');
    if (addRecipientBtn) {
        addRecipientBtn.addEventListener('click', addRecipient);
    }

    // Mail Group: File Upload
    const mailGroupFileBtn = document.getElementById('mailGroupFileBtn');
    const mailGroupFileInput = document.getElementById('mailGroupFile');
    if (mailGroupFileBtn && mailGroupFileInput) {
        mailGroupFileBtn.addEventListener('click', () => mailGroupFileInput.click());
        mailGroupFileInput.addEventListener('change', handleFileUpload);
    }

    // Clear Logs Buttons
    const clearAuditBtn = document.getElementById('clearAuditLogs');
    if (clearAuditBtn) {
        clearAuditBtn.addEventListener('click', () => clearLogs('audit'));
    }
    const clearDeviceBtn = document.getElementById('clearDeviceLogs');
    if (clearDeviceBtn) {
        clearDeviceBtn.addEventListener('click', () => clearLogs('device'));
    }

    // Test Buttons (SMTP, WiFi, Telegram, Webhook, MQTT)
    const testSmtpBtn = document.getElementById('testSmtpBtn');
    if (testSmtpBtn) {
        testSmtpBtn.addEventListener('click', () => sendTest('smtp'));
    }
    const testWifiBtn = document.getElementById('testWifiBtn');
    if (testWifiBtn) {
        testWifiBtn.addEventListener('click', () => sendTest('wifi'));
    }
    const testTelegramBtn = document.getElementById('testTelegramBtn');
    if (testTelegramBtn) {
        testTelegramBtn.addEventListener('click', () => sendTest('telegram'));
    }
    const testWebhookBtn = document.getElementById('testWebhookBtn');
    if (testWebhookBtn) {
        testWebhookBtn.addEventListener('click', () => sendTest('webhook'));
    }
    const testMqttBtn = document.getElementById('testMqttBtn');
    if (testMqttBtn) {
        testMqttBtn.addEventListener('click', () => sendTest('mqtt'));
    }

    // Enter Key Support
    initEnterKeyBindings();
}

// ─────────────────────────────────────────────────────────────────────────────
// Enter Key Bindings
// ─────────────────────────────────────────────────────────────────────────────
function initEnterKeyBindings() {
    function bindEnter(inputId, buttonId) {
        const inp = document.getElementById(inputId);
        const btn = document.getElementById(buttonId);
        if (inp && btn) {
            inp.addEventListener('keydown', (e) => {
                if (e.key === 'Enter') { e.preventDefault(); btn.click(); }
            });
        }
    }

    bindEnter('wifiPass', 'saveWifiBtn');
    bindEnter('wifiBackupPass', 'saveWifiBackupBtn');
    bindEnter('smtpApiKey', 'saveSmtpBtn');
    bindEnter('confirmPassword', 'changePasswordBtn');
    bindEnter('exportPasswordConfirm', 'startExportBtn');
    bindEnter('importPassword', 'startImportBtn');
}

// ─────────────────────────────────────────────────────────────────────────────
// Keyboard Shortcuts
// ─────────────────────────────────────────────────────────────────────────────
document.addEventListener('keydown', (e) => {
    // ESC - Modal kapat
    if (e.key === 'Escape' && !elements.settingsModal.classList.contains('hidden')) {
        closeSettings();
    }

    // R - Sifirla (sadece app sayfasinda)
    if (e.key === 'r' && state.isAuthenticated && !e.ctrlKey && !e.metaKey) {
        const target = e.target.tagName.toLowerCase();
        if (target !== 'input' && target !== 'textarea') {
            handleReset();
        }
    }
});
