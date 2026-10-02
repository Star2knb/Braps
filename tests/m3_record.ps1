# Recorder M3 acceptance: recording to an AVI file with rec_testapp.
#   powershell -File tests\m3_record.ps1 [-Bin build\release\bin] [-SoakMinutes 0]
#  1. steady game, lock mode: every frame the app presented is in the file exactly once (P9), file verifies,
#     FFmpeg converts it with the same frame count
#  2. freeze test: the app stops presenting for 3 s; the video carries on with DUPs and stays in step
#  3. a game slower than the recording rate (45 fps into 60): DUPs only where the app did not present
#  4. free mode (--no-lock): constant frame rate file, valid
#  5. host cost while recording 720p60 (plan P7: <= 2 logical CPUs busy, P8: <= 400 MB)
#  6. optional soak (-SoakMinutes N): a long recording; queue stays empty, memory flat
param(
    [string]$Bin = (Join-Path $PSScriptRoot '..\build\release\bin'),
    [double]$SoakMinutes = 0,
    [switch]$SoakOnly
)
$ErrorActionPreference = 'Continue'
$bin = (Resolve-Path $Bin).Path
$rec = Join-Path $bin 'rec.exe'
$testapp = Join-Path $bin 'rec_testapp.exe'
$outDir = Join-Path $env:TEMP 'rec_m3_out'
Remove-Item $outDir -Recurse -Force -ErrorAction SilentlyContinue
$script:failed = 0

function Check($name, $ok, $detail = '') {
    if ($ok) { Write-Host "  PASS  $name" } else { Write-Host "  FAIL  $name $detail"; $script:failed++ }
}

# Starts the test app, records it for $seconds, returns the newest recording and its summary.
function Record($appArgs, $recArgs, $seconds) {
    $before = @(Get-ChildItem $outDir -Filter *.avi -ErrorAction SilentlyContinue).Count
    $app = Start-Process $testapp -ArgumentList $appArgs -PassThru -WindowStyle Minimized
    Start-Sleep 1
    $text = & $rec attach --pid $app.Id --record-for $seconds --out $outDir @recArgs 2>&1 | Out-String
    $alive = -not $app.HasExited
    Stop-Process -Id $app.Id -Force -ErrorAction SilentlyContinue
    $avi = Get-ChildItem $outDir -Filter *.avi -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
    $made = (@(Get-ChildItem $outDir -Filter *.avi -ErrorAction SilentlyContinue).Count -gt $before)
    $json = $null
    if ($made) { $json = Get-Content ([System.IO.Path]::ChangeExtension($avi.FullName, '.summary.json')) -Raw | ConvertFrom-Json }
    [pscustomobject]@{ Text = $text; Avi = $avi; Json = $json; Made = $made; GameAlive = $alive }
}

function Verify($avi, [switch]$testapp) {
    $a = @('verify', $avi.FullName)
    if ($testapp) { $a += '--testapp' }
    $v = & $rec @a 2>&1 | Out-String
    [pscustomobject]@{ Text = $v; Pass = ($v -match '(?m)^\s+PASS'); Fail = ($v -match '(?m)^\s+FAIL') }
}
function Num($text, $pattern) { if ($text -match $pattern) { [double]$Matches[1] } else { [double]::NaN } }

