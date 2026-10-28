// Host stand-in: a filesystem that never mounts. learning.cpp and events.cpp
// already degrade gracefully (RAM-only) when it doesn't.
#pragma once
#include <Arduino.h> // the real FS.h includes it
#include <cstddef>
#include <cstdint>

#define FILE_READ "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"

class File {
 public:
  explicit operator bool() const { return false; }
  size_t size() { return 0; }
  int printf(const char*, ...) { return 0; }
  size_t print(const char*) { return 0; }
  template <typename T> int read(T*, size_t) { return 0; }
  int read() { return -1; }
  int available() { return 0; }
  void close() {}
};

class LittleFSFS {
 public:
  bool begin(bool = false) { return false; }
  File open(const char*, const char* = FILE_READ) { return File(); }
  bool remove(const char*) { return false; }
  bool rename(const char*, const char*) { return false; }
  bool exists(const char*) { return false; }
};
extern LittleFSFS LittleFS;
