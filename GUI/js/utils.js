/**
 * LebensSpur - Utility Functions
 */

// ─────────────────────────────────────────────────────────────────────────────
// Safe i18n helper — fallback if i18n.js not loaded yet
// ─────────────────────────────────────────────────────────────────────────────
if (typeof window.t !== 'function') {
    window.t = function(key) { return key; };
}

// ─────────────────────────────────────────────────────────────────────────────
// Authenticated Fetch Helper
// ─────────────────────────────────────────────────────────────────────────────
function authFetch(url, options = {}) {
    // Always include cookies explicitly
    options.credentials = 'include';
    // Send token via Authorization header (bypasses SW cookie issues)
    if (state.authToken) {
        if (!options.headers || options.headers instanceof Headers) {
            const h = new Headers(options.headers || {});
            h.set('Authorization', 'Bearer ' + state.authToken);
            options.headers = h;
        } else {
            options.headers = { ...options.headers, 'Authorization': 'Bearer ' + state.authToken };
        }
    }
    return fetch(url, options).then(res => {
        if (res.status === 401 && state.isAuthenticated) {
            // Session expired on server side
            state.isAuthenticated = false;
            state.authToken = null;
            localStorage.removeItem('ls_token');
            stopAutoLogoutTimer();
            if (timerPollInterval) {
                clearInterval(timerPollInterval);
                timerPollInterval = null;
            }
            if (timerCountdownInterval) {
                clearInterval(timerCountdownInterval);
                timerCountdownInterval = null;
            }
            showPage('login');
            showToast(t('toast.sessionExpired'), 'warning');
            throw new Error('SESSION_EXPIRED');
        }
        return res;
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// Formatting Helpers
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

function formatDateForFile(date) {
    return date.toISOString().slice(0, 10);
}

function sleep(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
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
