# test_proto.py —— 协议金标向量的"唯一来源"：负责生成 + 自检
# ---------------------------------------------------------------------------
#  这个脚本做两件事，合起来才能让"两端一致"这件事真的被验证：
#    1) 用 PC 端编码器打包一组样本，把 HEX / 原始整数 / 期望值写进
#       tools/ui_preview/golden_vectors.h，供 C++ 侧的 proto_test.cpp #include
#    2) 按 FIELD_SCALE 的尺度做回读比对，并给出**退出码**（以前只打印不判定）
#
#  为什么必须自动生成：
#    以前这里的 HEX 是手工复制粘贴进 proto_test.cpp 的。结果是：
#      改 proto.cpp 的 SCALE           -> C++ 测试失败（好）
#      改本文件的 FIELD_SCALE          -> 完全发现不了！
#        C++ 侧还在用旧的冻结向量，照样全绿。
#    现在向量由本脚本产出、C++ 侧只负责解码比对，两端漂移必然暴露。
#
#  用法：
#    python -B pc_side/test_proto.py           # 重新生成头文件 + 自检
#    python -B pc_side/test_proto.py --check   # 只校验磁盘上的头文件是否最新（给 CI 用）
# ---------------------------------------------------------------------------
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from proto_v2 import (pack_binary, pack_names, unpack_binary, crc8, pack_field,
                      PROTO_FIELD_COUNT, FIELD_KEYS, FIELD_SCALE, PROTO_INVALID,
                      PROTO_NAME_MAX, PROTO_NAME_BYTES_MAX)

HERE = os.path.dirname(os.path.abspath(__file__))
HEADER_PATH = os.path.normpath(
    os.path.join(HERE, '..', 'tools', 'ui_preview', 'golden_vectors.h'))

SEED = 7            # 主包序号
SEED_INVALID = 8    # 哨兵包序号

# 一组覆盖面广的样本值（含 0、小数、大数、以及"读不到"的 None）
SAMPLE = {
    'cpu_load': 63.4, 'cpu_temp': 57.2, 'cpu_fan': 1187, 'cpu_power': 88.9,
    'gpu_load': 41.8, 'gpu_temp': 62.5, 'gpu_mem': 8.24, 'gpu_fan': 1503,
    'gpu_power': 173.2, 'fps': 141, 'mem_used': 47.6, 'hdd_temp': 38.5,
    'gpu_mem_total': 12.0, 'cpu_clk': 4298,
    'cpu_volt': 1.264, 'cpu_core_max': 91.3, 'gpu_hotspot': 78.4,
    'gpu_mem_temp': 66.1, 'gpu_clk': 2535, 'gpu_mem_ctrl': 33.7,
    'mobo_fan': 940,
    'disk_read': 0.0, 'disk_write': 12.34, 'disk_used': 71.2,
    'net_down': 3.07, 'net_up': 0.42, 'ping_ms': 18.6, 'ping_loss': 0.0,
    'proc_count': 287, 'uptime_min': 5123, 'frame_ms': 7.09,
    'fps_low': 41,                                  # 1% low（追加字段）
}

# 哨兵包：gpu_hotspot / ping_ms / fps_low "读不到"，disk_read 是合法 0
SAMPLE_WITH_INVALID = dict(SAMPLE)
SAMPLE_WITH_INVALID['gpu_hotspot'] = None
SAMPLE_WITH_INVALID['ping_ms'] = None
SAMPLE_WITH_INVALID['fps_low'] = None

NAMES_CPU = 'i5-12400'
NAMES_GPU = 'RTX 4060'

fails = []


def check(name, cond, extra=''):
    print(f"  [{'OK ' if cond else '!! '}] {name} {extra}")
    if not cond:
        fails.append(name)


def tol_of(idx: int) -> float:
    """该字段允许的误差 = 半个最小刻度。

    这就是"容差按缩放来"的意思：x100 的字段一格是 0.01，允差只能是 0.005；
    以前两端都硬编码 0.06，于是 net_down 从 x100 错成 x10（3.07 -> 3.1，差 0.03）
    也能通过 —— 恰好漏掉最该抓的那类错误。
    """
    return 0.5 / FIELD_SCALE[idx] + 1e-3


def cfloat(v) -> str:
    """把期望值写成 C++ float 字面量"""
    if v is None:
        return '-1.0f'
    s = repr(float(v))
    if 'e' in s or 'E' in s:            # repr 用了科学计数法就退回定点
        s = f'{float(v):.6f}'
    if '.' not in s:
        s += '.0'
    return s + 'f'


