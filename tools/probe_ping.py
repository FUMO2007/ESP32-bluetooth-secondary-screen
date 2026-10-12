# -*- coding: utf-8 -*-
"""延迟探测探针：用 PC 端真实的 PingProbe 目标（Config.PING_HOST:PORT），
   同时对比几个国内/国外 DNS 的 TCP 连通性，并打印网卡计数器明细。

   看点：LOSS 是"TCP 连接失败率"。如果 1.1.1.1:53 在本地被拦，
   屏幕上的 LOSS 会一直 100%、PING 显示 --，看着就像"网络数据不对"。
"""
import asyncio
import os
import sys
import time

os.environ['PCMON_NO_HIDE'] = '1'
sys.path.insert(0, r'D:\PC_Monitor\PC端')

import psutil                                        # noqa: E402
import pc_monitor_libre as m                          # noqa: E402


async def probe(host, port, tries=4):
    ok, ms_list = 0, []
    for _ in range(tries):
        t0 = time.perf_counter()
        try:
            r, w = await asyncio.wait_for(asyncio.open_connection(host, port), timeout=1.5)
            ms_list.append((time.perf_counter() - t0) * 1000.0)
            ok += 1
            w.close()
            try:
                await w.wait_closed()
            except Exception:
                pass
        except Exception:
            pass
        await asyncio.sleep(0.3)
    avg = sum(ms_list) / len(ms_list) if ms_list else None
    return ok, tries, avg


async def main():
    print('PC 端配置的探测目标： %s:%s（每 %.0f 秒一次，窗口 %d 次）'
          % (m.Config.PING_HOST, m.Config.PING_PORT, m.Config.PING_INTERVAL, m.Config.PING_WINDOW))
    print('-' * 66)
    targets = [
        (m.Config.PING_HOST, m.Config.PING_PORT, '当前配置'),
        ('223.5.5.5',   53, '阿里 DNS'),
        ('119.29.29.29', 53, 'DNSPod'),
        ('8.8.8.8',     53, 'Google DNS'),
    ]
    for host, port, tag in targets:
        ok, n, avg = await probe(host, port)
        loss = 100.0 * (n - ok) / n
        print('%-12s %-15s TCP/%-3d 成功 %d/%d  丢包率 %5.1f%%  平均 %s'
              % (tag, host, port, ok, n, loss,
                 ('%.1f ms' % avg) if avg is not None else '--'))

    print('-' * 66)
    print('网卡计数器明细（屏幕上的 DOWN/UP 是这些的总和）：')
    per = psutil.net_io_counters(pernic=True)
    tot = psutil.net_io_counters()
    for name, c in per.items():
        print('  %-28s recv=%.1f MB  sent=%.1f MB' % (name, c.bytes_recv / 1048576, c.bytes_sent / 1048576))
    print('  合计 recv=%.1f MB  sent=%.1f MB' % (tot.bytes_recv / 1048576, tot.bytes_sent / 1048576))


asyncio.run(main())
