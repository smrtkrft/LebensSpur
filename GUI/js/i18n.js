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
    
    // Desteklenen diller (sadece en ve tr)
    const availableLanguages = [
        { code: 'en', name: 'English', native: 'English' },
        { code: 'tr', name: 'Turkish', native: 'Türkçe' }
    ];

    // ─────────────────────────────────────────────────────────────────────────
    // Private Methods
    // ─────────────────────────────────────────────────────────────────────────
    
    /**
     * Dil dosyasını yükle
     */
    async function loadLanguage(lang) {
        if (loadedLanguages.has(lang)) {
            return translations[lang];
        }
        
        try {
            const response = await fetch(`${lang}.json`);
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
            
            // Fallback dili dene
            if (lang !== fallbackLang) {
                return loadLanguage(fallbackLang);
            }
            
            return null;
        }
    }
    
    /**
     * Nested key ile değer al (örn: "settings.timer.duration")
     */
    function getNestedValue(obj, path) {
        return path.split('.').reduce((current, key) => {
            return current && current[key] !== undefined ? current[key] : undefined;
        }, obj);
    }
    
    /**
     * String içindeki {placeholder}'ları değiştir
     */
    function interpolate(str, params) {
        if (!params || typeof str !== 'string') return str;
        
        return str.replace(/\{(\w+)\}/g, (match, key) => {
            return params[key] !== undefined ? params[key] : match;
        });
    }
    
    /**
     * DOM'daki data-i18n attribute'larını güncelle
     */
    function updateDOM() {
        // data-i18n attribute'u olan elementleri bul
        document.querySelectorAll('[data-i18n]').forEach(element => {
            const key = element.getAttribute('data-i18n');
            const translation = t(key);
            
            if (translation) {
                // Input placeholder için
                if (element.tagName === 'INPUT' && element.placeholder) {
                    element.placeholder = translation;
                } 
                // Title attribute için
                else if (element.hasAttribute('data-i18n-title')) {
                    element.title = translation;
                }
                // Normal text content için
                else {
                    element.textContent = translation;
                }
            }
        });
        
        // data-i18n-placeholder için
        document.querySelectorAll('[data-i18n-placeholder]').forEach(element => {
            const key = element.getAttribute('data-i18n-placeholder');
            const translation = t(key);
            if (translation) {
                element.placeholder = translation;
            }
        });
        
        // data-i18n-title için
        document.querySelectorAll('[data-i18n-title]').forEach(element => {
            const key = element.getAttribute('data-i18n-title');
            const translation = t(key);
            if (translation) {
                element.title = translation;
            }
        });
        
        // HTML lang attribute güncelle
        document.documentElement.lang = currentLang;
        
        // RTL dil desteği
        const langData = translations[currentLang];
        if (langData?.meta?.rtl) {
            document.documentElement.dir = 'rtl';
        } else {
            document.documentElement.dir = 'ltr';
        }
        
        // Event tetikle
        window.dispatchEvent(new CustomEvent('languageChanged', { 
            detail: { language: currentLang } 
        }));
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Public API
    // ─────────────────────────────────────────────────────────────────────────
    
    /**
     * Çeviri al
     * @param {string} key - Nokta ile ayrılmış key (örn: "auth.login")
     * @param {object} params - Interpolasyon parametreleri
     * @returns {string} Çevrilmiş metin
     */
    function t(key, params = {}) {
        // Önce mevcut dilde ara
        let value = getNestedValue(translations[currentLang], key);
        
        // Bulunamazsa fallback'te ara
        if (value === undefined && currentLang !== fallbackLang) {
            value = getNestedValue(translations[fallbackLang], key);
        }
        
        // Hala bulunamazsa key'i döndür
        if (value === undefined) {
            console.warn(`[i18n] Missing translation: ${key}`);
            return key;
        }
        
        return interpolate(value, params);
    }
    
    /**
     * Dil değiştir
     */
    async function setLanguage(lang) {
        if (!availableLanguages.find(l => l.code === lang)) {
            console.warn(`[i18n] Unsupported language: ${lang}`);
            return false;
        }
        
        await loadLanguage(lang);
        
        // Fallback'i de yükle
        if (lang !== fallbackLang && !loadedLanguages.has(fallbackLang)) {
            await loadLanguage(fallbackLang);
        }
        
        currentLang = lang;
        localStorage.setItem('ls_language', lang);
        
        updateDOM();
        
        return true;
    }
    
    /**
     * Mevcut dili al
     */
    function getLanguage() {
        return currentLang;
    }
    
    /**
     * Desteklenen dilleri al
     */
    function getAvailableLanguages() {
        return [...availableLanguages];
    }
    
    /**
     * Dil yüklendi mi kontrol et
     */
    function isLoaded(lang) {
        return loadedLanguages.has(lang);
    }
    
    /**
     * Başlangıç - tarayıcı diline göre otomatik seç
     */
    async function init() {
        // Önce localStorage'dan kontrol et
        const savedLang = localStorage.getItem('ls_language');
        
        // Sonra tarayıcı dilini kontrol et
        const browserLang = navigator.language.split('-')[0];
        
        // Hangi dili kullanacağımızı belirle
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

// Global erişim için
window.I18n = I18n;
window.t = I18n.t;
