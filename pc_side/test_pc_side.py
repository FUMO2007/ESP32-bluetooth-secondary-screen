# test_pc_side.py —— 在没装 LibreHardwareMonitor/RTSS/BLE 的机器上，尽量多地验证 PC 端逻辑
# 用法： <venv python> -B pc_side/test_pc_side.py
# 说明：把 sys.platform 伪装成 linux，跳过"隐藏控制台"和 WinDLL 相关分支，
#       只验证纯逻辑：型号名缩短、定标打包、往返、CSV 兼容、以及采集函数能否跑通。
import os
import sys
import types

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ['PCMON_NO_HIDE'] = '1'      # 不要把我们自己的测试终端藏掉

fails = []


def check(name, cond, extra=""):
    print(f"  [{'OK ' if cond else '!! '}] {name} {extra}")
    if not cond:
        fails.append(name)


print("=== 1) 型号名缩短 ===")
try:
    import pc_monitor_v2 as m
except Exception as e:          # 缺 bleak/psutil 时给出清晰提示
    print(f"  导入失败（缺依赖？）: {e}")
    sys.exit(2)

cases = [
    ("12th Gen Intel(R) Core(TM) i5-12400", "i5-12400"),
    ("AMD Ryzen 5 5600X 6-Core Processor", "Ryzen 5 5600X"),
    ("Intel(R) Core(TM) Ultra 7 155H", "Ultra 7 155H"),
    ("NVIDIA GeForce RTX 4060", "RTX 4060"),
    # "Ti SUPER" 必须完整保留：以前的正则只写到 (Ti|SUPER)?，匹配到 "RTX 4070 Ti"
    # 就停了 —— 屏幕上认不出这是更强的 SUPER 版，而 4070 Ti 与 4070 Ti SUPER
    # 是两张性能不同的卡。这条期望值以前把错误行为固化了下来。
    ("NVIDIA GeForce RTX 4070 Ti SUPER", "RTX 4070 Ti SUPER"),
    ("NVIDIA GeForce RTX 4070 SUPER", "RTX 4070 SUPER"),
    ("NVIDIA GeForce RTX 4070 Ti", "RTX 4070 Ti"),
    ("AMD Radeon RX 6800 XT", "RX 6800 XT"),
    ("Intel(R) Arc(TM) A770 Graphics", "Arc A770"),
]
for raw, want in cases:
    got = m.short_name(raw)
    check(f"{raw!r} -> {got!r}", got == want, f"(期望 {want!r})")

print("\n=== 2) 采集函数（无硬件时应全部为 None，不崩）===")
mon = object.__new__(m.LibreHardwareMonitor)     # 不走 __init__（避免真去开 LHM）
mon._init_state()                                # 复用被测代码自己的初值，不再手工罗列
mon.rtss = None                                  # 单测里不碰 RTSS 共享内存
mon.disk_read = m.RateMeter(lambda: 0)           # 固定返回值，避免真去读磁盘/网卡
mon.disk_write = m.RateMeter(lambda: 0)
mon.net_down = m.RateMeter(lambda: 0)
mon.net_up = m.RateMeter(lambda: 0)

try:
    data = mon.collect_all_data()
    # 不写死 31：字段数是协议的核心参数，写死的话每次加字段都要来改测试，
    # 而漏改的结果是"测试失败但看起来像功能坏了"。直接引用常量。
    check(f"collect_all_data 返回 {m.PROTO_FIELD_COUNT} 个字段",
          len(data) == m.PROTO_FIELD_COUNT, f"实际 {len(data)}")
    missing = [k for k in m.FIELD_KEYS if k not in data]
    check("字段名与协议表一致", not missing, f"缺 {missing}")
    check("无硬件时 cpu_temp 为 None", data["cpu_temp"] is None)
except Exception as e:
    check(f"collect_all_data 不抛异常（{e}）", False)

print("\n=== 3) 打包往返（含哨兵与合法 0）===")
from proto_v2 import pack_binary as pb, unpack_binary as ub
sample = dict(data)
sample.update({'cpu_load': 12.3, 'disk_read': 0.0, 'net_down': 1.5, 'gpu_hotspot': None})
pkt = mon.pack_binary(sample, 42)
_want_pkt = 3 + m.PROTO_FIELD_COUNT * 2 + 1     # 包头+seq+len + 载荷 + CRC
check(f"主包 {_want_pkt} 字节", len(pkt) == _want_pkt, f"实际 {len(pkt)}")
back = ub(pkt)
check("seq 正确", pkt[1] == 42)
check("cpu_load 往返", abs(back['cpu_load'] - 12.3) < 0.05, f"{back['cpu_load']}")
check("合法 0 保留", back['disk_read'] == 0.0, f"{back['disk_read']}")
check("None 变哨兵", back['gpu_hotspot'] is None)

