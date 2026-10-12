# ============================================================================
#  build.ps1 —— 一键构建 + 验证
# ----------------------------------------------------------------------------
#  把 README 里靠手抄的几条命令收敛成一条，并且**每一步都检查退出码**。
#  以前的问题是：构建/测试命令散落在 README 和 pc_side/README.md 里，
#  跑完只看屏幕输出，失败了也照样往下走（尤其 preview.exe 永远返回 0）。
#
#  用法（两种 PowerShell 都行）：
#     powershell -NoProfile -File build.ps1     # Windows PowerShell 5.1（本机只有这个）
#     pwsh -NoProfile -File build.ps1           # PowerShell 7+（装了才用）
#
#     powershell -NoProfile -File build.ps1 -Quick   # 跳过最慢的 UI 仿真渲染
#
#  退出码：0 = 全部通过；1 = 有步骤失败（失败步骤名会列在最后）。
#  所有步骤都会跑完再汇总 —— 一次就能看到全部问题，不用来回试。
#
#  环境变量：
#     PCMON_PY   指定 Python 解释器（默认自动探测）
#     CXX        指定 C++ 编译器（默认 g++）
#     PCMON_ARDUINO_CLI  指定 arduino-cli（默认从 PATH 找；找不到第 8 步就跳过）
#     PCMON_FQBN         指定板型（默认 esp32:esp32:esp32s3）
#
#  !! 本文件必须保存为「UTF-8 带 BOM」!!
#     Windows PowerShell 5.1 读没有 BOM 的 .ps1 时会按系统 ANSI 代码页解码
#     （中文机器上是 GBK），中文注释会被拆坏，报出 "Missing closing '}'" 之类
#     的假语法错误。用编辑器改完请确认编码没有变成 "UTF-8 无 BOM"。
# ============================================================================
[CmdletBinding()]
param(
    [switch]$Quick
)

$ErrorActionPreference = 'Stop'
$Root = $PSScriptRoot
$Out  = Join-Path $Root 'build_out'
$Cxx  = if ($env:CXX) { $env:CXX } else { 'g++' }

$Results = New-Object System.Collections.ArrayList

function Step {
    param([string]$Name, [scriptblock]$Body)
    Write-Host ""
    Write-Host "── $Name " -ForegroundColor Cyan -NoNewline
    $pad = 62 - $Name.Length
    if ($pad -lt 0) { $pad = 0 }
    Write-Host ("-" * $pad) -ForegroundColor DarkGray

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        & $Body
        $sw.Stop()
        [void]$Results.Add([pscustomobject]@{
            Step = $Name; Ok = $true; Ms = $sw.ElapsedMilliseconds; Note = '' })
        Write-Host "   [PASS] $Name ($($sw.ElapsedMilliseconds) ms)" -ForegroundColor Green
    } catch {
        $sw.Stop()
        $msg = $_.Exception.Message
        [void]$Results.Add([pscustomobject]@{
            Step = $Name; Ok = $false; Ms = $sw.ElapsedMilliseconds; Note = $msg })
        Write-Host "   [FAIL] $Name" -ForegroundColor Red
        Write-Host "          $msg" -ForegroundColor Red
        # 故意不 rethrow：把所有步骤跑完再汇总，一次看到全部问题。
    }
}

function Invoke-Native {
    <#  跑一个原生命令；非 0 退出码就抛异常。
        PowerShell 不会因为原生命令失败而停止，必须显式检查 $LASTEXITCODE。 #>
    param([string]$Exe, [string[]]$ArgList, [string]$What)
    & $Exe @ArgList
    if ($LASTEXITCODE -ne 0) {
        throw "$What 失败 (exit $LASTEXITCODE): $Exe $($ArgList -join ' ')"
    }
}

function Invoke-NativeStrict {
    <#  同上，但额外把编译器告警也当成失败。
        只查退出码是不够的：g++ 有告警时照样返回 0，
        那样第 3 步标题里的"不接受警告"就是一句空话。 #>
    param([string]$Exe, [string[]]$ArgList, [string]$What)
    $out  = & $Exe @ArgList 2>&1
    $code = $LASTEXITCODE
    $text = ($out | Out-String).Trim()
    if ($text) { Write-Host $text }
    if ($code -ne 0) {
        throw "$What 失败 (exit $code): $Exe $($ArgList -join ' ')"
    }
    $warns = @($out | Where-Object { "$_" -match 'warning:|note:' })
    if ($warns.Count -gt 0) {
        throw "$What 有 $($warns.Count) 条编译器告警（本步骤约定零告警）"
    }
}

# ---------------------------------------------------------------- 准备
Write-Host "PC Monitor 构建 + 验证" -ForegroundColor White
Write-Host "仓库根目录: $Root"

