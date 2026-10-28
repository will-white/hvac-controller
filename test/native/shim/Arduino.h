// Host stand-in for the Arduino core, for the native test build only
// (test/native). Covers exactly what the logic modules call; anything else
// should fail to compile so a new dependency is noticed, not silently faked.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

// The real core pulls these in transitively; the firmware relies on that.
#include "esp_system.h"

using std::isnan;

#define HIGH 1
#define LOW 0
#define INPUT 0x01
#define OUTPUT 0x03
#define INPUT_PULLUP 0x05
#define PROGMEM
#define F(s) (s)

// Fake clock: starts at 1 s, advanced only by delay() and native_advanceMillis.
unsigned long millis();
void delay(unsigned long ms);
void native_advanceMillis(unsigned long ms);
inline void yield() {}

// Fake GPIO bank.
void pinMode(uint8_t pin, uint8_t mode);
void digitalWrite(uint8_t pin, uint8_t val);
int digitalRead(uint8_t pin);

[[noreturn]] void esp_restart();

class HostSerial {
 public:
  void begin(unsigned long) {}
  void setTxTimeoutMs(uint32_t) {}
  explicit operator bool() const { return true; }
  int available() { return 0; }
  int read() { return -1; }
  void flush() { fflush(stdout); }
  size_t write(uint8_t c) { return fwrite(&c, 1, 1, stdout); }
  size_t write(const uint8_t* b, size_t n) { return fwrite(b, 1, n, stdout); }
  int printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
  size_t print(const char* s) { return fputs(s, stdout) >= 0 ? strlen(s) : 0; }
  size_t print(char c) { return write((uint8_t)c); }
  size_t print(int v) { return printf("%d", v); }
  size_t print(unsigned v) { return printf("%u", v); }
  size_t print(long v) { return printf("%ld", v); }
  size_t print(unsigned long v) { return printf("%lu", v); }
  size_t print(double v) { return printf("%.2f", v); }
  size_t println() { return print("\n"); }
  template <typename T> size_t println(T v) { size_t n = print(v); return n + println(); }
};
extern HostSerial Serial;