print("\n=== 4) CSV 兼容路径 ===")
csv = mon.format_for_esp32(sample)
check("CSV 是 14 段", len(csv.split(',')) == 14, f"实际 {len(csv.split(','))}")
check("CSV 无读到值时为 0", csv.split(',')[1] == '0.0', f"{csv.split(',')[1]}")

print("\n=== 5) 名称包 ===")
mon.cpu_name = "12th Gen Intel(R) Core(TM) i5-12400"
mon.gpu_name = "NVIDIA GeForce RTX 4060"
np = mon.pack_names()
check("名称包可解出缩短后的型号",
      np[2:-1].decode() == "i5-12400|RTX 4060", np[2:-1].decode())

print("\n=== 5b) 握手包：必须是全无效，不得出现编造的读数 ===")
# 回归用：这里以前发的是一组手写的"演示值"（cpu_load=50%、cpu_temp=45°C、fps=60…），
# 连接/重连时屏幕上会闪一帧假读数 —— 和"拿不到就显示 --"的原则冲突。
import proto_v2
hp = mon.pack_test()
check("握手包 68 字节", len(hp) == 68, f"实际 {len(hp)}")
handshake = proto_v2.unpack_binary(hp)
bogus = {k: v for k, v in handshake.items() if v is not None}
check("握手包所有字段都是无效哨兵（屏幕只会显示 --）", not bogus,
      f"非无效字段 {bogus}")

print("\n=== 6) PresentMon CSV 解析（纯逻辑，不需要装 PresentMon）===")

# 列的顺序和数量都故意跟"最常见的那种"不一样：解析器必须按**列名**找，
# 不能按列号 —— PresentMon 的默认列集每个版本都在变（2.6.0 就加了
# VidPnSourceId / LayerIndex / PresentId）。下面第二张表用来验证这一点。
PM_HEADER_OLD = 'Application,ProcessID,PresentMode,MsBetweenPresents,DisplayedTime'
PM_HEADER_V26 = ('FrameType,Application,ProcessID,VidPnSourceId,LayerIndex,PresentId,'
                 'MsBetweenPresents,MsGPUBusy')


def pm_row(header, **kw):
    """按给定表头拼一行数据，缺的列填 0"""
    cols = header.split(',')
    vals = []
    for c in cols:
        vals.append(str(kw.get(c, 0)))
    return ','.join(vals)


# ---- 6.1 表头之前的噪声（实测 PresentMon 会先打一段多行 warning）----
csvp = m.PresentMonCsv()
noise = [
    'warning: PresentMon requires elevated privilege in order to query processes that are',
    '         short-running or started on another account.  Without it, those processes will',
    '         be listed as \'<unknown>\' and they can\'t be targeted by --process_name nor trigger',
    '         --terminate_on_proc_exit.',
    'error: failed to start trace session: access denied.',
    '',
    '\ufeff' + PM_HEADER_OLD,          # 带 BOM 的表头
]
for ln in noise:
    csvp.feed(ln)
check('能在多行 warning / error 之后认到表头', csvp.header_seen)

# ---- 6.2 正常帧：1000/16.67ms ≈ 60fps ----
for _ in range(60):
    csvp.feed(pm_row(PM_HEADER_OLD, Application='game.exe', ProcessID=4242,
                     MsBetweenPresents='16.6667'))
fps, fms = csvp.read(foreground_pid=4242)
check('60fps 场景算出 60fps', fps is not None and abs(fps - 60.0) < 0.1,
      f'fps={fps}')
check('frame_ms 等于窗口内的平均帧间隔',
      fms is not None and abs(fms - 16.6667) < 0.01, f'frame_ms={fms}')

# ---- 6.3 'NA' 必须被跳过（每个进程的第一帧没有"上一帧"）----
csvp2 = m.PresentMonCsv()
csvp2.feed(PM_HEADER_OLD)
check("'NA' 行被跳过", csvp2.feed(pm_row(PM_HEADER_OLD, ProcessID=1,
                                          MsBetweenPresents='NA')) is False)
check('正常行被接受', csvp2.feed(pm_row(PM_HEADER_OLD, ProcessID=1,
                                        MsBetweenPresents='10.0')) is True)

# ---- 6.4 只有一帧时不出数（至少两帧才能算间隔）----
fps1, _ = csvp2.read(foreground_pid=1)
check('只有 1 帧时返回 None（不瞎猜）', fps1 is None, f'fps={fps1}')

# ---- 6.5 2.6.0 风格的表头（列更多、顺序不同）----
csvp3 = m.PresentMonCsv()
csvp3.feed(PM_HEADER_V26)
for _ in range(30):
    csvp3.feed(pm_row(PM_HEADER_V26, Application='a.exe', ProcessID=7,
                      VidPnSourceId=1, LayerIndex=0, PresentId=99,
                      MsBetweenPresents='8.3333', MsGPUBusy='5.1'))
