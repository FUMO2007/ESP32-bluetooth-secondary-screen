// ============================================================================
//  proto_test.cpp —— 协议跨语言一致性测试（离线，不需要硬件）
// ----------------------------------------------------------------------------
//  金标向量不再手工粘贴：它由 pc_side/test_proto.py 生成到同目录的
//  golden_vectors.h，这里 #include 进来。
//      python -B pc_side/test_proto.py           # 生成（协议改了必须重跑）
//      python -B pc_side/test_proto.py --check   # 只校验是否最新，不改文件
//
//  编译（在项目根目录，命令行拆成几行只是排版，实际是一条命令）：
//      g++ -std=gnu++11 -I tools/ui_preview -I PC_Monitor
//          tools/ui_preview/proto_test.cpp PC_Monitor/proto.cpp
//          -o tools/ui_preview/proto_test.exe
//
//  为什么改成"生成"而不是"粘贴"：
//    以前这里是"人把 test_proto.py 打印的 HEX 复制过来"。
//      · 改 proto.cpp 的 SCALE      -> 测试失败（好）
//      · 改 proto_v2.py 的 FIELD_SCALE -> 完全发现不了！
//        C++ 侧还在用旧的冻结向量，照样全绿。
//    现在两端共用一份由 PC 端编码器产出的向量，漂移必然暴露。
//
//  容差也不再硬编码 0.06，而是由 protoFieldScale() 推出"半个最小刻度"：
//    x1 -> 0.5 / x10 -> 0.05 / x100 -> 0.005
//  硬编码 0.06 的坏处是：net_down 从 x100 整体退化成 x10 时误差只有 0.03，
//  旧的会放过，新的会抓住 —— 而那正是 PROTOCOL.md 最在意的一条缩放。
// ============================================================================
#include <cstdio>
#include <cstring>
#include <cmath>
#include "proto.h"
#include "golden_vectors.h"

// 下面按序号取字段的前提：HardwareData 是若干个 float 连续排列、没有别的成员、
// 顺序即 PROTOCOL.md 的字段表。这句断言把这几个前提钉在编译期。
static_assert(sizeof(HardwareData) == GOLDEN_FIELD_COUNT * sizeof(float),
              "HardwareData 的 float 个数必须与字段表一致（且无其它成员）");

static int failures = 0;

#define CHECK(cond, ...)                        \
  do {                                          \
    if (!(cond)) {                              \
      printf("  !! ");                          \
      printf(__VA_ARGS__);                      \
      printf("\n");                             \
      failures++;                               \
    }                                           \
  } while (0)

// 该字段允许的误差 = 半个最小刻度（与 pc_side/test_proto.py 的 tol_of 一致）
static float tolOf(int idx) {
  return 0.5f / (float)protoFieldScale((uint8_t)idx) + 1e-3f;
}

// 按序号取字段值。用 memcpy 而不是指针转型：避免依赖严格别名规则。
static float fieldAt(const HardwareData &d, int i) {
  float v;
  memcpy(&v, ((const char *)&d) + (size_t)i * sizeof(float), sizeof(float));
  return v;
}

static int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

static size_t hexToBytes(const char *hex, uint8_t *out, size_t maxLen) {
  size_t n = 0;
  for (const char *p = hex; p[0] && p[1]; p += 2) {
    int hi = hexNibble(p[0]), lo = hexNibble(p[1]);
    if (hi < 0 || lo < 0) break;
    if (n >= maxLen) break;
    out[n++] = (uint8_t)((hi << 4) | lo);
  }
  return n;
}

// 现场构造一个名称包（crcOk=false 时故意把 CRC 写坏）
static size_t buildNames(uint8_t *buf, const char *text, bool crcOk = true) {
  size_t n = strlen(text);
  buf[0] = PROTO_PKT_NAMES;
  buf[1] = (uint8_t)n;
  memcpy(buf + 2, text, n);
  uint8_t crc = protoCrc8(buf, 2 + n);
  buf[2 + n] = crcOk ? crc : (uint8_t)(crc ^ 0xFF);
  return 3 + n;
}

