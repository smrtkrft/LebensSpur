/**
 * LebensSpur - UI Components & Navigation
 */

// ─────────────────────────────────────────────────────────────────────────────
// DOM Cache
// ─────────────────────────────────────────────────────────────────────────────
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

// ─────────────────────────────────────────────────────────────────────────────
// Page Switching
// ─────────────────────────────────────────────────────────────────────────────
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
// Toast Notifications
// ─────────────────────────────────────────────────────────────────────────────
function showToast(message, type = 'success') {
    const container = document.getElementById('toastContainer');
    if (!container) return;

    const icons = {
        success: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
            <path d="M20 6L9 17l-5-5"/>
        </svg>`,
        info: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
            <circle cx="12" cy="12" r="10"/>
            <line x1="12" y1="16" x2="12" y2="12"/>
            <line x1="12" y1="8" x2="12.01" y2="8"/>
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

    // 3 saniye sonra kaldir
    setTimeout(() => {
        toast.style.opacity = '0';
        toast.style.transform = 'translateX(100%)';
        setTimeout(() => toast.remove(), 300);
    }, 3000);
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
    // Carousel'i baslat
    initCarousel();
    // Tum alt gorunumleri kapat
    closeAllSubViews();
    // Reload timer config from ESP32 to show current values
    if (state.isAuthenticated) {
        loadTimerConfig();
    }
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

    // Title guncelle
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

    // Carousel'i ve basligi gizle/goster
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

    // Info panel sub views
    const infoMainContent = document.getElementById('infoMainContent');
    if (infoMainContent) infoMainContent.classList.remove('hidden');
    document.querySelectorAll('#panel-info .subpage').forEach(sp => {
        sp.classList.add('hidden');
    });
}

function handleModalBack() {
    if (state.navStack.length <= 1) return;

    const currentView = state.navStack[state.navStack.length - 1];

    // Mevcut gorunumu gizle
    hideCurrentView(currentView.id);

    // Stack'ten cikar ve guncelle
    popView();

    // Onceki gorunumu goster
    const prevView = state.navStack[state.navStack.length - 1];
    showPreviousView(prevView.id);
}

function hideCurrentView(viewId) {
    // Aksiyon alt sayfalari
    const views = {
        'telegram': 'telegramConfigView',
        'early-mail': 'earlyMailConfigView',
        'early-api': 'apiConfigView',
        'trigger-mail': 'triggerMailConfigView',
        'trigger-api': 'apiConfigView',
        'relay': 'relayConfigView',
        'mail-group-detail': 'mailGroupDetailView'
    };

    // Aksiyon gorunumu mu?
    if (views[viewId]) {
        const viewElement = document.getElementById(views[viewId]);
        if (viewElement) viewElement.classList.add('hidden');
        return;
    }

    // Sistem alt sayfasi mi? (subpage ID direkt olarak geliyor)
    const subpage = document.getElementById(viewId);
    if (subpage && subpage.classList.contains('subpage')) {
        subpage.classList.add('hidden');
    }
}

function showPreviousView(viewId) {
    if (viewId === 'root') {
        // Ana icerigi goster - hangi sekmede oldugmuza gore
        document.getElementById('actionsMainContent')?.classList.remove('hidden');
        document.getElementById('systemMainContent')?.classList.remove('hidden');
        document.getElementById('infoMainContent')?.classList.remove('hidden');
    } else if (viewId === 'trigger-mail') {
        // Mail gruplari listesini goster
        document.getElementById('triggerMailConfigView')?.classList.remove('hidden');
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Settings Carousel Navigation
// ─────────────────────────────────────────────────────────────────────────────
function initCarousel() {
    state.carouselIndex = 0;
    state.isCarouselAnimating = false;

    // Ilk sekmeye active class ekle
    if (elements.navItems.length > 0) {
        elements.navItems.forEach(item => item.classList.remove('active'));
        elements.navItems[0].classList.add('active');
    }

    // Ilk paneli aktif yap, digerlerini kapat
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

    // Sonsuz dongu
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

    // Animasyon baslat
    state.isCarouselAnimating = true;

    // Tum nav itemlari guncelle - sadece active class
    elements.navItems.forEach((item, index) => {
        item.classList.remove('active');
        if (index === currentIndex) {
            item.classList.add('active');
        }
    });

    // Transform ile kaydir
    updateCarouselPosition(false);

    // Panel degistir
    const targetTab = state.carouselTabs[currentIndex];
    elements.settingsPanels.forEach(panel => {
        panel.classList.remove('active');
        if (panel.id === `panel-${targetTab}`) {
            panel.classList.add('active');
        }
    });

    // Animasyon bitisi
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
// UI Updates
// ─────────────────────────────────────────────────────────────────────────────
function updateUI() {
    updateTimerDisplay();
}

function updateInfoCards() {
    // Removed - data comes from polling
}

function updateStatusPill() {
    // Removed - no longer used
}
