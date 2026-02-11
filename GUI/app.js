/**
 * ═══════════════════════════════════════════════════════════════════════════
 * LebensSpur - UI Prototype JavaScript
 * 
 * Bu dosya sadece tasarım önizlemesi içindir.
 * Gerçek API çağrıları yerine simülasyon kullanılır.
 * ═══════════════════════════════════════════════════════════════════════════
 */

// ─────────────────────────────────────────────────────────────────────────────
// State Management
// ─────────────────────────────────────────────────────────────────────────────
const state = {
    isAuthenticated: false,
    timerDuration: 24 * 60 * 60, // 24 saat (saniye)
    timeRemaining: 2 * 24 * 60 * 60 + 14 * 60 * 60 + 32 * 60, // 2 gün 14 saat 32 dakika (demo için)
    lastReset: new Date(Date.now() - 6 * 60 * 60 * 1000), // 6 saat önce
    isActive: true,
    
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
        days: 7,
        startDate: null,
        remainingDays: 0
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
    
    // Mail Groups (max 10)
    mailGroups: [
        {
            id: 1,
            name: 'Aile',
            subject: 'LebensSpur - Önemli Bildirim',
            content: 'Merhaba,\n\nLebensSpur zamanlayıcısı tetiklendi. Bu otomatik bir bildirimdir.',
            files: [],
            recipients: ['anne@example.com', 'baba@example.com', 'kardes@example.com']
        },
        {
            id: 2,
            name: 'İş Arkadaşları',
            subject: 'LebensSpur Bildirimi',
            content: 'Merhaba,\n\nBu bir LebensSpur bildirimidir.',
            files: [],
            recipients: ['is1@company.com', 'is2@company.com']
        }
    ],
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
            recipients: [
                { email: 'primary@example.com', type: 'primary' },
                { email: 'contact@family.com', type: 'secondary' }
            ]
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
            trustedContacts: [
                { name: 'Alice', code: '****', access: 'read-only' },
                { name: 'Bob', code: '****', access: 'full' }
            ]
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
    initDemoLogs();
    updateUI();
    startTimerUpdate();
    fetchDeviceInfo();
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

    // Save Settings
    const saveSettingsBtn = document.getElementById('saveSettings');
    if (saveSettingsBtn) {
        saveSettingsBtn.addEventListener('click', handleSaveSettings);
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
    // if (exportBtn) {
    //     exportBtn.addEventListener('click', openExportModal);
    // }

    // Import Settings Button - Artık subpage açılıyor, eski dosya seçici kaldırıldı
    // const importBtn = document.getElementById('importSettingsBtn');
    // if (importBtn) {
    //     importBtn.addEventListener('click', importSettings);
    // }

    // Export Confirm Button
    const exportConfirmBtn = document.getElementById('exportConfirmBtn');
    if (exportConfirmBtn) {
        exportConfirmBtn.addEventListener('click', exportSettings);
    }

    // Close Export Modal
    const closeExportModal = document.getElementById('closeExportModal');
    if (closeExportModal) {
        closeExportModal.addEventListener('click', () => {
            document.getElementById('exportModal')?.classList.add('hidden');
        });
    }

    // OTA Check Button
    const otaCheckBtn = document.getElementById('otaCheckBtn');
    if (otaCheckBtn) {
        otaCheckBtn.addEventListener('click', checkForUpdates);
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
    
    // Export Button
    const startExportBtn = document.getElementById('startExportBtn');
    if (startExportBtn) {
        startExportBtn.addEventListener('click', handleExport);
    }
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
    
    // TODO: Backend'e şifre değiştirme isteği gönder
    // Şimdilik simüle ediyoruz
    showToast('Şifre başarıyla değiştirildi', 'success');
    
    // Alanları temizle
    document.getElementById('currentPassword').value = '';
    document.getElementById('newPassword').value = '';
    document.getElementById('confirmPassword').value = '';
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
    
    // TODO: Backend'ten tarayıcı şifresi doğrulaması
    // Şimdilik sadece boş olmadığını kontrol ediyoruz
    
    // Yedek şifresi doğrulaması
    if (!exportPassword) {
        showToast('Yedek şifresi belirleyin', 'error');
        return;
    }
    
    if (exportPassword.length < 4) {
        showToast('Yedek şifresi en az 4 karakter olmalı', 'error');
        return;
    }
    
    if (exportPassword !== exportPasswordConfirm) {
        showToast('Yedek şifreleri eşleşmiyor', 'error');
        return;
    }
    
    // TODO: Backend'e dışa aktarma isteği gönder
    showToast('Ayarlar dışa aktarılıyor...', 'success');
    
    // Alanları temizle
    document.getElementById('exportAuthPassword').value = '';
    document.getElementById('exportPassword').value = '';
    document.getElementById('exportPasswordConfirm').value = '';
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
        state.mailGroups.push({
            id: newId,
            name,
            subject,
            content,
            files,
            recipients
        });
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
    
    // Refresh list view
    renderMailGroupList();
    handleModalBack();
}

function deleteMailGroup() {
    if (!state.currentEditingGroup) return;
    
    const groupIndex = state.mailGroups.findIndex(g => g.id === state.currentEditingGroup);
    if (groupIndex > -1) {
        state.mailGroups.splice(groupIndex, 1);
        showToast('Mail grubu silindi', 'success');
        renderMailGroupList();
        handleModalBack();
    }
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
    // Gerçek uygulamada bu değer cihazdan gelecek
    // Şimdilik demo için örnek bir Device ID kullanıyoruz
    const deviceId = 'LS-12SE4F6K32'; // Demo Device ID
    
    const apSsidInput = document.getElementById('apSsid');
    const apMdnsInput = document.getElementById('apMdns');
    const wifiMdnsInput = document.getElementById('wifiMdnsHostname');
    const wifiBackupMdnsInput = document.getElementById('wifiBackupMdnsHostname');
    
    // AP Mod değerlerini ayarla (readonly)
    if (apSsidInput) apSsidInput.value = deviceId;
    if (apMdnsInput) apMdnsInput.value = deviceId + '.local';
    
    // WiFi mDNS placeholder'larını ayarla
    if (wifiMdnsInput) wifiMdnsInput.placeholder = deviceId;
    if (wifiBackupMdnsInput) wifiBackupMdnsInput.placeholder = deviceId;
    
    // AP Mode toggle event listener - uyarıyı göster/gizle
    const apModeToggle = document.getElementById('apModeEnabled');
    const apModeWarning = document.getElementById('apModeWarning');
    
    if (apModeToggle && apModeWarning) {
        apModeToggle.addEventListener('change', () => {
            // Toggle kapalıysa uyarıyı göster
            apModeWarning.classList.toggle('hidden', apModeToggle.checked);
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
        showToast('API ayarları kaydedildi', 'success');
        handleModalBack();
    });
    
    // Relay Config cancel/save buttons
    const relayCancelBtn = document.getElementById('relayConfigCancelBtn');
    const relaySaveBtn = document.getElementById('relayConfigSaveBtn');
    
    if (relayCancelBtn) relayCancelBtn.addEventListener('click', handleModalBack);
    if (relaySaveBtn) relaySaveBtn.addEventListener('click', () => {
        showToast('Röle ayarları kaydedildi', 'success');
        handleModalBack();
    });
    
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
        showToast('Telegram ayarları kaydedildi', 'success');
        handleModalBack();
    });
    
    // Early Mail Config cancel/save buttons
    const earlyMailCancelBtn = document.getElementById('earlyMailConfigCancelBtn');
    const earlyMailSaveBtn = document.getElementById('earlyMailConfigSaveBtn');
    
    if (earlyMailCancelBtn) earlyMailCancelBtn.addEventListener('click', handleModalBack);
    if (earlyMailSaveBtn) earlyMailSaveBtn.addEventListener('click', () => {
        showToast('Erken uyarı mail ayarları kaydedildi', 'success');
        handleModalBack();
    });
    
    // Trigger Mail Config back button
    const triggerMailCancelBtn = document.getElementById('triggerMailConfigCancelBtn');
    if (triggerMailCancelBtn) triggerMailCancelBtn.addEventListener('click', handleModalBack);
    
    // Mail Group Detail buttons
    const mailGroupCancelBtn = document.getElementById('mailGroupCancelBtn');
    const mailGroupSaveBtn = document.getElementById('mailGroupSaveBtn');
    
    if (mailGroupCancelBtn) mailGroupCancelBtn.addEventListener('click', handleModalBack);
    if (mailGroupSaveBtn) mailGroupSaveBtn.addEventListener('click', () => {
        showToast('Mail grubu kaydedildi', 'success');
        handleModalBack();
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
            updateVacationIndicator();
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
            state.vacationMode.remainingDays = val;
            updateVacationIndicator();
        });
    }
    
    if (vacationIncrease) {
        vacationIncrease.addEventListener('click', () => {
            let val = parseInt(vacationInput.value) || 1;
            val = Math.min(60, val + 1);
            vacationInput.value = val;
            state.vacationMode.days = val;
            state.vacationMode.remainingDays = val;
            updateVacationIndicator();
        });
    }
    
    if (vacationDecrease) {
        vacationDecrease.addEventListener('click', () => {
            let val = parseInt(vacationInput.value) || 1;
            val = Math.max(1, val - 1);
            vacationInput.value = val;
            state.vacationMode.days = val;
            state.vacationMode.remainingDays = val;
            updateVacationIndicator();
        });
    }
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
    
    if (state.vacationMode.enabled && state.vacationMode.remainingDays > 0) {
        indicator.classList.add('active');
        if (remaining) {
            remaining.textContent = `${state.vacationMode.remainingDays} gün`;
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

            // GUI version
            setTxt('sysGuiVersion', 'v0.1.0');
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
// Timer Logic
// ─────────────────────────────────────────────────────────────────────────────
function handleReset() {
    state.timeRemaining = state.timerDuration;
    state.lastReset = new Date();
    state.isActive = true;
    
    // Reset pause button to pause state
    updatePauseButton();
    
    updateTimerDisplay();
    showToast('Zamanlayıcı sıfırlandı', 'success');
}

function handlePause() {
    state.isActive = !state.isActive;
    updatePauseButton();
    
    if (state.isActive) {
        showToast('Geri sayım devam ediyor', 'success');
    } else {
        showToast('Geri sayım durduruldu', 'warning');
    }
}

function updatePauseButton() {
    const pauseBtn = document.getElementById('pauseBtn');
    if (!pauseBtn) return;
    
    const pauseIcon = pauseBtn.querySelector('.pause-icon');
    const playIcon = pauseBtn.querySelector('.play-icon');
    
    if (state.isActive) {
        // Timer running - show pause icon
        pauseBtn.classList.remove('paused');
        pauseBtn.title = 'Durdur';
        if (pauseIcon) pauseIcon.classList.remove('hidden');
        if (playIcon) playIcon.classList.add('hidden');
    } else {
        // Timer paused - show play icon
        pauseBtn.classList.add('paused');
        pauseBtn.title = 'Devam Et';
        if (pauseIcon) pauseIcon.classList.add('hidden');
        if (playIcon) playIcon.classList.remove('hidden');
    }
}

function startTimerUpdate() {
    // Her saniye güncelle
    setInterval(() => {
        if (state.isActive && !state.settings.timer.vacationMode && state.timeRemaining > 0) {
            state.timeRemaining--;
            updateTimerDisplay();
        }
    }, 1000);
}

function updateTimerDisplay() {
    const totalSeconds = state.timeRemaining;
    const days = Math.floor(totalSeconds / 86400);
    const hours = Math.floor((totalSeconds % 86400) / 3600);
    const minutes = Math.floor((totalSeconds % 3600) / 60);
    
    // Gün gösterimi
    if (elements.timeDays) {
        if (days > 0) {
            elements.timeDays.textContent = `${days}g`;
            elements.timeDays.classList.remove('hidden');
        } else {
            elements.timeDays.classList.add('hidden');
        }
    }
    
    // Saat gösterimi
    if (elements.timeHours) {
        if (days > 0 || hours > 0) {
            elements.timeHours.textContent = `${hours}s`;
            elements.timeHours.classList.remove('hidden');
        } else {
            elements.timeHours.classList.add('hidden');
        }
    }
    
    // Dakika gösterimi (her zaman görünür)
    if (elements.timeMinutes) {
        elements.timeMinutes.textContent = `${minutes}d`;
    }
    
    // Update ring progress
    const percentage = state.timeRemaining / state.timerDuration;
    const circumference = 2 * Math.PI * 90; // r=90
    const offset = circumference * (1 - percentage);
    
    const ringProgress = document.querySelector('.ring-progress');
    if (ringProgress) {
        ringProgress.style.strokeDashoffset = offset;
    }
    
    // Update ring color based on remaining time
    if (elements.timerRing) {
        elements.timerRing.classList.remove('warning', 'danger');
        if (percentage <= 0.25) {
            elements.timerRing.classList.add('danger');
        } else if (percentage <= 0.5) {
            elements.timerRing.classList.add('warning');
        }
    }
}

function updateStatusPill() {
    // Status pill artık kullanılmıyor - sadece uyumluluk için tutuldu
}

// ─────────────────────────────────────────────────────────────────────────────
// UI Updates
// ─────────────────────────────────────────────────────────────────────────────
function updateUI() {
    updateTimerDisplay();
}

function updateInfoCards() {
    // Son sıfırlama
    if (elements.lastResetTime) {
        elements.lastResetTime.textContent = formatTimeAgo(state.lastReset);
    }
    
    // Sonraki aksiyon
    if (elements.nextActionTime) {
        const nextAction = new Date(Date.now() + state.timeRemaining * 1000);
        elements.nextActionTime.textContent = formatTime(nextAction);
    }
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
    // Demo: Ayarları kaydet
    showToast('Ayarlar kaydedildi', 'success');
    closeSettings();
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
    // Activity list artık kullanılmıyor - sadece uyumluluk için tutuldu
    console.log('Activity:', text);
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
// Demo: Simülasyon için rastgele aktivite ekle
// ─────────────────────────────────────────────────────────────────────────────
const demoActivities = [
    'Otomatik sıfırlama (API)',
    'E-posta bildirimi gönderildi',
    'Uyarı seviyesine ulaşıldı',
    'Kritik seviyeye ulaşıldı',
    'Webhook tetiklendi',
    'Telegram bildirimi gönderildi'
];

// Her 30 saniyede rastgele bir aktivite ekle (demo için)
setInterval(() => {
    if (state.isAuthenticated && Math.random() > 0.7) {
        const randomActivity = demoActivities[Math.floor(Math.random() * demoActivities.length)];
        addActivityItem(randomActivity);
    }
}, 30000);

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
// Export / Import Settings
// ─────────────────────────────────────────────────────────────────────────────
function openExportModal() {
    const modal = document.getElementById('exportModal');
    if (modal) {
        modal.classList.remove('hidden');
    }
}

async function exportSettings() {
    const passwordInput = document.getElementById('exportPassword');
    const password = passwordInput?.value;
    
    if (!password || password.length < 4) {
        showToast('En az 4 karakterlik şifre girin', 'error');
        return;
    }
    
    try {
        const data = {
            settings: state.settings,
            exportDate: new Date().toISOString(),
            version: '1.0'
        };
        
        // Simple encryption (demo - gerçek uygulamada AES kullanılmalı)
        const encrypted = await encryptData(JSON.stringify(data), password);
        
        const blob = new Blob([encrypted], { type: 'application/octet-stream' });
        const url = URL.createObjectURL(blob);
        
        const a = document.createElement('a');
        a.href = url;
        a.download = `lebensspur-backup-${formatDateForFile(new Date())}.lsb`;
        a.click();
        
        URL.revokeObjectURL(url);
        
        document.getElementById('exportModal')?.classList.add('hidden');
        passwordInput.value = '';
        
        showToast('Yedekleme dosyası indirildi', 'success');
        addAuditLog('backup', 'Ayarlar dışa aktarıldı');
    } catch (error) {
        showToast('Dışa aktarma başarısız', 'error');
    }
}

async function importSettings() {
    const input = document.createElement('input');
    input.type = 'file';
    input.accept = '.lsb,.json';
    
    input.onchange = async (e) => {
        const file = e.target.files[0];
        if (!file) return;
        
        const password = prompt('Yedekleme şifresini girin:');
        if (!password) return;
        
        try {
            const content = await file.text();
            const decrypted = await decryptData(content, password);
            const data = JSON.parse(decrypted);
            
            if (data.settings) {
                Object.assign(state.settings, data.settings);
                showToast('Ayarlar geri yüklendi', 'success');
                addAuditLog('restore', 'Ayarlar içe aktarıldı');
            }
        } catch (error) {
            showToast('İçe aktarma başarısız. Şifre yanlış olabilir.', 'error');
        }
    };
    
    input.click();
}

// Simple encryption helpers (demo - production'da Web Crypto API kullanılmalı)
async function encryptData(data, password) {
    // Demo: Base64 encode with simple XOR
    const encoded = btoa(unescape(encodeURIComponent(data)));
    let result = '';
    for (let i = 0; i < encoded.length; i++) {
        result += String.fromCharCode(encoded.charCodeAt(i) ^ password.charCodeAt(i % password.length));
    }
    return btoa(result);
}

async function decryptData(data, password) {
    try {
        const decoded = atob(data);
        let result = '';
        for (let i = 0; i < decoded.length; i++) {
            result += String.fromCharCode(decoded.charCodeAt(i) ^ password.charCodeAt(i % password.length));
        }
        return decodeURIComponent(escape(atob(result)));
    } catch {
        throw new Error('Decryption failed');
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// OTA Updates
// ─────────────────────────────────────────────────────────────────────────────
async function checkForUpdates() {
    showToast('Güncelleme kontrol ediliyor...', 'success');
    
    // Simulate OTA check
    const progressDiv = document.querySelector('.ota-progress');
    if (progressDiv) {
        progressDiv.classList.add('active');
        
        // Simulate progress
        const fill = progressDiv.querySelector('.ota-progress-fill');
        const text = progressDiv.querySelector('.ota-progress-text');
        
        for (let i = 0; i <= 100; i += 10) {
            await sleep(200);
            fill.style.width = `${i}%`;
            text.textContent = `Kontrol ediliyor... ${i}%`;
        }
        
        await sleep(500);
        text.textContent = 'Güncelleme mevcut değil, sistem güncel.';
        fill.style.width = '100%';
        fill.style.background = 'var(--accent-success)';
        
        addAuditLog('ota', 'Güncelleme kontrolü yapıldı');
        
        setTimeout(() => {
            progressDiv.classList.remove('active');
            fill.style.width = '0%';
            fill.style.background = '';
        }, 3000);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Audit & Device Logs
// ─────────────────────────────────────────────────────────────────────────────
function initDemoLogs() {
    // Demo audit logs
    state.auditLogs = [
        { time: new Date(Date.now() - 3600000), type: 'login', text: 'Başarılı giriş', detail: 'IP: 192.168.1.100' },
        { time: new Date(Date.now() - 7200000), type: 'settings', text: 'Zamanlayıcı süresi değiştirildi', detail: '24 saat → 12 saat' },
        { time: new Date(Date.now() - 86400000), type: 'backup', text: 'Ayarlar dışa aktarıldı', detail: '' },
    ];
    
    // Demo device logs
    state.deviceLogs = [
        { time: new Date(Date.now() - 1800000), type: 'info', text: 'WiFi bağlantısı kuruldu', detail: 'RSSI: -45 dBm' },
        { time: new Date(Date.now() - 3600000), type: 'warning', text: 'Düşük bellek uyarısı', detail: 'Free heap: 45KB' },
        { time: new Date(Date.now() - 7200000), type: 'success', text: 'NTP senkronizasyonu başarılı', detail: 'pool.ntp.org' },
        { time: new Date(Date.now() - 86400000), type: 'error', text: 'E-posta gönderimi başarısız', detail: 'SMTP timeout' },
    ];
    
    renderAuditLogs();
    renderDeviceLogs();
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
