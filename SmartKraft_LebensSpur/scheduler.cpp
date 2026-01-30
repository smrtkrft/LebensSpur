#include "scheduler.h"

void CountdownScheduler::begin(ConfigStore *storePtr) {
    store = storePtr;
    loadFromStore();
}

void CountdownScheduler::configure(const TimerSettings &settings) {
    currentSettings = settings;
    
    // Eski total duration ve elapsed time'ı hesapla (timer aktifse)
    uint32_t oldElapsed = 0;
    uint32_t oldTotal = totalDurationSeconds(); // Henüz eski settings geçerli
    bool wasActive = runtime.timerActive;
    
    if (wasActive) {
        updateRemaining();
        oldElapsed = oldTotal - runtime.remainingSeconds;
    }
    
    // Yeni schedule oluştur (artık currentSettings güncel)
    regenerateSchedule();
    uint32_t newTotal = totalDurationSeconds();
    
    if (wasActive) {
        // Timer aktifse elapsed time'ı yeni total'e göre ayarla
        if (oldElapsed >= newTotal) {
            // Elapsed time yeni total'den büyükse, timer'ı resetle ama başlatma
            // Kullanıcı "reset" butonuna basınca tekrar başlayacak
            runtime.timerActive = false;
            runtime.paused = false;
            runtime.finalTriggered = false;
            runtime.nextAlarmIndex = 0;
            runtime.remainingSeconds = newTotal;
            runtime.deadlineMillis = 0;
        } else {
            // Elapsed time mantıklıysa, kalan süreyi güncelle
            runtime.remainingSeconds = newTotal - oldElapsed;
            runtime.deadlineMillis = millis() + (uint64_t)runtime.remainingSeconds * 1000ULL;
            
            // Alarm index'i yeni alarm sayısına göre ayarla
            // Eğer elapsed time bir alarm noktasını geçtiyse, sonraki alarma atla
            runtime.nextAlarmIndex = 0;
            for (uint8_t i = 0; i < alarmCount; ++i) {
                if (oldElapsed >= alarmMoments[i]) {
                    runtime.nextAlarmIndex = i + 1;
                }
            }
            runtime.nextAlarmIndex = min(runtime.nextAlarmIndex, alarmCount);
        }
    } else {
        // Timer aktif değilse, sadece total süreyi güncelle
        runtime.remainingSeconds = newTotal;
    }
    
    store->saveTimerSettings(settings);
    persist();
}

void CountdownScheduler::loadFromStore() {
    currentSettings = store->loadTimerSettings();
    runtime = store->loadRuntime();
    regenerateSchedule();
    uint32_t total = totalDurationSeconds();
    if (runtime.remainingSeconds == 0 || runtime.remainingSeconds > total) {
        runtime.remainingSeconds = total;
    }
    if (runtime.nextAlarmIndex > alarmCount) {
        runtime.nextAlarmIndex = alarmCount;
    }
    if (runtime.timerActive) {
        runtime.deadlineMillis = millis() + (uint64_t)runtime.remainingSeconds * 1000ULL;
    }
}

void CountdownScheduler::start() {
    if (!currentSettings.enabled) {
        return;
    }
    // Only start if timer is stopped (not running or paused)
    if (runtime.timerActive) {
        return; // Already running or paused, do nothing
    }
    runtime.timerActive = true;
    runtime.paused = false;
    runtime.finalTriggered = false;
    runtime.nextAlarmIndex = 0;
    runtime.deadlineMillis = millis() + (uint64_t)totalDurationSeconds() * 1000ULL;
    runtime.remainingSeconds = totalDurationSeconds();
    persist();
}

void CountdownScheduler::pause() {
    if (!runtime.timerActive || runtime.paused) {
        return; // Not running or already paused
    }
    updateRemaining(); // Update remaining time before pausing
    runtime.paused = true;
    persist();
}

void CountdownScheduler::resume() {
    if (!runtime.timerActive || !runtime.paused) {
        return; // Not paused, can't resume
    }
    runtime.paused = false;
    runtime.deadlineMillis = millis() + (uint64_t)runtime.remainingSeconds * 1000ULL;
    persist();
}

void CountdownScheduler::stop() {
    // Stop is deprecated but kept for compatibility - same as pause
    pause();
}

void CountdownScheduler::reset() {
    runtime.timerActive = false;
    runtime.paused = false;
    runtime.finalTriggered = false;
    runtime.nextAlarmIndex = 0;
    runtime.remainingSeconds = totalDurationSeconds();
    runtime.deadlineMillis = millis() + (uint64_t)runtime.remainingSeconds * 1000ULL;
    persist();
}

void CountdownScheduler::tick() {
    // Sadece aktif ve duraklamamış timer'lar için çalış
    if (!runtime.timerActive || runtime.paused) {
        return;
    }
    
    updateRemaining();
    
    // Süre dolduğunda final durumuna geç
    if (runtime.remainingSeconds == 0) {
        runtime.timerActive = false;
        runtime.paused = false;
        runtime.finalTriggered = true;
        persist();
    }
}

uint32_t CountdownScheduler::remainingSeconds() const {
    return runtime.remainingSeconds;
}