if (-not $SoakOnly) {
Write-Host '1. steady game (60 fps vsync, 1366x745), lock mode, 15 s'
$r = Record @('--vsync', '--width', '1366', '--height', '745', '--seconds', '60') @() 15
Check 'a file was written' $r.Made
if ($r.Made) {
    $j = $r.Json
    Check 'summary status OK' ($j.status -eq 'OK') $j.status
    Check 'about 900 frames (15 s at 60)' (($j.frames.output -ge 890) -and ($j.frames.output -le 925)) "got $($j.frames.output)"
    Check 'nothing dropped' (($j.frames.dropped -eq 0))
    Check 'DUPs only where the game was a whole tick late (<= 1% of frames)' ($j.frames.DUP_filled -le 0.01 * $j.frames.output) "DUP_filled $($j.frames.DUP_filled)"
    Check 'lossless compression better than 2:1' ($j.compression_ratio.real_frames -gt 2.0) "$($j.compression_ratio.real_frames)"
    Check 'no slow-write warnings on a healthy disk' (-not ($j.events.PSObject.Properties.Name -contains 'W4101')) ($j.events | Out-String)
    $v = Verify $r.Avi -testapp
    Write-Host ($v.Text -replace "`r?`n", "`n  | ").Insert(0, '  | ')
    Check 'rec verify --testapp: PASS' $v.Pass
    Check 'P9: no game frame missing, repeated or out of order' ($v.Text -match 'missing 0, repeated 0, out of order 0, unreadable 0')
    Check 'DUP frames never show a new picture' ($v.Text -match 'DUP with a new picture 0')
    $mp4 = Join-Path $outDir 'steady.mp4'
    $c = & $rec convert $r.Avi.FullName --out $mp4 2>&1 | Out-String
    Check 'rec convert wrote an MP4' (Test-Path $mp4) $c
    if (Test-Path $mp4) {
        $n = & ffprobe -v error -select_streams v:0 -count_frames -show_entries stream=nb_read_frames -of default=nw=1:nk=1 $mp4 2>&1
        Check 'FFmpeg reads the same number of frames' ([int]$n -eq [int]$j.frames.output) "mp4 $n, avi $($j.frames.output)"
    }
}

Write-Host '2. freeze test: the game stops presenting for 3 s'
$r = Record @('--vsync', '--pattern', 'freeze', '--freeze-ms', '3000', '--seconds', '60') @() 15
Check 'a file was written' $r.Made
if ($r.Made) {
    $j = $r.Json
    $stall = $j.frames.DUP_filled
    Write-Host "  DUP frames filled: $stall (3 s of freeze = 180 ticks), seconds: $($j.duration_s)"
    Check 'the freeze became about 3 s of DUPs' (($stall -ge 165) -and ($stall -le 200)) "got $stall"
    Check 'the video has one frame per tick over the whole recording (15 s)' (($j.duration_s -ge 14.7) -and ($j.duration_s -le 15.5)) "got $($j.duration_s)"
    $v = Verify $r.Avi -testapp
    Check 'rec verify --testapp: PASS' $v.Pass
    Check 'no game frame missing, repeated or out of order across the freeze' ($v.Text -match 'missing 0, repeated 0, out of order 0')
    $lost = Num $v.Text 'lost to stall latency (\d+)'
    Write-Host "  frames lost to stall latency (in the GPU read-back when the game stopped presenting): $lost (at most 8 expected)"
    Check 'only the in-flight frames were lost at the freeze (<= 8)' ($lost -le 8) "got $lost"
    Check 'DUP frames never show a new picture' ($v.Text -match 'DUP with a new picture 0')
    # The longest run of consecutive DUP rows in the telemetry is the freeze.
    $csv = [System.IO.Path]::ChangeExtension($r.Avi.FullName, '.frames.csv')
    $rows = Import-Csv $csv
    $run = 0; $best = 0
    foreach ($row in $rows) { if ($row.type -eq 'DUP') { $run++; if ($run -gt $best) { $best = $run } } else { $run = 0 } }
    Check 'one unbroken run of about 180 DUPs in the telemetry' (($best -ge 165) -and ($best -le 200)) "longest run $best"
    $mp4 = Join-Path $outDir 'freeze.mp4'
    & $rec convert $r.Avi.FullName --out $mp4 2>&1 | Out-Null
    $n = if (Test-Path $mp4) { & ffprobe -v error -select_streams v:0 -count_frames -show_entries stream=nb_read_frames -of default=nw=1:nk=1 $mp4 2>&1 } else { -1 }
    Check 'FFmpeg converts it with the same frame count' ([int]$n -eq [int]$j.frames.output) "mp4 $n, avi $($j.frames.output)"
}

Write-Host '3. game slower than the recording rate (45 fps into 60)'
$r = Record @('--fps-cap', '45', '--seconds', '60') @() 12
Check 'a file was written' $r.Made
if ($r.Made) {
    $j = $r.Json
    $expectedDup = 0.25 * $j.frames.output
    Write-Host "  DUP frames: $($j.frames.DUP) of $($j.frames.output) (a quarter expected: ~$([int]$expectedDup))"
    Check 'about a quarter of the frames are DUPs' (($j.frames.DUP -gt 0.18 * $j.frames.output) -and ($j.frames.DUP -lt 0.32 * $j.frames.output)) "got $($j.frames.DUP)"
    $v = Verify $r.Avi -testapp
    Check 'rec verify --testapp: PASS' $v.Pass
    Check 'no game frame missing, repeated or out of order' ($v.Text -match 'missing 0, repeated 0, out of order 0')
}

Write-Host '4. free mode (--no-lock)'
$r = Record @('--vsync', '--seconds', '60') @('--no-lock') 10
Check 'a file was written' $r.Made
if ($r.Made) {
    $j = $r.Json
    Check 'the summary says not locked' ($j.output.lock -eq $false)
    Check 'about 600 frames' (($j.frames.output -ge 590) -and ($j.frames.output -le 625)) "got $($j.frames.output)"
    $v = Verify $r.Avi
    Check 'rec verify: PASS' $v.Pass
}


Write-Host '5. host cost while recording (rec.exe, 720p60 lossless)'
$app = Start-Process $testapp -ArgumentList '--vsync', '--width', '1366', '--height', '745', '--seconds', '60' -PassThru -WindowStyle Minimized
Start-Sleep 1
$host1 = Start-Process $rec -ArgumentList 'attach', '--pid', $app.Id, '--record-for', '24', '--out', $outDir -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $env:TEMP 'rec_m3_host.txt')
Start-Sleep 8
$p = Get-Process -Id $host1.Id
$c0 = $p.TotalProcessorTime.TotalSeconds; $t0 = Get-Date
Start-Sleep 12
$p.Refresh()
$c1 = $p.TotalProcessorTime.TotalSeconds; $t1 = Get-Date
$cpus = ($c1 - $c0) / ($t1 - $t0).TotalSeconds
$mem = $p.PrivateMemorySize64 / 1MB; $ws = $p.WorkingSet64 / 1MB
$host1.WaitForExit(30000) | Out-Null
Stop-Process -Id $app.Id -Force -ErrorAction SilentlyContinue
Write-Host ("  rec.exe used {0:N2} logical CPUs on average (of 4) over 12 s; private memory {1:N0} MB, working set {2:N0} MB" -f $cpus, $mem, $ws)
Check 'P7: host CPU <= 2 logical CPUs' ($cpus -le 2.0) ("got {0:N2}" -f $cpus)
Check 'P8: host memory <= 400 MB' ($mem -le 400) ("got {0:N0} MB" -f $mem)

}

