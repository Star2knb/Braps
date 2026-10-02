# Recorder M5 acceptance: the rate controller, the monitors and every fault-injection flag (plan §9, §10.4, §14.3).
#   powershell -File tests\m5_faults.ps1 [-Bin build\release\bin] [-Only throttle,stall,...]
# rec_testapp's window is parked off the desktop (--offscreen), so this can run while a game is open. Close FRAPS and
# any other overlay first: they hook the same Present functions and the hook then sees no frames (benchmark hygiene).
#  1. --debug-throttle-disk: the rate controller reacts (NEAR levels, then dropped frames), the video stays in step
#  2. --debug-write-stall: slow writes are reported (E4102), nothing is lost
#  3. --debug-encoder-delay: W3102 / W3103, frames dropped on purpose, no ring overflow
#  4. --debug-drop-readback: W1201, DUPs, nothing lost between hook and host
#  5. --debug-hook-throw: E1107, the recording stops cleanly, the game keeps running
#  6. --debug-fill-disk: W4103 then E4104, the recording stops by itself and the file is finished
#  7. --debug-device-removed: E1209, capture resumes
#  8. --debug-kiero-fail: E1105 for that API only
#  9. bench-disk, the cache, doctor, and the startup check W4001
param(
    [string]$Bin = (Join-Path $PSScriptRoot '..\build\release\bin'),
    [string[]]$Only = @()
)
$ErrorActionPreference = 'Continue'
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })   # powershell -File passes -Only a,b as one string
$bin = (Resolve-Path $Bin).Path
$rec = Join-Path $bin 'rec.exe'
$testapp = Join-Path $bin 'rec_testapp.exe'
$outDir = Join-Path $env:TEMP 'rec_m5_out'
$dataDir = Join-Path $env:TEMP 'rec_m5_data'   # the disk-benchmark cache of these tests: not the user's
Remove-Item $outDir, $dataDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $dataDir -Force | Out-Null
$env:REC_DATA_DIR = $dataDir
$script:failed = 0

function Check($name, $ok, $detail = '') {
    if ($ok) { Write-Host "  PASS  $name" } else { Write-Host "  FAIL  $name $detail"; $script:failed++ }
}
function Wanted($key) { ($Only.Count -eq 0) -or ($Only -contains $key) }
function HasEvent($j, $code) { $j -and $j.events -and ($j.events.PSObject.Properties.Name -contains $code) }
function EventCount($j, $code) { if (HasEvent $j $code) { [int]$j.events.$code } else { 0 } }

if ((Get-Process -Name fraps -ErrorAction SilentlyContinue)) {
    Write-Host 'FRAPS is running: it hooks every new process and the recorder then sees no frames. Close it and run this again.'
    exit 2
}

# Starts the test app, records it for $seconds with the given rec options, returns what came out.
function Record($appArgs, $recArgs, $seconds) {
    $before = @(Get-ChildItem $outDir -Filter *.avi -ErrorAction SilentlyContinue).Count
    $app = Start-Process $testapp -ArgumentList (@($appArgs) + '--offscreen') -PassThru -WindowStyle Hidden
    Start-Sleep 1
    $text = & $rec attach --pid $app.Id --record-for $seconds --out $outDir @recArgs 2>&1 | Out-String
    $code = $LASTEXITCODE
    $alive = -not $app.HasExited
    Stop-Process -Id $app.Id -Force -ErrorAction SilentlyContinue
    $avi = Get-ChildItem $outDir -Filter *.avi -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
    $made = (@(Get-ChildItem $outDir -Filter *.avi -ErrorAction SilentlyContinue).Count -gt $before)
    $json = $null
    if ($made) { $json = Get-Content ([System.IO.Path]::ChangeExtension($avi.FullName, '.summary.json')) -Raw | ConvertFrom-Json }
    $log = ''
    if ($made) { $log = Get-Content ([System.IO.Path]::ChangeExtension($avi.FullName, '.log')) -Raw }
    [pscustomobject]@{ Text = $text; ExitCode = $code; Avi = $avi; Json = $json; Made = $made; GameAlive = $alive; Log = $log }
}
function Verify($avi) {
    $v = & $rec verify $avi.FullName --testapp 2>&1 | Out-String
    [pscustomobject]@{ Text = $v; Pass = ($v -match '(?m)^\s+PASS'); NoNewPictureInDup = ($v -match 'DUP with a new picture 0'); InOrder = ($v -match 'out of order 0') }
}
$noisy = @('--vsync', '--noise', '4500', '--width', '1366', '--height', '745', '--seconds', '120')   # ~16 MB/s of lossless video
$plain = @('--vsync', '--width', '1366', '--height', '745', '--seconds', '120')