fps3, fms3 = csvp3.read(foreground_pid=7)
check('列序/列数变了也照样解析（按列名找）',
      fps3 is not None and abs(fps3 - 120.0) < 0.5, f'fps={fps3}')

# ---- 6.6 进程筛选：可见窗口判据 + 排除 shell ----
# 这几条全部来自真机实测：一次 15 秒采样里同时出现了
#   dwm.exe 71.6fps / Endfield.exe 64.1fps / wallpaper64 29.8fps /
#   HYPHelper.exe 357.3fps（无窗口）/ WindowsTerminal.exe 3.4fps（前台）
# 第一版规则"前台优先，否则取最高"于是让屏幕在 3 / 64 / 357 之间乱跳。
csvp4 = m.PresentMonCsv()
csvp4.feed(PM_HEADER_OLD)
for _ in range(10):
    csvp4.feed(pm_row(PM_HEADER_OLD, Application='Endfield.exe', ProcessID=200,
                      MsBetweenPresents='15.6'))     # 64fps，有可见窗口
    csvp4.feed(pm_row(PM_HEADER_OLD, Application='dwm.exe', ProcessID=100,
                      MsBetweenPresents='14.0'))     # 71fps，合成器
    csvp4.feed(pm_row(PM_HEADER_OLD, Application='HYPHelper.exe', ProcessID=300,
                      MsBetweenPresents='2.8'))      # 357fps，无可见窗口

fps4, _ = csvp4.read(allow_pids={200}, foreground_pid=200)
check('只看有可见窗口的进程（357fps 的无窗口 helper 被排除）',
      fps4 is not None and abs(fps4 - 64.1) < 1.0, f'fps={fps4}（应为 64.1）')

fps4b, _ = csvp4.read(allow_pids={200, 100, 300}, foreground_pid=0)
check('dwm.exe(合成器) 即使帧率更高也按名字排除',
      fps4b is not None and abs(fps4b - 357.1) < 2.0,
      f'fps={fps4b}（dwm 71fps 被排除，剩下 helper 357fps 最高）')

# 这一条是第一版 bug 的直接回归测试：终端抢到前台时不能把它的 3fps 当帧率
csvp4c = m.PresentMonCsv()
csvp4c.feed(PM_HEADER_OLD)
for _ in range(10):
    csvp4c.feed(pm_row(PM_HEADER_OLD, Application='WindowsTerminal.exe', ProcessID=400,
                       MsBetweenPresents='300.0'))   # 3fps，但**在前台**
    csvp4c.feed(pm_row(PM_HEADER_OLD, Application='Endfield.exe', ProcessID=500,
                       MsBetweenPresents='15.6'))    # 64fps
fps4c, _ = csvp4c.read(allow_pids={400, 500}, foreground_pid=400)
check('前台是终端(3fps) 时不再误报 3fps，仍取游戏(64fps)',
      fps4c is not None and abs(fps4c - 64.1) < 1.0, f'fps={fps4c}（应为 64.1）')

# 帧率打平时才用前台决胜
csvp4d = m.PresentMonCsv()
csvp4d.feed(PM_HEADER_OLD)
for _ in range(10):
    csvp4d.feed(pm_row(PM_HEADER_OLD, Application='a.exe', ProcessID=600,
                       MsBetweenPresents='16.6667'))
    csvp4d.feed(pm_row(PM_HEADER_OLD, Application='b.exe', ProcessID=700,
                       MsBetweenPresents='16.6667'))
pid4d, _, _ = csvp4d.pick(allow_pids={600, 700}, foreground_pid=700)
check('帧率打平时用前台 pid 决胜', pid4d == 700, f'选中 pid={pid4d}（应为 700）')

# ---- 6.7 窗口只保留最近 N 帧 ----
csvp5 = m.PresentMonCsv(window=10)
csvp5.feed(PM_HEADER_OLD)
for _ in range(50):
    csvp5.feed(pm_row(PM_HEADER_OLD, ProcessID=5, MsBetweenPresents='100.0'))  # 10fps
for _ in range(10):
    csvp5.feed(pm_row(PM_HEADER_OLD, ProcessID=5, MsBetweenPresents='10.0'))   # 100fps
fps5, _ = csvp5.read(foreground_pid=5)
check('窗口只算最近 N 帧（旧样本会滑出）',
      fps5 is not None and abs(fps5 - 100.0) < 0.5, f'fps={fps5}（应为 100）')

# ---- 6.8 进程名里带逗号（走 csv 模块而不是 split(',')）----
csvp6 = m.PresentMonCsv()
csvp6.feed(PM_HEADER_OLD)
csvp6.feed('"weird,name.exe",9,Composed: Flip,16.0,NA')
csvp6.feed('"weird,name.exe",9,Composed: Flip,16.0,NA')
check('进程名含逗号时不会被拆错列', csvp6.rows == 2, f'接受帧数={csvp6.rows}')