if (-not (Get-Command $Cxx -ErrorAction SilentlyContinue)) {
    Write-Host "找不到 C++ 编译器 '$Cxx'。装 MinGW-w64 或设置环境变量 CXX。" -ForegroundColor Red
    exit 1
}
if (-not (Test-Path $Out)) { New-Item -ItemType Directory -Force -Path $Out | Out-Null }

$Inc = @('-I', (Join-Path $Root 'tools\ui_preview'), '-I', (Join-Path $Root 'PC_Monitor'))

# Python 解释器：test_pc_side.py 需要 psutil/bleak（test_proto.py 不需要）
$Py = $null
$cands = @(
    $env:PCMON_PY,
    'D:\PC_Monitor\PC端\venv\Scripts\python.exe',
    (Join-Path $Root '.venv\Scripts\python.exe'),
    (Join-Path $Root 'venv\Scripts\python.exe')
)
foreach ($cand in $cands) {
    if ($cand -and (Test-Path $cand)) { $Py = $cand; break }
}
if (-not $Py) {
    $cmd = Get-Command python -ErrorAction SilentlyContinue
    if ($cmd) { $Py = $cmd.Source }
}
if (-not $Py) {
    Write-Host "找不到 Python 解释器。设置环境变量 PCMON_PY 指向 python.exe。" -ForegroundColor Red
    exit 1
}
Write-Host "编译器    : $Cxx"
Write-Host "Python    : $Py"

# 探测 Python 能否 import 运行时依赖（缺了只影响 test_pc_side，不影响协议测试）
$PyHasDeps = $true
& $Py -B -c "import psutil, bleak" 2>$null
if ($LASTEXITCODE -ne 0) { $PyHasDeps = $false }

$TestProto = Join-Path $Root 'pc_side\test_proto.py'

# ---------------------------------------------------------------- 1. 金标向量
Step '1) 检查金标向量是否已同步 (--check)' {
    # 顺序很重要：这一步必须在"重新生成"**之前**跑。
    # 先重新生成再检查的话，刚写出来的文件当然是最新的 —— 那个检查永远不会
    # 失败，等于没有（这正是它上一版的毛病）。
    # 这里只报告不算失败：本地构建紧接着就会重新生成好。要卡住提交/CI，
    # 直接单独跑 `test_proto.py --check` 并看退出码。
    $out = & $Py -B $TestProto --check 2>&1
    $code = $LASTEXITCODE
    $out | ForEach-Object { Write-Host "   $_" }
    if ($code -ne 0) {
        Write-Host "   头文件是过期的（协议表/缩放改过但没重新生成）" -ForegroundColor Yellow
        Write-Host "   下一步会重新生成；如果是提交前检查，这里应该算失败" -ForegroundColor Yellow
    }
}

Step '2) 重新生成金标向量' {
    Invoke-Native $Py @('-B', $TestProto) '生成金标向量'
}

# ---------------------------------------------------------------- 3. PresentMon
Step '3) 校验随包的 PresentMon（真帧率来源）' {
    $pm   = Join-Path $Root 'vendor\PresentMon-2.6.0-x64.exe'
    $want = 'b2a706bc6ad475749e3b7e3409263aa1e6906d45bdcf993f6dbc0f660188f1af'
    if (-not (Test-Path $pm)) {
        # 不是失败：没有它只是丢帧率功能（屏幕显示 --），别人从源码构建
        # 不该被迫先去下一个第三方二进制。
        Write-Host "   未找到 $pm" -ForegroundColor Yellow
        Write-Host "   下载: https://github.com/GameTechDev/PresentMon/releases" -ForegroundColor Yellow
        Write-Host "   打包后不会有真帧率（屏幕显示 --），但构建可以继续" -ForegroundColor Yellow
    } else {
        $h = (Get-FileHash $pm -Algorithm SHA256).Hash.ToLower()
        Write-Host "   $pm"
        Write-Host "   SHA256 $h"
        if ($h -ne $want) {
            throw "PresentMon 的 SHA256 与官方 release digest 不符（期望 $want）—— 别打包它"
        }
        Write-Host "   与官方 release digest 一致" -ForegroundColor Gray
    }
}

