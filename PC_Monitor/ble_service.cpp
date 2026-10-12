#include "ble_service.h"
#include "proto.h"

BLEServer *bleServer = nullptr;
BLECharacteristic *writeChar = nullptr;
BLECharacteristic *notifyChar = nullptr;

// ---------------- 数据发布 ----------------
// BLE 回调（蓝牙任务）写入 dataPack，主循环（loopTask）读取，
// 两边并发访问会撕裂，因此所有读写都在这把锁里整体完成。
static portMUX_TYPE dataMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t dataSeq = 0;        // 每成功解析一包 +1
static volatile uint32_t lastPacketMs = 0;   // 最近一包的时间戳
static uint32_t takenSeq = 0;                // 主循环已经消费到的序号

// 型号名（名称包写入，UI 读来做卡片副标题）
static char cpuNameBuf[PROTO_NAME_MAX] = {0};
static char gpuNameBuf[PROTO_NAME_MAX] = {0};

bool bleDataSnapshot(HardwareData &out) {
  uint32_t seq;
  portENTER_CRITICAL(&dataMux);
  out = dataPack;
  seq = dataSeq;
  portEXIT_CRITICAL(&dataMux);
  return seq != 0;              // 从未收到过数据 -> false，UI 显示 "--"
}

bool bleDataTake(HardwareData &out, uint32_t &seq) {
  bool isNew;
  portENTER_CRITICAL(&dataMux);
  out = dataPack;
  seq = dataSeq;
  isNew = (seq != takenSeq);
  takenSeq = seq;
  portEXIT_CRITICAL(&dataMux);
  return isNew;
}

uint32_t bleLastPacketMs(void) {
  return lastPacketMs;
}

uint32_t blePacketCount(void) {
  return dataSeq;
}

// 取型号名（没有收到过名称包时返回空串）
void bleDeviceNames(char *cpu, size_t cpuSz, char *gpu, size_t gpuSz) {
  portENTER_CRITICAL(&dataMux);
  if (cpu && cpuSz) { strncpy(cpu, cpuNameBuf, cpuSz - 1); cpu[cpuSz - 1] = 0; }
  if (gpu && gpuSz) { strncpy(gpu, gpuNameBuf, gpuSz - 1); gpu[gpuSz - 1] = 0; }
  portEXIT_CRITICAL(&dataMux);
}

// ---------------- 取值保护 ----------------
// 只钳上界，**负数原样保留**：协议里 0xFFFF 是"读不到"，proto.cpp 把它解成 -1。
// 用 constrain(v, 0, hi) 会把 -1 变成 0，屏幕上就成了"CPU 负载 0%"这种编出来的读数
// （PROTOCOL.md §4：0 是合法值，不能兼职表示读不到）。
static inline float clampHigh(float v, float hi) { return v > hi ? hi : v; }

// 把整包标成"读不到"（-1）。旧 CSV 协议只带 14 个字段，其余字段必须留 -1 而不是 0，
// 否则 disk_used 会显示 0.0%、proc_count 会显示 0（"磁盘空着/没有进程"）。
static void markAllInvalid(HardwareData &d) {
  static_assert(sizeof(HardwareData) == PROTO_FIELD_COUNT * sizeof(float),
                "HardwareData 必须全由 float 组成（markAllInvalid 按 float 遍历）");
  float *p = reinterpret_cast<float *>(&d);
  for (size_t i = 0; i < PROTO_FIELD_COUNT; i++) p[i] = -1.0f;
}

// ---------------- 旧 CSV 文本协议的兼容解析 ----------------
// 首字节不是包头（0xA5/0xB1）时按旧格式解析：14 个 float 的逗号分隔文本
static bool parseCsvLegacy(const char *rawData, HardwareData &out) {
  HardwareData tmp;
  markAllInvalid(tmp);
  int parsed = sscanf(rawData,
    "%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f",
    &tmp.cpuLoad, &tmp.cpuTemp, &tmp.cpuFan, &tmp.cpuPower,
    &tmp.gpuLoad, &tmp.gpuTemp, &tmp.gpuMem,
    &tmp.gpuFan, &tmp.gpuPower, &tmp.fps,
    &tmp.memUsed, &tmp.hddTemp, &tmp.gpuMemTotal,
    &tmp.cpuClk);
  if (parsed != 14) {
    Serial.printf("Parse error: expected 14, got %d\n", parsed);
    Serial.println("Raw: " + String(rawData));
    return false;
  }
  tmp.cpuLoad     = clampHigh(tmp.cpuLoad, 100.0f);
  tmp.gpuLoad     = clampHigh(tmp.gpuLoad, 100.0f);
  tmp.fps         = clampHigh(tmp.fps, 1000.0f);
  tmp.memUsed     = clampHigh(tmp.memUsed, 100.0f);
  tmp.gpuMemTotal = clampHigh(tmp.gpuMemTotal, 128.0f);
  tmp.cpuClk      = clampHigh(tmp.cpuClk, 20000.0f);
  // 旧协议没有的字段保持 -1（UI 显示 --），见 markAllInvalid()
  out = tmp;
  return true;
}

