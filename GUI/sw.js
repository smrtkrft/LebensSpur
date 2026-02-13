/**
 * ═══════════════════════════════════════════════════════════════════════════
 * LebensSpur - Service Worker v0.4.0
 *
 * Passthrough mode: no request interception.
 * Cleans up old caches and takes over from previous SW versions.
 * Push notifications and background sync remain functional.
 * ═══════════════════════════════════════════════════════════════════════════
 */

const CACHE_VERSION = 'v0.4.0';

// ─────────────────────────────────────────────────────────────────────────────
// Install - Immediately take over from any old SW
// ─────────────────────────────────────────────────────────────────────────────
self.addEventListener('install', (event) => {
    console.log('[SW] Installing v0.4.0 (passthrough mode)');
    self.skipWaiting();
});

// ─────────────────────────────────────────────────────────────────────────────
// Activate - Delete ALL old caches and claim clients
// ─────────────────────────────────────────────────────────────────────────────
self.addEventListener('activate', (event) => {
    console.log('[SW] Activating v0.4.0');
    event.waitUntil(
        caches.keys()
            .then((names) => Promise.all(
                names.filter(n => n.startsWith('lebensspur-'))
                     .map(n => { console.log('[SW] Deleting cache:', n); return caches.delete(n); })
            ))
            .then(() => self.clients.claim())
    );
});

// ─────────────────────────────────────────────────────────────────────────────
// Fetch - NO interception, let browser handle everything natively
// ─────────────────────────────────────────────────────────────────────────────
// (No fetch event listener = all requests go directly to network)

// ─────────────────────────────────────────────────────────────────────────────
// Background Sync - Timer reset
// ─────────────────────────────────────────────────────────────────────────────
self.addEventListener('sync', (event) => {
    if (event.tag === 'timer-reset') {
        event.waitUntil(
            fetch('/api/timer/reset', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                credentials: 'include'
            })
            .then(res => {
                if (res.ok) {
                    self.registration.showNotification('LebensSpur', {
                        body: 'Timer reset successful',
                        icon: '/pic/logo.png'
                    });
                }
            })
        );
    }
});

// ─────────────────────────────────────────────────────────────────────────────
// Push Notifications
// ─────────────────────────────────────────────────────────────────────────────
self.addEventListener('push', (event) => {
    let data = { title: 'LebensSpur', body: 'Notification' };
    if (event.data) {
        try { data = event.data.json(); } catch (e) { data.body = event.data.text(); }
    }

    event.waitUntil(
        self.registration.showNotification(data.title, {
            body: data.body,
            icon: '/pic/logo.png',
            badge: '/pic/logo.png',
            vibrate: [200, 100, 200],
            tag: data.tag || 'default',
            requireInteraction: data.critical || false,
            actions: data.actions || [
                { action: 'reset', title: 'Reset Timer' },
                { action: 'dismiss', title: 'Dismiss' }
            ],
            data: data
        })
    );
});

self.addEventListener('notificationclick', (event) => {
    event.notification.close();
    if (event.action === 'reset') {
        event.waitUntil(
            fetch('/api/timer/reset', { method: 'POST', credentials: 'include' })
                .then(() => clients.openWindow('/'))
        );
    } else if (event.action !== 'dismiss') {
        event.waitUntil(
            clients.matchAll({ type: 'window' }).then((list) => {
                for (const c of list) {
                    if ('focus' in c) return c.focus();
                }
                return clients.openWindow('/');
            })
        );
    }
});

// ─────────────────────────────────────────────────────────────────────────────
// Message Handler
// ─────────────────────────────────────────────────────────────────────────────
self.addEventListener('message', (event) => {
    if (event.data.type === 'SKIP_WAITING') self.skipWaiting();
    if (event.data.type === 'GET_VERSION') {
        event.ports[0].postMessage({ version: CACHE_VERSION });
    }
});
