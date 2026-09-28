<#
.SYNOPSIS
    Compression report: RCV1 vs FRAPS vs Ut Video vs FFV1 on identical frames (codec plan §11.3, §11.5).

.DESCRIPTION
    - FRAPS: per-frame packet sizes read from the original recording (-Fraps).
    - Ut Video / FFV1: the raw corpus is encoded by FFmpeg with one thread, streamed as NUT
      straight into ffprobe; only the per-frame packet sizes are kept (nothing large is written).
      The input is declared yuv420p so FFmpeg performs no colour conversion.
    - RCV1: rcv_bench encodes, decodes and verifies every frame, and prints one table for all codecs.

.EXAMPLE
    .\codec\bench\compare_baselines.ps1 -Yuv corpus\minecraft_1360x744.yuv -Size 1360x744 `
        -Fraps "Minecraft 2026-09-28 15-15-15-36.avi" -Report corpus\m2_report.md
#>
param(
    [Parameter(Mandatory = $true)] [string] $Yuv,
    [Parameter(Mandatory = $true)] [string] $Size,
    [int] $Fps = 60,
    [string] $Fraps = "",
    [string] $Bench = "",
    [string] $Report = "",
    [string] $BenchArgs = "--predictor both --slices 8,1"
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
if (-not $Bench) { $Bench = Join-Path $root 'build\release\codec\rcv_bench.exe' }
if (-not (Test-Path $Bench)) { throw "rcv_bench not found at $Bench - run build.bat release first" }

function Find-Tool([string] $name) {
    $c = Get-Command $name -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    $w = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter "$name.exe" -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($w) { return $w.FullName }
    throw "$name not found (install FFmpeg, e.g. 'winget install Gyan.FFmpeg')"
}
$ffmpeg = Find-Tool 'ffmpeg'
$ffprobe = Find-Tool 'ffprobe'

$Yuv = (Resolve-Path $Yuv).Path
$work = Split-Path -Parent $Yuv
$w, $h = $Size.Split('x') | ForEach-Object { [int]$_ }
$frameBytes = $w * $h * 3 / 2
$frames = [long]((Get-Item $Yuv).Length / $frameBytes)
Write-Host "Corpus: $Yuv ($frames frames of $Size)"

# Runs one command line through cmd.exe via a temporary .cmd file, so binary pipes and
# quoting behave (Windows PowerShell 5.1 mangles both for native commands).
function Invoke-Cmd([string] $line) {
    $bat = Join-Path $work "_compare_tmp.cmd"
    Set-Content -Path $bat -Value "@echo off`r`n$line" -Encoding ASCII
    try { & cmd.exe /c $bat } finally { Remove-Item $bat -ErrorAction SilentlyContinue }
    if ($LASTEXITCODE -ne 0) { throw "command failed ($LASTEXITCODE): $line" }
}

# Writes the size file with a '# enc_ms=' header taken from ffmpeg -benchmark (user CPU time).
function Get-FfmpegSizes([string] $label, [string] $codecArgs, [string] $file) {
    Write-Host "Encoding with $label ..."
    $log = "$file.log"
    Invoke-Cmd ("`"$ffmpeg`" -hide_banner -nostats -benchmark -f rawvideo -pix_fmt yuv420p -s $Size -r $Fps " +
                "-i `"$Yuv`" -threads 1 $codecArgs -f nut - 2>`"$log`" | " +
                "`"$ffprobe`" -v error -f nut -i pipe:0 -select_streams v:0 -show_entries packet=size -of csv=p=0 > `"$file`"")
    $sizes = @(Get-Content $file | Where-Object { $_ -match '^\d+' })
    if ($sizes.Count -ne $frames) { throw "$label produced $($sizes.Count) packets for $frames frames (see $log)" }
    $m = Select-String -Path $log -Pattern 'utime=([\d.]+)s' | Select-Object -Last 1
    $header = @()
    if ($m) {
        $encMs = [double]$m.Matches[0].Groups[1].Value * 1000 / $frames
        $header = @("# enc_ms={0:F3}" -f $encMs)
        Write-Host ("  {0}: {1:F2} ms/frame (ffmpeg user CPU time, 1 thread)" -f $label, $encMs)
    }
    Set-Content -Path $file -Value ($header + $sizes) -Encoding ASCII
}

$compare = @()
if ($Fraps) {
    $Fraps = (Resolve-Path $Fraps).Path
    $file = Join-Path $work 'sizes_fraps.txt'
    & $ffprobe -v error -select_streams v:0 -show_entries packet=size -of csv=p=0 $Fraps |
        Set-Content -Path $file -Encoding ASCII
    $compare += "FRAPS=$file"
}
$ut = Join-Path $work 'sizes_utvideo.txt'
Get-FfmpegSizes 'Ut Video' '-c:v utvideo -pred median' $ut
$compare += "Ut Video (median, 1 thread)=$ut"
$ffv1 = Join-Path $work 'sizes_ffv1.txt'
Get-FfmpegSizes 'FFV1' '-c:v ffv1 -level 3' $ffv1
$compare += "FFV1 level 3 (1 thread)=$ffv1"

Write-Host "Running rcv_bench ..."
$benchArgList = @('-i', $Yuv, '-s', $Size, '-r', "$Fps") + $BenchArgs.Split(' ', [StringSplitOptions]::RemoveEmptyEntries)
foreach ($c in $compare) { $benchArgList += @('--compare', $c) }
$csv = Join-Path $work 'bench_frames.csv'
$benchArgList += @('--csv', $csv)
# rcv_bench prints progress on stderr. In Windows PowerShell 5.1, with 'Stop' in effect and stderr
# redirected by the caller, those lines would become terminating errors - so relax it for this call
# and show stderr lines as plain text while collecting stdout (the report).
$ErrorActionPreference = 'Continue'
$out = & $Bench @benchArgList 2>&1 | ForEach-Object {
    if ($_ -is [System.Management.Automation.ErrorRecord]) { Write-Host $_.ToString(); } else { $_ }
}
$benchExit = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
$out | ForEach-Object { Write-Output $_ }
if ($Report) { $out | Set-Content -Path $Report -Encoding UTF8; Write-Host "Report table written to $Report" }
if ($benchExit -ne 0) { throw "rcv_bench failed or a round trip was not bit-exact (exit $benchExit)" }
