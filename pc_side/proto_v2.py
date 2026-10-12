# proto_v2.py —— PC 侧协议实现（与固件 proto.cpp / PROTOCOL.md 一一对应）
# 只依赖 struct，方便离线单测（不 import psutil/bleak/clr）
import struct

PROTO_FIELD_COUNT = 32
PROTO_PKT_DATA = 0xA5
PROTO_PKT_NAMES = 0xB1
PROTO_INVALID = 0xFFFF

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
    """把一个值定标成 uint16；None / 负数 / NaN -> 无效哨兵 0xFFFF"""
    if value is None:
        return PROTO_INVALID
    try:
        v = float(value)
    except (TypeError, ValueError):
        return PROTO_INVALID
    if v != v or v < 0:                      # NaN 或负数
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
    """名称包：0xB1 + len + 'CPU|GPU' + CRC8"""
    text = (cpu + "|" + gpu).encode('utf-8')[:120]
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
