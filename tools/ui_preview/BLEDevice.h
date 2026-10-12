// 仅用于离线语法/类型检查的 BLE 桩
#pragma once
#include <Arduino.h>

class BLEService;
class BLEServer;
class BLECharacteristic;
class BLEDescriptor;

class BLEServerCallbacks {
public:
  virtual ~BLEServerCallbacks() {}
  virtual void onConnect(BLEServer *) {}
  virtual void onDisconnect(BLEServer *) {}
};

class BLECharacteristicCallbacks {
public:
  virtual ~BLECharacteristicCallbacks() {}
  virtual void onWrite(BLECharacteristic *) {}
};

class BLECharacteristic {
public:
  static const uint32_t PROPERTY_WRITE = 1;
  static const uint32_t PROPERTY_WRITE_NR = 2;
  static const uint32_t PROPERTY_NOTIFY = 4;
  void setCallbacks(BLECharacteristicCallbacks *) {}
  String getValue() { return String(); }
  // 真实库（esp32 core 3.3.10 的 BLECharacteristic.h:209-210）确实有这两个：
  // 二进制载荷必须走它们，getValue() 返回 String 会在 0x00 字节处截断
  uint8_t *getData() { return buf; }
  size_t getLength() const { return len; }
  void addDescriptor(BLEDescriptor *) {}
  void setValue(const char *) {}
  uint8_t buf[160] = {0};
  size_t len = 0;
};

class BLEDescriptor {
public:
  BLEDescriptor(const char *) {}
};

class BLE2902 : public BLEDescriptor {
public:
  BLE2902() : BLEDescriptor("2902") {}
};

class BLEService {
public:
  BLECharacteristic *createCharacteristic(const char *, uint32_t) { return new BLECharacteristic(); }
  void start() {}
};

class BLEAdvertising {
public:
  void addServiceUUID(const char *) {}
  void setScanResponse(bool) {}
  void setAppearance(uint16_t) {}
  void setMinInterval(uint16_t) {}
  void setMaxInterval(uint16_t) {}
  void setName(const char *) {}
  void start() {}
};

class BLEServer {
public:
  void setCallbacks(BLEServerCallbacks *) {}
  BLEService *createService(const char *) { return new BLEService(); }
  void startAdvertising() {}
};

class BLEDevice {
public:
  static void init(const char *) {}
  static void setMTU(uint16_t) {}
  static BLEServer *createServer() { return new BLEServer(); }
  static BLEAdvertising *getAdvertising() { return new BLEAdvertising(); }
};
