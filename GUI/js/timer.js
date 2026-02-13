/**
 * LebensSpur - Timer Logic & Configuration
 */

// ─────────────────────────────────────────────────────────────────────────────
// Timer Polling & Countdown
// ─────────────────────────────────────────────────────────────────────────────
function startTimerUpdate() {
    // Poll timer status from ESP32 every 5 seconds
    pollTimerStatus();
    if (timerPollInterval) clearInterval(timerPollInterval);
    timerPollInterval = setInterval(pollTimerStatus, 5000);

    // Local 1-second countdown for smooth display updates between polls
    if (timerCountdownInterval) clearInterval(timerCountdownInterval);
    timerCountdownInterval = setInterval(() => {
        if (!state.isAuthenticated) return;
        if (state.timerState === 'RUNNING' || state.timerState === 'WARNING') {
            if (state.timeRemainingMs > 0) {
                state.timeRemainingMs = Math.max(0, state.timeRemainingMs - 1000);
                updateTimerDisplay();
            }
        }
    }, 1000);
}

function pollTimerStatus() {
    if (!state.isAuthenticated) return;
    authFetch('/api/timer/status')
        .then(res => {
            if (!res.ok) throw new Error('Not authenticated');
            return res.json();
        })
        .then(data => {
            state.timerState = data.state || 'DISABLED';
            state.timeRemainingMs = data.timeRemainingMs || 0;
            state.intervalMinutes = data.intervalMinutes || 1440;
            state.warningsSent = data.warningsSent || 0;
            state.resetCount = data.resetCount || 0;
            state.triggerCount = data.triggerCount || 0;
            state.timerEnabled = data.enabled || false;

            // Update vacation state
            if (data.vacationEnabled) {
                state.vacationMode.enabled = true;
                state.vacationMode.days = data.vacationDays || 0;
            }

            updateTimerDisplay();
            updatePauseButton();
            updateVacationIndicator();
        })
        .catch(() => {
            // Silent fail during polling
        });
}

