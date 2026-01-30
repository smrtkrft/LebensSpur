#pragma once

#include <Arduino.h>
#include "scheduler.h"
#include "mail_functions.h"

class TestInterface {
public:
    void begin(CountdownScheduler *scheduler, MailAgent *mailAgent);
    void processSerial();

private:
    CountdownScheduler *scheduler = nullptr;
    MailAgent *mail = nullptr;
};