# ---- 6.9 没有任何帧时不报数 ----
csvp7 = m.PresentMonCsv()
csvp7.feed(PM_HEADER_OLD)
fps7, fms7 = csvp7.read()
check('一帧都没有时返回 (None, None)', fps7 is None and fms7 is None)

# ---- 6.10 逐帧数据的真正价值：平均值看不出卡顿，1% low 能 ----
# 注：frame_ms 与 1000/fps 数学上相等，所以"平均值更准"不是换用 PresentMon 的
# 理由。理由是拿到逐帧数据后能算分布指标 —— 下面这个例子就是它存在的意义。
csvp8 = m.PresentMonCsv(window=120)
csvp8.feed(PM_HEADER_OLD)
for _ in range(99):
    csvp8.feed(pm_row(PM_HEADER_OLD, ProcessID=1, MsBetweenPresents='16.6667'))
csvp8.feed(pm_row(PM_HEADER_OLD, ProcessID=1, MsBetweenPresents='200.0'))  # 卡了一顿
fps8, _ = csvp8.read(foreground_pid=1)
low8 = csvp8.low_fps(1.0, foreground_pid=1)
check('平均帧率被一次尖峰拖到约 54fps（看不出"卡了一顿"）',
      fps8 is not None and abs(fps8 - 54.0) < 1.0, f'fps={fps8}')
check('1% low 直接把最慢的帧指出来（200ms -> 5fps）',
      low8 is not None and low8 < 10.0, f'1% low={low8}')

# ---- 6.11 诊断输出本身也要能用 ----
# 这一组是补缺口：探针跑真机时在最后一步崩过一次 ——
# PresentMonCsv 里有**两个** describe()（旧的不带参数、定义在后面，把新的覆盖了），
# 而当时没有任何测试调用 describe()，所以一路绿着漏过去了。
# 教训：公开方法都要被至少调用一次，哪怕只是"不抛异常"。
d4 = csvp4.describe(allow_pids={200}, foreground_pid=200)
check('describe() 能调用，并标出被选中的进程', '<-PICK' in d4, d4[:70])
check('describe() 会标注被跳过的原因（shell / 无窗口）',
      ('[skip:shell]' in d4) or ('[skip:no-visible-window]' in d4), d4[:120])

d_empty = m.PresentMonCsv()
check('describe() 在"还没有表头"时不抛异常', isinstance(d_empty.describe(), str),
      d_empty.describe())
d_empty.feed(PM_HEADER_OLD)
check('describe() 在"有表头但没帧"时不抛异常', isinstance(d_empty.describe(), str),
      d_empty.describe())
check('describe() 在不传过滤参数时也能用（单测/日志路径）',
      isinstance(csvp4.describe(), str))

# 公开方法逐个摸一遍，防止再有"定义了但没人调用过"的死角
_smoke = m.PresentMonCsv()
_smoke.feed(PM_HEADER_OLD)
_smoke.feed(pm_row(PM_HEADER_OLD, Application='x.exe', ProcessID=1,
                   MsBetweenPresents='16.0'))
check('PresentMonCsv 公开方法全都能调用',
      all(callable(getattr(_smoke, n)) for n in
          ('feed', 'read', 'pick', 'low_fps', 'describe')))

# ---- 6.12 1% low：语义 + 窗口 + 样本门槛 ----
# 这是新协议字段（fps_low）的核心逻辑，三条都容易写错：
#   a) 算成"最差单帧"（那是 min，不是 1% low）
#   b) 用那个 120 帧的短窗口（120 帧的 1% 只有 1.2 帧 -> 退化成 min）
#   c) 样本不够也硬报一个数出来

# (a) 语义：1000 帧里 10 帧卡顿 —— 取这 10 帧的**平均**，不是最差的那一帧
csvp11 = m.PresentMonCsv()
csvp11.feed(PM_HEADER_OLD)
for _ in range(990):
    csvp11.feed(pm_row(PM_HEADER_OLD, ProcessID=3, MsBetweenPresents='16.6667'))
for i in range(10):
    # 40,50,...,130ms -> 平均 85ms -> 11.76fps；而最差单帧是 130ms -> 7.69fps
    csvp11.feed(pm_row(PM_HEADER_OLD, ProcessID=3,
                       MsBetweenPresents=str(40.0 + i * 10)))
lo11 = csvp11.low_fps(1.0, allow_pids={3}, foreground_pid=3)
check('1% low 取"最慢的 1% 帧的平均"(11.76fps)，不是最差单帧(7.69fps)',
      lo11 is not None and abs(lo11 - 11.76) < 0.3, f'1% low={lo11}')

