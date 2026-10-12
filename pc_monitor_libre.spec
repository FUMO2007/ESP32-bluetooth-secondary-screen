# -*- mode: python ; coding: utf-8 -*-
# ============================================================================
#  pc_monitor_libre.spec —— PyInstaller 打包配置（onefile）
# ----------------------------------------------------------------------------
#  这个 spec 同时适用于两个位置，靠下面的 _find() 按候选路径探测：
#    · 仓库里      PC_Monitor_1/           (源码在 pc_side/，PresentMon 在 vendor/)
#    · 部署目录    D:\PC_Monitor\PC端\      (源码就是同目录的 pc_monitor_libre.py)
#  这样只需要维护一份，不会出现"两份 spec 悄悄不一致"。
#
#  打包：
#      pyinstaller --clean --noconfirm pc_monitor_libre.spec
#
#  关于 PresentMon：
#    它随包分发，用来提供真帧率（见 pc_side/README.md）。
#    官方 release: https://github.com/GameTechDev/PresentMon/releases
#    期望 SHA256 : b2a706bc6ad475749e3b7e3409263aa1e6906d45bdcf993f6dbc0f660188f1af
#    找不到它**不会**让打包失败，只是丢掉帧率功能（屏幕显示 --），
#    所以别人从源码构建时不需要先去下这个二进制。
# ============================================================================
import os
import hashlib

PM_NAME = 'PresentMon-2.6.0-x64.exe'
PM_SHA256 = 'b2a706bc6ad475749e3b7e3409263aa1e6906d45bdcf993f6dbc0f660188f1af'


def _find(*rels):
    """在 spec 所在目录按候选相对路径找第一个存在的"""
    for r in rels:
        p = os.path.normpath(os.path.join(SPECPATH, r))
        if os.path.exists(p):
            return p
    return None


def _sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


_entry = _find('pc_monitor_libre.py', 'pc_side/pc_monitor_libre.py',
               'pc_side/pc_monitor_v2.py')
if _entry is None:
    raise SystemExit('找不到入口脚本 pc_monitor_libre.py / pc_monitor_v2.py')

# ---- dlls 目录（LibreHardwareMonitorLib.dll 等）----
_dlls = _find('dlls', '../dlls')
_datas = [(_dlls, 'dlls')] if _dlls else []
if not _dlls:
    print('[spec] 警告：没找到 dlls 目录，打出来的 exe 会因为没有 '
          'LibreHardwareMonitorLib.dll 而无法采集')

# ---- PresentMon（帧率来源，可选）----
_pm = _find('vendor/' + PM_NAME, PM_NAME, '../vendor/' + PM_NAME,
            'third_party/' + PM_NAME)
if _pm:
    got = _sha256(_pm)
    if got != PM_SHA256:
        # 不直接失败：用户可能有意换成别的版本。但必须显眼地说出来。
        print('[spec] 警告：PresentMon 的 SHA256 与预期不符')
        print('[spec]   期望 %s' % PM_SHA256)
        print('[spec]   实际 %s' % got)
    else:
        print('[spec] PresentMon 校验通过: %s' % _pm)
    _datas.append((_pm, '.'))
else:
    print('[spec] 未找到 %s —— 打包后没有真帧率（屏幕显示 --）。' % PM_NAME)
    print('[spec]   下载：https://github.com/GameTechDev/PresentMon/releases')

a = Analysis(
    [_entry],
    pathex=[],
    binaries=[],
    datas=_datas,
    hiddenimports=['clr', 'pythonnet', 'bleak', 'psutil'],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    # 排除 Windows 事件日志那一套。
    # 来路：pc_monitor_v2.py 里的 `import logging.handlers` 会（间接）引用
    # win32evtlog —— 那是 logging.handlers.NTEventLogHandler 用的。PyInstaller
    # 静态分析就跟着把 pywin32 的 win32evtlog/win32api/pywintypes 全打了进来
    # （实测 ~350 KB 原始体积）。
    # 我们只往文件写日志，从不写事件日志，所以直接排掉：
    # 体积小一点，更重要的是**少打包几个用不到的原生 .pyd** —— 这个程序会提权运行，
    # 而 onefile 会把它们解压到 %TEMP% 这种用户可写目录里。
    excludes=['win32evtlog', 'win32evtlogutil', 'win32security', 'win32con',
              'win32api', 'pywintypes', 'win32'],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='pc_monitor_libre',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)
