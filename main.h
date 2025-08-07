#ifndef MAIN_H
#define MAIN_H

#include "config.h" // For TimeInfo struct

// Global Variables needed by other files
extern HvacPerformance performance;
extern bool systemLockedOut;

// Function Declarations needed by other files
struct TimeInfo; // Forward declaration
TimeInfo getCurrentTime();
float readTemperature();
float celsiusToFahrenheit(float c);
unsigned long currentTime();
void writeRelay(int pin, bool value);
bool readRelay(int pin);
void saveSettings();
int getCurrentSeason();

#endif // MAIN_H