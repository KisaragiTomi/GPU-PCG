# 实例路性能基线 / A-B 的包装脚本（InstanceCullingOptimization.md §4.1、§9.1）。
#
# 起一个 -game 进程跑 `CSGpuInstanced.Bench`（Source/ComputeShaderGenerator/Private/Bench/CSGpuInstancedBench.cpp），
# 等它自己退出，产物在 <工程>/Saved/InstanceCullOpt/Bench/<Name>/：summary.json、每个机位一份 CsvProfile 的 .csv、
# 每个场景每个机位一张 .png（多个变体时是固定机位头两对里每一步一张，给 GpuInstancedBenchImageDiff.py 比）、bench.log。
#
#   .\RunGpuInstancedBench.ps1                                  # 整套基线
#   .\RunGpuInstancedBench.ps1 -Suite smoke -Frames 60          # 冒烟
#   .\RunGpuInstancedBench.ps1 -Filter "classic_cpu_100k+nanite_gpu" -Name step2     # 子串匹配，+ 连接；`名字!` = 整名相同
#   .\RunGpuInstancedBench.ps1 -Variants "base:r.Foo@0~new:r.Foo@1" -Pairs 4   # 交错 A/B，命令里的空格写成 @
#   .\RunGpuInstancedBench.ps1 -Suite map -Map /PCGPlugins/HouseTest/L_HouseGroundDemo -Delay 15   # 量一张现成的地图
#
# ⚠️ 这是**另一个引擎进程**：别的 UnrealEditor 开着时 GPU 读数会互相污染，而且它攥着包的文件锁、
#    那边存盘会失败。默认发现别的 UnrealEditor 就不跑，-Force 才硬跑。

param(
    [string]$Suite = "baseline",
    [string]$Name = "",
    [int]$Frames = 200,
    [int]$Pairs = 1,
    [string]$Filter = "",
    [string]$Variants = "",
    # -Suite map 用：要量的地图（包路径），以及进图后等多少秒再开始（让地图上的生产者把实例建完）。
    [string]$Map = "/Engine/Maps/Entry",
    [int]$Delay = 0,
    # -Suite map 用：再加一个对着世界坐标 "x,y,z" 的机位（离它 LookDistance 厘米，斜上方俯视）。组件的位置在 bench.log 里。
    [string]$LookAt = "",
    [int]$LookDistance = 2000,
    [int]$ResX = 1920,
    [int]$ResY = 1080,
    [switch]$ShowWindow,
    [switch]$NoCpuOps,
    [switch]$Force,
    [int]$TimeoutMinutes = 90,
    [string]$EngineRoot = $env:UE_ROOT
)

$ErrorActionPreference = "Stop"

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$uproject = Get-ChildItem -Path $projectRoot -Filter *.uproject | Select-Object -First 1
if (-not $uproject) { throw "在 $projectRoot 下找不到 .uproject" }

if (-not $EngineRoot) {
    # 没设 UE_ROOT：按 .uproject 的 EngineAssociation 去注册表里找源码版引擎。
    $association = (Get-Content $uproject.FullName -Raw -Encoding UTF8 | ConvertFrom-Json).EngineAssociation
    $builds = Get-ItemProperty "HKCU:\Software\Epic Games\Unreal Engine\Builds" -ErrorAction SilentlyContinue
    if ($builds -and $association -and $builds.PSObject.Properties[$association]) { $EngineRoot = $builds.PSObject.Properties[$association].Value }
}
$editor = Join-Path $EngineRoot "Engine\Binaries\Win64\UnrealEditor.exe"
if (-not (Test-Path $editor)) { throw "找不到引擎：$editor（设 UE_ROOT 或传 -EngineRoot）" }

$others = Get-Process -Name "UnrealEditor*" -ErrorAction SilentlyContinue
if ($others -and -not $Force) {
    throw "有别的 UnrealEditor 在跑（PID $($others.Id -join ', ')）：读数会互相污染，那边存盘也会被文件锁挡住。确认没问题再加 -Force。"
}

if (-not $Name) { $Name = "{0}_{1}" -f $Suite, (Get-Date -Format "yyyyMMdd_HHmmss") }
$outDir = Join-Path $projectRoot "Saved\InstanceCullOpt\Bench\$Name"
New-Item -ItemType Directory -Force $outDir | Out-Null
$outArg = $outDir -replace "\\", "/"

$benchArgs = "suite=$Suite out=$outArg frames=$Frames pairs=$Pairs res=${ResX}x${ResY}"
if ($Filter) { $benchArgs += " filter=$Filter" }
if ($Variants) { $benchArgs += " variants=$Variants" }
if ($NoCpuOps) { $benchArgs += " cpuops=0" }
if ($Delay -gt 0) { $benchArgs += " delay=$Delay" }
# 坐标里的逗号换成 /：-ExecCmds 拿逗号分命令。
if ($LookAt) { $benchArgs += " look=$($LookAt -replace '[,\s]+', '/') dist=$LookDistance" }

$window = if ($ShowWindow) { "" } else { "-RenderOffscreen" }
$arguments = "`"$($uproject.FullName)`" ${Map}?game=/Script/Engine.GameModeBase -game -windowed -ResX=$ResX -ResY=$ResY -ForceRes " +
    "-unattended -nosplash -nosound -NoLoadingScreen $window -ExecCmds=`"CSGpuInstanced.Bench $benchArgs`" -AbsLog=`"$outDir\bench.log`""

Write-Host "[Bench] $editor"
Write-Host "[Bench] $arguments"
$started = Get-Date
$process = Start-Process -FilePath $editor -ArgumentList $arguments -PassThru
if (-not $process.WaitForExit($TimeoutMinutes * 60 * 1000)) {
    $process.Kill()
    throw "超过 $TimeoutMinutes 分钟没跑完，已杀进程。日志：$outDir\bench.log"
}

$summary = Join-Path $outDir "summary.json"
$elapsed = [int]((Get-Date) - $started).TotalSeconds
if (-not (Test-Path $summary)) { throw "进程退出了（$elapsed 秒）但没有 summary.json —— 看 $outDir\bench.log" }
Write-Host "[Bench] 完成，用时 $elapsed 秒：$summary"
