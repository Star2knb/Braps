# Recorder M3: recording while the CPU is busy (a heavy game leaves little for the host).
#   powershell -File tests\m3_stress.ps1 [-Bin build\release\bin] [-Hogs 3] [-Seconds 15] [-HogPriority Normal|AboveNormal|High]
# Records rec_testapp twice, unloaded and with $Hogs busy-loop processes competing for the CPUs, and prints
# how the host coped: encoder spikes, frames dropped because the host fell behind (W1202), DUPs filled.
# Fails if the loaded recording drops more than 1% of its frames or if the file does not verify.
param(
    [string]$Bin = (Join-Path $PSScriptRoot '..\build\release\bin'),
    [int]$Hogs = 3,
    [int]$Seconds = 15,
    [string]$HogPriority = 'Normal'    # Normal | AboveNormal | High: a foreground game's threads can outrank the host
)
$ErrorActionPreference = 'Continue'
$bin = (Resolve-Path $Bin).Path
$rec = Join-Path $bin 'rec.exe'
$testapp = Join-Path $bin 'rec_testapp.exe'
$outDir = Join-Path $env:TEMP 'rec_m3_stress'
Remove-Item $outDir -Recurse -Force -ErrorAction SilentlyContinue
$script:failed = 0
function Check($name, $ok, $detail = '') {
    if ($ok) { Write-Host "  PASS  $name" } else { Write-Host "  FAIL  $name $detail"; $script:failed++ }
}

function Run($label, $hogs) {
    $procs = @()
    for ($i = 0; $i -lt $hogs; $i++) {
        $procs += Start-Process powershell -ArgumentList '-NoProfile', '-Command', "`$e=(Get-Date).AddSeconds($($Seconds + 8)); while((Get-Date) -lt `$e){}" -PassThru -WindowStyle Hidden
        $procs[-1].PriorityClass = $HogPriority
    }
    $app = Start-Process $testapp -ArgumentList '--vsync', '--width', '1366', '--height', '745', '--seconds', ($Seconds + 30) -PassThru -WindowStyle Minimized
    Start-Sleep 1
    $text = & $rec attach --pid $app.Id --record-for $Seconds --out $outDir 2>&1 | Out-String
    Stop-Process -Id $app.Id -Force -ErrorAction SilentlyContinue
    $procs | ForEach-Object { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
    $avi = Get-ChildItem $outDir -Filter *.avi | Sort-Object LastWriteTime | Select-Object -Last 1
    $j = Get-Content ([System.IO.Path]::ChangeExtension($avi.FullName, '.summary.json')) -Raw | ConvertFrom-Json
    $v = & $rec verify $avi.FullName --testapp 2>&1 | Out-String
    $drops = if ($j.events.PSObject.Properties.Name -contains 'W1202') { $j.events.W1202 } else { 0 }
    $captured = $j.frames.captured
    $lost = $j.frames.output - $captured + $j.frames.DUP_identical   # ticks that got a DUP because the host dropped a frame
    Write-Host ("  {0}: {1} frames, DUP filled {2}, encode avg {3:N1} / p99 {4:N1} / max {5:N1} ms, W1202 events {6}, game {7:N1} fps, pacing p99 {8:N1} ms" -f $label, $j.frames.output, $j.frames.DUP_filled, $j.encode_ms.avg, $j.encode_ms.p99, $j.encode_ms.max, $drops, $j.game_fps.avg, $j.pacing.error_p99_ms)
    [pscustomobject]@{ Json = $j; Verify = $v; Filled = $j.frames.DUP_filled; Frames = $j.frames.output }
}

Write-Host "unloaded"
$a = Run 'unloaded' 0
Check 'unloaded: verifies' ($a.Verify -match '(?m)^\s+PASS')
Write-Host "loaded with $Hogs busy processes at $HogPriority priority"
$b = Run "loaded($Hogs)" $Hogs
Check 'loaded: verifies' ($b.Verify -match '(?m)^\s+PASS')
Check 'loaded: host dropped no more than 1% of the frames (DUP filled <= 1%)' ($b.Filled -le 0.01 * $b.Frames) "filled $($b.Filled) of $($b.Frames)"

if ($script:failed) { Write-Host "$($script:failed) check(s) FAILED"; exit 1 }
Write-Host 'PASS'