if (Wanted 'throttle') {
    Write-Host '1. --debug-throttle-disk 10 (noisy video needs ~16 MB/s): the rate controller steps in, the video stays in step'
    $r = Record $noisy @('--debug-throttle-disk', '10') 25
    Check 'a file was written' $r.Made
    if ($r.Made) {
        $j = $r.Json
        $above = $j.seconds_at_rate_level[1] + $j.seconds_at_rate_level[2] + $j.seconds_at_rate_level[3] + $j.seconds_at_rate_level[4]
        Write-Host ("  seconds at lossless / NEAR1 / NEAR2 / NEAR3 / dropping: " + ($j.seconds_at_rate_level -join ' / ') + "; dropped on purpose $($j.frames.dropped_rate)")
        Check 'the rate controller left lossless (W3101 logged)' (($above -gt 0) -and (HasEvent $j 'W3101')) ($j.events | Out-String)
        Check 'no frame lost to a full packet queue' ($j.frames.dropped_queue -eq 0) "dropped_queue $($j.frames.dropped_queue)"
        Check 'the video is as long as the recording (one frame per tick)' ([math]::Abs($j.duration_s - 25) -lt 1.5) "duration $($j.duration_s)"
        Check 'the game was never held up by the host (about 60 fps)' ($j.game_fps.avg -gt 55) "$($j.game_fps.avg)"
        Check 'nothing dropped between the hook and the host (W1202)' (-not (HasEvent $j 'W1202'))
        $v = Verify $r.Avi
        Check 'rec verify --testapp: PASS, no DUP shows a new picture, in order' ($v.Pass -and $v.NoNewPictureInDup -and $v.InOrder) $v.Text
    }
    Write-Host '   the plan''s case, a disk far slower than the video needs (the 256 MB queue absorbs a small deficit for a long time): 1080p native with noise (~15 MB/s) on a disk throttled to 6 MB/s'
    $r = Record @('--vsync', '--noise', '3500', '--width', '1920', '--height', '1080', '--seconds', '120') @('--size', 'native', '--debug-throttle-disk', '6') 20
    Check 'a file was written' $r.Made
    if ($r.Made) {
        $j = $r.Json
        Check 'the rate controller acted (a level above lossless, or frames dropped on purpose)' (($j.seconds_at_rate_level[1] + $j.seconds_at_rate_level[2] + $j.seconds_at_rate_level[3] + $j.seconds_at_rate_level[4] -gt 0) -or ($j.frames.dropped_rate -gt 0)) ($j.seconds_at_rate_level -join '/')
        Check 'stays in sync: one frame per tick, the packet queue never overflowed' (([math]::Abs($j.duration_s - 20) -lt 1.5) -and ($j.frames.dropped_queue -eq 0)) "duration $($j.duration_s), dropped_queue $($j.frames.dropped_queue)"
        Check 'no stalls: the game ran at about 60 fps' ($j.game_fps.avg -gt 55) "$($j.game_fps.avg)"
        Check 'rec verify PASS' (Verify $r.Avi).Pass
    }
}

if (Wanted 'stall') {
    Write-Host '2. --debug-write-stall 500 3: one write in three seconds takes 500 ms longer'
    $r = Record $noisy @('--debug-write-stall', '500', '3') 15
    Check 'a file was written' $r.Made
    if ($r.Made) {
        $j = $r.Json
        Check 'the very slow writes are reported (E4102)' (EventCount $j 'E4102' -ge 2) ($j.events | Out-String)
        Check 'nothing was lost (the queue absorbed them)' (($j.frames.dropped_queue -eq 0) -and ($j.frames.dropped -eq 0)) "dropped $($j.frames.dropped)"
        Check 'rec verify PASS' (Verify $r.Avi).Pass
    }
}

if (Wanted 'encoder') {
    Write-Host '3. --debug-encoder-delay 22: every frame takes longer than the frame interval to encode'
    $r = Record $plain @('--debug-encoder-delay', '22') 15
    Check 'a file was written' $r.Made
    if ($r.Made) {
        $j = $r.Json
        Check 'W3103 slow encode' (HasEvent $j 'W3103') ($j.events | Out-String)
        Check 'W3102 encoder overloaded' (HasEvent $j 'W3102') ($j.events | Out-String)
        Check 'frames were dropped on purpose (turned into DUPs)' ($j.frames.dropped_rate -gt 0) "$($j.frames.dropped_rate)"
        Check 'the video is as long as the recording' ([math]::Abs($j.duration_s - 15) -lt 1.5) "$($j.duration_s)"
        Check 'the game ran at about 60 fps' ($j.game_fps.avg -gt 55) "$($j.game_fps.avg)"
        Check 'the host never let the frame ring overflow (no W1202)' (-not (HasEvent $j 'W1202')) ($j.events | Out-String)
        $v = Verify $r.Avi
        Check 'rec verify PASS, in order' ($v.Pass -and $v.InOrder) $v.Text
    }
}

if (Wanted 'readback') {
    Write-Host '4. --debug-drop-readback 10: the hook loses one finished read-back in ten'
    $r = Record $plain @('--debug-drop-readback', '10') 12
    Check 'a file was written' $r.Made
    if ($r.Made) {
        $j = $r.Json
        Check 'W1201 gpu_backlog logged' (HasEvent $j 'W1201') ($j.events | Out-String)
        Check 'about a tenth of the ticks became DUPs' (($j.frames.DUP_filled -gt 0.06 * $j.frames.output) -and ($j.frames.DUP_filled -lt 0.16 * $j.frames.output)) "DUP_filled $($j.frames.DUP_filled) of $($j.frames.output)"
        Check 'rec verify PASS, DUPs show no new picture' ((Verify $r.Avi).Pass -and (Verify $r.Avi).NoNewPictureInDup)
    }
}

