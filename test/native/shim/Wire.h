// Host stand-in: an I2C bus with nothing attached (MOD-IO2 and SHT4x absent).
#pragma once
#include <cstdint>
#include <cstddef>

class TwoWire {
 public:
  bool begin(int = -1, int = -1, uint32_t = 0) { return true; }
  void setClock(uint32_t) {}
  void beginTransmission(uint8_t) {}
  size_t write(uint8_t) { return 1; }
  uint8_t endTransmission(bool = true) { return 2; } // NACK on address
  uint8_t requestFrom(int, int) { return 0; }
  int available() { return 0; }
  int read() { return -1; }
};
extern TwoWire Wire;
