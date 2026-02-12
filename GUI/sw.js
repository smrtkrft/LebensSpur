/**
 * ═══════════════════════════════════════════════════════════════════════════
 * LebensSpur - Service Worker
 * 
 * PWA desteği için offline caching ve background sync
 * ═══════════════════════════════════════════════════════════════════════════
 */

const CACHE_VERSION = 'v0.1.0';
const CACHE_NAME = `lebensspur-${CACHE_VERSION}`;

// Cache'lenecek statik dosyalar
const STATIC_ASSETS = [
    '/',
    '/index.html',
    '/style.css',
    '/app.js',
    '/i18n.js',
    '/manifest.json',
    '/sw.js',
    '/logo.png',
    '/darklogo.png'
];

// API endpoint'leri (cache'lenmeyecek, sadece network)
const API_ROUTES = [
    '/api/',
    '/login',
    '/logout'
];

// ─────────────────────────────────────────────────────────────────────────────
// Install Event - Statik dosyaları cache'le
// ─────────────────────────────────────────────────────────────────────────────
self.addEventListener('install', (event) => {
    console.log('[SW] Installing service worker...');
    
    event.waitUntil(
        caches.open(CACHE_NAME)
            .then((cache) => {
                console.log('[SW] Caching static assets...');
                return cache.addAll(STATIC_ASSETS);
            })
            .then(() => {
                console.log('[SW] Static assets cached');
                return self.skipWaiting();
            })
            .catch((error) => {
                console.error('[SW] Cache failed:', error);
            })
    );
});

// ─────────────────────────────────────────────────────────────────────────────
// Activate Event - Eski cache'leri temizle
// ─────────────────────────────────────────────────────────────────────────────
self.addEventListener('activate', (event) => {
    console.log('[SW] Activating service worker...');
    
    event.waitUntil(
        caches.keys()
            .then((cacheNames) => {
                return Promise.all(
                    cacheNames
                        .filter((name) => name.startsWith('lebensspur-') && name !== CACHE_NAME)
                        .map((name) => {
                            console.log('[SW] Deleting old cache:', name);
                            return caches.delete(name);
                        })
                );
            })
            .then(() => {
                console.log('[SW] Service worker activated');
                return self.clients.claim();
            })
    );
});

// ─────────────────────────────────────────────────────────────────────────────
// Fetch Event - Cache stratejisi
// ─────────────────────────────────────────────────────────────────────────────
self.addEventListener('fetch', (event) => {
    const url = new URL(event.request.url);
    
    // API istekleri için Network Only
    if (API_ROUTES.some(route => url.pathname.startsWith(route))) {
        event.respondWith(
            fetch(event.request)
                .catch(() => {
                    // Offline ise hata döndür
                    return new Response(
                        JSON.stringify({ error: 'Offline', message: 'Network unavailable' }),
                        { status: 503, headers: { 'Content-Type': 'application/json' } }
                    );
                })
        );
        return;
    }
    
    // Statik dosyalar için Cache First, Network Fallback
    event.respondWith(
        caches.match(event.request)
            .then((cachedResponse) => {
                if (cachedResponse) {
                    // Cache'de varsa döndür, arka planda güncelle
                    event.waitUntil(updateCache(event.request));
                    return cachedResponse;
                }
                
                // Cache'de yoksa network'ten al
                return fetch(event.request)
                    .then((networkResponse) => {
                        // Başarılıysa cache'le
                        if (networkResponse.ok) {
                            const responseToCache = networkResponse.clone();
                            caches.open(CACHE_NAME)
                                .then((cache) => cache.put(event.request, responseToCache));
                        }
                        return networkResponse;
                    })
                    .catch(() => {
                        // Offline ve cache'de yok
                        return offlineFallback(event.request);
                    });
            })
    );
});

// ─────────────────────────────────────────────────────────────────────────────
// Background Sync - Timer reset için
// ─────────────────────────────────────────────────────────────────────────────
self.addEventListener('sync', (event) => {
    if (event.tag === 'timer-reset') {
        console.log('[SW] Background sync: timer-reset');
        event.waitUntil(syncTimerReset());
    }
});

async function syncTimerReset() {
    try {
        const response = await fetch('/api/timer/reset', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' }
        });
        
        if (response.ok) {
            // Kullanıcıya bildir
            self.registration.showNotification('LebensSpur', {
                body: 'Timer reset successful',
                icon: '/logo.png',
                badge: '/logo.png'
            });
        }
    } catch (error) {
        console.error('[SW] Sync failed:', error);
        throw error; // Retry
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Push Notifications
// ─────────────────────────────────────────────────────────────────────────────
self.addEventListener('push', (event) => {
    console.log('[SW] Push received');
    
    let data = { title: 'LebensSpur', body: 'Notification' };
    
    if (event.data) {
        try {
            data = event.data.json();
        } catch (e) {
            data.body = event.data.text();
        }
    }
    
    const options = {
        body: data.body,
        icon: '/logo.png',
        badge: '/logo.png',
        vibrate: [200, 100, 200],
        tag: data.tag || 'default',
        requireInteraction: data.critical || false,
        actions: data.actions || [
            { action: 'reset', title: 'Reset Timer' },
            { action: 'dismiss', title: 'Dismiss' }
        ],
        data: data
    };
    
    event.waitUntil(
        self.registration.showNotification(data.title, options)
    );
});

self.addEventListener('notificationclick', (event) => {
    console.log('[SW] Notification clicked:', event.action);
    
    event.notification.close();
    
    if (event.action === 'reset') {
        // Timer'ı sıfırla
        event.waitUntil(
            fetch('/api/timer/reset', { method: 'POST' })
                .then(() => clients.openWindow('/'))
        );
    } else if (event.action === 'dismiss') {
        // Sadece kapat
        return;
    } else {
        // Uygulamayı aç
        event.waitUntil(
            clients.matchAll({ type: 'window' })
                .then((clientList) => {
                    for (const client of clientList) {
                        if (client.url === '/' && 'focus' in client) {
                            return client.focus();
                        }
                    }
                    if (clients.openWindow) {
                        return clients.openWindow('/');
                    }
                })
        );
    }
});

// ─────────────────────────────────────────────────────────────────────────────
// Helper Functions
// ─────────────────────────────────────────────────────────────────────────────

async function updateCache(request) {
    try {
        const response = await fetch(request);
        if (response.ok) {
            const cache = await caches.open(CACHE_NAME);
            await cache.put(request, response);
        }
    } catch (error) {
        // Network hatası, sessizce geç
    }
}

function offlineFallback(request) {
    // HTML istekleri için offline sayfası
    const accept = request.headers.get('Accept') || '';
    if (accept.includes('text/html')) {
        return caches.match('/index.html');
    }
    
    // Diğer istekler için boş response
    return new Response('Offline', { status: 503 });
}

// ─────────────────────────────────────────────────────────────────────────────
// Message Handler - Ana uygulamadan komut al
// ─────────────────────────────────────────────────────────────────────────────
self.addEventListener('message', (event) => {
    console.log('[SW] Message received:', event.data);
    
    if (event.data.type === 'SKIP_WAITING') {
        self.skipWaiting();
    }
    
    if (event.data.type === 'CLEAR_CACHE') {
        caches.delete(CACHE_NAME).then(() => {
            event.ports[0].postMessage({ success: true });
        });
    }
    
    if (event.data.type === 'GET_VERSION') {
        event.ports[0].postMessage({ version: CACHE_VERSION });
    }
});