def build_header() -> str:
    """用 PC 端编码器产出头文件全文（纯函数：--check 也走这里，保证两条路径同源）"""
    pkt = pack_binary(SAMPLE, seq=SEED)
    pkt_bad = pack_binary(SAMPLE_WITH_INVALID, seq=SEED_INVALID)
    names = pack_names(NAMES_CPU, NAMES_GPU)

    raw_ok = [pack_field(SAMPLE.get(k), i) for i, k in enumerate(FIELD_KEYS)]
    raw_bad = [pack_field(SAMPLE_WITH_INVALID.get(k), i) for i, k in enumerate(FIELD_KEYS)]

    L = []
    L.append('// ============================================================================')
    L.append('//  golden_vectors.h —— 协议金标向量（**由 pc_side/test_proto.py 自动生成**）')
    L.append('// ----------------------------------------------------------------------------')
    L.append('//  不要手工编辑这个文件：改动会在下次运行 test_proto.py 时被覆盖。')
    L.append('//')
    L.append('//  重新生成：')
    L.append('//      python -B pc_side/test_proto.py')
    L.append('//  只校验是否最新（不改文件，给 CI / build.ps1 用）：')
    L.append('//      python -B pc_side/test_proto.py --check')
    L.append('//')
    L.append('//  这些 HEX 是 PC 端编码器（proto_v2.py）的真实输出；proto_test.cpp 用固件')
    L.append('//  解码器（proto.cpp）解它，再和下面的 GOLDEN_RAW / GOLDEN_EXPECT 比对。')
    L.append('//  于是"两端字段顺序 + 缩放表是否一致"变成一个可自动判定的事实，')
    L.append('//  而不是靠人把 HEX 复制粘贴过去（那样 PC 端改错不会被发现）。')
    L.append('// ============================================================================')
    L.append('#ifndef GOLDEN_VECTORS_H')
    L.append('#define GOLDEN_VECTORS_H')
    L.append('')
    L.append(f'#define GOLDEN_FIELD_COUNT {PROTO_FIELD_COUNT}')
    L.append(f'#define GOLDEN_SEQ {SEED}')
    L.append(f'#define GOLDEN_INVALID_SEQ {SEED_INVALID}')
    L.append('')
    L.append('// 主包：全部字段都有值')
    L.append(f'static const char *GOLDEN = "{pkt.hex().upper()}";')
    L.append('')
    L.append('// 哨兵包：gpu_hotspot / ping_ms 读不到（raw 0xFFFF），disk_read 是合法 0')
    L.append(f'static const char *GOLDEN_INVALID = "{pkt_bad.hex().upper()}";')
    L.append('')
    L.append('// 名称包："CPU型号|GPU型号"')
    L.append(f'static const char *GOLDEN_NAMES = "{names.hex().upper()}";')
    L.append(f'static const char *GOLDEN_NAMES_CPU = "{NAMES_CPU}";')
    L.append(f'static const char *GOLDEN_NAMES_GPU = "{NAMES_GPU}";')
    L.append('')
    L.append('// 原始 uint16（编码器定标后的值），顺序与 FIELD_KEYS / PROTOCOL.md 字段表一致。')
    L.append('// 这是最紧的断言：固件解出来的 raw 必须逐个等于它，缩放或顺序错一格都会红。')
    L.append('static const unsigned short GOLDEN_RAW[GOLDEN_FIELD_COUNT] = {')
    L.extend(_emit_u16_array(raw_ok))
    L.append('};')
    L.append('static const unsigned short GOLDEN_INVALID_RAW[GOLDEN_FIELD_COUNT] = {')
    L.extend(_emit_u16_array(raw_bad))
    L.append('};')
    L.append('')
    L.append('// 解码后的浮点期望值（哨兵为 -1.0f）。容差由 protoFieldScale() 推出：')
    L.append('// 半个最小刻度，见 proto_test.cpp 的 tolOf()。')
    L.append('static const float GOLDEN_EXPECT[GOLDEN_FIELD_COUNT] = {')
    L.extend(_emit_f32_array([SAMPLE.get(k) for k in FIELD_KEYS]))
    L.append('};')
    L.append('static const float GOLDEN_INVALID_EXPECT[GOLDEN_FIELD_COUNT] = {')
    L.extend(_emit_f32_array([SAMPLE_WITH_INVALID.get(k) for k in FIELD_KEYS]))
    L.append('};')
    L.append('')
    L.append('#endif  // GOLDEN_VECTORS_H')
    L.append('')
    return '\n'.join(L)


