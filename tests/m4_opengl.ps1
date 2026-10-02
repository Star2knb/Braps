# Recorder M4 acceptance: the OpenGL backend with rec_testapp --gl.
#   powershell -File tests\m4_opengl.ps1 [-Bin build\release\bin] [-Cycles 20]
# The test app's window is parked off the desktop and never activated, so this can run while someone is
# playing a game (no hotkeys, no display-mode changes).
#  1. steady GL game, gdi32 SwapBuffers (what GLFW/SDL/LWJGL call): every frame once, file verifies, FFmpeg converts it
#  2. the game's GL state is exactly as it was after every capture (--state-check), also with wglSwapBuffers
#  3. a 3.3 core-profile context and a 4x multisampled default framebuffer
#  4. a window with another aspect ratio is letterboxed, not stretched
#  5. freeze: the video carries on with DUPs
#  6. launch: the game loads opengl32.dll late (deferred install)
#  7. attach/detach cycles: the game survives, the hook DLL unloads, no handles pile up
#  8. a Direct3D 11 game is still measured as D3D11 (and does not load opengl32.dll)
param(
    [string]$Bin = (Join-Path $PSScriptRoot '..\build\release\bin'),
    [int]$Cycles = 20
)
$ErrorActionPreference = 'Continue'
$bin = (Resolve-Path $Bin).Path
$rec = Join-Path $bin 'rec.exe'
$testapp = Join-Path $bin 'rec_testapp.exe'
$outDir = Join-Path $env:TEMP 'rec_m4_out'
Remove-Item $outDir -Recurse -Force -ErrorAction SilentlyContinue
$script:failed = 0

function Check($name, $ok, $detail = '') {
    if ($ok) { Write-Host "  PASS  $name" } else { Write-Host "  FAIL  $name $detail"; $script:failed++ }
}

# Starts the test app (off-screen), records it for $seconds, returns the newest recording and its summary.
function Record($appArgs, $recArgs, $seconds) {
    $before = @(Get-ChildItem $outDir -Filter *.avi -ErrorAction SilentlyContinue).Count
    $stdout = Join-Path $env:TEMP 'rec_m4_app.txt'
    $app = Start-Process $testapp -ArgumentList (@($appArgs) + '--offscreen') -PassThru -WindowStyle Hidden -RedirectStandardOutput $stdout
    Start-Sleep 1
    $text = & $rec attach --pid $app.Id --record-for $seconds --out $outDir @recArgs 2>&1 | Out-String
    $alive = -not $app.HasExited
    Stop-Process -Id $app.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 200
    $appText = if (Test-Path $stdout) { Get-Content $stdout -Raw } else { '' }
    $avi = Get-ChildItem $outDir -Filter *.avi -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
    $made = (@(Get-ChildItem $outDir -Filter *.avi -ErrorAction SilentlyContinue).Count -gt $before)
    $json = $null
    if ($made) { $json = Get-Content ([System.IO.Path]::ChangeExtension($avi.FullName, '.summary.json')) -Raw | ConvertFrom-Json }
    [pscustomobject]@{ Text = $text; Avi = $avi; Json = $json; Made = $made; GameAlive = $alive; AppText = $appText }
}

function Verify($avi) {
    $v = & $rec verify $avi.FullName --testapp 2>&1 | Out-String
    [pscustomobject]@{ Text = $v; Pass = ($v -match '(?m)^\s+PASS') }
}
function Num($text, $pattern) { if ($text -match $pattern) { [double]$Matches[1] } else { [double]::NaN } }

