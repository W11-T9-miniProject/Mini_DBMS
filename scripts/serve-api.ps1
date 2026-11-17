param(
    [string]$Db = 'data',
    [int]$Port = 8080,
    [int]$Workers = 4,
    [int]$QueueSize = 64,
    [string]$HostName = '127.0.0.1',
    [string]$PythonPath
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (!$PythonPath) {
    $installed = Get-Command python -ErrorAction SilentlyContinue
    if ($installed) { $PythonPath = $installed.Source }
    else {
        $bundled = Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
        if (Test-Path -LiteralPath $bundled) { $PythonPath = $bundled }
        else { throw 'Python 3.10+ is required. Install Python or pass -PythonPath.' }
    }
}
if (!(Test-Path -LiteralPath (Join-Path $repo 'build/minidb.dll'))) {
    throw 'Build the native library first: powershell -File scripts/build.ps1 -Target minidb'
}
Push-Location $repo
try {
    & $PythonPath -m api.server --db $Db --host $HostName --port $Port --workers $Workers --queue-size $QueueSize
    $result = $LASTEXITCODE
} finally { Pop-Location }
exit $result
