#include "learning.h"
#include "config.h"
#include "main.h"
#include "SPIFFS.h"

void initialize_learning() {
    if (!SPIFFS.begin(true)) {
        Serial.println("An Error has occurred while mounting SPIFFS");
    }
}

void log_manual_adjustment(float newTempF) {
    TimeInfo now = getCurrentTime();
    File file = SPIFFS.open(LEARNING_LOG_FILE, FILE_APPEND);
    if (!file) { return; }
    file.printf("%d,%d,%.1f\n", now.dayOfWeek, now.hour, newTempF);
    file.close();
}

void analyze_and_learn_if_needed() {
    // Placeholder for the full analysis logic
}