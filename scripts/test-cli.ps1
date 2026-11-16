param([string]$Executable = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (!$Executable) { $Executable = Join-Path $repo 'build/mini_db.exe' }
$database = Join-Path $repo ('build/cli-test-' + [Guid]::NewGuid().ToString('N'))

function Assert-True($condition, [string]$message) {
    if (!$condition) { throw $message }
}

$output = & $Executable --db $database --file (Join-Path $repo 'examples/demo.sql')
Assert-True ($LASTEXITCODE -eq 0) 'SQL file execution failed'
Assert-True (($output -join "`n").Contains('first_id=4')) 'Multi-statement INSERT failed'
Assert-True (($output -join "`n").Contains('INDEX LOOKUP')) 'Index lookup missing'
Assert-True (($output -join "`n").Contains('INDEX RANGE SCAN')) 'Range lookup missing'
Assert-True (($output -join "`n").Contains('TABLE SCAN')) 'Table scan missing'

$output = & $Executable --db $database --sql 'SELECT * FROM users WHERE id=2'
Assert-True ($LASTEXITCODE -eq 0) 'Reopen failed'
Assert-True (($output -join "`n").Contains('rows=1 examined=1')) 'Rebuilt index did not read exactly one row'

$output = & $Executable --db $database --scan --sql 'SELECT * FROM users WHERE id=2'
Assert-True ($LASTEXITCODE -eq 0) 'Forced scan failed'
Assert-True (($output -join "`n").Contains('rows=1 examined=4')) 'Forced scan did not read four rows'

# Native stderr is captured in a file to avoid PowerShell treating
# an expected SQL error as a terminating NativeCommandError.
$errorFile = Join-Path $database 'expected-error.txt'
$process = Start-Process -FilePath $Executable -ArgumentList @('--db', ('"' + $database + '"'), '--sql', '"SELECT * FROM nonexistent"') -NoNewWindow -Wait -PassThru -RedirectStandardError $errorFile
Assert-True ($process.ExitCode -eq 1) 'Invalid SQL must return exit code 1'
Assert-True ((Get-Content -Raw $errorFile).Contains('SQL_ERROR')) 'SQL error message missing'

$output = "SELECT name FROM users WHERE id=3;" | & $Executable --db $database
Assert-True ($LASTEXITCODE -eq 0) 'Standard input execution failed'
Assert-True (($output -join "`n").Contains("O'Brien; Kim")) 'Quoted semicolon string was not preserved'

# Remove only files this test created inside its unique directory.
foreach ($name in @('users.db', 'users.lock', 'expected-error.txt')) {
    Remove-Item -LiteralPath (Join-Path $database $name)
}
Remove-Item -LiteralPath $database
Write-Output 'PASS CLI: SQL file, restart, index/scan, error exit, stdin'
