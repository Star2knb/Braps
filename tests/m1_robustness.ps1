# Recorder M1 acceptance: the paths that go wrong.
#   powershell -File tests\m1_robustness.ps1 [-Bin build\release\bin]
#  1. a process that never loads Direct3D: the hook waits (I1103), then detaches cleanly
#  2. the host dies while attached: the hook goes idle (E1401), a new rec takes over, `rec detach` removes it
#  3. a game that presents with Present1 is measured
#  4. anti-cheat files next to the game: launch is refused; --force overrides; nothing is started when refused
#  5. launch of a game that loads Direct3D late: I1103, then the hooks go in
param([string]$Bin = (Join-Path $PSScriptRoot '..\build\release\bin'))
$ErrorActionPreference = 'Continue'  # rec writes warnings and errors to stderr; the checks below judge the results
$bin = (Resolve-Path $Bin).Path
$rec = Join-Path $bin 'rec.exe'
$testapp = Join-Path $bin 'rec_testapp.exe'
$log = Join-Path $env:LOCALAPPDATA 'rec\logs\rec.log'
$script:failed = 0

function Check($name, $ok, $detail = '') {
    if ($ok) { Write-Host "  PASS  $name" } else { Write-Host "  FAIL  $name $detail"; $script:failed++ }
}
function LogSince($offset) {
    $s = [System.IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
    $s.Seek($offset, 'Begin') | Out-Null
    $r = New-Object System.IO.StreamReader($s)
    $t = $r.ReadToEnd(); $r.Close(); $t
}
function LogEnd() { (Get-Item $log).Length }
function HookLoaded($p) { $p.Refresh(); [bool]($p.Modules | Where-Object { $_.ModuleName -like 'rec_hook*' }) }

Write-Host '1. a process that never loads Direct3D'
$off = LogEnd
$np = Start-Process (Join-Path $env:SystemRoot 'System32\PING.EXE') -ArgumentList '-n', '60', '127.0.0.1' -PassThru -WindowStyle Hidden
Start-Sleep 1
$out = & $rec attach --pid $np.Id --duration 3 2>&1 | Out-String
$t = LogSince $off
Check 'attach to a console process succeeds' ($LASTEXITCODE -eq 0 -and $out -match 'Hooked') $out
Check 'I1103 deferred install logged' ($t -match 'I1103')
Check 'detached' ($t -match '\] detached')
Check 'hook DLL unloaded' (-not (HookLoaded $np))
Stop-Process -Id $np.Id -Force -ErrorAction SilentlyContinue

Write-Host '2. host killed while attached'
$app = Start-Process $testapp -ArgumentList '--vsync', '--seconds', '120' -PassThru -WindowStyle Minimized
Start-Sleep 2
$off = LogEnd
$host1 = Start-Process $rec -ArgumentList 'attach', '--pid', $app.Id -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $env:TEMP 'rec_m1_host1.txt')
Start-Sleep 3
Stop-Process -Id $host1.Id -Force
Start-Sleep 5
Check 'game still running after the host was killed' (-not $app.HasExited)
Check 'hook still loaded (idle)' (HookLoaded $app)
$out = & $rec attach --pid $app.Id --duration 3 2>&1 | Out-String
$t = LogSince $off
Check 'E1401 host_lost logged by the hook' ($t -match 'E1401')
Check 'a new rec takes over the old hook' ($out -match 'earlier rec is still') $out
Check 'the taken-over hook measures frames' ($out -match 'D3D11 1280x720 \| game (5\d|6\d)\.\d fps') $out
# Leave the hook in place again by killing that host, then remove it with rec detach.
$host2 = Start-Process $rec -ArgumentList 'attach', '--pid', $app.Id -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $env:TEMP 'rec_m1_host2.txt')
Start-Sleep 3
Stop-Process -Id $host2.Id -Force
$out = & $rec detach 2>&1 | Out-String
Check 'rec detach finds and removes the hook' ($out -match "pid $($app.Id): detached, hook unloaded") $out
Check 'game survived all of it' (-not $app.HasExited)
Check 'hook DLL unloaded' (-not (HookLoaded $app))
Stop-Process -Id $app.Id -Force -ErrorAction SilentlyContinue

Write-Host '3. Present1'
$app = Start-Process $testapp -ArgumentList '--vsync', '--present1', '--seconds', '30' -PassThru -WindowStyle Minimized
Start-Sleep 2
$out = & $rec attach --pid $app.Id --duration 4 2>&1 | Out-String
Check 'frame rate measured through Present1' ($out -match 'D3D11 1280x720 \| game (5\d|6\d)\.\d fps') $out
Stop-Process -Id $app.Id -Force -ErrorAction SilentlyContinue

Write-Host '4. anti-cheat'
$dir = Join-Path $env:TEMP 'rec_m1_ac'
Remove-Item $dir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $dir | Out-Null
Copy-Item $testapp $dir
New-Item (Join-Path $dir 'EasyAntiCheat_x64.dll') -ItemType File | Out-Null
$before = @(Get-Process rec_testapp -ErrorAction SilentlyContinue).Count
$off = LogEnd
$out = & $rec launch (Join-Path $dir 'rec_testapp.exe') --duration 3 -- --seconds 10 2>&1 | Out-String
Check 'launch refused (exit 1) with the finding named' ($LASTEXITCODE -eq 1 -and $out -match 'EasyAntiCheat_x64.dll' -and $out -match '--force') $out
Check 'E1004 logged' ((LogSince $off) -match 'E1004')
Check 'no game was started' (@(Get-Process rec_testapp -ErrorAction SilentlyContinue).Count -le $before)
$out = & $rec launch (Join-Path $dir 'rec_testapp.exe') --duration 3 --force -- --seconds 6 2>&1 | Out-String
Check '--force overrides, with a warning' ($LASTEXITCODE -eq 0 -and $out -match 'continuing because of --force' -and $out -match 'Hooked') $out
Start-Sleep 5
Get-Process rec_testapp -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$dir*" } | Stop-Process -Force -ErrorAction SilentlyContinue
Remove-Item $dir -Recurse -Force -ErrorAction SilentlyContinue

Write-Host '5. launch: the game loads Direct3D late (deferred install)'
$off = LogEnd
$out = & $rec launch $testapp --duration 5 -- --seconds 8 --late-load-ms 1500 2>&1 | Out-String
$t = LogSince $off
Check 'launch succeeds and the game is hooked' ($LASTEXITCODE -eq 0 -and $out -match 'Hooked rec_testapp') $out
Check 'I1103 hook_install_deferred logged' ($t -match 'I1103')
Check 'backend selected after the late load' ($t -match 'I1101 backend_selected D3D11 1280x720')
Check 'frame rate shown' ($out -match 'D3D11 1280x720 \| game \d+\.\d fps')
Start-Sleep 4
Get-Process rec_testapp -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

if ($script:failed) { Write-Host "$($script:failed) check(s) FAILED"; exit 1 }
Write-Host 'PASS'
