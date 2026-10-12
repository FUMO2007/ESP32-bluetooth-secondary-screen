# ============================================================================
#  deploy.ps1 —— 把工作区的 PC 端同步到实际运行目录
# ----------------------------------------------------------------------------
#  为什么需要它：
#    工作区（PC_Monitor_1）是开发副本，实际运行的是 D:\PC_Monitor\PC端\。
#    以前靠手工复制 + 改名，结果两边悄悄分叉过（备份文件的时间戳能看出来），
#    而且"我现在测的到底是哪一份"完全靠记忆。
#
#  用法：
#      .\deploy.cmd                                          # 推荐入口（理由见下）
#      powershell -NoProfile -File deploy.ps1              # 复制并校验
#      powershell -NoProfile -File deploy.ps1 -WhatIf      # 只看会改什么，不动手
#      powershell -NoProfile -File deploy.ps1 -Target <路径>
#
#  同步内容见下面的 $Map。dlls\ 是**可选**条目：仓库里不放第三方二进制，
#  源目录没有它时只提示（目标已有的那份不会被删），不算失败。
#
#  退出码：0 = 同步完成且校验一致；1 = 有文件没同步成功。
#
#  ⚠ 如果看到「The file ... is not digitally signed」：本机有活动的代码完整性策略，
#    **新建或被编辑过的 .ps1 会被拒绝执行**，与脚本内容无关（一直没动过的版本能跑，
#    它的字节副本却不能）。用同目录的 deploy.cmd，或自己加 -ExecutionPolicy Bypass。
#    完整证据写在 build.ps1 头部。
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
#   Dir = $true  这是一整个目录，按文件逐个比对哈希
#   Opt = $true  源缺失可以接受：明确说清后果，但不算失败（第三方二进制就属于这种）
$Map = @(
    @{ Src = 'pc_side\pc_monitor_v2.py';        Dst = 'pc_monitor_libre.py'; Why = '主程序（部署时改名）' },
    @{ Src = 'pc_side\proto_v2.py';             Dst = 'proto_v2.py';        Why = '协议实现，必须和主程序同目录' },
    @{ Src = 'pc_monitor_libre.spec';           Dst = 'pc_monitor_libre.spec'; Why = 'PyInstaller 打包配置' },
    @{ Src = 'vendor\PresentMon-2.6.0-x64.exe'; Dst = 'PresentMon-2.6.0-x64.exe'; Why = '真帧率来源（打包时会被塞进 exe）' },
    # dlls\ 是 LibreHardwareMonitor 的托管 DLL 集合（第三方二进制，仓库里不放）。
    # 以前它**不在**同步表里：换台机器部署就会漏掉，而 spec 找不到 dlls 只打一行
    # 警告 —— 打出来的 exe 能启动、界面也在，但每一个读数都是 --，很难看出是漏了文件。
    @{ Src = 'dlls'; Dst = 'dlls'; Dir = $true; Opt = $true
       Why = '传感器采集依赖的托管 DLL（缺了 exe 读不到任何读数）' }
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
        if ($m.Opt) {
            # 可选源缺失：说清后果与补救办法，但不计为失败
            Write-Host ("  {0,-34} 源没有，跳过" -f $m.Dst) -NoNewline
            Write-Host ("   ({0})" -f $m.Why) -ForegroundColor DarkGray
            Write-Host "      目标里已有的那份不会被动；如果目标也没有，exe 打出来会读不到任何读数。" -ForegroundColor Yellow
            Write-Host "      要同步的话，把 $($m.Src)\ 放到仓库根目录再跑一次。" -ForegroundColor Yellow
            continue
        }
        Write-Host ("  [FAIL] 源文件不存在: {0}" -f $m.Src) -ForegroundColor Red
        $failed++
        continue
    }

    if ($m.Dir) {
        # ---- 目录条目：逐文件比对（只复制变了的），子目录结构保持一致 ----
        $srcFiles = @(Get-ChildItem -LiteralPath $src -File -Recurse)
        if ($srcFiles.Count -eq 0) {
            Write-Host ("  {0,-34} 源目录是空的，跳过" -f $m.Dst) -ForegroundColor Yellow
            continue
        }

        $changed = @()
        foreach ($f in $srcFiles) {
            $rel     = $f.FullName.Substring($src.Length).TrimStart('\')
            $dstFile = Join-Path $dst $rel
            if (-not (Test-Path $dstFile) -or (Hash12 $f.FullName) -ne (Hash12 $dstFile)) {
                $changed += [pscustomobject]@{ Src = $f.FullName; Dst = $dstFile; Rel = $rel }
            }
        }

        $state = if ($changed.Count -eq 0) { "已一致，跳过（$($srcFiles.Count) 个文件）" }
                 else { "复制 $($changed.Count)/$($srcFiles.Count) 个文件" }
        Write-Host ("  {0,-34} {1}" -f $m.Dst, $state) -NoNewline
        Write-Host ("   ({0})" -f $m.Why) -ForegroundColor DarkGray

        if ($changed.Count -eq 0) { continue }

        if ($WhatIf) {
            $changed | Select-Object -First 5 | ForEach-Object {
                Write-Host ("      {0}" -f $_.Rel) -ForegroundColor DarkGray
            }
            if ($changed.Count -gt 5) {
                Write-Host ("      … 还有 $($changed.Count - 5) 个") -ForegroundColor DarkGray
            }
            continue
        }

        $dirFailed = 0
        foreach ($c in $changed) {
            try {
                $parent = Split-Path $c.Dst -Parent
                if (-not (Test-Path $parent)) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
                Copy-Item -LiteralPath $c.Src -Destination $c.Dst -Force
                # 复制完再校验一次：文件被占用/杀软拦截时 Copy-Item 可能"成功"但内容不对
                if ((Hash12 $c.Src) -ne (Hash12 $c.Dst)) { throw "复制后哈希不一致（文件被占用？）" }
            } catch {
                Write-Host ("      [FAIL] {0}: {1}" -f $c.Rel, $_.Exception.Message) -ForegroundColor Red
                $dirFailed++
            }
        }
        if ($dirFailed -eq 0) {
            Write-Host ("      [OK] $($changed.Count) 个文件全部哈希一致") -ForegroundColor Green
        } else {
            $failed += $dirFailed
        }
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

# 关键依赖自检。spec 找不到 dlls 只打一行警告，打出来的 exe「能启动、界面也在，
# 但每个读数都是 --」—— 这种失败从表面上看不出来，所以在部署的出口处再确认一次。
$crt = Join-Path $Target 'dlls\LibreHardwareMonitorLib.dll'
if (Test-Path $crt) {
    Write-Host ("关键依赖 dlls\LibreHardwareMonitorLib.dll 就位（sha256 {0}）" -f (Hash12 $crt)) -ForegroundColor DarkGray
} else {
    Write-Host "[警告] 目标目录缺 dlls\LibreHardwareMonitorLib.dll —— 打出来的 exe 会读不到任何传感器读数" -ForegroundColor Yellow
}
Write-Host ""

Write-Host "同步完成。下一步打包：" -ForegroundColor Green
Write-Host "  cd $Target"
Write-Host "  & venv\Scripts\python.exe -m PyInstaller --clean --noconfirm pc_monitor_libre.spec"
Write-Host ""
Write-Host "打包日志里要出现：[spec] PresentMon 校验通过" -ForegroundColor DarkGray
exit 0
