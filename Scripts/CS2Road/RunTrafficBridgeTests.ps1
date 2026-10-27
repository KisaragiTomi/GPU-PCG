$ErrorActionPreference = 'Stop'
$ProjectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
$OutputDirectory = Join-Path $ProjectRoot 'Saved/CodexTrafficBridge/Tests'
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$VsRoot = & "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -property installationPath
$VcTools = Get-Item (Join-Path $VsRoot 'VC/Tools/MSVC/14.38.33130')
$SdkRoot = "${env:ProgramFiles(x86)}/Windows Kits/10"
$SdkVersion = Get-Item (Join-Path $SdkRoot 'Include/10.0.22621.0')
$OldInclude = $env:INCLUDE
$OldLib = $env:LIB
try {
    $env:INCLUDE = (@((Join-Path $VcTools.FullName 'include')) + @('ucrt', 'um', 'shared') | ForEach-Object { if ([IO.Path]::IsPathRooted($_)) { $_ } else { Join-Path $SdkVersion.FullName $_ } }) -join ';'
    $env:LIB = @((Join-Path $VcTools.FullName 'lib/x64'), (Join-Path $SdkRoot "Lib/$($SdkVersion.Name)/ucrt/x64"), (Join-Path $SdkRoot "Lib/$($SdkVersion.Name)/um/x64")) -join ';'
    $Exe = Join-Path $OutputDirectory 'TrafficBridgeTests.exe'
    & (Join-Path $VcTools.FullName 'bin/Hostx64/x64/cl.exe') /nologo /std:c++17 /EHsc /W4 /WX /O2 "/I$ProjectRoot/Plugins/PCGPlugins/Source/CSRoadNet/Private" (Join-Path $PSScriptRoot 'Tests/TrafficBridgeTests.cpp') "/Fo$OutputDirectory/TrafficBridgeTests.obj" "/Fe$Exe"
    if ($LASTEXITCODE -ne 0) { throw 'Traffic/bridge test compilation failed.' }
    & $Exe
    if ($LASTEXITCODE -ne 0) { throw 'Traffic/bridge regression failed.' }
} finally {
    $env:INCLUDE = $OldInclude
    $env:LIB = $OldLib
}
