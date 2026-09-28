<#
.SYNOPSIS
    Lossless RGB comparison: RCV1 (GBR) vs Ut Video (planar RGB) vs FFV1 (RGB) on identical frames.

.DESCRIPTION
    Frames are decoded from a recording to packed BGRA by FFmpeg and piped straight into each encoder,
    so no multi-gigabyte RGB corpus is written to disk. Ut Video gets them as gbrp and FFV1 as bgr0 -
    both exact rearrangements of the same BGRA pixels (alpha dropped) - with one thread each; only the
    per-frame packet sizes are kept. rcv_bench then prints one table for all codecs.
    Timings in that table are NOT representative: FFmpeg decodes the source on the same CPU at the
    same time. Measure speed from a BGRA file on disk instead.

.EXAMPLE
    .\codec\bench\compare_rgb.ps1 -Source "Minecraft 2026-09-28 15-15-15-36.avi" -Size 1360x744 `
        -Report corpus\minecraft_rgb_report.md
#>
param(
    [Parameter(Mandatory = $true)] [string] $Source,
    [Parameter(Mandatory = $true)] [string] $Size,
    [int] $Fps = 60,
    [int] $Frames = 0,
    [string] $Bench = "",
    [string] $Report = "",
    [string] $BenchArgs = "--threads 2 --skip both",
    [switch] $ReuseSizes  # keep existing Ut Video / FFV1 size files instead of re-encoding
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
$Source = (Resolve-Path $Source).Path
$work = Join-Path $root 'corpus'
$tag = [IO.Path]::GetFileNameWithoutExtension($Source) -replace '[^\w-]', '_'
$limit = if ($Frames -gt 0) { "-frames:v $Frames" } else { "" }
# One decode of the recording to packed BGRA, piped into each consumer.
$decode = "`"$ffmpeg`" -v error -i `"$Source`" -an $limit -pix_fmt bgra -f rawvideo -"

# Windows PowerShell 5.1 mangles binary pipes between native programs, so pipelines run in cmd.exe.
function Invoke-Cmd([string] $line) {
    $bat = Join-Path $work "_compare_rgb_tmp.cmd"
    Set-Content -Path $bat -Value "@echo off`r`n$line" -Encoding ASCII
    try { & cmd.exe /c $bat } finally { Remove-Item $bat -ErrorAction SilentlyContinue }
    if ($LASTEXITCODE -ne 0) { throw "command failed ($LASTEXITCODE): $line" }
}

function Get-RgbSizes([string] $label, [string] $codecArgs, [string] $file) {
    if ($ReuseSizes -and (Test-Path $file)) {
        Write-Host "Reusing $file"
        return
    }
    Write-Host "Encoding with $label ..."
    $log = "$file.log"
    Invoke-Cmd ("$decode | `"$ffmpeg`" -hide_banner -nostats -benchmark -f rawvideo -pix_fmt bgra -s $Size -r $Fps " +
                "-i - -threads 1 $codecArgs -f nut - 2>`"$log`" | " +
                "`"$ffprobe`" -v error -f nut -i pipe:0 -select_streams v:0 -show_entries packet=size -of csv=p=0 > `"$file`"")
    $sizes = @(Get-Content $file | Where-Object { $_ -match '^\d+' })
    $m = Select-String -Path $log -Pattern 'utime=([\d.]+)s' | Select-Object -Last 1
    $header = @()
    if ($m -and $sizes.Count) {
        $encMs = [double]$m.Matches[0].Groups[1].Value * 1000 / $sizes.Count
        $header = @("# enc_ms={0:F3}" -f $encMs)
        Write-Host ("  {0}: {1} frames, {2:F2} ms/frame (ffmpeg user CPU time incl. BGRA conversion, 1 thread)" -f $label, $sizes.Count, $encMs)
    }
    Set-Content -Path $file -Value ($header + $sizes) -Encoding ASCII
}

$ut = Join-Path $work "$tag.rgb.sizes_utvideo.txt"
Get-RgbSizes 'Ut Video (gbrp)' '-pix_fmt gbrp -c:v utvideo -pred median' $ut
$ffv1 = Join-Path $work "$tag.rgb.sizes_ffv1.txt"
Get-RgbSizes 'FFV1 (bgr0)' '-pix_fmt bgr0 -c:v ffv1 -level 3' $ffv1

Write-Host "Running rcv_bench on the piped frames ..."
$out = Join-Path $work "$tag.rgb.bench.md"
# rcv_bench's progress goes to stderr; send it to a log so PowerShell doesn't treat it as an error.
Invoke-Cmd ("$decode | `"$Bench`" -i - -s $Size -r $Fps --format gbr $BenchArgs " +
            "--compare `"Ut Video (median, gbrp, 1 thread)=$ut`" --compare `"FFV1 level 3 (bgr0, 1 thread)=$ffv1`" " +
            "> `"$out`" 2>`"$out.log`"")
$lines = Get-Content $out
$lines | ForEach-Object { Write-Output $_ }
if ($Report) { $lines | Set-Content -Path $Report -Encoding UTF8; Write-Host "Report table written to $Report" }