def _emit_u16_array(vals):
    """每行 10 个，附字段名注释"""
    out = []
    for i in range(0, len(vals), 10):
        chunk = vals[i:i + 10]
        body = ', '.join(f'{v:5d}' for v in chunk)
        names = ', '.join(FIELD_KEYS[j] for j in range(i, min(i + 10, len(vals))))
        out.append(f'  {body},   // {names}')
    return out


def _emit_f32_array(vals):
    out = []
    for i in range(0, len(vals), 5):
        chunk = vals[i:i + 5]
        body = ', '.join(cfloat(v) for v in chunk)
        names = ', '.join(FIELD_KEYS[j] for j in range(i, min(i + 5, len(vals))))
        out.append(f'  {body},   // {names}')
    return out


def self_check():
    """用 PC 端自己的解码器回读一遍，按缩放尺度判定"""
    print('=== 1) 打包 / 回读（容差 = 半个最小刻度）===')
    pkt = pack_binary(SAMPLE, seq=SEED)
    print(f'  字段数={PROTO_FIELD_COUNT}  主包长度={len(pkt)} 字节')
    _want = 3 + PROTO_FIELD_COUNT * 2 + 1     # 包头+seq+len + 载荷 + CRC
    check(f'主包 {_want} 字节', len(pkt) == _want, f'实际 {len(pkt)}')
    check('序号正确', pkt[1] == SEED, f'实际 {pkt[1]}')

    back = unpack_binary(pkt)
    for i, k in enumerate(FIELD_KEYS):
        want = SAMPLE.get(k)
        got = back[k]
        if want is None:
            continue
        tol = tol_of(i)
        # got 为 None 时不能直接减：以前 abs(None - want) 会抛 TypeError，
        # 测试是"崩掉"而不是"FAIL"，看起来像测试本身坏了。
        check(f'{k}: {got} ≈ {want} (tol {tol:g})',
              got is not None and abs(got - want) <= tol)

    print('\n=== 2) 哨兵与合法 0 ===')
    back2 = unpack_binary(pack_binary(SAMPLE_WITH_INVALID, seq=SEED_INVALID))
    check('gpu_hotspot -> 哨兵', back2['gpu_hotspot'] is None, f'{back2["gpu_hotspot"]}')
    check('ping_ms -> 哨兵', back2['ping_ms'] is None, f'{back2["ping_ms"]}')
    check('disk_read 保留合法 0', back2['disk_read'] == 0.0, f'{back2["disk_read"]}')

    print('\n=== 3) CRC-8/ATM 已知向量 ===')
    c = crc8(b'123456789')
    check("CRC-8/ATM('123456789') = 0xF4", c == 0xF4, f'实际 0x{c:02X}')

    print('\n=== 4) 缩放表与 PROTOCOL.md 的护栏 ===')
    check('FIELD_KEYS 与 FIELD_SCALE 长度一致',
          len(FIELD_KEYS) == len(FIELD_SCALE) == PROTO_FIELD_COUNT)
    check('缩放只允许 1/10/100', all(s in (1, 10, 100) for s in FIELD_SCALE))
    check('缩放 >= 10 的字段容差都比旧的硬编码 0.06 更紧',
          all(tol_of(i) < 0.06 for i in range(PROTO_FIELD_COUNT) if FIELD_SCALE[i] >= 10),
          f'最大 {max(tol_of(i) for i in range(PROTO_FIELD_COUNT) if FIELD_SCALE[i] >= 10):g}')

    # 具体演示旧容差为什么抓不到"两端一起把缩放改退化"：
    # PROTOCOL.md 规定 net_down 是 x100（0.01 MB/s 一格，空闲流量才看得出变化）。
    # 如果两端都错用 x10：3.07 -> raw=31 -> 解码 3.1，误差 0.03。
    #   0.03 < 0.06（旧的硬编码容差：放过）   0.03 > 0.006（x100 的新容差：抓住）
    i = FIELD_KEYS.index('net_down')
    check('net_down 缩放应为 x100（PROTOCOL.md 规定，固件 proto.cpp 同值）',
          FIELD_SCALE[i] == 100, f'实际 x{FIELD_SCALE[i]}')
    want = SAMPLE['net_down']
    degraded = round(want * 10) / 10.0        # 两端都错用 x10 时的解码结果
    err = abs(degraded - want)
    new_tol = 0.5 / 100 + 1e-3
    check(f'缩放退化解落在"旧容差放过、新容差抓住"的窗口（得 {degraded}，误差 {err:g}）',
          new_tol < err < 0.06,
          f'x100 新容差 {new_tol:g} < 误差 {err:g} < 旧容差 0.06')

    print('\n=== 5) 脏数据不能把整包打挂 ===')
    # inf 以前会一路走到 int(round(inf)) 抛 OverflowError：那一轮**一包都发不出去**，
    # 屏幕上整片变 --，日志里却是每秒一条的全栈（看着像程序崩了）。
    i_net = FIELD_KEYS.index('net_down')
    for bad, name in ((float('inf'), '+inf'), (float('-inf'), '-inf'), (float('nan'), 'NaN')):
        try:
            raw = pack_field(bad, i_net)
            check(f'{name} -> 哨兵（不抛异常）', raw == PROTO_INVALID, f'raw={raw}')
        except Exception as e:
            check(f'{name} -> 哨兵（不抛异常）', False, f'{type(e).__name__}: {e}')
    check('超大但有限的值撞量程上限，不是哨兵',
          pack_field(1e30, i_net) == PROTO_INVALID - 1,
          f'raw={pack_field(1e30, i_net)}')

    print('\n=== 6) 名称包按字符截断（不能切断多字节字符）===')
    # 以前是 (cpu + '|' + gpu).encode('utf-8')[:120]：按字节切会把中文型号名切成
    # 非法 UTF-8；切在 '|' 前面时固件找不到分隔符，GPU 名整个丢掉。
    long_cpu = '锐龙' * 30          # 远超 23 字符，且是多字节
    long_gpu = '影驰' * 30
    pkt = pack_names(long_cpu, long_gpu)
    text = pkt[2:-1]
    check('名称包长度字段与实际内容一致', pkt[1] == len(text), f'{pkt[1]} vs {len(text)}')
    try:
        decoded = text.decode('utf-8')
        ok_utf8 = True
    except UnicodeDecodeError:
        decoded, ok_utf8 = '', False
    check('内容是合法 UTF-8（没被从字符中间切断）', ok_utf8, repr(decoded[:20]))
    check("分隔符 '|' 还在（否则固件会把 GPU 名整个丢掉）", '|' in decoded, repr(decoded))
    if '|' in decoded:
        c, g = decoded.split('|', 1)
        check(f'CPU 名压到 {PROTO_NAME_MAX - 1} 字符以内', len(c) <= PROTO_NAME_MAX - 1, f'{len(c)}')
        check(f'GPU 名压到 {PROTO_NAME_MAX - 1} 字符以内', len(g) <= PROTO_NAME_MAX - 1, f'{len(g)}')
    check('名称包不超过一个 BLE 写的上限（超过就会碎包，固件收不到）',
          len(text) <= PROTO_NAME_BYTES_MAX, f'{len(text)} 字节')
    # 只按"每边 23 字符"留是 139 字节，超出 MTU 128 的有效载荷 —— 这条断言就是
    # 防止有人把字节上限当成多余的限制又去掉。
    check('中文型号名（每边 30 个汉字）确实触发了收紧', len(text) < 139, f'{len(text)} 字节')


