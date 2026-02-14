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
// GUI Slot Status
// ─────────────────────────────────────────────────────────────────────────────
async function loadGuiSlotStatus() {
    try {
        const res = await authFetch('/api/gui/status');
        const data = await res.json();

        const setTxt = (id, val) => {
            const el = document.getElementById(id);
            if (el) el.textContent = val;
        };

        setTxt('guiActiveSlot', (data.active || 'a').toUpperCase());
        setTxt('guiActiveVersion', data.ver_active || '-');
        setTxt('guiBackupVersion', data.ver_backup || '-');
        setTxt('guiBootCount', data.boot_count != null ? data.boot_count + '/3' : '-');

        // Rollback butonu: yedek versiyon varsa aktif
        const rollbackBtn = document.getElementById('guiRollbackBtn');
        if (rollbackBtn) {
            const hasBackup = data.ver_backup && data.ver_backup !== '';
            rollbackBtn.disabled = !hasBackup;
        }

        // GUI version for status card
        if (data.ver_active) {
            setTxt('currentGuiVersion', 'v' + data.ver_active);
        }
    } catch (err) {
        // Silently fail
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// GUI Rollback
// ─────────────────────────────────────────────────────────────────────────────
async function handleGuiRollback() {
    const btn = document.getElementById('guiRollbackBtn');
    const backupVer = document.getElementById('guiBackupVersion')?.textContent || '?';

    if (!confirm(`Yedek versiyona (${backupVer}) geri dönülsün mü?\nSayfa yenilenecektir.`)) {
        return;
    }

    if (btn) { btn.disabled = true; btn.textContent = 'Geri dönülüyor...'; }

    try {
        const res = await authFetch('/api/gui/rollback', { method: 'POST' });
        const data = await res.json();

        if (data.success) {
            showToast('Rollback başarılı! Sayfa yenileniyor...', 'success');
            setTimeout(() => location.reload(), 1500);
        } else {
            showToast(data.error || 'Rollback başarısız', 'error');
            if (btn) { btn.disabled = false; btn.textContent = 'Yedek Versiyona Geri Dön'; }
        }
    } catch (err) {
        showToast('Bağlantı hatası', 'error');
        if (btn) { btn.disabled = false; btn.textContent = 'Yedek Versiyona Geri Dön'; }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// GUI OTA Update (downloads to inactive slot)
// ─────────────────────────────────────────────────────────────────────────────
function handleGuiUpdate() {
    const btn = document.getElementById('updateGuiBtn');
    if (btn) { btn.disabled = true; btn.textContent = 'Güncelleniyor...'; }

    showToast('Web arayüzü güncelleniyor (yedek slot\'a indiriliyor)...', 'success');

    authFetch('/api/gui/download', { method: 'POST' })
        .then(res => res.json())
        .then(data => {
            if (data.started || data.success) {
                const pollInterval = setInterval(() => {
                    authFetch('/api/gui/download/status')
                        .then(r => r.json())
                        .then(st => {
                            // state: 0=idle, 4=complete, 5=error
                            if (st.state === 4 || st.state === 0) {
                                clearInterval(pollInterval);
                                if (btn) { btn.disabled = false; btn.textContent = 'Web Arayüzünü Güncelle'; }
                                if (st.files > 0) {
                                    showToast(`GUI güncellendi! ${st.files} dosya indirildi. Slot değiştirildi.`, 'success');
                                } else {
                                    showToast('GUI güncelleme tamamlandı', 'success');
                                }
                                // Slot bilgisini yenile
                                loadGuiSlotStatus();
                            } else if (st.state === 5) {
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