function CheckRecording($label, $r, $minFrames, $maxFrames) {
    Check "$label : a file was written" $r.Made
    if (-not $r.Made) { return }
    $j = $r.Json
    Check "$label : backend OpenGL, summary OK" (($j.backend -eq 'OpenGL') -and ($j.status -eq 'OK')) "$($j.backend) $($j.status)"
    Check "$label : $minFrames-$maxFrames frames" (($j.frames.output -ge $minFrames) -and ($j.frames.output -le $maxFrames)) "got $($j.frames.output)"
    Check "$label : nothing dropped" ($j.frames.dropped -eq 0)
    # A game that itself falls a tick behind gets a DUP there (this machine's GPU is shared with whatever else is running):
    # judge by the game's own frame rate.
    $short = [math]::Max(0, 60 - $j.game_fps.avg) / 60   # the share of ticks the game did not make
    $slip = $j.frames.output * [math]::Max(0.01, 1.5 * $short)
    Check "$label : DUPs only where the game was a whole tick late (<= 1%, or what the game's own rate explains)" ($j.frames.DUP_filled -le $slip) "DUP_filled $($j.frames.DUP_filled), game $($j.game_fps.avg) fps"
    Check "$label : hook time p50 under 1 ms" ($j.hook_cost_ms.p50 -lt 1.0) "$($j.hook_cost_ms.p50)"
    Check "$label : host BGRA conversion p50 under 3 ms" ($j.convert_ms.p50 -lt 3.0) "$($j.convert_ms.p50)"
    Check "$label : the game's GL state was never disturbed" (-not ($r.AppText -match 'STATE CHANGED')) $r.AppText
    $v = Verify $r.Avi
    Check "$label : rec verify --testapp PASS" $v.Pass
    Check "$label : no game frame missing, repeated or out of order" ($v.Text -match 'missing 0, repeated 0, out of order 0, unreadable 0')
    Check "$label : DUP frames never show a new picture" ($v.Text -match 'DUP with a new picture 0')
    $d = Num $r.Text 'largest difference Y (\d+)'
    Check "$label : colours within 2 levels of BT.601" ($d -le 2) "Y diff $d"
    return $v
}

Write-Host '1. steady GL game (gdi32 SwapBuffers, 60 fps vsync, 1366x745), lock mode, 15 s'
$r = Record @('--gl', '--vsync', '--width', '1366', '--height', '745', '--seconds', '60', '--state-check') @() 15
$null = CheckRecording 'steady' $r 890 925
if ($r.Made) {
    $mp4 = Join-Path $outDir 'steady.mp4'
    & $rec convert $r.Avi.FullName --out $mp4 2>&1 | Out-Null
    $n = if (Test-Path $mp4) { & ffprobe -v error -select_streams v:0 -count_frames -show_entries stream=nb_read_frames -of default=nw=1:nk=1 $mp4 2>&1 } else { -1 }
    Check 'steady : FFmpeg reads the same number of frames' ([int]$n -eq [int]$r.Json.frames.output) "mp4 $n, avi $($r.Json.frames.output)"
}

Write-Host '2. wglSwapBuffers (opengl32) instead of SwapBuffers, state check'
$r = Record @('--gl', '--wgl', '--vsync', '--seconds', '60', '--state-check') @() 8
$null = CheckRecording 'wgl' $r 470 500

Write-Host '3a. 3.3 core-profile context'
$r = Record @('--gl', '--core', '--vsync', '--seconds', '60', '--state-check') @() 8
$null = CheckRecording 'core' $r 470 500
Write-Host '3b. 4x multisampled default framebuffer'
$r = Record @('--gl', '--msaa', '4', '--vsync', '--seconds', '60', '--state-check') @() 8
Check 'msaa : the app got a multisampled framebuffer' ($r.AppText -match 'samples 4') $r.AppText
$null = CheckRecording 'msaa' $r 470 500

Write-Host '4. a 1000x800 window into 1280x720: letterboxed'
$r = Record @('--gl', '--vsync', '--width', '1000', '--height', '800', '--seconds', '60') @() 8
$null = CheckRecording 'letterbox' $r 470 500

