/**
 * LebensSpur - Theme, Auto Logout & PWA
 */

// ─────────────────────────────────────────────────────────────────────────────
// Theme Management
// ─────────────────────────────────────────────────────────────────────────────
function initTheme() {
    const savedTheme = localStorage.getItem('theme') || 'dark';
    setTheme(savedTheme, false);

    // Sistem tema degisikliklerini dinle (auto mod icin)
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

    const brandLogo = document.getElementById('brandLogo');
    if (brandLogo) {
        brandLogo.src = actualTheme === 'dark' ? 'logo.png' : 'darklogo.png';
    }

    const metaThemeColor = document.querySelector('meta[name="theme-color"]');
    if (metaThemeColor) {
        metaThemeColor.setAttribute('content', actualTheme === 'dark' ? '#0a0a0a' : '#ffffff');
    }
}

function setTheme(theme, save = true) {
    state.theme = theme;

    const actualTheme = theme === 'auto' ? getSystemTheme() : theme;
    applyTheme(actualTheme);

    if (save) {
        localStorage.setItem('theme', theme);
        const themeNames = { 'dark': 'Koyu', 'light': 'Açık', 'auto': 'Otomatik' };
        addAuditLog('settings', `Tema değiştirildi: ${themeNames[theme] || theme}`);
    }

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

        const elapsed = (Date.now() - state.lastActivity) / 60000;

        if (elapsed >= state.autoLogoutTime) {
            handleAutoLogout();
        }
    }, 10000);
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
    showToast(t('toast.autoLogoutMsg'), 'warning');
    addAuditLog('auto_logout', `${state.autoLogoutTime} dakika işlem yapılmadı`);
    handleLogout();
}

// ─────────────────────────────────────────────────────────────────────────────
// PWA Installation
// ─────────────────────────────────────────────────────────────────────────────
function initPWA() {
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
