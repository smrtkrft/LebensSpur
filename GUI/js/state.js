/**
 * LebensSpur - State & DOM Element References
 */

// ─────────────────────────────────────────────────────────────────────────────
// State Management
// ─────────────────────────────────────────────────────────────────────────────
const state = {
    isAuthenticated: false,
    authToken: localStorage.getItem('ls_token') || null,  // Session token (localStorage'dan oku)
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
    navStack: [],

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
            telegramChat: '',
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
// Timer Interval References
// ─────────────────────────────────────────────────────────────────────────────
let timerPollInterval = null;
let timerCountdownInterval = null;
