/**
 * ═══════════════════════════════════════════════════════════════════════════
 * LebensSpur - Internationalization (i18n) Module
 * 
 * JSON tabanlı çoklu dil desteği
 * Yeni dil eklemek için: i18n/{lang}.json dosyası oluşturun
 * ═══════════════════════════════════════════════════════════════════════════
 */

const I18n = (function() {
    // ─────────────────────────────────────────────────────────────────────────
    // Private State
    // ─────────────────────────────────────────────────────────────────────────
    let currentLang = 'en';
    let translations = {};
    let fallbackLang = 'en';
    let loadedLanguages = new Set();
    
    // Desteklenen diller
    const availableLanguages = [
        { code: 'en', name: 'English', native: 'English' },
        { code: 'tr', name: 'Turkish', native: 'Türkçe' }
    ];

    // ─────────────────────────────────────────────────────────────────────────
    // DOM Selector Map — CSS selector → i18n key
    // mode: 'text' = textContent, 'last' = last text node (for mixed SVG+text),
    //        'ph' = placeholder, 'title' = title attribute
    // ─────────────────────────────────────────────────────────────────────────
    const _domMap = [
        // ── LOGIN ──
        { s: '#login-form label[for="password"]', k: 'auth.password' },
        { s: '#login-form button[type="submit"]', k: 'auth.login' },

        // ── MAIN TIMER (tooltips) ──
        { s: '#settingsBtn', k: 'settings.title', m: 'title' },
        { s: '#resetBtn', k: 'timer.reset', m: 'title' },
        { s: '#logoutBtn', k: 'auth.login', m: 'title' },

        // ── SETTINGS NAVIGATION ──
        { s: '.nav-item[data-tab="timer"]', k: 'settings.timer' },
        { s: '.nav-item[data-tab="mail"]', k: 'settings.connections' },
        { s: '.nav-item[data-tab="actions"]', k: 'settings.actions' },
        { s: '.nav-item[data-tab="system"]', k: 'settings.system' },

        // ── MODAL FOOTER ──
        { s: '#cancelSettings', k: 'common.cancel' },
        { s: '#saveSettings', k: 'common.save' },

        // ── TIMER SETTINGS (#panel-timer) ──
        // -- Countdown duration section
        { s: '#panel-timer .setting-section:nth-child(1) .section-title', k: 'settingsTimer.interval' },
        { s: '#panel-timer .setting-section:nth-child(1) .section-desc', k: 'settingsTimer.intervalDesc' },
        { s: '.unit-btn[data-unit="days"]', k: 'timer.days' },
        { s: '.unit-btn[data-unit="hours"]', k: 'timer.hours' },
        { s: '.unit-btn[data-unit="minutes"]', k: 'timer.minutes' },
        // -- Alarm section
        { s: '#panel-timer .setting-section:nth-child(2) .section-title', k: 'settingsTimer.alarmCount' },
        { s: '#panel-timer .setting-section:nth-child(2) .section-desc', k: 'settingsTimer.alarmCountDesc' },
        // -- Vacation section
        { s: '#panel-timer .setting-section:nth-child(3) .section-title', k: 'timer.vacation' },
        { s: '#panel-timer .setting-section:nth-child(3) > .section-desc', k: 'settingsTimer.activeHoursDesc' },

        // ── CONNECTIONS (#panel-mail) ──
        { s: '#smtpAccordionHeader .section-title', k: 'settingsConn.smtp', m: 'last' },
        { s: '#smtpAccordionContent > .section-desc', k: 'settingsConn.smtpServer' },
        { s: '#saveSmtpBtn', k: 'common.save' },
        { s: '#saveWifiBtn', k: 'common.save' },
        { s: '#saveWifiBackupBtn', k: 'common.save' },

        // ── SYSTEM (#panel-system) ──
        { s: '#panel-system #systemMainContent .setting-section:nth-child(1) .section-title', k: 'settingsSys.interface' },
        { s: 'label[for="themeSelect"], .setting-inline-label:has(+ #themeSelect)', k: 'settingsSys.theme' },
        { s: 'label[for="languageSelect"], .setting-inline-label:has(+ #languageSelect)', k: 'settingsSys.language' },
        { s: '#themeSelect option[value="dark"]', k: 'settingsSys.themeDark' },
        { s: '#themeSelect option[value="light"]', k: 'settingsSys.themeLight' },
        { s: '#themeSelect option[value="auto"]', k: 'settingsSys.themeAuto' },

        { s: '#securityToggleBtn', k: 'settingsSys.securityBtn', m: 'last' },
        { s: '#pwaInstallBtn', k: 'settingsSys.installBtn', m: 'last' },

        { s: '#otaFirmwareBtn', k: 'settingsSys.hwOta', m: 'last' },
        { s: '#otaWebBtn', k: 'settingsSys.guiOta', m: 'last' },

        { s: '#importSettingsBtn', k: 'settingsSys.import', m: 'last' },
        { s: '#exportSettingsBtn', k: 'settingsSys.export', m: 'last' },

        { s: '#rebootBtn', k: 'settingsSys.reboot', m: 'last' },
        { s: '#factoryResetBtn', k: 'settingsSys.factoryReset', m: 'last' },

        // ── SECURITY SUBPAGE ──
        { s: '#securitySubpage [data-title]', k: 'security.title', m: 'attr', a: 'data-title' },
        { s: '#securitySubpage > .setting-section:nth-child(1) .section-desc', k: 'security.desc' },
        { s: '#securitySubpage > .setting-section:nth-child(2) .section-title', k: 'security.changePassword' },
        { s: '#changePasswordBtn', k: 'security.changeBtn', m: 'last' },

        // ── HW OTA SUBPAGE ──
        { s: '#hwOtaSubpage [data-title]', k: 'hwOta.title', m: 'attr', a: 'data-title' },
        { s: '#hwOtaSubpage > .setting-section:nth-child(1) .section-desc', k: 'hwOta.desc' },
        { s: '#checkFirmwareUpdateBtn', k: 'hwOta.checkBtn', m: 'last' },

        // ── GUI OTA SUBPAGE ──
        { s: '#guiOtaSubpage [data-title]', k: 'guiOta.title', m: 'attr', a: 'data-title' },
        { s: '#guiOtaSubpage > .setting-section:nth-child(1) .section-desc', k: 'guiOta.desc' },
        { s: '#updateGuiBtn', k: 'guiOta.updateBtn', m: 'last' },

        // ── IMPORT SUBPAGE ──
        { s: '#importSubpage [data-title]', k: 'importExport.importTitle', m: 'attr', a: 'data-title' },
        { s: '#importSubpage > .setting-section:nth-child(1) .section-desc', k: 'importExport.importDesc' },
        { s: '#startImportBtn', k: 'importExport.startImport', m: 'last' },

        // ── EXPORT SUBPAGE ──
        { s: '#exportSubpage [data-title]', k: 'exportExport.exportTitle', m: 'attr', a: 'data-title' },
        { s: '#exportSubpage > .setting-section:nth-child(1) .section-desc', k: 'importExport.exportDesc' },
        { s: '#startExportBtn', k: 'importExport.startExport', m: 'last' },

        // ── INFO TAB ──
        { s: '.info-card[data-subpage="deviceInfoSubpage"] .info-card-title', k: 'info.title' },
        { s: '.info-card[data-subpage="logsSubpage"] .info-card-title', k: 'info.auditLogs' },

        // ── DEVICE INFO SUBPAGE ──
        { s: '#deviceInfoSubpage [data-title]', k: 'info.title', m: 'attr', a: 'data-title' },
    ];

    // ─────────────────────────────────────────────────────────────────────────
    // Private Methods
    // ─────────────────────────────────────────────────────────────────────────
    
    async function loadLanguage(lang) {
        if (loadedLanguages.has(lang)) {
            return translations[lang];
        }
        
        try {
            const response = await fetch(`i18n/${lang}.json`);
            if (!response.ok) {
                throw new Error(`Language file not found: ${lang}`);
            }
            
            const data = await response.json();
            translations[lang] = data;
            loadedLanguages.add(lang);
            
            console.log(`[i18n] Loaded language: ${lang}`);
            return data;
        } catch (error) {
            console.warn(`[i18n] Failed to load ${lang}:`, error.message);
            
            if (lang !== fallbackLang) {
                return loadLanguage(fallbackLang);
            }
            
            return null;
        }
    }
    
    function getNestedValue(obj, path) {
        return path.split('.').reduce((current, key) => {
            return current && current[key] !== undefined ? current[key] : undefined;
        }, obj);
    }
    
    function interpolate(str, params) {
        if (!params || typeof str !== 'string') return str;
        
        return str.replace(/\{(\w+)\}/g, (match, key) => {
            return params[key] !== undefined ? params[key] : match;
        });
    }
    
    /**
     * Set text on an element, handling mixed content (SVG + text)
     */
    function _setText(el, text, mode, attrName) {
        if (!el || !text || text === el.textContent) return;
        
        if (mode === 'title') {
            el.title = text;
            return;
        }
        if (mode === 'ph') {
            el.placeholder = text;
            return;
        }
        if (mode === 'attr' && attrName) {
            el.setAttribute(attrName, text);
            return;
        }
        
        // 'last' mode: only set the last text node (for elements with SVG children)
        if (mode === 'last') {
            const nodes = el.childNodes;
            for (let i = nodes.length - 1; i >= 0; i--) {
                if (nodes[i].nodeType === Node.TEXT_NODE && nodes[i].textContent.trim()) {
                    nodes[i].textContent = '\n                                    ' + text + '\n                                ';
                    return;
                }
            }
            // No text node found, append
            el.appendChild(document.createTextNode(' ' + text));
            return;
        }
        
        // Default: set textContent (only if no child elements, or element is simple)
        if (el.children.length === 0) {
            el.textContent = text;
        } else {
            // Has child elements — find text nodes
            const nodes = el.childNodes;
            let found = false;
            for (let i = nodes.length - 1; i >= 0; i--) {
                if (nodes[i].nodeType === Node.TEXT_NODE && nodes[i].textContent.trim()) {
                    nodes[i].textContent = text;
                    found = true;
                    break;
                }
            }
            if (!found) {
                // Try first text node
                for (let i = 0; i < nodes.length; i++) {
                    if (nodes[i].nodeType === Node.TEXT_NODE) {
                        nodes[i].textContent = text;
                        break;
                    }
                }
            }
        }
    }
    
    /**
     * DOM güncelle — data-i18n attributes + selector mapping
     */
    function updateDOM() {
        // 1) Standard data-i18n attributes
        document.querySelectorAll('[data-i18n]').forEach(element => {
            const key = element.getAttribute('data-i18n');
            const translation = t(key);
            if (translation && translation !== key) {
                if (element.tagName === 'INPUT') {
                    element.placeholder = translation;
                } else {
                    element.textContent = translation;
                }
            }
        });
        
        document.querySelectorAll('[data-i18n-placeholder]').forEach(element => {
            const key = element.getAttribute('data-i18n-placeholder');
            const translation = t(key);
            if (translation && translation !== key) element.placeholder = translation;
        });
        
        document.querySelectorAll('[data-i18n-title]').forEach(element => {
            const key = element.getAttribute('data-i18n-title');
            const translation = t(key);
            if (translation && translation !== key) element.title = translation;
        });
        
        // 2) Selector-based mapping (no HTML changes needed)
        _domMap.forEach(entry => {
            try {
                const el = document.querySelector(entry.s);
                if (el) {
                    const text = t(entry.k);
                    if (text && text !== entry.k) {
                        _setText(el, text, entry.m, entry.a);
                    }
                }
            } catch(e) {
                // Ignore invalid selectors
            }
        });
        
        // HTML lang attribute
        document.documentElement.lang = currentLang;
        
        // RTL support
        const langData = translations[currentLang];
        if (langData?.meta?.rtl) {
            document.documentElement.dir = 'rtl';
        } else {
            document.documentElement.dir = 'ltr';
        }
        
        // Dispatch event for JS-driven dynamic content
        window.dispatchEvent(new CustomEvent('languageChanged', { 
            detail: { language: currentLang } 
        }));
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Public API
    // ─────────────────────────────────────────────────────────────────────────
    
    function t(key, params = {}) {
        let value = getNestedValue(translations[currentLang], key);
        
        if (value === undefined && currentLang !== fallbackLang) {
            value = getNestedValue(translations[fallbackLang], key);
        }
        
        if (value === undefined) {
            return key;
        }
        
        return interpolate(value, params);
    }
    
    async function setLanguage(lang) {
        if (!availableLanguages.find(l => l.code === lang)) {
            console.warn(`[i18n] Unsupported language: ${lang}`);
            return false;
        }
        
        await loadLanguage(lang);
        
        if (lang !== fallbackLang && !loadedLanguages.has(fallbackLang)) {
            await loadLanguage(fallbackLang);
        }
        
        currentLang = lang;
        localStorage.setItem('ls_language', lang);
        
        updateDOM();
        
        return true;
    }
    
    function getLanguage() {
        return currentLang;
    }
    
    function getAvailableLanguages() {
        return [...availableLanguages];
    }
    
    function isLoaded(lang) {
        return loadedLanguages.has(lang);
    }
    
    async function init() {
        const savedLang = localStorage.getItem('ls_language');
        const browserLang = navigator.language.split('-')[0];
        
        let targetLang = fallbackLang;
        
        if (savedLang && availableLanguages.find(l => l.code === savedLang)) {
            targetLang = savedLang;
        } else if (availableLanguages.find(l => l.code === browserLang)) {
            targetLang = browserLang;
        }
        
        await setLanguage(targetLang);
        
        console.log(`[i18n] Initialized with language: ${currentLang}`);
    }
    
    // ─────────────────────────────────────────────────────────────────────────
    // Export
    // ─────────────────────────────────────────────────────────────────────────
    return {
        t,
        setLanguage,
        getLanguage,
        getAvailableLanguages,
        isLoaded,
        init,
        updateDOM
    };
})();

window.I18n = I18n;
window.t = I18n.t;