if ($SoakMinutes -gt 0) {
    Write-Host "6. soak: $SoakMinutes minutes at 720p60"
    $app = Start-Process $testapp -ArgumentList '--vsync', '--width', '1366', '--height', '745', '--seconds', ([int]($SoakMinutes * 60 + 120)) -PassThru -WindowStyle Minimized
    Start-Sleep 2
    $app.Refresh(); $h0 = $app.HandleCount; $m0 = $app.PrivateMemorySize64
    $text = & $rec attach --pid $app.Id --record-for ([int]($SoakMinutes * 60)) --out $outDir 2>&1 | Out-String
    $app.Refresh(); $h1 = $app.HandleCount; $m1 = $app.PrivateMemorySize64
    Stop-Process -Id $app.Id -Force -ErrorAction SilentlyContinue
    $avi = Get-ChildItem $outDir -Filter *.avi | Sort-Object LastWriteTime | Select-Object -Last 1
    $j = Get-Content ([System.IO.Path]::ChangeExtension($avi.FullName, '.summary.json')) -Raw | ConvertFrom-Json
    Write-Host "  frames $($j.frames.output), $([int]($avi.Length / 1MB)) MB, game handles $h0 -> $h1, game memory $([int]($m0/1MB)) -> $([int]($m1/1MB)) MB"
    Check 'soak: status OK, nothing dropped' (($j.status -eq 'OK') -and ($j.frames.dropped -eq 0))
    Check 'soak: frame count matches the time' ([math]::Abs($j.frames.output - $SoakMinutes * 3600) -le 30) "got $($j.frames.output)"
    Check 'soak: game handles flat (<= +6)' (($h1 - $h0) -le 6)
    Check 'soak: game memory flat (<= +30 MB)' (($m1 - $m0) -le 30MB)
    $v = Verify $avi -testapp
    Check 'soak: rec verify --testapp passes with nothing missing' ($v.Pass -and ($v.Text -match 'missing 0, repeated 0, out of order 0'))
}

if ($script:failed) { Write-Host "$($script:failed) check(s) FAILED"; exit 1 }
Write-Host 'PASS'