# (b) 窗口：故意把短窗口设成 10 帧，1% low 仍应算得出（它用独立的长窗口）
csvp10 = m.PresentMonCsv(window=10)
csvp10.feed(PM_HEADER_OLD)
for _ in range(300):
    csvp10.feed(pm_row(PM_HEADER_OLD, ProcessID=2, MsBetweenPresents='16.6667'))
lo10 = csvp10.low_fps(1.0, allow_pids={2}, foreground_pid=2)
check('1% low 用独立的长窗口，不受 PRESENTMON_WINDOW 限制',
      lo10 is not None and abs(lo10 - 60.0) < 0.5,
      f'window=10 时 1% low={lo10}')

# (c) 样本门槛
csvp9 = m.PresentMonCsv()
csvp9.feed(PM_HEADER_OLD)
for _ in range(m.LOW_MIN_FRAMES - 1):
    csvp9.feed(pm_row(PM_HEADER_OLD, ProcessID=1, MsBetweenPresents='16.6667'))
check(f'样本不足 {m.LOW_MIN_FRAMES} 帧时 1% low 返回 None（宁可 -- 也不给没意义的数）',
      csvp9.low_fps(1.0, allow_pids={1}, foreground_pid=1) is None)
for _ in range(2):
    csvp9.feed(pm_row(PM_HEADER_OLD, ProcessID=1, MsBetweenPresents='16.6667'))
lo9 = csvp9.low_fps(1.0, allow_pids={1}, foreground_pid=1)
check('样本够了就给数', lo9 is not None and abs(lo9 - 60.0) < 0.5, f'{lo9}')

print("\n=== 7) 找不到 PresentMon 时必须返回 None（不许编数）===")
mon.presentmon = None
mon.rtss = None
fps_none, fms_none = mon.get_fps()
check('两个来源都不可用时 get_fps 返回 (None, None)',
      fps_none is None and fms_none is None, f'得到 {fps_none}/{fms_none}')
check('已彻底删除估算法（源码里不该再有 FPS_ESTIMATE_FALLBACK）',
      not hasattr(m.Config, 'FPS_ESTIMATE_FALLBACK'))

print("\n=== 8) 打包后（onefile）PresentMon 的释放路径 ===")
# 这是整条链路里最容易忽略的一环，也是唯一没法靠"跑一下 exe"来验的：
#   onefile 的 sys._MEIPASS 是临时目录，**父进程一退出就被删**；而 PresentMon 是
#   独立子进程、可能比我们活得久。所以代码会把随包的那份先复制到
#   %LOCALAPPDATA%\PCMonitor 再启动。
# 这里用假的 _MEIPASS + 假的 LOCALAPPDATA 把这段逻辑单独跑一遍。
import tempfile
import shutil as _shutil
import hashlib as _hl

_fake_mei = tempfile.mkdtemp(prefix='mei_')
_fake_local = tempfile.mkdtemp(prefix='local_')
_saved_meipass = getattr(sys, '_MEIPASS', None)
_saved_localappdata = os.environ.get('LOCALAPPDATA')
_saved_pm_path = m.Config.PRESENTMON_PATH
try:
    with open(os.path.join(_fake_mei, m.PRESENTMON_EXE), 'wb') as _f:
        _f.write(b'MZ' + b'\0' * 4096)          # 内容无所谓，只要是个存在的 .exe
    sys._MEIPASS = _fake_mei
    os.environ['LOCALAPPDATA'] = _fake_local
    m.Config.PRESENTMON_PATH = ''

    _expect = os.path.join(_fake_local, 'PCMonitor', m.PRESENTMON_EXE)
    _got = m.find_presentmon_exe('')
    check('把随包的 PresentMon 复制到稳定目录再返回',
          _got is not None and os.path.normcase(_got) == os.path.normcase(_expect),
          f'得到 {_got}')
    check('复制出来的文件真的存在（不是只返回了个路径）',
          bool(_got) and os.path.isfile(_got))
    check('返回的不是 _MEIPASS 里的临时副本（那个会被删）',
          bool(_got) and os.path.normcase(os.path.dirname(_got))
          != os.path.normcase(_fake_mei))

    # 第二次调用应该直接用已经复制好的那份，不再依赖 _MEIPASS
    del sys._MEIPASS
    _got2 = m.find_presentmon_exe('')
    check('_MEIPASS 消失后仍能通过缓存目录找到',
          _got2 is not None and os.path.normcase(_got2) == os.path.normcase(_expect),
          f'得到 {_got2}')
finally:
    if _saved_meipass is None:
        if hasattr(sys, '_MEIPASS'):
            del sys._MEIPASS
    else:
        sys._MEIPASS = _saved_meipass
    if _saved_localappdata is None:
        os.environ.pop('LOCALAPPDATA', None)
    else:
        os.environ['LOCALAPPDATA'] = _saved_localappdata
    m.Config.PRESENTMON_PATH = _saved_pm_path
    _shutil.rmtree(_fake_mei, ignore_errors=True)
    _shutil.rmtree(_fake_local, ignore_errors=True)

