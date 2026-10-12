// ============================================================================
//  proto.h —— PC ⇄ ESP32 二进制协议（v2）
// ----------------------------------------------------------------------------
//  独立成一个编译单元，方便离线测试：不依赖蓝牙、不依赖屏幕。
//  字段表与缩放规则见项目根目录 PROTOCOL.md，改协议时两边一起改。
// ============================================================================
#ifndef PROTO_H
#define PROTO_H

#include <stdint.h>
#include <stddef.h>
#include "config.h"

#define PROTO_FIELD_COUNT 32          // 主包字段数（加字段请追加在最后，见 PROTOCOL.md）
#define PROTO_PKT_DATA    0xA5        // 主包（实时数据）
#define PROTO_PKT_NAMES   0xB1        // 名称包（CPU/GPU 型号）
#define PROTO_INVALID     0xFFFF      // 无效值哨兵（0 是合法值！）
#define PROTO_NAME_MAX    24          // 型号名缓冲区长度（含结尾 0）

// CRC-8/ATM：多项式 0x07、初值 0x00、不反转、不异或输出
uint8_t protoCrc8(const uint8_t *data, size_t len);

// 第 idx 个字段的缩放系数（×1 / ×10 / ×100）
uint16_t protoFieldScale(uint8_t idx);

// 最近一次成功解析的主包里，第 idx 个字段的原始整数值（未除缩放；无效为 0xFFFF）。
// 只给屏幕调试用：把它和 PC 端打印的 raw 对一下，就能确认两端刻度表是否一致
// （不一致时数值会差 10 倍，肉眼很难判断是谁的错）。
uint16_t protoLastRaw(uint8_t idx);

// 解析主包：成功时写入 out 并返回 true（seq 为包序号）
bool protoDecodeData(const uint8_t *buf, size_t len, HardwareData &out, uint8_t &seq);

// 解析名称包：文本格式 "CPU名称|GPU名称"
bool protoDecodeNames(const uint8_t *buf, size_t len, char *cpuName, char *gpuName);

#endif
