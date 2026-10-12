# ============================================================================
#  deploy.ps1 —— 把工作区的 PC 端同步到实际运行目录
# ----------------------------------------------------------------------------
#  为什么需要它：
#    工作区（PC_Monitor_1）是开发副本，实际运行的是 D:\PC_Monitor\PC端\。
#    以前靠手工复制 + 改名，结果两边悄悄分叉过（备份文件的时间戳能看出来），
#    而且"我现在测的到底是哪一份"完全靠记忆。
#
#  用法：
#      powershell -NoProfile -File deploy.ps1              # 复制并校验
#      powershell -NoProfile -File deploy.ps1 -WhatIf      # 只看会改什么，不动手
#      powershell -NoProfile -File deploy.ps1 -Target <路径>
#
#  退出码：0 = 同步完成且校验一致；1 = 有文件没同步成功。
#
#  ⚠ 本文件必须保存为「UTF-8 带 BOM」（Windows PowerShell 5.1 读无 BOM 的 .ps1
#    会按系统 ANSI 代码页解码，中文被拆坏并报出假的语法错误）。
# ============================================================================
[CmdletBinding()]
param(
    [string]$Target = 'D:\PC_Monitor\PC端',
    [switch]$WhatIf
)

$ErrorActionPreference = 'Stop'
$Root = $PSScriptRoot
$failed = 0

# 源 -> 目标文件名。注意主要脚本在部署时要改名。
$Map = @(
    @{ Src = 'pc_side\pc_monitor_v2.py';        Dst = 'pc_monitor_libre.py'; Why = '主程序（部署时改名）' },
    @{ Src = 'pc_side\proto_v2.py';             Dst = 'proto_v2.py';        Why = '协议实现，必须和主程序同目录' },
    @{ Src = 'pc_monitor_libre.spec';           Dst = 'pc_monitor_libre.spec'; Why = 'PyInstaller 打包配置' },
    @{ Src = 'vendor\PresentMon-2.6.0-x64.exe'; Dst = 'PresentMon-2.6.0-x64.exe'; Why = '真帧率来源（打包时会被塞进 exe）' }
)

function Hash12($p) {
    if (-not (Test-Path $p)) { return '<缺失>' }
    return (Get-FileHash $p -Algorithm SHA256).Hash.Substring(0, 12)
}

Write-Host "同步 PC 端代码" -ForegroundColor White
Write-Host "  源  : $Root"
Write-Host "  目标: $Target"
if ($WhatIf) { Write-Host "  模式: -WhatIf（只报告，不写入）" -ForegroundColor Yellow }
Write-Host ""

if (-not (Test-Path $Target)) {
    Write-Host "目标目录不存在：$Target" -ForegroundColor Red
    exit 1
}

foreach ($m in $Map) {
    $src = Join-Path $Root $m.Src
    $dst = Join-Path $Target $m.Dst

    if (-not (Test-Path $src)) {
        Write-Host ("  [FAIL] 源文件不存在: {0}" -f $m.Src) -ForegroundColor Red
        $failed++
        continue
    }

    $same = (Test-Path $dst) -and ((Hash12 $src) -eq (Hash12 $dst))
    $state = if ($same) { '已一致，跳过' } else { '复制' }

    Write-Host ("  {0,-34} {1}" -f $m.Dst, $state) -NoNewline
    Write-Host ("   ({0})" -f $m.Why) -ForegroundColor DarkGray

    if ($same) { continue }

    if ($WhatIf) {
        Write-Host ("      {0}  ->  {1}" -f (Hash12 $src), (Hash12 $dst)) -ForegroundColor DarkGray
        continue
    }

    try {
        Copy-Item -LiteralPath $src -Destination $dst -Force
    } catch {
        Write-Host ("  [FAIL] 复制失败: {0}" -f $_.Exception.Message) -ForegroundColor Red
        $failed++
        continue
    }

    # 复制完再校验一次：文件被占用/杀软拦截时 Copy-Item 可能"成功"但内容不对
    if ((Hash12 $src) -eq (Hash12 $dst)) {
        Write-Host ("      [OK] sha256 {0}" -f (Hash12 $dst)) -ForegroundColor Green
    } else {
        Write-Host ("      [FAIL] 复制后哈希不一致（{0} != {1}）—— 文件被占用？" -f (Hash12 $src), (Hash12 $dst)) -ForegroundColor Red
        $failed++
    }
}

Write-Host ""
if ($WhatIf) {
    Write-Host "（-WhatIf：以上是将会发生的改动，文件没有被动过）" -ForegroundColor Yellow
    exit 0
}

if ($failed -gt 0) {
    Write-Host "$failed 个文件同步失败" -ForegroundColor Red
    exit 1
}

Write-Host "同步完成。下一步打包：" -ForegroundColor Green
Write-Host "  cd $Target"
Write-Host "  & venv\Scripts\python.exe -m PyInstaller --clean --noconfirm pc_monitor_libre.spec"
Write-Host ""
Write-Host "打包日志里要出现：[spec] PresentMon 校验通过" -ForegroundColor DarkGray
exit 0
