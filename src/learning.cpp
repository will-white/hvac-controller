#include "learning.h"
#include "config.h"
#include "main.h"
#include <LittleFS.h>

void initialize_learning() {
    // LittleFS over SPIFFS: much faster first-boot format (SPIFFS could block
    // long enough to matter) and actively maintained in Arduino core 3.x.
    if (!LittleFS.begin(true)) {
        Serial.println("An Error has occurred while mounting LittleFS");
    }
}

void log_manual_adjustment(float newTempF) {
    TimeInfo now = getCurrentTime();
    File file = LittleFS.open(LEARNING_LOG_FILE, FILE_APPEND);
    if (!file) { return; }
    // Cap the log: unbounded growth would eventually fill the partition.
    if (file.size() > LEARNING_LOG_MAX_BYTES) {
        file.close();
        LittleFS.remove(LEARNING_LOG_FILE);
        file = LittleFS.open(LEARNING_LOG_FILE, FILE_APPEND);
        if (!file) { return; }
    }
    file.printf("%d,%d,%.1f\n", now.dayOfWeek, now.hour, newTempF);
    file.close();
}

void analyze_and_learn_if_needed() {
    // Placeholder for the full analysis logic
}
