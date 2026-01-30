#include "test_functions.h"

void TestInterface::begin(CountdownScheduler *sched, MailAgent *mailAgent) {
    scheduler = sched;
    mail = mailAgent;
}

void TestInterface::processSerial() {
    if (!Serial.available()) return;
    String command = Serial.readStringUntil('\n');
    command.trim();
    if (command == "status") {
        ScheduleSnapshot snap = scheduler->snapshot();
    } else if (command == "start") {
        scheduler->start();
    } else if (command == "reset") {
        scheduler->reset();
    } else if (command == "stop") {
        scheduler->stop();
    } else if (command == "mail") {
        String error;
        ScheduleSnapshot snap = scheduler->snapshot();
        mail->sendWarning(snap.nextAlarmIndex, snap, error);
    }
}
