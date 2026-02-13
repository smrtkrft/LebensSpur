/**
 * LebensSpur - OTA & GUI Updates
 */

// ─────────────────────────────────────────────────────────────────────────────
// Firmware OTA Check
// ─────────────────────────────────────────────────────────────────────────────
async function checkForUpdates() {
    showToast('Güncelleme kontrol ediliyor...', 'success');

    const now = new Date();
    const lastCheckEl = document.getElementById('lastOtaCheck');
    if (lastCheckEl) lastCheckEl.textContent = now.toLocaleString();

    const progressDiv = document.querySelector('.ota-progress');
    if (progressDiv) {
        progressDiv.classList.add('active');
        const fill = progressDiv.querySelector('.ota-progress-fill');
        const text = progressDiv.querySelector('.ota-progress-text');

        if (fill) fill.style.width = '50%';
        if (text) text.textContent = 'Sunucu kontrol ediliyor...';

        try {
            const res = await authFetch('/api/ota/check');
            const data = await res.json();

            if (fill) fill.style.width = '100%';

            if (data.updateAvailable) {
                if (text) text.textContent = `Yeni sürüm mevcut: ${data.version}`;
                if (fill) fill.style.background = 'var(--accent-warning)';
            } else {
                if (text) text.textContent = 'Güncelleme mevcut değil, sistem güncel.';
                if (fill) fill.style.background = 'var(--accent-success)';
            }

            if (data.currentVersion) {
                const fwEl = document.getElementById('currentFirmwareVersion');
                if (fwEl && fwEl.textContent === '-') fwEl.textContent = 'v' + data.currentVersion;
            }

            addAuditLog('ota', 'Güncelleme kontrolü yapıldı');
        } catch (err) {
            if (fill) fill.style.width = '100%';
            if (fill) fill.style.background = 'var(--accent-error)';
            if (text) text.textContent = 'Güncelleme kontrolü başarısız';
        }

        setTimeout(() => {
            progressDiv.classList.remove('active');
            if (fill) { fill.style.width = '0%'; fill.style.background = ''; }
        }, 3000);
    } else {
        try {
            const res = await authFetch('/api/ota/check');
            const data = await res.json();
            if (data.updateAvailable) {
                showToast(`Yeni sürüm mevcut: ${data.version}`, 'warning');
            } else {
                showToast('Sistem güncel', 'success');
            }
        } catch (err) {
            showToast('Güncelleme kontrolü başarısız', 'error');
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// GUI OTA Update
// ─────────────────────────────────────────────────────────────────────────────
function handleGuiUpdate() {
    const btn = document.getElementById('updateGuiBtn');
    if (btn) { btn.disabled = true; btn.textContent = 'Güncelleniyor...'; }

    showToast('Web arayüzü güncelleniyor...', 'success');

    authFetch('/api/gui/download', { method: 'POST' })
        .then(res => res.json())
        .then(data => {
            if (data.started || data.success) {
                const pollInterval = setInterval(() => {
                    authFetch('/api/gui/download/status')
                        .then(r => r.json())
                        .then(st => {
                            if (st.status === 'done' || st.status === 'idle') {
                                clearInterval(pollInterval);
                                if (btn) { btn.disabled = false; btn.textContent = 'Web Arayüzünü Güncelle'; }
                                if (st.downloaded > 0) {
                                    showToast(`GUI güncellendi! ${st.downloaded} dosya indirildi.`, 'success');
                                } else {
                                    showToast('GUI güncelleme tamamlandı', 'success');
                                }
                            } else if (st.status === 'error') {
                                clearInterval(pollInterval);
                                if (btn) { btn.disabled = false; btn.textContent = 'Web Arayüzünü Güncelle'; }
                                showToast('GUI güncelleme hatası: ' + (st.error || 'Bilinmeyen hata'), 'error');
                            }
                        })
                        .catch(() => {});
                }, 2000);
            } else {
                if (btn) { btn.disabled = false; btn.textContent = 'Web Arayüzünü Güncelle'; }
                showToast(data.error || 'GUI güncelleme başlatılamadı', 'error');
            }
        })
        .catch(() => {
            if (btn) { btn.disabled = false; btn.textContent = 'Web Arayüzünü Güncelle'; }
            showToast('Bağlantı hatası', 'error');
        });
}
