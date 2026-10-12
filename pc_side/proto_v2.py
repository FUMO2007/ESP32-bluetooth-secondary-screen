# proto_v2.py —— PC 侧协议实现（与固件 proto.cpp / PROTOCOL.md 一一对应）
# 只依赖 struct/math，方便离线单测（不 import psutil/bleak/clr）
import math
import struct

PROTO_FIELD_COUNT = 32
PROTO_PKT_DATA = 0xA5
PROTO_PKT_NAMES = 0xB1
PROTO_INVALID = 0xFFFF
# 与固件 PC_Monitor/proto.h 的 PROTO_NAME_MAX 一致：23 个可见字符 + 结尾 NUL
PROTO_NAME_MAX = 24
# 名称包文本的**字节**上限。BLE MTU 128，ATT 开销 3 字节，所以一次写最多约 124 字节；
# 旧的 120 就是按这个来的（120 + 包头 + 长度 + CRC = 123）。超过它就得靠协议栈拆成
# 多个包（"碎包"），而固件是按整包解析的 —— 名称会直接丢掉。
PROTO_NAME_BYTES_MAX = 120

# 字段顺序（必须与 PROTOCOL.md 一致）
FIELD_KEYS = [
    'cpu_load', 'cpu_temp', 'cpu_fan', 'cpu_power',
    'gpu_load', 'gpu_temp', 'gpu_mem', 'gpu_fan', 'gpu_power',
    'fps', 'mem_used', 'hdd_temp', 'gpu_mem_total', 'cpu_clk',
    'cpu_volt', 'cpu_core_max', 'gpu_hotspot', 'gpu_mem_temp', 'gpu_clk',
    'gpu_mem_ctrl', 'mobo_fan',
    'disk_read', 'disk_write', 'disk_used',
    'net_down', 'net_up', 'ping_ms', 'ping_loss',
    'proc_count', 'uptime_min', 'frame_ms',
    # ---- 追加字段一律加在**最后** ----
    # 这样前面所有字段的索引和缩放都不动，两端升级时旧字段不会错位。
    'fps_low',
]

# 每个字段的缩放（与固件 proto.cpp 的 SCALE 表一致）
# 网络速率两个字段用 x100（一格 = 0.01 MB/s = 10 KB/s）：
#   空闲时网络只有几~几十 KB/s，用 x10（0.1 MB/s 一格）会被 round 成 raw=0，
#   屏幕上永远显示 0.0，看着像没数据。x100 的代价是上限 655 MB/s（够用）。
FIELD_SCALE = [
    10, 10, 1, 10, 10, 10, 100, 1, 10, 1, 10, 10, 100, 1, 100,
    10, 10, 10, 1, 10, 1, 10, 10, 10, 100, 100, 10, 10, 1, 1, 100,
    1,      # fps_low：1% low 帧率，同 fps 用 x1
]

assert len(FIELD_KEYS) == PROTO_FIELD_COUNT
assert len(FIELD_SCALE) == PROTO_FIELD_COUNT


def crc8(data: bytes) -> int:
    """CRC-8/ATM：多项式 0x07、初值 0x00、不反转、不异或输出"""
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if (crc & 0x80) else ((crc << 1) & 0xFF)
    return crc


def pack_field(value, idx: int) -> int:
    """把一个值定标成 uint16；None / 负数 / NaN / ±inf -> 无效哨兵 0xFFFF"""
    if value is None:
        return PROTO_INVALID
    try:
        v = float(value)
    except (TypeError, ValueError):
        return PROTO_INVALID
    # 必须用 isfinite：`v != v` 只认得出 NaN，而 int(round(inf)) 会抛 OverflowError。
    # 它抛出的位置是每轮采集的打包路径 —— 一旦有 inf，这一轮**一包都发不出去**，
    # 屏幕上整片变 --，而日志里是每秒一条的全栈（看着像崩溃，其实是脏数据）。
    # （协方差/除法类的派生量在分母极小或数据为 0 时确实可能算出 inf。）
    if not math.isfinite(v) or v < 0:
        return PROTO_INVALID
    raw = int(round(v * FIELD_SCALE[idx]))
    if raw > PROTO_INVALID - 1:
        raw = PROTO_INVALID - 1              # 0xFFFF 留给哨兵
    return raw


def pack_binary(data: dict, seq: int) -> bytes:
    """主包：0xA5 + seq + len + 31×uint16(小端) + CRC8"""
    payload = bytearray()
    for i, key in enumerate(FIELD_KEYS):
        payload += struct.pack('<H', pack_field(data.get(key), i))
    body = bytes([PROTO_PKT_DATA, seq & 0xFF, len(payload)]) + bytes(payload)
    return body + bytes([crc8(body)])


def pack_names(cpu: str, gpu: str) -> bytes:
    """名称包：0xB1 + len + 'CPU|GPU' + CRC8

    两条约束同时成立，缺一个都会出问题：
      1. **按字符**截断，不按字节。以前是 `(cpu + "|" + gpu).encode('utf-8')[:120]`：
         多字节字符（中文型号名）会被从中间切断，产生非法 UTF-8；切在 '|' 前面时
         固件按分隔符拆包会找不到它，GPU 名整个丢掉。
      2. 总长度仍要放进**一个 BLE 写**（≤ PROTO_NAME_BYTES_MAX）。只按 23 字符 ×
         2 边留是 139 字节，超出 MTU 128 的有效载荷 —— 那样会碎包，固件收不到。
    """
    cpu = cpu[:PROTO_NAME_MAX - 1]
    gpu = gpu[:PROTO_NAME_MAX - 1]

    def _text(a, b):
        return (a + "|" + b).encode('utf-8')

    # 超长时从长的那一边削字符（'|' 与两边都不会被削没）
    while len(_text(cpu, gpu)) > PROTO_NAME_BYTES_MAX and (cpu or gpu):
        if len(cpu.encode('utf-8')) >= len(gpu.encode('utf-8')) and cpu:
            cpu = cpu[:-1]
        elif gpu:
            gpu = gpu[:-1]
        else:
            break
    text = _text(cpu, gpu)
    assert len(text) <= PROTO_NAME_BYTES_MAX, len(text)
    body = bytes([PROTO_PKT_NAMES, len(text)]) + text
    return body + bytes([crc8(body)])


def unpack_binary(buf: bytes) -> dict:
    """仅测试用：把主包解回字典（固件不这么做，它按标量取用）"""
    assert len(buf) == 3 + PROTO_FIELD_COUNT * 2 + 1, f"长度不对: {len(buf)}"
    assert buf[0] == PROTO_PKT_DATA, "包头不对"
    assert buf[2] == PROTO_FIELD_COUNT * 2, "长度字段不对"
    assert crc8(buf[:-1]) == buf[-1], "CRC 不对"
    out = {}
    for i, key in enumerate(FIELD_KEYS):
        raw = struct.unpack_from('<H', buf, 3 + i * 2)[0]
        out[key] = None if raw == PROTO_INVALID else raw / FIELD_SCALE[i]
    return out
