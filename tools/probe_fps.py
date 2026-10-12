# probe_fps.py —— 不上板验证帧率来源（PresentMon / RTSS）
# ---------------------------------------------------------------------------
#  为什么需要这个脚本：
#    "屏幕显示 --" 有四五种完全不同的原因 —— 没放 PresentMon、没提权、
#    没在跑 3D 程序、游戏没被识别、RTSS 没装。靠盯着 ESP32 屏幕是分不出来的。
#    这个脚本把每一层都打出来，一眼就能定位卡在哪。
#
#  用法（在仓库根目录）：
#      <venv python> -B tools\probe_fps.py                # 采 10 秒
#      <venv python> -B tools\probe_fps.py --seconds 20
#      <venv python> -B tools\probe_fps.py --raw 8        # 先原样打印前 8 行 CSV
#
#  注意：PresentMon 需要管理员权限（或属于 "Performance Log Users" 组）。
#  请用**管理员终端**运行本脚本，否则结果没有参考价值。
# ---------------------------------------------------------------------------
import argparse
import os
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                '..', 'pc_side'))

# ！！必须在 import pc_monitor_v2 **之前**设置 ！！
# 那个模块在**导入时**就会隐藏控制台窗口并调用 kernel32.FreeConsole()
# —— 它本来是要做成"静默后台程序"的。不设这个变量的话，本脚本自己的终端
# 会被当场解绑：你会看到打了 "starting..." 之后再无任何输出，连报错都不出来，
# 命令看起来就像"执行完什么都没干"。
# test_pc_side.py 里也有同样的这一行，就是为了这个。
os.environ['PCMON_NO_HIDE'] = '1'

try:
    # 行缓冲：输出被重定向到文件/管道时，Python 默认是块缓冲，什么都看不到，
    # 看起来就像"卡住了"。诊断工具必须边跑边出字。
    sys.stdout.reconfigure(line_buffering=True, errors='replace')
    sys.stderr.reconfigure(line_buffering=True, errors='replace')
except Exception:
    pass

# 先出个字。下面 import pc_monitor_v2 会连带拉 psutil/bleak（clr 更慢），
# 有 1~2 秒是完全静默的 —— 那几秒里用户会以为"没反应"。
print('probe_fps: starting... (导入依赖中)', flush=True)

try:
    import pc_monitor_v2 as m
except Exception as e:
    print('导入 pc_monitor_v2 失败（缺依赖？）: %s' % e)
    sys.exit(2)

# 探针的报告必须干净：把日志的**终端**输出摘掉（文件日志照常写，要看细节去翻
# pc_monitor.log）。否则 INFO/WARNING 和报告交错在一起，还容易被外层 shell 的
# stderr 处理误判成出错。
import logging.handlers
for _h in list(m.log.handlers):
    if not isinstance(_h, logging.handlers.RotatingFileHandler):
        m.log.removeHandler(_h)


def is_admin() -> bool:
    try:
        import ctypes
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
        return False