Write-Host '5. freeze: the game stops presenting for 3 s'
$r = Record @('--gl', '--vsync', '--pattern', 'freeze', '--freeze-ms', '3000', '--seconds', '60') @() 12
if ($r.Made) {
    $j = $r.Json
    $v = Verify $r.Avi
    Check 'freeze : verify PASS' $v.Pass
    Check 'freeze : the freeze became about 3 s of DUPs' (($j.frames.DUP_filled -ge 165) -and ($j.frames.DUP_filled -le 200)) "got $($j.frames.DUP_filled)"
    Check 'freeze : no game frame missing, repeated or out of order' ($v.Text -match 'missing 0, repeated 0, out of order 0')
    $lost = Num $v.Text 'lost to stall latency (\d+)'
    Check 'freeze : only the in-flight frames were lost (<= 8)' ($lost -le 8) "got $lost"
    Check 'freeze : DUP frames never show a new picture' ($v.Text -match 'DUP with a new picture 0')
} else { Check 'freeze : a file was written' $false }

Write-Host '6. launch: the game loads opengl32.dll late'
$out = & $rec launch $testapp --duration 5 -- --gl --offscreen --seconds 9 --late-load-ms 1500 2>&1 | Out-String
Check 'launch succeeds and the game is hooked' ($LASTEXITCODE -eq 0 -and $out -match 'Hooked rec_testapp') $out
Check 'the install waited for the game to load a graphics API, then the OpenGL hooks went in' ($out -match 'hook installs when the game loads' -and $out -match 'OpenGL 1280x720') $out
Check 'the backend is OpenGL' ($out -match 'OpenGL') $out

Write-Host "7. $Cycles attach/detach cycles on a GL game"
$app = Start-Process $testapp -ArgumentList '--gl', '--vsync', '--offscreen', '--seconds', ([string]($Cycles * 5 + 60)) -PassThru -WindowStyle Hidden
Start-Sleep 2
$app.Refresh()
$handles0 = $app.HandleCount
$bad = 0
for ($i = 1; $i -le $Cycles; $i++) {
    $o = & $rec attach --pid $app.Id --duration 1 2>&1 | Out-String
    $code = $LASTEXITCODE
    $app.Refresh()
    $loaded = $false
    if (-not $app.HasExited) { $loaded = [bool]($app.Modules | Where-Object { $_.ModuleName -like 'rec_hook*' }) }
    if ($code -ne 0 -or $app.HasExited -or $loaded -or $o -notmatch 'Hooked rec_testapp') { $bad++; Write-Host "  cycle $i failed (exit $code, exited $($app.HasExited), still loaded $loaded)" }
}
$app.Refresh()
$alive = -not $app.HasExited
$handles1 = if ($alive) { $app.HandleCount } else { -1 }
Write-Host "  handles: $handles0 -> $handles1"
Check 'the game survived every cycle and the hook unloaded each time' ($alive -and $bad -eq 0) "bad cycles $bad"
Check 'no handles piled up (grew by < 30 over the cycles)' ($alive -and ($handles1 - $handles0) -lt 30) "$handles0 -> $handles1"
if ($alive) { Stop-Process -Id $app.Id -Force }

Write-Host '8. a Direct3D 11 game is still D3D11, with no opengl32.dll'
$app = Start-Process $testapp -ArgumentList '--vsync', '--offscreen', '--seconds', '25' -PassThru -WindowStyle Hidden
Start-Sleep 1
$mods = @($app.Modules | ForEach-Object { $_.ModuleName })
Check 'the D3D11 test app has not loaded opengl32.dll' (-not ($mods -contains 'opengl32.dll'))
$o = & $rec attach --pid $app.Id --record-for 4 --out $outDir 2>&1 | Out-String
Stop-Process -Id $app.Id -Force -ErrorAction SilentlyContinue
$avi = Get-ChildItem $outDir -Filter *.avi | Sort-Object LastWriteTime | Select-Object -Last 1
$j = Get-Content ([System.IO.Path]::ChangeExtension($avi.FullName, '.summary.json')) -Raw | ConvertFrom-Json
Check 'backend D3D11, no BGRA conversion' (($j.backend -eq 'D3D11') -and ($null -eq $j.convert_ms)) "$($j.backend)"
$v = Verify $avi
Check 'rec verify --testapp PASS' $v.Pass

if ($script:failed) { Write-Host "$($script:failed) check(s) FAILED"; exit 1 }
Write-Host 'PASS'
