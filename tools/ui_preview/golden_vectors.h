// ============================================================================
//  golden_vectors.h —— 协议金标向量（**由 pc_side/test_proto.py 自动生成**）
// ----------------------------------------------------------------------------
//  不要手工编辑这个文件：改动会在下次运行 test_proto.py 时被覆盖。
//
//  重新生成：
//      python -B pc_side/test_proto.py
//  只校验是否最新（不改文件，给 CI / build.ps1 用）：
//      python -B pc_side/test_proto.py --check
//
//  这些 HEX 是 PC 端编码器（proto_v2.py）的真实输出；proto_test.cpp 用固件
//  解码器（proto.cpp）解它，再和下面的 GOLDEN_RAW / GOLDEN_EXPECT 比对。
//  于是"两端字段顺序 + 缩放表是否一致"变成一个可自动判定的事实，
//  而不是靠人把 HEX 复制粘贴过去（那样 PC 端改错不会被发现）。
// ============================================================================
#ifndef GOLDEN_VECTORS_H
#define GOLDEN_VECTORS_H

#define GOLDEN_FIELD_COUNT 32
#define GOLDEN_SEQ 7
#define GOLDEN_INVALID_SEQ 8

// 主包：全部字段都有值
static const char *GOLDEN = "A507407A023C02A3047903A20171023803DF05C4068D00DC018101B004CA107E00910310039502E7095101AC0300007B00C80233012A00BA0000001F010314C5022900DA";

// 哨兵包：gpu_hotspot / ping_ms 读不到（raw 0xFFFF），disk_read 是合法 0
static const char *GOLDEN_INVALID = "A508407A023C02A3047903A20171023803DF05C4068D00DC018101B004CA107E009103FFFF9502E7095101AC0300007B00C80233012A00FFFF00001F010314C502FFFF14";

// 名称包："CPU型号|GPU型号"
static const char *GOLDEN_NAMES = "B11169352D31323430307C5254582034303630DC";
static const char *GOLDEN_NAMES_CPU = "i5-12400";
static const char *GOLDEN_NAMES_GPU = "RTX 4060";

// 原始 uint16（编码器定标后的值），顺序与 FIELD_KEYS / PROTOCOL.md 字段表一致。
// 这是最紧的断言：固件解出来的 raw 必须逐个等于它，缩放或顺序错一格都会红。
static const unsigned short GOLDEN_RAW[GOLDEN_FIELD_COUNT] = {
    634,   572,  1187,   889,   418,   625,   824,  1503,  1732,   141,   // cpu_load, cpu_temp, cpu_fan, cpu_power, gpu_load, gpu_temp, gpu_mem, gpu_fan, gpu_power, fps
    476,   385,  1200,  4298,   126,   913,   784,   661,  2535,   337,   // mem_used, hdd_temp, gpu_mem_total, cpu_clk, cpu_volt, cpu_core_max, gpu_hotspot, gpu_mem_temp, gpu_clk, gpu_mem_ctrl
    940,     0,   123,   712,   307,    42,   186,     0,   287,  5123,   // mobo_fan, disk_read, disk_write, disk_used, net_down, net_up, ping_ms, ping_loss, proc_count, uptime_min
    709,    41,   // frame_ms, fps_low
};
static const unsigned short GOLDEN_INVALID_RAW[GOLDEN_FIELD_COUNT] = {
    634,   572,  1187,   889,   418,   625,   824,  1503,  1732,   141,   // cpu_load, cpu_temp, cpu_fan, cpu_power, gpu_load, gpu_temp, gpu_mem, gpu_fan, gpu_power, fps
    476,   385,  1200,  4298,   126,   913, 65535,   661,  2535,   337,   // mem_used, hdd_temp, gpu_mem_total, cpu_clk, cpu_volt, cpu_core_max, gpu_hotspot, gpu_mem_temp, gpu_clk, gpu_mem_ctrl
    940,     0,   123,   712,   307,    42, 65535,     0,   287,  5123,   // mobo_fan, disk_read, disk_write, disk_used, net_down, net_up, ping_ms, ping_loss, proc_count, uptime_min
    709, 65535,   // frame_ms, fps_low
};

// 解码后的浮点期望值（哨兵为 -1.0f）。容差由 protoFieldScale() 推出：
// 半个最小刻度，见 proto_test.cpp 的 tolOf()。
static const float GOLDEN_EXPECT[GOLDEN_FIELD_COUNT] = {
  63.4f, 57.2f, 1187.0f, 88.9f, 41.8f,   // cpu_load, cpu_temp, cpu_fan, cpu_power, gpu_load
  62.5f, 8.24f, 1503.0f, 173.2f, 141.0f,   // gpu_temp, gpu_mem, gpu_fan, gpu_power, fps
  47.6f, 38.5f, 12.0f, 4298.0f, 1.264f,   // mem_used, hdd_temp, gpu_mem_total, cpu_clk, cpu_volt
  91.3f, 78.4f, 66.1f, 2535.0f, 33.7f,   // cpu_core_max, gpu_hotspot, gpu_mem_temp, gpu_clk, gpu_mem_ctrl
  940.0f, 0.0f, 12.34f, 71.2f, 3.07f,   // mobo_fan, disk_read, disk_write, disk_used, net_down
  0.42f, 18.6f, 0.0f, 287.0f, 5123.0f,   // net_up, ping_ms, ping_loss, proc_count, uptime_min
  7.09f, 41.0f,   // frame_ms, fps_low
};
static const float GOLDEN_INVALID_EXPECT[GOLDEN_FIELD_COUNT] = {
  63.4f, 57.2f, 1187.0f, 88.9f, 41.8f,   // cpu_load, cpu_temp, cpu_fan, cpu_power, gpu_load
  62.5f, 8.24f, 1503.0f, 173.2f, 141.0f,   // gpu_temp, gpu_mem, gpu_fan, gpu_power, fps
  47.6f, 38.5f, 12.0f, 4298.0f, 1.264f,   // mem_used, hdd_temp, gpu_mem_total, cpu_clk, cpu_volt
  91.3f, -1.0f, 66.1f, 2535.0f, 33.7f,   // cpu_core_max, gpu_hotspot, gpu_mem_temp, gpu_clk, gpu_mem_ctrl
  940.0f, 0.0f, 12.34f, 71.2f, 3.07f,   // mobo_fan, disk_read, disk_write, disk_used, net_down
  0.42f, -1.0f, 0.0f, 287.0f, 5123.0f,   // net_up, ping_ms, ping_loss, proc_count, uptime_min
  7.09f, -1.0f,   // frame_ms, fps_low
};

#endif  // GOLDEN_VECTORS_H
