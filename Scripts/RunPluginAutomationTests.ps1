# 无头跑本插件的自动化测试（真 RHI —— House / GpuInstancedMesh 那几条 GPU 用例在 -NullRHI 下不成立）。
#
#   .\RunPluginAutomationTests.ps1                                              # 整套 PCGPlugins
#   .\RunPluginAutomationTests.ps1 -Filter PCGPlugins.ComputeShaderGenerator.GpuInstancedMesh -Name instanced
#   .\RunPluginAutomationTests.ps1 -CVars "r.CSGpuInstanced.VisibleSourceIndex=0"     # 整套在某个 CVar 取值下再跑一遍
#
# 产物在 <工程>/Saved/InstanceCullOpt/Tests/<Name>/：tests.log、report/index.json。结尾打印通过 / 失败数与失败清单，
# 进程崩了（"Found N tests" 之后没跑完）会单独说出来 —— 那种情况下"0 失败"不算通过。
#
# ⚠️ 这是另一个编辑器进程：它攥着包的文件锁，别的编辑器那边存盘会失败。默认发现别的 UnrealEditor 就不跑，-Force 才硬跑。
# ⚠️ -ini 那一条不是装饰：Fab 插件启动时就建了一个 CEF 浏览器窗口（GPU 加速绘制），而引擎在自动化测试期间把
#    `BUseSupportedRHIRenderer()` 置假（CEFWebBrowserWindowRHIHelper.cpp:88），那个窗口此后任何一次加速绘制都撞
#    同文件 241 行的 check —— 整个进程带着没跑完的用例一起死，与被测代码无关（2026-10-02 实测：240 条跑到第 21 条崩）。
#    关掉 CEF 的 GPU 加速，浏览器走软件绘制，就没有这条竞态。

param(
    [string]$Filter = "PCGPlugins",
    [string]$Name = "",
    # 逗号分隔的 `名字=值`，启动时按 [ConsoleVariables] 的优先级设进去（比代码里 SetByCode 低，用例自己切 CVar 不受影响）。
    [string]$CVars = "",
    [switch]$Force,
    [int]$TimeoutMinutes = 20,
    [string]$EngineRoot = $env:UE_ROOT
)

$ErrorActionPreference = "Stop"

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$uproject = Get-ChildItem -Path $projectRoot -Filter *.uproject | Select-Object -First 1
if (-not $uproject) { throw "在 $projectRoot 下找不到 .uproject" }

if (-not $EngineRoot) {
    $association = (Get-Content $uproject.FullName -Raw -Encoding UTF8 | ConvertFrom-Json).EngineAssociation
    $builds = Get-ItemProperty "HKCU:\Software\Epic Games\Unreal Engine\Builds" -ErrorAction SilentlyContinue
    if ($builds -and $association -and $builds.PSObject.Properties[$association]) { $EngineRoot = $builds.PSObject.Properties[$association].Value }
}
$editor = Join-Path $EngineRoot "Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
if (-not (Test-Path $editor)) { throw "找不到引擎：$editor（设 UE_ROOT 或传 -EngineRoot）" }

$others = Get-Process -Name "UnrealEditor*" -ErrorAction SilentlyContinue
if ($others -and -not $Force) {
    throw "有别的 UnrealEditor 在跑（PID $($others.Id -join ', ')）：那边存盘会被文件锁挡住。确认没问题再加 -Force。"
}

if (-not $Name) { $Name = Get-Date -Format "yyyyMMdd_HHmmss" }
$outDir = Join-Path $projectRoot "Saved\InstanceCullOpt\Tests\$Name"
New-Item -ItemType Directory -Force $outDir | Out-Null
$log = Join-Path $outDir "tests.log"

$iniOverride = "-ini:Engine:[ConsoleVariables]:r.CEFGPUAcceleration=0"
foreach ($entry in ($CVars -split "," | Where-Object { $_.Trim() })) { $iniOverride += ",[ConsoleVariables]:" + $entry.Trim() }

$arguments = "`"$($uproject.FullName)`" -ExecCmds=`"Automation RunTests $Filter; Quit`" -TestExit=`"Automation Test Queue Empty`" " +
    "-ReportExportPath=`"$outDir\report`" -unattended -nopause -nosplash -nosound -stdout -AutoDeclinePackageRecovery " +
    "$iniOverride -AbsLog=`"$log`""

$started = Get-Date
$process = Start-Process -FilePath $editor -ArgumentList $arguments -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $outDir "stdout.txt")
if (-not $process.WaitForExit($TimeoutMinutes * 60 * 1000)) {
    $process.Kill()
    throw "超过 $TimeoutMinutes 分钟没跑完，已杀进程。日志：$log"
}
$elapsed = [int]((Get-Date) - $started).TotalSeconds

$lines = Get-Content $log -Encoding UTF8
$found = 0
$foundLine = $lines | Select-String "Found (\d+) automation tests" | Select-Object -Last 1
if ($foundLine) { $found = [int]$foundLine.Matches[0].Groups[1].Value }
$completed = @($lines | Select-String "Test Completed\. Result=\{(\w+)\}")
$failed = @($completed | Where-Object { $_.Matches[0].Groups[1].Value -ne "Success" })
$crash = $lines | Select-String "Assertion failed|=== Critical error|Unhandled Exception|Fatal error" | Select-Object -First 1

$cvarNote = if ($CVars) { "（$CVars）" } else { "" }
Write-Host ("[Tests] {0}{5}：发现 {1} 条，跑完 {2} 条，失败 {3} 条，用时 {4} 秒" -f $Filter, $found, $completed.Count, $failed.Count, $elapsed, $cvarNote)
foreach ($item in $failed) { Write-Host ("  FAIL " + ($item.Line -replace '^.*Test Completed\. ', '')) }
if ($crash) { Write-Host ("[Tests] 进程崩了：" + $crash.Line.Substring(0, [Math]::Min(300, $crash.Line.Length))) }
if ($found -eq 0 -or $completed.Count -lt $found) {
    Write-Host "[Tests] ⚠️ 没跑完（发现 $found 条、只跑完 $($completed.Count) 条）—— 不算通过。日志：$log"
    exit 2
}
Write-Host "[Tests] 日志：$log"
if ($failed.Count -gt 0) { exit 1 }