def dump_raw(path: str, n: int, seconds: float = 5.0):
    """直接跑一下 PresentMon，把前 n 行原样打出来。

    用来核对真实表头/列名 —— 本项目刻意按**列名**解析（列序和列数每个版本
    都在变），所以你需要能看到自己这台机器上的实际列名。
    """
    print('--- 2) RAW CSV / 原始样本（最多 %.0f 秒）---' % seconds)
    print('  （没在跑 3D 程序时 PresentMon 可能一行都不输出；本步骤到点会自己结束）')
    sys.stdout.flush()
    try:
        p = subprocess.Popen(
            [path, '--output_stdout', '--no_console_stats', '--exclude_dropped',
             '--session_name', 'PCMonRawDump', '--stop_existing_session'],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL, text=True, encoding='utf-8',
            errors='replace', bufsize=1,
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    except Exception as e:
        print('  启动失败: %s: %s' % (type(e).__name__, e))
        return

    # 定时兜底 —— 这一条是必须的。
    # `for line in p.stdout` 会阻塞在 readline() 上，如果 PresentMon 只吐了表头
    # 就没有后续（没开游戏时就是这样），循环里的 deadline 检查**永远轮不到**，
    # 整个脚本就死等在那里（"输入后没反应"就是这么来的）。
    # 到点直接 terminate，循环自然以 EOF 收场。
    def _kill():
        try:
            p.terminate()
        except Exception:
            pass

    killer = threading.Timer(seconds, _kill)
    killer.daemon = True
    killer.start()

    got = 0
    try:
        for line in p.stdout:
            print('  ' + line.rstrip())
            sys.stdout.flush()
            got += 1
            if got >= n:
                break          # 凑够行数就主动收工，不必等满 seconds
    except Exception as e:
        print('  读取异常: %s: %s' % (type(e).__name__, e))
    finally:
        killer.cancel()
        try:
            p.terminate()
        except Exception:
            pass
        try:
            p.wait(timeout=3)
        except Exception:
            try:
                p.kill()
            except Exception:
                pass

    if got == 0:
        print('  [N/A] 没有任何输出。可能原因：')
        print('        · 没有管理员权限（日志 %s 里找 access denied）' % m.LOG_PATH)
        print('        · 没有正在渲染的 3D 程序 —— PresentMon 这时可能一行都不出')
    else:
        print('  收到 %d 行' % got)
    print('')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--seconds', type=float, default=10.0, help='采样时长（秒）')
    ap.add_argument('--raw', type=int, default=0, help='先原样打印前 N 行 CSV')
    args = ap.parse_args()

    print('=== FPS PROBE / 帧率来源探针 ===')
    print('  配置 CONFIG   : FPS_SOURCE=%r  RTSS_ENABLE=%s' %
          (m.Config.FPS_SOURCE, m.Config.RTSS_ENABLE))
    admin = is_admin()
    print('  管理员 ADMIN  : %s' % ('YES' if admin else 'NO  <-- PresentMon 大概率起不来'))
    print('  日志 LOG      : %s' % m.LOG_PATH)
    # 打印实际加载的是哪一份代码。工作区是开发副本、部署目录才是运行副本，
    # 不写出来的话"我到底在测哪一份"只能靠记忆（这个项目已经因此分叉过一次）。
    print('  模块 MODULE   : %s' % getattr(m, '__file__', '?'))
    print('')

    # ---- 1. 找 PresentMon ----
    print('--- 1) FIND / 查找 PresentMon ---')
    path = m.find_presentmon_exe(m.Config.PRESENTMON_PATH)
    if path:
        print('  [OK] 找到 / found: %s  (%d 字节)' % (path, os.path.getsize(path)))
    else:
        print('  [FAIL] 没找到 / not found: %s' % m.PRESENTMON_EXE)
        print('  下载 download: https://github.com/GameTechDev/PresentMon/releases')
        print('  期望 SHA256  : %s' % m.PRESENTMON_SHA256)
        print('  放到程序旁边，或用 ini 的 PRESENTMON_PATH 指定路径')
    print('')

    if not admin:
        print('  !! 当前不是管理员。PresentMon 起 ETW 会话会被拒绝，下面大概率全是 --。')
        print('     请用"以管理员身份运行"的终端重跑本脚本。')
        print('')

    if args.raw and path:
        dump_raw(path, args.raw)

    # ---- 3. 起 PresentMon 正式采集 ----
    reader = None
    if path:
        print('--- 3) CAPTURE / 启动 PresentMon 采集 ---')
        reader = m.PresentMonReader(path)
        time.sleep(2.0)
        if not reader.available:
            print('  [FAIL] 未启动（平台不支持？）')
            reader = None
        else:
            print('  已启动，采样 %.0f 秒…' % args.seconds)
            print('  （如果现在没在跑 3D 程序，帧率会是 -- 且列表为空，这是正常的）')
        print('')

    deadline = time.time() + args.seconds
    last = None
    try:
        while reader is not None and time.time() < deadline:
            fps, fms = reader.read()
            low = reader.low_fps(1.0)
            line = 'fps=%s  frame_ms=%s  1%%low=%s' % (
                '--' if fps is None else '%.1f' % fps,
                '--' if fms is None else '%.2f' % fms,
                '--' if low is None else '%.1f' % low)
            if line != last:
                print('  ' + line)
                last = line
            time.sleep(1.0)
    except KeyboardInterrupt:
        print('  中断')

    # ---- 4. 汇总 ----
    print('')
    print('=' * 62)
    print('--- 4) RESULT / 结果 ---')
    print('=' * 62)
    print('  前台 pid / foreground : %d' % m._foreground_pid())

    verdict = 'NO-FPS'      # 最后打印成一行纯 ASCII 结论，编码怎么折腾都认得
    try:
        if reader is not None:
            print('  PresentMon : %s' % reader.describe())
            fps, _ = reader.read()
            if fps is None:
                print('  [FAIL] 没有拿到帧率 / no FPS. 依次确认：')
                print('         a) 是不是用管理员终端跑的？（看上面 "admin: NO"）')
                print('         b) 有没有正在运行的 3D 程序/游戏？桌面上是没有帧率的')
                print('         c) 日志里出现 "access denied" 就说明需要提权，')
                print('            或把自己加进组：compmgmt.msc -> 本地用户和组 ->')
                print('            组 -> Performance Log Users（加完要重新登录）')
            else:
                print('  [OK] 真帧率工作正常 / real FPS works')
                verdict = 'OK-PRESENTMON'
        else:
            print('  [SKIP] PresentMon 未启用（没找到可执行文件 / exe not found）')
    finally:
        # 放在 finally 里：这个脚本之前在 describe() 上崩过一次
        # （两个 describe 重名），结果 PresentMon 子进程没被收掉，
        # 一直占着 ETW 会话。
        if reader is not None:
            reader.close()

    # ---- 5. RTSS ----
    print('')
    print('--- 5) RTSS ---')
    rtss = m.RTSSReader() if m.Config.RTSS_ENABLE else None
    if rtss is None:
        print('  [SKIP] 已禁用（Config.RTSS_ENABLE=0）')
    elif not rtss.available:
        print('  [N/A] 打不开 RTSSSharedMemoryV2 —— RTSS 没装或没在运行')
        print('        安装 / install: winget install Guru3D.RTSS')
    else:
        print('  共享内存可打开；连读 3 次（每次间隔 >= 0.2s）看帧数有没有增长…')
        for i in range(3):
            fps, fms = rtss.read()
            print('    第 %d 次: fps=%s frame_ms=%s' % (i + 1, fps, fms))
            time.sleep(0.5)
        if rtss.read()[0] is None:
            print('  [N/A] 仍然拿不到 —— 通常是因为当时没有 3D 程序在渲染')
        elif verdict == 'NO-FPS':
            verdict = 'OK-RTSS'

    print('')
    print('=' * 62)
    print('VERDICT: %s' % verdict)
    print('=' * 62)
    if verdict == 'NO-FPS':
        print('把上面的输出发我，我来判断卡在哪一层。')
    else:
        print('真帧率通了。下一步：同步到部署目录 + 打包。')


if __name__ == '__main__':
    main()