# ---------------------------------------------------------------- 4. 固件语法检查
Step '4) 固件语法检查 + 引脚表自检 (-Wall -Wextra，不接受警告)' {
    $common = @('-std=gnu++11', '-fsyntax-only', '-Wall', '-Wextra') + $Inc
    Invoke-NativeStrict $Cxx ($common + @((Join-Path $Root 'PC_Monitor\screen_draw.cpp'))) `
        'screen_draw.cpp 语法检查'
    Invoke-NativeStrict $Cxx ($common + @((Join-Path $Root 'PC_Monitor\ble_service.cpp'))) `
        'ble_service.cpp 语法检查'
    Invoke-NativeStrict $Cxx ($common + @('-x', 'c++', (Join-Path $Root 'PC_Monitor\PC_Monitor.ino'))) `
        'PC_Monitor.ino 语法检查'
    # 引脚表自检。tft_setup.h 是"本项目唯一的引脚定义处"，但它以前**从来没有被编译过**：
    # 真机上它由 TFT_eSPI.h 的 __has_include(<tft_setup.h>) 包含，而这里的语法检查
    # 走的是 tools/ui_preview 下的桩头。tools/tft_setup_check.cpp 直接包含真实引脚表，
    # static_assert 它的不变量（必需宏齐全、GPIO 范围、引脚不重复、分辨率）。
    # 它是"不变量"检查，换接线时只改 tft_setup.h，这个文件不用动。
    Invoke-NativeStrict $Cxx ($common + @((Join-Path $Root 'tools\tft_setup_check.cpp'))) `
        'tft_setup.h 引脚表自检'
}

# ---------------------------------------------------------------- 5. 跨语言协议测试
Step '5) 协议跨语言测试 (Python 编码 -> C++ 解码)' {
    $exe = Join-Path $Out 'proto_test.exe'
    Invoke-Native $Cxx (@('-std=gnu++11', '-Wall', '-Wextra') + $Inc + @(
        (Join-Path $Root 'tools\ui_preview\proto_test.cpp'),
        (Join-Path $Root 'PC_Monitor\proto.cpp'),
        '-o', $exe)) '编译 proto_test'
    Invoke-Native $exe @() 'proto_test'
}

# ---------------------------------------------------------------- 6. PC 端逻辑测试
Step '6) PC 端逻辑测试 (无硬件，含 PresentMon CSV 解析)' {
    if (-not $PyHasDeps) {
        throw "解释器缺少 psutil/bleak，无法跑 test_pc_side.py。请先： `"$Py`" -m pip install -r requirements.txt"
    }
    $env:PCMON_NO_HIDE = '1'      # 别把测试终端藏掉
    Invoke-Native $Py @('-B', (Join-Path $Root 'pc_side\test_pc_side.py')) 'test_pc_side'
}

# ---------------------------------------------------------------- 7. UI 仿真
if ($Quick) {
    Write-Host ""
    Write-Host "── 7) UI 仿真渲染 + 画面断言 " -ForegroundColor DarkGray -NoNewline
    Write-Host "-----------------------------" -ForegroundColor DarkGray
    Write-Host "   [SKIP] -Quick 指定跳过（渲染 + 全部画面断言）" -ForegroundColor Yellow
    [void]$Results.Add([pscustomobject]@{
        Step = '7) UI 仿真渲染 + 画面断言'; Ok = $true; Ms = 0; Note = 'SKIP (-Quick)' })
} else {
    Step '7) UI 仿真渲染 + 画面断言' {
        $exe    = Join-Path $Out 'preview.exe'
        $report = Join-Path $Out 'report.txt'

        # -DFORCE_PAGE=0 必须带：config.h 里的调试开关默认可能钉死某一页，
        # 那样翻页类断言会全部失效（config.h 自己警告过这一点）。
        Invoke-Native $Cxx (@('-std=gnu++11', '-O0', '-DFORCE_PAGE=0') + $Inc + @(
            (Join-Path $Root 'tools\ui_preview\preview_impl.cpp'),
            (Join-Path $Root 'tools\ui_preview\preview_main.cpp'),
            (Join-Path $Root 'PC_Monitor\screen_draw.cpp'),
            (Join-Path $Root 'PC_Monitor\proto.cpp'),
            '-x', 'c++', (Join-Path $Root 'PC_Monitor\PC_Monitor.ino'),
            '-o', $exe)) '编译 preview'

        # 用 cmd 重定向拿原始字节，避免 PowerShell 按自己的编码再转一遍
        # （report.txt 以前就因此变成过 UTF-16）。工作目录设到 $Out，
        # BMP 也是按相对路径写出来的，正好都落在产物目录里。
        Push-Location $Out
        try {
            cmd /c "`"$exe`" > report.txt 2>&1"
        } finally {
            Pop-Location
        }
        if (-not (Test-Path $report)) { throw "preview 没有产出 report.txt" }

        # preview.exe 自身永远返回 0（preview_main.cpp 正常路径无条件 return 0），
        # 所以退出码在这里没有信息量，只能读它的报告。
        # 按 tools/ui_preview/README.md 的约定：任何 "!!" 开头的行都是失败。
        $lines = @(Get-Content -Path $report -Encoding UTF8)
        $bad = @($lines | Where-Object { $_ -match '^\s*!!' })
        # 结论行形如「结论：通过」；不能用 '====' 去找 —— 报告里的画面渲染行
        # 本身就有大串 '='，会误命中。
        $concl = $lines | Where-Object { $_ -match '^\s*结论' } | Select-Object -Last 1

        Write-Host "   报告: $report"
        if ($concl) { Write-Host "   $($concl.Trim())" }
        if ($concl -and $concl -match '失败') {
            throw "UI 仿真的结论是「失败」（详见 report.txt）"
        }
        if ($bad.Count -gt 0) {
            $bad | Select-Object -First 10 | ForEach-Object { Write-Host "   $_" -ForegroundColor Red }
            throw "UI 仿真报告里有 $($bad.Count) 条 '!!' 失败行（详见 report.txt）"
        }
        Write-Host "   报告里没有 '!!' 失败行" -ForegroundColor Gray
    }
}

