// 仅用于离线语法/类型检查的 Arduino 桩（不参与实际编译）
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>

#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define INPUT 0

#define constrain(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))

typedef uint8_t byte;

class String {
public:
  String() {}
  String(const char *p) : s(p ? p : "") {}
  String(const std::string &x) : s(x) {}
  String(float v, int dec) { char b[40]; snprintf(b, sizeof(b), "%.*f", dec, (double)v); s = b; }
  String(double v, int dec) { char b[40]; snprintf(b, sizeof(b), "%.*f", dec, v); s = b; }
  String(int v) { char b[24]; snprintf(b, sizeof(b), "%d", v); s = b; }
  String(unsigned int v) { char b[24]; snprintf(b, sizeof(b), "%u", v); s = b; }
  String(int v, int) { char b[24]; snprintf(b, sizeof(b), "%d", v); s = b; }
  unsigned int length() const { return (unsigned int)s.size(); }
  const char *c_str() const { return s.c_str(); }
  void toCharArray(char *b, unsigned int n) const {
    unsigned int k = 0;
    for (; k + 1 < n && k < s.size(); k++) b[k] = s[k];
    b[k] = 0;
  }
  bool operator==(const char *p) const { return s == (p ? p : ""); }
  bool operator!=(const char *p) const { return !(*this == p); }
  String operator+(const char *p) const { return String(s + (p ? p : "")); }
  String operator+(const String &o) const { return String(s + o.s); }
  const std::string &str() const { return s; }
private:
  std::string s;
};

inline String operator+(const char *p, const String &o) { return String(std::string(p ? p : "") + o.str()); }

class _SerialStub {
public:
  void begin(unsigned long) {}
  void println(const char *) {}
  void println(const String &) {}
  void printf(const char *, ...) {}
};
extern _SerialStub Serial;

unsigned long millis(void);
void pinMode(uint8_t, uint8_t);
void digitalWrite(uint8_t, uint8_t);
void delay(unsigned long);

// FreeRTOS 临界区桩
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
inline void portENTER_CRITICAL(portMUX_TYPE *) {}
inline void portEXIT_CRITICAL(portMUX_TYPE *) {}

// ESP 对象（真实库里由 Esp.h 提供，screenInit 用它打印空闲堆）
class _EspStub {
public:
  uint32_t getFreeHeap(void) { return 300000; }
  uint32_t getPsramSize(void) { return 0; }
};
extern _EspStub ESP;
