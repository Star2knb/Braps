# Recorder M1 acceptance: attach and detach many times without hurting the game.
#   powershell -File tests\m1_attach_detach.ps1 [-Bin build\release\bin] [-Cycles 100]
# Starts rec_testapp, then repeats: rec attach --duration 1 (hook in, measure, hook out). After every
# cycle the game must still be running and the hook DLL must be gone; at the end the log must show
# one hook-installed and one detached line per cycle.
param(
    [string]$Bin = (Join-Path $PSScriptRoot '..\build\release\bin'),
    [int]$Cycles = 100
)
$ErrorActionPreference = 'Stop'
$bin = (Resolve-Path $Bin).Path
$log = Join-Path $env:LOCALAPPDATA 'rec\logs\rec.log'
$logStart = if (Test-Path $log) { (Get-Item $log).Length } else { 0 }

$seconds = [int]($Cycles * 4 + 60)
$app = Start-Process (Join-Path $bin 'rec_testapp.exe') -ArgumentList '--vsync', '--seconds', $seconds -PassThru -WindowStyle Minimized
Start-Sleep -Seconds 2
$failures = 0
for ($i = 1; $i -le $Cycles; $i++) {
    $out = & (Join-Path $bin 'rec.exe') attach --pid $app.Id --duration 1 2>&1 | Out-String
    $code = $LASTEXITCODE
    $app.Refresh()
    $stillLoaded = $false
    if (-not $app.HasExited) {
        $stillLoaded = [bool]($app.Modules | Where-Object { $_.ModuleName -like 'rec_hook*' })
    }
    if ($code -ne 0 -or $app.HasExited -or $stillLoaded -or $out -notmatch 'Hooked rec_testapp') {
        $failures++
        Write-Host "cycle $i FAILED (exit $code, game exited: $($app.HasExited), hook still loaded: $stillLoaded)"
        Write-Host $out
        if ($app.HasExited) { break }
    } elseif ($i % 10 -eq 0) {
        Write-Host "cycle $i ok"
    }
}
$alive = -not $app.HasExited
if ($alive) { Stop-Process -Id $app.Id -Force }

$text = [System.IO.File]::OpenRead($log)
$text.Seek($logStart, 'Begin') | Out-Null
$reader = New-Object System.IO.StreamReader($text)
$new = $reader.ReadToEnd()
$reader.Close()
$installed = ([regex]::Matches($new, 'hooks installed')).Count
$detached = ([regex]::Matches($new, '\] detached')).Count
Write-Host "game survived: $alive; failed cycles: $failures; log: $installed x 'hooks installed', $detached x 'detached' (cycles: $Cycles)"
if (-not $alive -or $failures -or $installed -ne $Cycles -or $detached -ne $Cycles) { exit 1 }
Write-Host 'PASS'
