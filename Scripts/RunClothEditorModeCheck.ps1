# 在真编辑器里（不进 PIE）跑 ClothEditorModeCheck.py：放一个默认的 ACSClothActor，验它放下去就有布、只靠编辑器 tick
# 一直在模拟、布真的垂下来、拖动时钉边跟着走、改网格参数当场重建。判据与机位见那个脚本的文件头。
#
#   .\RunClothEditorModeCheck.ps1
#
# 产物在 <工程>/Saved/ClothSimShots/：editor_check.log、editor_placed.png、editor_hanging.png、editor_hanging_back.png。
# 结尾打印日志里的 CLOTH EDITOR 行；VERDICT 不是 OK（包括超时被杀）就以非零退出。
#
# ⚠️ 必须关掉「Use Less CPU when in Background」：脚本拉起的编辑器拿不到焦点，那一项会把所有视口的 Realtime 关掉，
#    编辑器 world 退成 LEVELTICK_TimeOnly，任何 actor 都不 tick。Python 改不了它（类没暴露给 Python），控制台 `set`
#    会 SaveConfig 写进用户偏好，所以走命令行 `-ini:` 覆盖。它属于 config=EditorSettings —— 引擎级的那份 EditorSettings.ini
#    （源码版在 <引擎>/Engine/Saved/Config/WindowsEditor/，安装版在 %LOCALAPPDATA%/UnrealEngine/<版本>/Saved/Config/WindowsEditor/），
#    不是工程的 EditorPerProjectUserSettings.ini。编辑器退出时会把配置写回，所以两份都在跑之前备份、跑完原样恢复
#    （原来没有的就删掉新生成的）。
# ⚠️ 这是另一个编辑器进程：默认发现别的 UnrealEditor 就不跑，-Force 才硬跑。

param(
    [int]$TimeoutMinutes = 10,
    [switch]$Force,
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
$editor = Join-Path $EngineRoot "Engine\Binaries\Win64\UnrealEditor.exe"
if (-not (Test-Path $editor)) { throw "找不到引擎：$editor（设 UE_ROOT 或传 -EngineRoot）" }

$others = Get-Process -Name "UnrealEditor*" -ErrorAction SilentlyContinue
if ($others -and -not $Force) {
    throw "有别的 UnrealEditor 在跑（PID $($others.Id -join ', ')）：那边存盘会被文件锁挡住。确认没问题再加 -Force。"
}

$outDir = Join-Path $projectRoot "Saved\ClothSimShots"
New-Item -ItemType Directory -Force $outDir | Out-Null
$log = Join-Path $outDir "editor_check.log"
if (Test-Path $log) { Remove-Item $log -Force }

# 编辑器退出时会写回的配置：引擎级 EditorSettings（源码版 / 安装版两处都备）与本工程的 EditorPerProjectUserSettings。
$engineVersion = (Get-ChildItem (Join-Path $env:LOCALAPPDATA "UnrealEngine") -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^\d+\.\d+$' } | Sort-Object Name -Descending | Select-Object -First 1).Name
$guardedInis = @(
    (Join-Path $EngineRoot "Engine\Saved\Config\WindowsEditor\EditorSettings.ini"),
    (Join-Path $projectRoot "Saved\Config\WindowsEditor\EditorPerProjectUserSettings.ini")
)
if ($engineVersion) { $guardedInis += Join-Path $env:LOCALAPPDATA "UnrealEngine\$engineVersion\Saved\Config\WindowsEditor\EditorSettings.ini" }
$guards = foreach ($ini in $guardedInis) {
    $existed = Test-Path $ini
    if ($existed) { Copy-Item $ini "$ini.clothcheck.bak" -Force }
    [pscustomobject]@{ Path = $ini; Existed = $existed }
}

$script = (Join-Path $PSScriptRoot "ClothEditorModeCheck.py") -replace '\\', '/'
$arguments = "`"$($uproject.FullName)`" -ExecCmds=`"py $script`" -AutoDeclinePackageRecovery -nosplash " +
    "-ini:EditorSettings:[/Script/UnrealEd.EditorPerformanceSettings]:bThrottleCPUWhenNotForeground=False " +
    "-AbsLog=`"$log`""

$timedOut = $false
try {
    $process = Start-Process -FilePath $editor -ArgumentList $arguments -PassThru
    if (-not $process.WaitForExit($TimeoutMinutes * 60 * 1000)) {
        $process.Kill()
        $timedOut = $true
    }
}
finally {
    foreach ($guard in $guards) {
        if ($guard.Existed) {
            Copy-Item "$($guard.Path).clothcheck.bak" $guard.Path -Force
            Remove-Item "$($guard.Path).clothcheck.bak" -Force
        }
        elseif (Test-Path $guard.Path) {
            Remove-Item $guard.Path -Force
        }
    }
}

$lines = @()
if (Test-Path $log) { $lines = @(Get-Content $log -Encoding UTF8 | Select-String "CLOTH EDITOR") }
foreach ($line in $lines) { Write-Host ($line.Line -replace '^.*?(CLOTH EDITOR)', '$1') }
if ($timedOut) { Write-Host "[ClothEditor] 超过 $TimeoutMinutes 分钟没跑完，已杀进程。日志：$log" }
Write-Host "[ClothEditor] 日志：$log；用户偏好已恢复"
if ($timedOut -or -not ($lines | Where-Object { $_.Line -match "CLOTH EDITOR VERDICT OK" })) { exit 1 }
