/**
 * LebensSpur - Audit & Device Logs
 */

// ─────────────────────────────────────────────────────────────────────────────
// Init & Fetch
// ─────────────────────────────────────────────────────────────────────────────
function initLogs() {
    state.auditLogs = [];
    state.deviceLogs = [];
    fetchLogs();
}

function fetchLogs() {
    authFetch('/api/logs')
        .then(res => res.json())
        .then(data => {
            if (data.entries && Array.isArray(data.entries)) {
                state.auditLogs = [];
                state.deviceLogs = [];
                data.entries.forEach(entry => {
                    const log = {
                        time: new Date(entry.timestamp * 1000),
                        type: entry.category || 'info',
                        text: entry.message || '',
                        detail: ''
                    };
                    const auditTypes = ['auth', 'config', 'login', 'settings', 'backup', 'timer'];
                    if (auditTypes.includes(log.type)) {
                        state.auditLogs.push(log);
                    } else {
                        state.deviceLogs.push(log);
                    }
                });
                renderAuditLogs();
                renderDeviceLogs();
            }
        })
        .catch(err => console.error('Log fetch failed:', err));
}

// ─────────────────────────────────────────────────────────────────────────────
// Add Logs
// ─────────────────────────────────────────────────────────────────────────────
function addAuditLog(type, text, detail = '') {
    const log = {
        time: new Date(),
        type: type,
        text: text,
        detail: detail
    };

    state.auditLogs.unshift(log);

    if (state.auditLogs.length > 100) {
        state.auditLogs = state.auditLogs.slice(0, 100);
    }

    renderAuditLogs();
}

function addDeviceLog(type, text, detail = '') {
    const log = {
        time: new Date(),
        type: type,
        text: text,
        detail: detail
    };

    state.deviceLogs.unshift(log);

    if (state.deviceLogs.length > 100) {
        state.deviceLogs = state.deviceLogs.slice(0, 100);
    }

    renderDeviceLogs();
}

// ─────────────────────────────────────────────────────────────────────────────
// Render Logs
// ─────────────────────────────────────────────────────────────────────────────
function renderAuditLogs(filter = 'all') {
    const list = document.getElementById('auditLogList');
    if (!list) return;

    const logs = filter === 'all'
        ? state.auditLogs
        : state.auditLogs.filter(log => log.type === filter);

    list.innerHTML = logs.map(log => `
        <div class="log-item log-info">
            <span class="log-time">${formatDate(log.time)}</span>
            <div class="log-content">
                <span class="log-text">${log.text}</span>
            </div>
            ${log.detail ? `<span class="log-detail">${log.detail}</span>` : ''}
        </div>
    `).join('');
}

function renderDeviceLogs(filter = 'all') {
    const list = document.getElementById('deviceLogList');
    if (!list) return;

    const logs = filter === 'all'
        ? state.deviceLogs
        : state.deviceLogs.filter(log => log.type === filter);

    list.innerHTML = logs.map(log => `
        <div class="log-item log-${log.type}">
            <span class="log-time">${formatDate(log.time)}</span>
            <div class="log-content">
                <span class="log-text">${log.text}</span>
            </div>
            ${log.detail ? `<span class="log-detail">${log.detail}</span>` : ''}
        </div>
    `).join('');
}

// ─────────────────────────────────────────────────────────────────────────────
// Filter / Export / Clear
// ─────────────────────────────────────────────────────────────────────────────
function filterLogs(filter, section) {
    const isAudit = section?.querySelector('#auditLogList');

    if (isAudit) {
        renderAuditLogs(filter);
    } else {
        renderDeviceLogs(filter);
    }
}

function exportLogs(type) {
    const logs = type === 'audit' ? state.auditLogs : state.deviceLogs;
    const content = logs.map(log =>
        `${formatDate(log.time)} | ${log.type} | ${log.text}${log.detail ? ' | ' + log.detail : ''}`
    ).join('\n');

    const blob = new Blob([content], { type: 'text/plain' });
    const url = URL.createObjectURL(blob);

    const a = document.createElement('a');
    a.href = url;
    a.download = `lebensspur-${type}-logs-${formatDateForFile(new Date())}.txt`;
    a.click();

    URL.revokeObjectURL(url);
    showToast(`${type === 'audit' ? 'İşlem' : 'Cihaz'} logları indirildi`, 'success');
}

function clearLogs(type) {
    if (!confirm(`${type === 'audit' ? 'İşlem' : 'Cihaz'} logları silinecek. Devam edilsin mi?`)) return;
    authFetch('/api/logs', { method: 'DELETE' })
        .then(res => res.json())
        .then(data => {
            if (data.success) {
                state.auditLogs = [];
                state.deviceLogs = [];
                renderAuditLogs();
                renderDeviceLogs();
                showToast(t('toast.logsCleared'), 'success');
            } else showToast(data.error || 'Hata', 'error');
        })
        .catch(() => showToast('Bağlantı hatası', 'error'));
}

// ─────────────────────────────────────────────────────────────────────────────
// Test Service
// ─────────────────────────────────────────────────────────────────────────────
function sendTest(service) {
    showToast(`${service.toUpperCase()} test gönderiliyor...`, 'info');
    authFetch(`/api/test/${service}`, { method: 'POST' })
        .then(res => res.json())
        .then(data => {
            if (data.success) showToast(`${service.toUpperCase()} test başarılı`, 'success');
            else showToast(data.error || `${service.toUpperCase()} test başarısız`, 'error');
        })
        .catch(err => {
            if (err.message !== 'SESSION_EXPIRED') {
                showToast('Bağlantı hatası', 'error');
            }
        });
}