def main():
    check_only = '--check' in sys.argv
    text = build_header()

    if check_only:
        print('=== 校验 golden_vectors.h 是否为最新 ===')
        try:
            with open(HEADER_PATH, 'r', encoding='utf-8') as f:
                on_disk = f.read()
        except FileNotFoundError:
            print(f'  [!! ] 文件不存在: {HEADER_PATH}')
            print('  运行 python -B pc_side/test_proto.py 生成它')
            sys.exit(1)
        if on_disk == text:
            print(f'  [OK ] 已是最新: {HEADER_PATH}')
            sys.exit(0)
        print(f'  [!! ] 磁盘上的头文件已过期: {HEADER_PATH}')
        print('  说明协议（FIELD_SCALE / FIELD_KEYS）改过但没重新生成。')
        print('  运行 python -B pc_side/test_proto.py 重新生成。')
        sys.exit(1)

    # 先把旧内容读出来，用于报告"是否发生了变化"
    old = None
    try:
        with open(HEADER_PATH, 'r', encoding='utf-8') as f:
            old = f.read()
    except FileNotFoundError:
        pass

    os.makedirs(os.path.dirname(HEADER_PATH), exist_ok=True)
    with open(HEADER_PATH, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)

    print('=== 0) 生成金标向量 ===')
    state = '新建' if old is None else ('未变化' if old == text else '**已更新**')
    print(f'  [OK ] {HEADER_PATH}  ({state})')
    if old is not None and old != text:
        print('  注意：头文件变了，说明协议表或缩放动过 —— proto_test.cpp 必须重新编译。')

    print()
    self_check()

    print()
    if fails:
        print(f'==== 有失败（{len(fails)} 项）: {", ".join(fails)} ====')
        sys.exit(1)
    print('==== 全部通过 ====')
    sys.exit(0)


if __name__ == '__main__':
    main()