int main() {
  uint8_t buf[256];
  HardwareData d;
  uint8_t seq = 0;

  printf("=== proto_test：跨语言协议一致性 ===\n");
  printf("（金标向量来自 %s 生成的 golden_vectors.h）\n", "pc_side/test_proto.py");

  // ---- 1. CRC 标准向量 ----
  printf("\n-- 1) CRC-8/ATM 已知向量 --\n");
  const char *check = "123456789";
  uint8_t crc = protoCrc8((const uint8_t *)check, 9);
  printf("  CRC-8/ATM(\"%s\") = 0x%02X（期望 0xF4）\n", check, crc);
  CHECK(crc == 0xF4, "CRC-8/ATM 已知向量不符: 0x%02X", crc);

  // ---- 2. 主包：长度 / 解析 / seq ----
  printf("\n-- 2) 主包解码 --\n");
  size_t n = hexToBytes(GOLDEN, buf, sizeof(buf));
  // 长度不写死：字段数是协议的核心参数，加字段时这里应该自动跟上，
  // 而不是变成"测试挂了但看起来像功能坏了"。
  const size_t wantLen = 3 + (size_t)GOLDEN_FIELD_COUNT * 2 + 1;
  printf("  主包长度 = %u 字节（期望 %u）\n", (unsigned)n, (unsigned)wantLen);
  CHECK(n == wantLen, "主包长度应为 %u，实际 %u", (unsigned)wantLen, (unsigned)n);

  memset(&d, 0, sizeof(d));
  bool ok = protoDecodeData(buf, n, d, seq);
  printf("  解析结果 = %s，seq = %u（期望 %u）\n",
         ok ? "成功" : "失败", (unsigned)seq, (unsigned)GOLDEN_SEQ);
  CHECK(ok, "主包解析失败");
  CHECK(seq == GOLDEN_SEQ, "seq 应为 %u，实际 %u",
        (unsigned)GOLDEN_SEQ, (unsigned)seq);

  // ---- 3. 原始整数逐个比对（最紧的断言）----
  // protoDecodeData 会把最后一包的 raw 存下来，正好用来做逐字段校验。
  printf("\n-- 3) 原始 uint16 逐个比对（缩放/顺序错一格都会红）--\n");
  {
    int bad = 0;
    for (int i = 0; i < GOLDEN_FIELD_COUNT; i++) {
      uint16_t got = protoLastRaw((uint8_t)i);
      if (got != (uint16_t)GOLDEN_RAW[i]) {
        printf("    !! 字段 #%d raw 期望 %u 实际 %u\n",
               i, (unsigned)GOLDEN_RAW[i], (unsigned)got);
        bad++;
      }
    }
    printf("  %d 个字段 raw 全部一致\n", GOLDEN_FIELD_COUNT - bad);
    failures += bad;
  }

  // ---- 4. 解码后的浮点值（容差 = 半个最小刻度）----
  printf("\n-- 4) 解码浮点值（容差 = 半个最小刻度）--\n");
  if (ok) {
    int bad = 0;
    for (int i = 0; i < GOLDEN_FIELD_COUNT; i++) {
      float want = GOLDEN_EXPECT[i];
      float got = fieldAt(d, i);
      float tol = tolOf(i);
      if (fabsf(got - want) > tol) {
        printf("    !! 字段 #%d 期望 %.4f 实际 %.4f（容差 %g）\n",
               i, want, got, tol);
        bad++;
      }
    }
    printf("  %d 个字段全部在容差内（最大容差 %.3f）\n",
           GOLDEN_FIELD_COUNT - bad, tolOf(0));
    failures += bad;
  }

  // ---- 5. 哨兵包 ----
  printf("\n-- 5) 哨兵包（0xFFFF = 读不到；0 是合法值）--\n");
  n = hexToBytes(GOLDEN_INVALID, buf, sizeof(buf));
  memset(&d, 0, sizeof(d));
  ok = protoDecodeData(buf, n, d, seq);
  printf("  解析 = %s\n", ok ? "成功" : "失败");
  CHECK(ok, "哨兵包解析失败");
  CHECK(seq == GOLDEN_INVALID_SEQ, "哨兵包 seq 应为 %u，实际 %u",
        (unsigned)GOLDEN_INVALID_SEQ, (unsigned)seq);
  {
    int bad = 0;
    for (int i = 0; i < GOLDEN_FIELD_COUNT; i++) {
      uint16_t wantRaw = (uint16_t)GOLDEN_INVALID_RAW[i];
      uint16_t gotRaw = protoLastRaw((uint8_t)i);
      float want = GOLDEN_INVALID_EXPECT[i];
      float got = fieldAt(d, i);
      bool wantInvalid = (wantRaw == PROTO_INVALID);
      if (gotRaw != wantRaw) {
        printf("    !! 字段 #%d raw 期望 %u 实际 %u\n", i,
               (unsigned)wantRaw, (unsigned)gotRaw);
        bad++;
      } else if (wantInvalid) {
        // 固件内部约定：哨兵存成 -1.0f
        if (got >= 0.0f) {
          printf("    !! 字段 #%d 应为哨兵 -1，实际 %.2f\n", i, got);
          bad++;
        }
      } else if (fabsf(got - want) > tolOf(i)) {
        printf("    !! 字段 #%d 期望 %.4f 实际 %.4f\n", i, want, got);
        bad++;
      }
    }
    printf("  哨兵字段 gpu_hotspot=%.1f  ping_ms=%.1f  disk_read=%.2f（0 必须保留）\n",
           d.gpuHotspot, d.pingMs, d.diskRead);
    failures += bad;
  }

  // ---- 6. 名称包 ----
  printf("\n-- 6) 名称包 --\n");
  n = hexToBytes(GOLDEN_NAMES, buf, sizeof(buf));
  char cpu[PROTO_NAME_MAX], gpu[PROTO_NAME_MAX];
  bool nok = protoDecodeNames(buf, n, cpu, gpu);
  printf("  解析 = %s  CPU=\"%s\"  GPU=\"%s\"\n", nok ? "成功" : "失败", cpu, gpu);
  CHECK(nok, "名称包解析失败");
  CHECK(strcmp(cpu, GOLDEN_NAMES_CPU) == 0, "CPU 名不符: \"%s\"", cpu);
  CHECK(strcmp(gpu, GOLDEN_NAMES_GPU) == 0, "GPU 名不符: \"%s\"", gpu);

  // 边界情况（以前完全没测）
  {
    char c2[PROTO_NAME_MAX], g2[PROTO_NAME_MAX];
    size_t m;

    m = buildNames(buf, "AMD Ryzen 5 5600X|RX 6800 XT");
    CHECK(protoDecodeNames(buf, m, c2, g2) && strcmp(c2, "AMD Ryzen 5 5600X") == 0
          && strcmp(g2, "RX 6800 XT") == 0, "普通名称包解析失败");

    m = buildNames(buf, "  i5-12400  |  RTX 4060  ");
    CHECK(protoDecodeNames(buf, m, c2, g2) && strcmp(c2, "i5-12400") == 0
          && strcmp(g2, "RTX 4060") == 0,
          "首尾空格应被去掉，实际 CPU=\"%s\" GPU=\"%s\"", c2, g2);

    m = buildNames(buf, "ONLY-CPU");
    CHECK(protoDecodeNames(buf, m, c2, g2) && strcmp(c2, "ONLY-CPU") == 0
          && g2[0] == 0, "没有 '|' 时应只填 CPU，实际 GPU=\"%s\"", g2);

    m = buildNames(buf, "");
    CHECK(!protoDecodeNames(buf, m, c2, g2),
          "n=0（len=3）应被拒绝：固件要求 len>=4，见 proto.cpp:128");

    // 上面那条是固件的既定行为，不是缺陷：PC 端永远是 cpu + "|" + gpu，
    // 两个名字都空时文本也是 "|"（1 字节，len=4），所以 n=0 产生不出来。
    // 真正可达的"两个名字都空"是下面这种：
    m = buildNames(buf, "|");
    CHECK(protoDecodeNames(buf, m, c2, g2) && c2[0] == 0 && g2[0] == 0,
          "只有 '|' 时应解析出两个空名，实际 CPU=\"%s\" GPU=\"%s\"", c2, g2);

    // 超长名称必须被截断到 PROTO_NAME_MAX-1 且仍然有结尾 0
    m = buildNames(buf, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA|BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB");
    CHECK(protoDecodeNames(buf, m, c2, g2), "超长名称包应能解析");
    CHECK(strlen(c2) == PROTO_NAME_MAX - 1 && strlen(g2) == PROTO_NAME_MAX - 1,
          "超长名称应截断到 %d 字符，实际 CPU=%u GPU=%u",
          PROTO_NAME_MAX - 1, (unsigned)strlen(c2), (unsigned)strlen(g2));

    // CRC 坏 / 长度不够：都必须拒绝
    m = buildNames(buf, "i5-12400|RTX 4060", /*crcOk=*/false);
    CHECK(!protoDecodeNames(buf, m, c2, g2), "CRC 坏掉时不应通过");
    m = buildNames(buf, "i5-12400|RTX 4060");
    CHECK(!protoDecodeNames(buf, m - 1, c2, g2), "长度少 1 字节时不应通过");
    CHECK(!protoDecodeNames(buf, 3, c2, g2), "长度 < 4 时不应通过");
    CHECK(!protoDecodeNames(nullptr, 10, c2, g2), "空指针不应通过");
  }

  // ---- 7. 损坏检测：改一个字节 / 截断 / 错长度字段 ----
  printf("\n-- 7) 损坏检测 --\n");
  {
    HardwareData d2;
    n = hexToBytes(GOLDEN, buf, sizeof(buf));
    buf[10] ^= 0x01;
    bool badOk = protoDecodeData(buf, n, d2, seq);
    printf("  篡改一个字节 = %s（期望被拒绝）\n", badOk ? "成功(错误!)" : "被拒绝");
    CHECK(!badOk, "篡改字节后应被拒绝");

    n = hexToBytes(GOLDEN, buf, sizeof(buf));
    bool lenOk = protoDecodeData(buf, n - 1, d2, seq);
    printf("  长度少 1 字节 = %s（期望被拒绝）\n", lenOk ? "成功(错误!)" : "被拒绝");
    CHECK(!lenOk, "长度不足应被拒绝");

    n = hexToBytes(GOLDEN, buf, sizeof(buf));
    buf[2] = 60;                        // 长度字段与实际不符
    bool lfOk = protoDecodeData(buf, n, d2, seq);
    printf("  长度字段被改 = %s（期望被拒绝）\n", lfOk ? "成功(错误!)" : "被拒绝");
    CHECK(!lfOk, "长度字段不符应被拒绝");

    n = hexToBytes(GOLDEN, buf, sizeof(buf));
    buf[0] = 0xA6;                      // 包头错
    bool hdOk = protoDecodeData(buf, n, d2, seq);
    printf("  包头被改 = %s（期望被拒绝）\n", hdOk ? "成功(错误!)" : "被拒绝");
    CHECK(!hdOk, "包头不符应被拒绝");

    CHECK(!protoDecodeData(nullptr, wantLen, d2, seq), "空指针不应通过");
  }

  printf("\n%s（失败项 %d）\n",
         failures ? "==== 有失败 ====" : "==== 全部通过 ====", failures);
  return failures ? 1 : 0;
}