if (Wanted 'throw') {
    Write-Host '5. --debug-hook-throw: an exception inside the hook'
    $r = Record $plain @('--debug-hook-throw') 12
    Check 'the game kept running' $r.GameAlive
    Check 'E1107 hook_exception in the recording''s log' ($r.Log -match 'E1107') $r.Log
    Check 'a file was written and finished' ($r.Made -and $r.Json.status -ne $null)
    if ($r.Made) { Check 'rec verify PASS' (Verify $r.Avi).Pass }
}

if (Wanted 'fill') {
    Write-Host '6. --debug-fill-disk: free space shrinks by 1 GB per second'
    $r = Record $plain @('--debug-fill-disk') 20
    Check 'a file was written' $r.Made
    if ($r.Made) {
        $j = $r.Json
        Check 'W4103 low disk space' (HasEvent $j 'W4103') ($j.events | Out-String)
        Check 'E4104 critical disk space' (HasEvent $j 'E4104') ($j.events | Out-String)
        Check 'the recording stopped by itself, well before the 20 s asked for' (($j.duration_s -gt 4) -and ($j.duration_s -lt 12)) "duration $($j.duration_s)"
        Check 'the file is finished and verifies' (Verify $r.Avi).Pass
        Check 'the game kept running' $r.GameAlive
    }
}

if (Wanted 'device') {
    Write-Host '7. --debug-device-removed: the device is lost once, 2 s in'
    $r = Record $plain @('--debug-device-removed') 12
    Check 'a file was written' $r.Made
    if ($r.Made) {
        $j = $r.Json
        Check 'E1209 device_removed logged' (HasEvent $j 'E1209') ($j.events | Out-String)
        Check 'capture resumed: more than 90% of the ticks have a real frame' (($j.frames.DUP_filled -lt 0.1 * $j.frames.output)) "DUP_filled $($j.frames.DUP_filled) of $($j.frames.output)"
        Check 'no capture error' ($j.status -eq 'OK') $j.status
        Check 'rec verify PASS' (Verify $r.Avi).Pass
    }
}

if (Wanted 'kiero') {
    Write-Host '8. --debug-kiero-fail d3d11: the Direct3D 11 lookup fails; opengl on a D3D app changes nothing'
    $app = Start-Process $testapp -ArgumentList '--vsync', '--offscreen', '--seconds', '25' -PassThru -WindowStyle Hidden
    Start-Sleep 1
    $o = & $rec attach --pid $app.Id --duration 3 --debug-kiero-fail d3d11 2>&1 | Out-String
    $code = $LASTEXITCODE
    $alive = -not $app.HasExited
    Stop-Process -Id $app.Id -Force -ErrorAction SilentlyContinue
    Check 'E1105 locate_failed is logged and the hook reports it cannot install' (($o -match 'could not install') -and ($code -ne 0)) $o
    Check 'the game keeps running' $alive
    Check 'the console shows E1105' ($o -match 'E1105')
    $r = Record $plain @('--debug-kiero-fail', 'opengl') 6
    Check 'a Direct3D app is unaffected by an OpenGL lookup failure' ($r.Made -and $r.Json.frames.output -gt 300)
}

if (Wanted 'bench') {
    Write-Host '9. bench-disk, its cache, doctor and the startup check'
    $bench = Join-Path $env:TEMP 'rec_m5_bench'
    $o = & $rec bench-disk --size 128MB --path $bench 2>&1 | Out-String
    Check 'bench-disk prints a sustained speed and saves it' (($o -match 'sustained \d+ MB/s') -and ($o -match 'saved for the startup check')) $o
    Check 'the cache went to REC_DATA_DIR, not to the user''s folder' (Test-Path (Join-Path $dataDir 'diskbench.toml'))
    $d = & $rec doctor 2>&1 | Out-String
    Check 'doctor shows the measured speed' ($d -match 'Disk speed\s+\d+ MB/s measured') $d
    $o2 = & $rec bench-disk --size 5XB 2>&1 | Out-String
    Check 'a bad --size is refused' ($o2 -match '--size must look like')
    # Pretend the drive is slow: rewrite the cached figure, then record.
    $toml = Join-Path $dataDir 'diskbench.toml'
    (Get-Content $toml) -replace 'sustained_mb_s = [0-9.]+', 'sustained_mb_s = 20.0' | Set-Content $toml
    $r = Record $plain @() 5
    Check 'W4001 disk_may_be_too_slow at the start of a recording (cached 20 MB/s, 720p60 needs ~33)' ($r.Made -and (HasEvent $r.Json 'W4001')) ($r.Json.events | Out-String)
    Check 'the recording itself still works' ($r.Made -and $r.Json.status -eq 'OK')
}

Remove-Item Env:\REC_DATA_DIR -ErrorAction SilentlyContinue
if ($script:failed) { Write-Host "$($script:failed) check(s) FAILED"; exit 1 }
Write-Host 'PASS'
