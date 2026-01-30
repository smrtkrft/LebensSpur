#pragma once

#include <Arduino.h>
#include "config_store.h"

static const uint8_t MAX_ALARMS = 10;

struct ScheduleSnapshot {
    bool timerActive = false;
    uint32_t remainingSeconds = 0;
    uint8_t nextAlarmIndex = 0;
    uint8_t totalAlarms = 0;
    uint32_t alarmOffsets[MAX_ALARMS] = {0}; // seconds from start
    bool finalTriggered = false;
};

class CountdownScheduler {
public:
    void begin(ConfigStore *store);
    void configure(const TimerSettings &settings);
    void loadFromStore();

    void start();
    void pause();
    void resume();
    void stop();
    void reset();

    void tick();

    bool isActive() const { return runtime.timerActive && !runtime.paused; }
    bool isPaused() const { return runtime.paused; }
    bool isStopped() const { return !runtime.timerActive; }
    uint32_t remainingSeconds() const;
    ScheduleSnapshot snapshot() const;
    uint32_t totalSeconds() const;

    bool alarmDue(uint8_t &alarmIndexOut);
    bool finalDue() const;

    void acknowledgeAlarm(uint8_t alarmIndex);
    void acknowledgeFinal();
    void updateRuntime(const TimerRuntime &newRuntime); // Runtime'ı güncelle (grup gönderim durumları için)

    const TimerSettings &settings() const { return currentSettings; }
    TimerRuntime runtimeState() const;
    void persist();

private:
    ConfigStore *store = nullptr;
    TimerSettings currentSettings;
    TimerRuntime runtime;
    uint32_t alarmMoments[MAX_ALARMS] = {0}; // seconds from start
    uint8_t alarmCount = 0;

    void regenerateSchedule();
    uint32_t totalDurationSeconds() const;
    uint32_t unitStepSeconds() const;
    void updateRemaining();
};