print("\n=== 8b) 稳定目录里的缓存被改过就必须重放（只看大小是不够的）===")
# 回归用：以前判断"要不要重新复制"只比文件大小，于是**同尺寸但内容不同**的缓存
# 会被直接执行，而它是从 %LOCALAPPDATA%（用户可写目录）里启动的。
_fake_mei2 = tempfile.mkdtemp(prefix='mei2_')
_fake_local2 = tempfile.mkdtemp(prefix='local2_')
_saved_meipass2 = getattr(sys, '_MEIPASS', None)
_saved_localappdata2 = os.environ.get('LOCALAPPDATA')
_saved_pm_path2 = m.Config.PRESENTMON_PATH
_saved_sha2 = m.PRESENTMON_SHA256
try:
    _good = b'G' * 4096
    _tampered = b'B' * 4096                      # 同样 4096 字节，内容不同
    _src = os.path.join(_fake_mei2, m.PRESENTMON_EXE)
    _dst_dir = os.path.join(_fake_local2, 'PCMonitor')
    os.makedirs(_dst_dir)
    _dst = os.path.join(_dst_dir, m.PRESENTMON_EXE)
    with open(_src, 'wb') as _f:
        _f.write(_good)
    with open(_dst, 'wb') as _f:
        _f.write(_tampered)

    sys._MEIPASS = _fake_mei2
    os.environ['LOCALAPPDATA'] = _fake_local2
    m.Config.PRESENTMON_PATH = ''
    m.PRESENTMON_SHA256 = _hl.sha256(_good).hexdigest()   # 当成"随包记录的官方值"
    _got = m.find_presentmon_exe('')
    with open(_dst, 'rb') as _f:
        _after = _f.read()
    check('同尺寸但内容不同的缓存被重新复制',
          _after == _good, f'复制后是 {len(_after)} 字节、内容{"对" if _after == _good else "**不对**"}')
    check('返回的就是稳定目录里那份（已修好）',
          _got is not None and os.path.normcase(_got) == os.path.normcase(_dst), f'得到 {_got}')
    check('哈希自检能发现被改过的文件',
          _hl.sha256(_tampered).hexdigest() != _hl.sha256(_good).hexdigest())
finally:
    if _saved_meipass2 is None:
        if hasattr(sys, '_MEIPASS'):
            del sys._MEIPASS
    else:
        sys._MEIPASS = _saved_meipass2
    if _saved_localappdata2 is None:
        os.environ.pop('LOCALAPPDATA', None)
    else:
        os.environ['LOCALAPPDATA'] = _saved_localappdata2
    m.Config.PRESENTMON_PATH = _saved_pm_path2
    m.PRESENTMON_SHA256 = _saved_sha2
    _shutil.rmtree(_fake_mei2, ignore_errors=True)
    _shutil.rmtree(_fake_local2, ignore_errors=True)

print("\n=== 9) 传感器名字匹配（这一层最容易出错）===")
# 补课：cpu_volt 曾因为匹配规则没覆盖 AMD 的 "Core #1 VID" 而**永远读不到**，
# 而当时没有任何测试覆盖 _parse_cpu / _parse_board 的名字匹配逻辑。
# 这类代码天生脆（厂商给传感器起什么名字都不一定），所以用假传感器对象
# 把匹配层单独测一遍，不依赖真硬件。


class _FSensor:
    def __init__(self, name, stype, value):
        self.Name = name
        self.SensorType = stype
        self.Value = value


class _FHardware:
    def __init__(self, *sensors):
        self.Name = 'FakeHw'
        self.Sensors = list(sensors)
        self.SubHardware = []


def _fresh_mon():
    x = object.__new__(m.LibreHardwareMonitor)
    x._init_state()
    return x


_a = _fresh_mon()
_a._parse_cpu(_FHardware(_FSensor('Core #1 VID', 'Voltage', 1.55)))
check('cpu_volt 能匹配 "Core #1 VID"（AMD 笔记本实机就是这个命名）',
      _a.cpu_volt == 1.55, f'得到 {_a.cpu_volt}')

_b = _fresh_mon()
_b._parse_cpu(_FHardware(_FSensor('Core #1 VID', 'Voltage', 1.55),
                         _FSensor('CPU Core', 'Voltage', 1.20)))
check('cpu_volt 优先取实测的 "CPU Core" 而不是 VID',
      _b.cpu_volt == 1.20, f'得到 {_b.cpu_volt}')

_c = _fresh_mon()
_c._parse_cpu(_FHardware(_FSensor('CPU Core', 'Voltage', 12.0)))
check('cpu_volt 过滤掉不合理电压（12V 不是核心电压）',
      _c.cpu_volt is None, f'得到 {_c.cpu_volt}')

