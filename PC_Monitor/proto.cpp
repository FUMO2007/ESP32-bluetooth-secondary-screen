// ============================================================================
//  proto.cpp —— PC ⇄ ESP32 二进制协议（v2）实现
//  字段顺序/缩放见 PROTOCOL.md（两端必须一致）
// ============================================================================
#include <string.h>
#include "proto.h"

// 每个字段的缩放系数，顺序与字段表一致
static const uint16_t SCALE[PROTO_FIELD_COUNT] = {
  10,   // 0  cpu_load      %
  10,   // 1  cpu_temp      °C
  1,    // 2  cpu_fan       RPM
  10,   // 3  cpu_power     W
  10,   // 4  gpu_load      %
  10,   // 5  gpu_temp      °C
  100,  // 6  gpu_mem       GB
  1,    // 7  gpu_fan       RPM
  10,   // 8  gpu_power     W
  1,    // 9  fps           fps
  10,   // 10 mem_used      %
  10,   // 11 hdd_temp      °C
  100,  // 12 gpu_mem_total GB
  1,    // 13 cpu_clk       MHz
  100,  // 14 cpu_volt      V
  10,   // 15 cpu_core_max  %
  10,   // 16 gpu_hotspot   °C
  10,   // 17 gpu_mem_temp  °C
  1,    // 18 gpu_clk       MHz
  10,   // 19 gpu_mem_ctrl  %
  1,    // 20 mobo_fan      RPM
  10,   // 21 disk_read     MB/s
  10,   // 22 disk_write    MB/s
  10,   // 23 disk_used     %
  100,  // 24 net_down      MB/s（x100：0.01 MB/s = 10 KB/s 一格，空闲流量才看得出变化）
  100,  // 25 net_up        MB/s（同上；上限 655 MB/s）
  10,   // 26 ping_ms       ms
  10,   // 27 ping_loss     %
  1,    // 28 proc_count    个
  1,    // 29 uptime_min    min
  100,  // 30 frametime_ms  ms
  1,    // 31 fps_low       fps（1% low；追加字段一律加在最后，前面索引不动）
};

// 主包长度：包头 + 序号 + 长度 + 载荷 + CRC
#define PROTO_DATA_LEN (3 + PROTO_FIELD_COUNT * 2 + 1)

uint16_t protoFieldScale(uint8_t idx) {
  return (idx < PROTO_FIELD_COUNT) ? SCALE[idx] : 1;
}

// 最近一包各字段的原始整数（调试用，见 proto.h 的说明）
static uint16_t g_lastRaw[PROTO_FIELD_COUNT];

uint16_t protoLastRaw(uint8_t idx) {
  return (idx < PROTO_FIELD_COUNT) ? g_lastRaw[idx] : 0;
}

uint8_t protoCrc8(const uint8_t *data, size_t len) {
  uint8_t crc = 0x00;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

bool protoDecodeData(const uint8_t *buf, size_t len, HardwareData &out, uint8_t &seq) {
  if (buf == nullptr || len != PROTO_DATA_LEN) return false;      // 66 字节
  if (buf[0] != PROTO_PKT_DATA) return false;
  if (buf[2] != PROTO_FIELD_COUNT * 2) return false;
  if (protoCrc8(buf, len - 1) != buf[len - 1]) return false;

  float v[PROTO_FIELD_COUNT];
  for (int i = 0; i < PROTO_FIELD_COUNT; i++) {
    uint16_t raw = (uint16_t)buf[3 + i * 2] | ((uint16_t)buf[3 + i * 2 + 1] << 8);
    g_lastRaw[i] = raw;                                    // 调试用，见 protoLastRaw()
    v[i] = (raw == PROTO_INVALID) ? -1.0f : (float)raw / (float)SCALE[i];
  }

  // ---- 按字段表落到具名字段（改协议时这里和 PROTOCOL.md 一起改）----
  out.cpuLoad      = v[0];
  out.cpuTemp      = v[1];
  out.cpuFan       = v[2];
  out.cpuPower     = v[3];
  out.gpuLoad      = v[4];
  out.gpuTemp      = v[5];
  out.gpuMem       = v[6];
  out.gpuFan       = v[7];
  out.gpuPower     = v[8];
  out.fps          = v[9];
  out.memUsed      = v[10];
  out.hddTemp      = v[11];
  out.gpuMemTotal  = v[12];
  out.cpuClk       = v[13];
  out.cpuVolt      = v[14];
  out.cpuCoreMax   = v[15];
  out.gpuHotspot   = v[16];
  out.gpuMemTemp   = v[17];
  out.gpuClk       = v[18];
  out.gpuMemCtrl   = v[19];
  out.moboFan      = v[20];
  out.diskRead     = v[21];
  out.diskWrite    = v[22];
  out.diskUsed     = v[23];
  out.netDown      = v[24];
  out.netUp        = v[25];
  out.pingMs       = v[26];
  out.pingLoss     = v[27];
  out.procCount    = v[28];
  out.uptimeMin    = v[29];
  out.frameMs      = v[30];
  out.fpsLow       = v[31];

  seq = buf[1];
  return true;
}

// 把 src 复制到 dst（长度上限 PROTO_NAME_MAX），并去掉首尾空格
static void copyName(const char *src, size_t len, char *dst) {
  while (len > 0 && (src[0] == ' ' || src[0] == '\t')) { src++; len--; }
  while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\t')) len--;
  if (len > PROTO_NAME_MAX - 1) len = PROTO_NAME_MAX - 1;
  memcpy(dst, src, len);
  dst[len] = 0;
}

bool protoDecodeNames(const uint8_t *buf, size_t len, char *cpuName, char *gpuName) {
  if (buf == nullptr || len < 4) return false;
  if (buf[0] != PROTO_PKT_NAMES) return false;
  size_t n = buf[1];
  if (len != 3 + n) return false;
  if (protoCrc8(buf, len - 1) != buf[len - 1]) return false;

  const char *s = (const char *)(buf + 2);
  size_t bar = n;
  for (size_t i = 0; i < n; i++) {
    if (s[i] == '|') { bar = i; break; }
  }
  copyName(s, bar, cpuName);
  if (bar < n) copyName(s + bar + 1, n - bar - 1, gpuName);
  else         gpuName[0] = 0;
  return true;
}
