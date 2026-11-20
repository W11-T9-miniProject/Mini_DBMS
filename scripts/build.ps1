param([switch]$Test, [switch]$Sanitize,
      [Alias('Target')][ValidateSet('mini_db', 'db_tests', 'db_bench', 'db_load', 'minidb')][string[]]$BuildTarget)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path $vswhere)) { throw 'Install Visual Studio Build Tools with Desktop development with C++.' }
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$installation) { throw 'MSVC C/C++ tools were not found.' }
$vcvars = Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
# Import the compiler environment into this PowerShell process only.
$environment = & cmd.exe /d /c "call `"$vcvars`" >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'Cannot initialize MSVC environment.' }
foreach ($line in $environment) {
    $separator = $line.IndexOf('=')
    if ($separator -gt 0) { [Environment]::SetEnvironmentVariable($line.Substring(0, $separator), $line.Substring($separator + 1), 'Process') }
}
$core = @('abi', 'error', 'platform', 'bptree', 'parser', 'storage', 'engine', 'script') | ForEach-Object { Join-Path $repo "src/$_.c" }
$targets = @{
    mini_db = 'src/main.c'
    db_tests = 'tests/test_main.c'
    db_bench = 'tools/benchmark.c'
    db_load = 'tools/load_benchmark.c'
    minidb = ''
}
if ($Sanitize) { $targets = @{ db_tests_asan = 'tests/test_main.c' } }
elseif ($BuildTarget) {
    $selectedTargets = @{}
    foreach ($name in $BuildTarget) { $selectedTargets[$name] = $targets[$name] }
    if ($Test) { $selectedTargets['db_tests'] = 'tests/test_main.c' }
    $targets = $selectedTargets
}
$compilerFlags = @('/nologo', '/std:c11', '/W4', '/WX', '/Zi', '/D_CRT_SECURE_NO_WARNINGS')
if ($Sanitize) { $compilerFlags += @('/O1', '/fsanitize=address') }
else { $compilerFlags += '/O2' }
foreach ($target in $targets.Keys) {
    $objectDirectory = Join-Path $repo "build/obj/$target"
    New-Item -ItemType Directory -Force -Path $objectDirectory | Out-Null
    Push-Location $objectDirectory
    try {
        if ($target -eq 'minidb') {
            $output = Join-Path $repo 'build/minidb.dll'
            & cl.exe @compilerFlags /LD /DMINIDB_SHARED "/I$repo/include" "/I$repo/src" @core "/Fe:$output" /link /INCREMENTAL:NO
        } else {
            $output = Join-Path $repo "build/$target.exe"
            & cl.exe @compilerFlags "/I$repo/include" "/I$repo/src" @core (Join-Path $repo $targets[$target]) "/Fe:$output" /link /INCREMENTAL:NO
        }
        if ($LASTEXITCODE -ne 0) { throw "Build failed: $target" }
    } finally { Pop-Location }
}
if ($Test -or $Sanitize) {
    Push-Location $repo
    try {
        $testExecutable = if ($Sanitize) { 'build/db_tests_asan.exe' } else { 'build/db_tests.exe' }
        & (Join-Path $repo $testExecutable)
        if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
    } finally { Pop-Location }
}