_d = _fresh_mon()
_d._parse_cpu(_FHardware(_FSensor('Core (Tctl/Tdie)', 'Temperature', 95.0),
                         _FSensor('CCD1 (Tdie)', 'Temperature', 88.0),
                         _FSensor('CPU Package', 'Temperature', 90.0)))
check('cpu_temp 优先取 Tctl/Tdie（而不是 CCD 或 Package）',
      _d.cpu_tctl_tdie == 95.0, f'得到 {_d.cpu_tctl_tdie}')

_e = _fresh_mon()
_e._parse_cpu(_FHardware(_FSensor('CPU Core #1', 'Load', 20.0),
                         _FSensor('CPU Core #2', 'Load', 91.0),
                         _FSensor('CPU Total', 'Load', 55.0)))
check('cpu_core_max 取最忙单核，cpu_total_load 取总计',
      _e.cpu_core_max == 91.0 and _e.cpu_total_load == 55.0,
      f'core_max={_e.cpu_core_max} total={_e.cpu_total_load}')

_f = _fresh_mon()
_f._parse_board(_FHardware(_FSensor('Fan #1', 'Fan', 1200.0),
                           _FSensor('Fan #2', 'Fan', 900.0)))
check('风扇没命名时：第一个当 CPU、第二个当机箱',
      _f.cpu_fan == 1200 and _f.mobo_fan == 900,
      f'cpu={_f.cpu_fan} mobo={_f.mobo_fan}')

_g = _fresh_mon()
_g._parse_board(_FHardware(_FSensor('Chassis Fan', 'Fan', 800.0),
                           _FSensor('CPU Fan', 'Fan', 1500.0)))
check('有命名时按名字认 CPU 风扇', _g.cpu_fan == 1500, f'cpu={_g.cpu_fan}')

_h = _fresh_mon()
_h._parse_board(_FHardware(_FSensor('CPU Fan', 'Fan', 0.0)))
check('0 转的风扇被忽略（不当成有效读数）', _h.cpu_fan is None,
      f'cpu={_h.cpu_fan}')

print("\n=== 10) Job Object（防止 PresentMon 变成孤儿）===")
# 实测发生过孤儿：程序被强杀时 atexit / finally 都不执行，PresentMon 留了下来
# 还占着 ETW 会话。Job Object 是唯一能覆盖"父进程被强杀"的机制。
# 这里测的是"能不能建起来 + 能不能绑进程"—— ctypes 结构体布局一旦写错，
# SetInformationJobObject 就会失败、handle 变成 None，而那时功能会**静默退化**
# 成"可能留孤儿"，所以必须有测试盯着。
_job = m._KillOnCloseJob()
if sys.platform == 'win32':
    check('能创建 job object（ctypes 结构体布局正确）', _job.handle is not None,
          f'handle={_job.handle}')
    if _job.handle:
        import subprocess as _sp
        _ch = _sp.Popen([sys.executable, '-c', 'import time; time.sleep(30)'],
                        creationflags=getattr(_sp, 'CREATE_NO_WINDOW', 0))
        check('能把子进程绑进 job', _job.assign(_ch), f'pid={_ch.pid}')
        _ch.terminate()
        try:
            _ch.wait(timeout=5)
        except Exception:
            _ch.kill()
        _job.close()
        check('close() 之后 handle 被清空', _job.handle is None)
else:
    check('非 Windows 平台下不报错（handle 为 None）', _job.handle is None)

print("\n=== 11) 超量程上报无效，而不是悄悄顶在 65534 ===")
# 协议是定标 uint16（上限 65534）。以前超量程是**静默夹住**的：屏幕上是一个
# 看起来很正常的数字，但它是错的（uptime 连续开机 45.5 天后永远显示 65534）。
check('uptime_min 超过 65534 分钟（约 45.5 天）报无效',
      m._fit_field('uptime_min', 70000.0) is None)
check('uptime_min 正常值原样保留', m._fit_field('uptime_min', 1234.0) == 1234.0)
check('net_down 超过 655.34 MB/s 报无效（万兆网卡跑满就到这儿）',
      m._fit_field('net_down', 900.0) is None)
check('net_down 正常值原样保留', m._fit_field('net_down', 12.5) == 12.5)
check('disk_read 超过 6553.4 MB/s 报无效（PCIe 5.0 NVMe 能超）',
      m._fit_field('disk_read', 8000.0) is None)
check('None 原样返回（别把"读不到"变成别的意思）',
      m._fit_field('net_up', None) is None)