// ─────────────────────────────────────────────────────────────────────────────
// Timer Display
// ─────────────────────────────────────────────────────────────────────────────
function updateTimerDisplay() {
    const totalMs = state.timeRemainingMs;
    const totalSeconds = Math.floor(totalMs / 1000);
    const days = Math.floor(totalSeconds / 86400);
    const hours = Math.floor((totalSeconds % 86400) / 3600);
    const minutes = Math.floor((totalSeconds % 3600) / 60);

    // Days display
    if (elements.timeDays) {
        if (days > 0) {
            elements.timeDays.textContent = `${days}${t('timer.dayShort')}`;
            elements.timeDays.classList.remove('hidden');
        } else {
            elements.timeDays.classList.add('hidden');
        }
    }

    // Hours display
    if (elements.timeHours) {
        if (days > 0 || hours > 0) {
            elements.timeHours.textContent = `${hours}${t('timer.hourShort')}`;
            elements.timeHours.classList.remove('hidden');
        } else {
            elements.timeHours.classList.add('hidden');
        }
    }

    // Minutes display (always visible)
    if (elements.timeMinutes) {
        elements.timeMinutes.textContent = `${minutes}${t('timer.minShort')}`;
    }

    // Timer label based on state
    const timerLabel = document.querySelector('.timer-label');
    if (timerLabel) {
        const labels = {
            'DISABLED': t('timer.stateDisabled'),
            'RUNNING': t('timer.stateRunning'),
            'WARNING': t('timer.stateWarning'),
            'TRIGGERED': t('timer.stateTriggered'),
            'PAUSED': t('timer.statePaused'),
            'VACATION': t('timer.stateVacation')
        };
        timerLabel.textContent = labels[state.timerState] || t('timer.stateRunning');
    }

    // Ring progress
    const intervalMs = state.intervalMinutes * 60 * 1000;
    const percentage = intervalMs > 0 ? Math.min(1, totalMs / intervalMs) : 0;
    const circumference = 2 * Math.PI * 90;
    const offset = circumference * (1 - percentage);

    const ringProgress = document.querySelector('.ring-progress');
    if (ringProgress) {
        ringProgress.style.strokeDashoffset = offset;
    }

    // Ring color
    if (elements.timerRing) {
        elements.timerRing.classList.remove('warning', 'danger');
        if (state.timerState === 'TRIGGERED') {
            elements.timerRing.classList.add('danger');
        } else if (state.timerState === 'WARNING') {
            elements.timerRing.classList.add('warning');
        } else if (percentage <= 0.25 && state.timerState === 'RUNNING') {
            elements.timerRing.classList.add('danger');
        } else if (percentage <= 0.5 && state.timerState === 'RUNNING') {
            elements.timerRing.classList.add('warning');
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Timer Actions
// ─────────────────────────────────────────────────────────────────────────────
function handleReset() {
    if (state.timerState === 'TRIGGERED') {
        // Acknowledge triggered timer
        authFetch('/api/timer/acknowledge', { method: 'POST' })
            .then(res => res.json())
            .then(data => {
                if (data.success) {
                    showToast(t('toast.triggerAcknowledged'), 'success');
                    pollTimerStatus();
                } else {
                    showToast(data.error || 'Hata', 'error');
                }
            })
            .catch(() => showToast('Bağlantı hatası', 'error'));
    } else if (state.timerState === 'DISABLED') {
        // Enable timer
        authFetch('/api/timer/enable', { method: 'POST' })
            .then(res => res.json())
            .then(data => {
                if (data.success) {
                    showToast(t('toast.timerEnabled'), 'success');
                    pollTimerStatus();
                }
            })
            .catch(() => showToast('Bağlantı hatası', 'error'));
    } else {
        // Normal reset
        authFetch('/api/timer/reset', { method: 'POST' })
            .then(res => res.json())
            .then(data => {
                if (data.success) {
                    showToast(t('toast.timerReset'), 'success');
                    pollTimerStatus();
                } else {
                    showToast(data.error || 'Sıfırlama başarısız', 'error');
                }
            })
            .catch(() => showToast('Bağlantı hatası', 'error'));
    }
}

function handlePause() {
    if (state.timerState === 'DISABLED') {
        authFetch('/api/timer/enable', { method: 'POST' })
            .then(res => res.json())
            .then(data => {
                if (data.success) {
                    showToast(t('toast.countStarted'), 'success');
                    pollTimerStatus();
                }
            })
            .catch(() => showToast('Bağlantı hatası', 'error'));
    } else {
        authFetch('/api/timer/disable', { method: 'POST' })
            .then(res => res.json())
            .then(data => {
                if (data.success) {
                    showToast(t('toast.countStopped'), 'info');
                    pollTimerStatus();
                }
            })
            .catch(() => showToast('Bağlantı hatası', 'error'));
    }
}

function updatePauseButton() {
    const pauseBtn = document.getElementById('pauseBtn');
    if (!pauseBtn) return;

    const pauseIcon = pauseBtn.querySelector('.pause-icon');
    const playIcon = pauseBtn.querySelector('.play-icon');

    const isRunning = state.timerState === 'RUNNING' || state.timerState === 'WARNING';

    if (isRunning) {
        pauseBtn.classList.remove('paused');
        pauseBtn.title = 'Durdur';
        if (pauseIcon) pauseIcon.classList.remove('hidden');
        if (playIcon) playIcon.classList.add('hidden');
    } else {
        pauseBtn.classList.add('paused');
        pauseBtn.title = 'Başlat';
        if (pauseIcon) pauseIcon.classList.add('hidden');
        if (playIcon) playIcon.classList.remove('hidden');
    }
}

function updateVacationIndicator() {
    const indicator = document.getElementById('vacationIndicator');
    const remaining = document.getElementById('vacationRemaining');

    if (!indicator) return;

    if (state.timerState === 'VACATION' || state.vacationMode.enabled) {
        indicator.classList.add('active');
        if (remaining) {
            remaining.textContent = `${state.vacationMode.days} gün`;
        }
    } else {
        indicator.classList.remove('active');
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Timer Configuration
// ─────────────────────────────────────────────────────────────────────────────
function initTimerConfig() {
    // Load timer config from ESP32
    loadTimerConfig();

    // Unit selector buttons (Gün/Saat/Dakika)
    document.querySelectorAll('.unit-btn').forEach(btn => {
        btn.addEventListener('click', () => {
            document.querySelectorAll('.unit-btn').forEach(b => b.classList.remove('active'));
            btn.classList.add('active');
            state.timerConfig.unit = btn.dataset.unit;
            updateAlarmConstraints();
            updateAlarmInfo();
        });
    });

    // Timer value input + buttons
    const timerInput = document.getElementById('timerDuration');
    const timerIncrease = document.getElementById('timerIncrease');
    const timerDecrease = document.getElementById('timerDecrease');

    if (timerInput) {
        timerInput.addEventListener('change', () => {
            let val = parseInt(timerInput.value) || 1;
            val = Math.max(1, Math.min(60, val));
            timerInput.value = val;
            state.timerConfig.value = val;
            updateAlarmConstraints();
            updateAlarmInfo();
        });
    }

    if (timerIncrease) {
        timerIncrease.addEventListener('click', () => {
            let val = parseInt(timerInput.value) || 1;
            val = Math.min(60, val + 1);
            timerInput.value = val;
            state.timerConfig.value = val;
            updateAlarmConstraints();
            updateAlarmInfo();
        });
    }

    if (timerDecrease) {
        timerDecrease.addEventListener('click', () => {
            let val = parseInt(timerInput.value) || 1;
            val = Math.max(1, val - 1);
            timerInput.value = val;
            state.timerConfig.value = val;
            updateAlarmConstraints();
            updateAlarmInfo();
        });
    }

    // Alarm slider
    const alarmSlider = document.getElementById('alarmCount');
    const alarmValue = document.getElementById('alarmCountValue');

    if (alarmSlider) {
        alarmSlider.addEventListener('input', () => {
            const val = parseInt(alarmSlider.value);
            state.timerConfig.alarmCount = val;
            if (alarmValue) alarmValue.textContent = val;
            updateAlarmInfo();
        });
    }

    // Vacation mode toggle
    const vacationToggle = document.getElementById('vacationMode');
    const vacationDaysGroup = document.getElementById('vacationDaysGroup');

    if (vacationToggle) {
        vacationToggle.addEventListener('change', () => {
            state.vacationMode.enabled = vacationToggle.checked;
            if (vacationDaysGroup) {
                vacationDaysGroup.classList.toggle('hidden', !vacationToggle.checked);
            }
        });
    }

    // Vacation days input + buttons
    const vacationInput = document.getElementById('vacationDays');
    const vacationIncrease = document.getElementById('vacationIncrease');
    const vacationDecrease = document.getElementById('vacationDecrease');

    if (vacationInput) {
        vacationInput.addEventListener('change', () => {
            let val = parseInt(vacationInput.value) || 1;
            val = Math.max(1, Math.min(60, val));
            vacationInput.value = val;
            state.vacationMode.days = val;
        });
    }

    if (vacationIncrease) {
        vacationIncrease.addEventListener('click', () => {
            let val = parseInt(vacationInput.value) || 1;
            val = Math.min(60, val + 1);
            vacationInput.value = val;
            state.vacationMode.days = val;
        });
    }

    if (vacationDecrease) {
        vacationDecrease.addEventListener('click', () => {
            let val = parseInt(vacationInput.value) || 1;
            val = Math.max(1, val - 1);
            vacationInput.value = val;
            state.vacationMode.days = val;
        });
    }
}

function loadTimerConfig() {
    authFetch('/api/config/timer')
        .then(res => {
            if (!res.ok) throw new Error('Not authenticated');
            return res.json();
        })
        .then(data => {
            // Map intervalMinutes to unit + value
            const minutes = data.intervalMinutes || 1440;
            let unit = 'hours';
            let value = Math.round(minutes / 60);

            if (minutes >= 1440 && minutes % 1440 === 0) {
                unit = 'days';
                value = minutes / 1440;
            } else if (minutes < 60 || minutes % 60 !== 0) {
                unit = 'minutes';
                value = minutes;
            } else {
                unit = 'hours';
                value = minutes / 60;
            }

            state.timerConfig.unit = unit;
            state.timerConfig.value = value;
            state.timerConfig.alarmCount = data.alarmCount || 0;
            state.vacationMode.enabled = data.vacationEnabled || false;
            state.vacationMode.days = data.vacationDays || 7;

            // Update UI elements
            document.querySelectorAll('.unit-btn').forEach(b => {
                b.classList.toggle('active', b.dataset.unit === unit);
            });

            const timerInput = document.getElementById('timerDuration');
            if (timerInput) timerInput.value = value;

            const alarmSlider = document.getElementById('alarmCount');
            if (alarmSlider) alarmSlider.value = data.alarmCount || 0;

            const alarmValueEl = document.getElementById('alarmCountValue');
            if (alarmValueEl) alarmValueEl.textContent = data.alarmCount || 0;

            const vacationToggle = document.getElementById('vacationMode');
            if (vacationToggle) vacationToggle.checked = data.vacationEnabled || false;

            const vacationDaysGroup = document.getElementById('vacationDaysGroup');
            if (vacationDaysGroup) {
                vacationDaysGroup.classList.toggle('hidden', !data.vacationEnabled);
            }

            const vacationInput = document.getElementById('vacationDays');
            if (vacationInput) vacationInput.value = data.vacationDays || 7;

            updateAlarmConstraints();
            updateAlarmInfo();
            updateVacationIndicator();
        })
        .catch(() => {});
}

function updateAlarmConstraints() {
    const alarmSlider = document.getElementById('alarmCount');
    if (!alarmSlider) return;

    const maxAlarms = Math.min(10, Math.floor(state.timerConfig.value / 2));
    alarmSlider.max = maxAlarms;

    if (state.timerConfig.alarmCount > maxAlarms) {
        state.timerConfig.alarmCount = maxAlarms;
        alarmSlider.value = maxAlarms;
        const alarmValueEl = document.getElementById('alarmCountValue');
        if (alarmValueEl) alarmValueEl.textContent = maxAlarms;
    }
}

function updateAlarmInfo() {
    const alarmInfo = document.getElementById('alarmInfo');
    if (!alarmInfo) return;

    const { unit, value, alarmCount } = state.timerConfig;

    if (alarmCount === 0) {
        alarmInfo.textContent = 'Uyarı alarmı yok';
        return;
    }

    const unitNames = {
        days: { singular: 'gün', plural: 'günler', suffix: '.' },
        hours: { singular: 'saat', plural: 'saatler', suffix: '.' },
        minutes: { singular: 'dakika', plural: 'dakikalar', suffix: '.' }
    };

    const unitInfo = unitNames[unit];
    const alarmTimes = [];

    for (let i = 1; i <= alarmCount; i++) {
        alarmTimes.push(value - i);
    }

    const timeStr = alarmTimes.reverse().join(', ');
    alarmInfo.textContent = `${timeStr}${unitInfo.suffix} ${unitInfo.singular}de alarm çalar`;
}

// ─────────────────────────────────────────────────────────────────────────────
// Save Timer Settings
// ─────────────────────────────────────────────────────────────────────────────
async function handleSaveSettings() {
    console.log('[LebensSpur] handleSaveSettings called', JSON.stringify(state.timerConfig));

    try {
        // Convert unit+value to intervalMinutes
        const { unit, value, alarmCount } = state.timerConfig;
        let intervalMinutes = value;
        if (unit === 'days') intervalMinutes = value * 24 * 60;
        else if (unit === 'hours') intervalMinutes = value * 60;

        // Calculate warningMinutes: spread alarms evenly over the interval
        let warningMinutes = 60;
        if (alarmCount > 0) {
            if (unit === 'hours') warningMinutes = alarmCount * 60;
            else if (unit === 'days') warningMinutes = alarmCount * 24 * 60;
            else if (unit === 'minutes') warningMinutes = alarmCount;
        }

        const timerPayload = {
            enabled: true,
            intervalMinutes: intervalMinutes,
            warningMinutes: warningMinutes,
            alarmCount: alarmCount
        };

        console.log('[LebensSpur] Saving timer:', JSON.stringify(timerPayload));

        const res = await authFetch('/api/config/timer', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(timerPayload)
        });

        console.log('[LebensSpur] Timer save response:', res.status);
        const data = await res.json();
        console.log('[LebensSpur] Timer save result:', JSON.stringify(data));

        if (!data.success) {
            showToast(data.error || t('toast.saveError'), 'error');
            return;
        }

        // Handle vacation mode change
        const vacPayload = state.vacationMode.enabled
            ? { enabled: true, days: state.vacationMode.days }
            : { enabled: false };

        await authFetch('/api/timer/vacation', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(vacPayload)
        });

        // Update local state immediately
        state.intervalMinutes = intervalMinutes;
        state.timerEnabled = true;

        // Close settings and show success
        closeSettings();
        showToast(t('toast.settingsSaved'), 'success');

        // Poll timer status immediately + delayed to catch backend processing
        pollTimerStatus();
        setTimeout(() => pollTimerStatus(), 1000);

    } catch (e) {
        console.error('[LebensSpur] handleSaveSettings error:', e);
        if (e.message !== 'SESSION_EXPIRED') {
            showToast(t('toast.connectionError'), 'error');
        }
    }
}

function initToggles() {
    // Quiet Hours Toggle
    const quietToggle = document.getElementById('quiet-toggle');
    if (quietToggle) {
        quietToggle.addEventListener('change', (e) => {
            state.settings.timer.quietHoursEnabled = e.target.checked;
        });
    }

    // Telegram Toggle
    const telegramToggle = document.getElementById('telegram-toggle');
    if (telegramToggle) {
        telegramToggle.addEventListener('change', (e) => {
            state.settings.notifications.telegramEnabled = e.target.checked;
        });
    }
}