# ---------------------------------------------------------------- 8. 真机工具链
# 上面所有固件检查都跑在宿主机 g++ + 手写桩头上，只能证明"和自写桩的签名一致"，
# 不能证明固件能烧进板子（桩和真库的差异只有真编译器看得出来）。
# 装了 arduino-cli 就自动启用这一步；没装就明确跳过，而不是假装通过。
$ArduinoCli = if ($env:PCMON_ARDUINO_CLI) { $env:PCMON_ARDUINO_CLI } else { 'arduino-cli' }
$Fqbn       = if ($env:PCMON_FQBN) { $env:PCMON_FQBN } else { 'esp32:esp32:esp32s3' }
$CliPath    = $null
if (Test-Path $ArduinoCli) {
    $CliPath = $ArduinoCli
} else {
    $cliCmd = Get-Command $ArduinoCli -ErrorAction SilentlyContinue
    if ($cliCmd) { $CliPath = $cliCmd.Source }
}

if (-not $CliPath) {
    Write-Host ""
    Write-Host "── 8) 真机工具链编译 (arduino-cli) " -ForegroundColor DarkGray -NoNewline
    Write-Host "------------------------------" -ForegroundColor DarkGray
    Write-Host "   [SKIP] 找不到 arduino-cli" -ForegroundColor Yellow
    Write-Host "          宿主机语法检查用的是 tools/ui_preview 的桩头，只能验证" -ForegroundColor Yellow
    Write-Host "          \"签名和自写桩一致\"，替代不了真机编译。" -ForegroundColor Yellow
    Write-Host "          装上就会自动启用：" -ForegroundColor Gray
    Write-Host "            winget install ArduinoSA.CLI" -ForegroundColor Gray
    Write-Host "            arduino-cli core install esp32:esp32   # 开发用 core 3.3.10" -ForegroundColor Gray
    Write-Host "            arduino-cli lib install TFT_eSPI       # 2.5.43" -ForegroundColor Gray
    Write-Host "          已有的 CLI/板型用 PCMON_ARDUINO_CLI / PCMON_FQBN 指定。" -ForegroundColor Gray
    [void]$Results.Add([pscustomobject]@{
        Step = '8) 真机工具链编译'; Ok = $true; Ms = 0; Note = 'SKIP (未安装 arduino-cli)' })
} else {
    Step "8) 真机工具链编译 ($Fqbn)" {
        Write-Host "   使用: $CliPath"
        Invoke-Native $CliPath @('compile', '--fqbn', $Fqbn, '--warnings', 'all',
                                 (Join-Path $Root 'PC_Monitor')) 'arduino-cli compile'
    }
}

# ---------------------------------------------------------------- 汇总
Write-Host ""
Write-Host ("=" * 64) -ForegroundColor DarkGray
$failed = @($Results | Where-Object { -not $_.Ok })
foreach ($r in $Results) {
    $mark = 'PASS'
    $col  = 'Green'
    if (-not $r.Ok) { $mark = 'FAIL'; $col = 'Red' }
    Write-Host ("  [{0}] {1,-38} {2,6} ms" -f $mark, $r.Step, $r.Ms) -ForegroundColor $col
    if (-not $r.Ok) { Write-Host "         $($r.Note)" -ForegroundColor Red }
}
Write-Host ("=" * 64) -ForegroundColor DarkGray
if ($failed.Count -gt 0) {
    Write-Host "结果：$($failed.Count) 个步骤失败" -ForegroundColor Red
    exit 1
}
Write-Host "结果：全部通过" -ForegroundColor Green
Write-Host "产物目录：$Out" -ForegroundColor DarkGray
exit 0