print("\n=== 12) ini 解析：BOM / 未知键 / 范围 / 枚举 ===")
_ini_dir = tempfile.mkdtemp(prefix='ini_')
_saved_ini = m._INI
try:
    _p = os.path.join(_ini_dir, 'pc_monitor.ini')
    # encoding='utf-8-sig'：记事本里"UTF-8"另存会把 BOM 写进文件头。用 'utf-8' 读的话
    # 第一个键会变成 '\ufeffPING_HOST' —— 不等于 'PING_HOST'，那一条就**静默失效**，
    # 而且只有第一条会中招（最难发现）。
    with open(_p, 'w', encoding='utf-8-sig') as _f:
        _f.write('PING_HOST = 1.2.3.4\nCOLLECT_INTERVAL = 2.0\nNOT_A_KEY = 5\n')
    _cfg = m._load_ini(_p)
    check('带 BOM 的 ini 第一个键能正确读到（不是 \\ufeffPING_HOST）',
          _cfg.get('PING_HOST') == '1.2.3.4', f'读到的键: {sorted(_cfg)}')
    check('键名统一转成大写', 'COLLECT_INTERVAL' in _cfg and _cfg['COLLECT_INTERVAL'] == '2.0',
          f'{sorted(_cfg)}')

    # 模拟"程序读了一部分键"：没被读过的键必须能识别出来
    _cfg.get('PING_HOST')
    check('写了但没被读的键能被识别出来（键名拼错就靠这个）',
          sorted(set(_cfg) - _cfg.used) == ['COLLECT_INTERVAL', 'NOT_A_KEY'],
          f'{sorted(set(_cfg) - _cfg.used)}')

    # 范围与枚举校验：都是"改一个数字就能造成的难查故障"
    m._INI = m._IniValues()
    m._INI.update({'COLLECT_INTERVAL': '0'})
    check('COLLECT_INTERVAL=0 被夹到下限（否则主循环满速忙等）',
          m._ini_ranged('COLLECT_INTERVAL', 1.0, 0.1, 60.0) == 0.1,
          f'{m._ini_ranged("COLLECT_INTERVAL", 1.0, 0.1, 60.0)}')
    m._INI = m._IniValues()
    m._INI.update({'PING_WINDOW': '0'})
    check('PING_WINDOW=0 被夹到 1（否则丢包率分母为 0）',
          m._ini_ranged('PING_WINDOW', 10, 1, 1000, integer=True) == 1)
    m._INI = m._IniValues()
    m._INI.update({'FPS_SOURCE': 'presentmno'})
    check('FPS_SOURCE 拼错会退回默认值（以前静默失效 -> 帧率永远 --）',
          m._ini_choice('FPS_SOURCE', 'auto', ('auto', 'presentmon', 'rtss', 'none')) == 'auto')
    m._INI = m._IniValues()
    m._INI.update({'FPS_SOURCE': 'RTSS'})
    check('枚举值大小写不敏感',
          m._ini_choice('FPS_SOURCE', 'auto', ('auto', 'presentmon', 'rtss', 'none')) == 'rtss')
    m._INI = m._IniValues()
    m._INI.update({'PROTOCOL': 'text'})
    check('PROTOCOL 只认 binary/csv，其它回退 binary',
          m._ini_choice('PROTOCOL', 'binary', ('binary', 'csv')) == 'binary')
finally:
    m._INI = _saved_ini
    _shutil.rmtree(_ini_dir, ignore_errors=True)

print("\n=== 13) 重连前必须重新扫描（ESP32 重启会换随机 BLE 地址）===")
# 回归用：connect() 以前是 `if not self.device: return False`，而重连路径永远带着
# 上一次的 device 对象 —— ESP32 一重启（刷固件/断电）地址就变了，于是 3 次重连
# 全部失败后程序直接退出，而设备其实就在旁边好好广播着。
import asyncio as _aio


class _FakeBleakClient:
    def __init__(self, device, timeout=None):
        self.device = device
        self.is_connected = False

    async def connect(self):
        self.is_connected = True


_orig_client = m.BleakClient
try:
    m.BleakClient = _FakeBleakClient
    _s = m.BLESender.__new__(m.BLESender)      # 不走 __init__（不必碰 Config/日志）
    _s.device = None                           # 地址已失效
    _s.reconnect_attempts = 3
    _s.last_success_time = 0.0
    _scans = []

    async def _fake_find():
        _scans.append(1)
        _s.device = object()                   # 扫到了"新地址"
        return True

    _s.find_device = _fake_find
    _ok = _aio.run(_s.connect())
    check('device 为空时会先重新扫描（旧代码在这里直接 return False）',
          bool(_ok) and len(_scans) == 1, f'扫描 {len(_scans)} 次, 连接结果 {_ok}')
    check('连接成功后重连计数清零', _s.reconnect_attempts == 0,
          f'{_s.reconnect_attempts}')
finally:
    m.BleakClient = _orig_client

print(f"\n{'==== 全部通过 ====' if not fails else '==== 有失败: ' + ', '.join(fails) + ' ===='}")
sys.exit(1 if fails else 0)