class ServerCallback : public BLEServerCallbacks {
  // 形参不取名：这里操作的是全局 bleServer，参数用不上。
  // 留着名字会触发 -Wunused-parameter（build.ps1 的语法检查按"零警告"卡）。
  void onConnect(BLEServer*) {
    bleConnected = true;
    Serial.println("BLE Device Connected");
  }
  
  void onDisconnect(BLEServer*) {
    bleConnected = false;
    bleServer->startAdvertising();
    Serial.println("BLE Device Disconnected");
  }
};

class CharCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *ch) {
    // 二进制协议必须用 getData()/getLength()：
    // getValue() 返回 String，遇到 0x00 字节会被截断（二进制载荷里 0x00 很常见）
    uint8_t *raw = ch->getData();
    size_t   len = ch->getLength();
    if (raw == nullptr || len == 0) return;

    HardwareData tmp;

    if (raw[0] == PROTO_PKT_NAMES) {
      char c[PROTO_NAME_MAX], g[PROTO_NAME_MAX];
      if (protoDecodeNames(raw, len, c, g)) {
        portENTER_CRITICAL(&dataMux);
        memcpy(cpuNameBuf, c, PROTO_NAME_MAX);
        memcpy(gpuNameBuf, g, PROTO_NAME_MAX);
        portEXIT_CRITICAL(&dataMux);
        Serial.printf("Names: CPU=%s GPU=%s\n", c, g);
      } else {
        Serial.println("Names packet rejected (len/crc)");
      }
      return;
    }

    if (raw[0] == PROTO_PKT_DATA) {
      uint8_t seq = 0;
      if (!protoDecodeData(raw, len, tmp, seq)) {
        Serial.printf("Data packet rejected: len=%u crc=%02X\n",
                      (unsigned)len, (unsigned)protoCrc8(raw, len - 1));
        return;
      }
    } else {
      // 兼容旧 CSV 文本协议。
      // 不能直接往 getData() 返回的缓冲写结束符（那是 BLE 库自己的存储，改它有副作用）；
      // 复制到本地缓冲再终结。140 与原实现的上限一致（CSV 实际约 90 字节）。
      char text[140];
      size_t n = (len < sizeof(text)) ? len : sizeof(text) - 1;
      memcpy(text, raw, n);
      text[n] = 0;
      if (!parseCsvLegacy(text, tmp)) return;
    }

    // 范围保护（避免异常值把界面画崩）。
    // 用 clampHigh 而不是 constrain：负数（-1）是"读不到"哨兵，必须留到 UI 显示 --，
    // 否则读不到 CPU 负载时屏幕会显示一个假的 0%。
    tmp.cpuLoad = clampHigh(tmp.cpuLoad, 100.0f);
    tmp.gpuLoad = clampHigh(tmp.gpuLoad, 100.0f);
    tmp.fps     = clampHigh(tmp.fps, 1000.0f);
    tmp.memUsed = clampHigh(tmp.memUsed, 100.0f);
    tmp.cpuClk  = clampHigh(tmp.cpuClk, 20000.0f);

    uint32_t now = millis();
    portENTER_CRITICAL(&dataMux);
    dataPack = tmp;
    dataSeq++;
    lastPacketMs = now;
    portEXIT_CRITICAL(&dataMux);

    // 波形环形缓冲的推进放在主循环里（见 PC_Monitor.ino 的 wavePush），
    // 那里和绘制是同一个任务，不需要再加锁。
  }
};

void bleInit() {
  BLEDevice::init(BLE_DEVICE_NAME);
  
  #ifdef BLE_MTU_SIZE
  BLEDevice::setMTU(BLE_MTU_SIZE);
  Serial.printf("BLE MTU set to %d\n", BLE_MTU_SIZE);
  #endif
  
  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallback());

  BLEService *service = bleServer->createService(SERVICE_UUID);
  
  writeChar = service->createCharacteristic(
    CHAR_WRITE_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  writeChar->setCallbacks(new CharCallback());
  
  // 通知特征
  notifyChar = service->createCharacteristic(
    "beb5483e-36e1-4688-b7f5-ea07361b26a9",
    BLECharacteristic::PROPERTY_NOTIFY
  );
  notifyChar->addDescriptor(new BLE2902());
  
  service->start();

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  adv->setAppearance(0x00);
  adv->setMinInterval(0x20);
  adv->setMaxInterval(0x40);
  adv->setName(BLE_DEVICE_NAME);
  
  adv->start();
  Serial.println("BLE Advertising started");
}