uint32_t CountdownScheduler::totalSeconds() const {
    return totalDurationSeconds();
}

ScheduleSnapshot CountdownScheduler::snapshot() const {
    ScheduleSnapshot snap;
    snap.timerActive = runtime.timerActive;
    snap.remainingSeconds = runtime.remainingSeconds;
    snap.nextAlarmIndex = runtime.nextAlarmIndex;
    snap.totalAlarms = alarmCount;
    snap.finalTriggered = runtime.finalTriggered;
    for (uint8_t i = 0; i < alarmCount; ++i) {
        snap.alarmOffsets[i] = alarmMoments[i];
    }
    return snap;
}

bool CountdownScheduler::alarmDue(uint8_t &alarmIndexOut) {
    if (!runtime.timerActive || runtime.paused || runtime.nextAlarmIndex >= alarmCount) {
        return false;
    }
    updateRemaining();
    uint32_t elapsed = totalDurationSeconds() - runtime.remainingSeconds;
    if (elapsed >= alarmMoments[runtime.nextAlarmIndex]) {
        alarmIndexOut = runtime.nextAlarmIndex;
        return true;
    }
    return false;
}

bool CountdownScheduler::finalDue() const {
    return runtime.finalTriggered && runtime.remainingSeconds == 0;
}

void CountdownScheduler::acknowledgeAlarm(uint8_t alarmIndex) {
    if (alarmIndex == runtime.nextAlarmIndex && runtime.nextAlarmIndex < alarmCount) {
        runtime.nextAlarmIndex++;
        persist();
    }
}

void CountdownScheduler::acknowledgeFinal() {
    runtime.finalTriggered = false;
    // Grup gönderim durumlarını sıfırla
    for (uint8_t i = 0; i < MAX_MAIL_GROUPS; ++i) {
        runtime.finalGroupsSent[i] = false;
    }
    persist();
}

void CountdownScheduler::updateRuntime(const TimerRuntime &newRuntime) {
    // Grup gönderim durumlarını güncelle
    for (uint8_t i = 0; i < MAX_MAIL_GROUPS; ++i) {
        runtime.finalGroupsSent[i] = newRuntime.finalGroupsSent[i];
    }
    persist();
}

TimerRuntime CountdownScheduler::runtimeState() const {
    return runtime;
}

void CountdownScheduler::persist() {
    TimerRuntime snapshotRuntime = runtime;
    if (runtime.timerActive) {
        updateRemaining();
        snapshotRuntime.remainingSeconds = runtime.remainingSeconds;
        snapshotRuntime.deadlineMillis = runtime.deadlineMillis;
    }
    store->saveRuntime(snapshotRuntime);
}

void CountdownScheduler::regenerateSchedule() {
    alarmCount = min(currentSettings.alarmCount, MAX_ALARMS);
    uint32_t step = unitStepSeconds();
    uint32_t total = totalDurationSeconds();
    uint32_t minimumRequired = step * alarmCount + step;

    if (total <= step) {
        alarmCount = 0;
    }

    if (alarmCount > 0) {
        if (total < minimumRequired) {
            // evenly distribute if not enough whole units
            for (uint8_t i = 0; i < alarmCount; ++i) {
                alarmMoments[i] = (total * (i + 1)) / (alarmCount + 1);
            }
        } else {
            for (uint8_t i = 0; i < alarmCount; ++i) {
                uint8_t remainingAlarms = alarmCount - i;
                alarmMoments[i] = total - remainingAlarms * step;
            }
        }
    }
}

uint32_t CountdownScheduler::totalDurationSeconds() const {
    if (currentSettings.unit == TimerSettings::MINUTES) {
        return (uint32_t)currentSettings.totalValue * 60UL; // Dakika → Saniye
    } else if (currentSettings.unit == TimerSettings::HOURS) {
        return (uint32_t)currentSettings.totalValue * 60UL * 60UL; // Saat → Saniye
    } else { // DAYS
        return (uint32_t)currentSettings.totalValue * 24UL * 60UL * 60UL; // Gün → Saniye
    }
}

uint32_t CountdownScheduler::unitStepSeconds() const {
    if (currentSettings.unit == TimerSettings::MINUTES) {
        return 60UL; // 1 dakika = 60 saniye
    } else if (currentSettings.unit == TimerSettings::HOURS) {
        return 60UL * 60UL; // 1 saat = 3600 saniye
    } else { // DAYS
        return 24UL * 60UL * 60UL; // 1 gün = 86400 saniye
    }
}

void CountdownScheduler::updateRemaining() {
    if (!runtime.timerActive || runtime.paused) {
        return;
    }
    uint64_t now = millis();
    
    // ⚠️ millis() overflow koruması (49.7 günde taşar)
    // Eğer deadline geçmişse VEYA now > deadline + 1 saat (wrap-around tespiti)
    if (runtime.deadlineMillis > now) {
        runtime.remainingSeconds = (runtime.deadlineMillis - now) / 1000ULL;
    } else if (now > runtime.deadlineMillis + 3600000ULL) {
        runtime.deadlineMillis = now + (uint64_t)runtime.remainingSeconds * 1000ULL;
    } else {
        runtime.remainingSeconds = 0;
    }
}
