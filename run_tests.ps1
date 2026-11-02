<#
  Rayman Revolution port: build, then run the game unattended and check it.
  Usage (from any folder):
    run_tests.bat                       build + all scenarios
    run_tests.bat -NoBuild              skip the build
    run_tests.bat -Only choose_english  one scenario
    run_tests.bat -UpdateGolden         save this run's checkpoint pictures as the new golden pictures
  Scenarios and checks: tests\scenarios.json. Report: out\test_report.txt. Exit code 0 = no FAIL.
  Written for Windows PowerShell 5.1 (no PowerShell 7 syntax).
#>
param(
    [switch]$NoBuild,
    [string]$Only = '',
    [switch]$UpdateGolden
)

$ErrorActionPreference = 'Stop'
$Root = $PSScriptRoot
Set-Location $Root
$OutDir = Join-Path $Root 'out'
$TestsOut = Join-Path $OutDir 'tests'
$GoldenDir = Join-Path $Root 'tests\golden'
$Exe = Join-Path $Root 'out\game\ps2xRuntime\ps2EntryRunner.exe'
$Elf = Join-Path $Root 'Rayman revolution files\SLES_500.44'
$ReportPath = Join-Path $OutDir 'test_report.txt'
New-Item -ItemType Directory -Force $TestsOut | Out-Null

Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class ShotMath
{
    static byte[] Load(string path, out int w, out int h)
    {
        using (var bmp = new Bitmap(path))
        {
            w = bmp.Width; h = bmp.Height;
            var data = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            var bytes = new byte[data.Stride * h];
            Marshal.Copy(data.Scan0, bytes, 0, bytes.Length);
            bmp.UnlockBits(data);
            return bytes;
        }
    }
    // Luma variance: near 0 for a black or flat picture.
    public static double Variance(string path)
    {
        int w, h; var px = Load(path, out w, out h);
        double sum = 0, sq = 0; long n = (long)w * h;
        for (long i = 0; i < n; i++)
        {
            double l = 0.114 * px[i * 4] + 0.587 * px[i * 4 + 1] + 0.299 * px[i * 4 + 2];
            sum += l; sq += l * l;
        }
        double mean = sum / n;
        return sq / n - mean * mean;
    }
    // Mean absolute difference per colour channel (0-255); -1 if the sizes differ.
    public static double MeanDiff(string a, string b)
    {
        int wa, ha, wb, hb;
        var pa = Load(a, out wa, out ha); var pb = Load(b, out wb, out hb);
        if (wa != wb || ha != hb) return -1;
        double total = 0; long n = (long)wa * ha;
        for (long i = 0; i < n; i++)
            for (int c = 0; c < 3; c++)
                total += Math.Abs(pa[i * 4 + c] - pb[i * 4 + c]);
        return total / (n * 3);
    }
}
'@

$results = New-Object System.Collections.Generic.List[object]
function Add-Result($scenario, $label, $status, $detail) {
    $results.Add([pscustomobject]@{ Scenario = $scenario; Check = $label; Status = $status; Detail = $detail })
    $color = 'Gray'
    if ($status -eq 'PASS') { $color = 'Green' }
    elseif ($status -eq 'FAIL') { $color = 'Red' }
    elseif ($status -eq 'KNOWN-FAIL' -or $status -eq 'SKIP') { $color = 'Yellow' }
    elseif ($status -eq 'FIXED?') { $color = 'Cyan' }
    Write-Host ("  [{0,-10}] {1} - {2}" -f $status, $label, $detail) -ForegroundColor $color
}
# A check result, turned into KNOWN-FAIL / FIXED? when the check carries known_fail.
function Write-CheckResult($scenario, $check, [bool]$ok, $detail) {
    $label = $check.label
    if (-not $label) { $label = $check.type }
    $known = $check.PSObject.Properties['known_fail']
    if ($known) {
        if ($ok) { Add-Result $scenario $label 'FIXED?' ("passes now, remove known_fail? " + $detail) }
        else { Add-Result $scenario $label 'KNOWN-FAIL' ($detail + " | known: " + $known.Value) }
    }
    elseif ($ok) { Add-Result $scenario $label 'PASS' $detail }
    else { Add-Result $scenario $label 'FAIL' $detail }
}

# ---- 1. Build -------------------------------------------------------------------------------------
if (-not $NoBuild) {
    Write-Host '=== Building (cmake --build out\game --target ps2EntryRunner) ==='
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vsPath = & $vswhere -latest -products * -property installationPath
    $vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
    $buildLog = Join-Path $OutDir 'build_game.log'
    $started = Get-Date
    cmd /c "call `"$vcvars`" >nul 2>&1 && cmake --build out\game --target ps2EntryRunner > `"$buildLog`" 2>&1"
    $buildOk = ($LASTEXITCODE -eq 0)
    $minutes = [math]::Round(((Get-Date) - $started).TotalMinutes, 1)
    if ($buildOk) { Add-Result 'build' 'cmake --build' 'PASS' "$minutes min" }
    else {
        $tail = (Get-Content $buildLog -Tail 15) -join "`n"
        Add-Result 'build' 'cmake --build' 'FAIL' "see out\build_game.log`n$tail"
    }
}
else { $buildOk = $true }

# ---- 2. Scenarios ---------------------------------------------------------------------------------
# One unattended game run. Returns the exit code ('timeout' if it had to be killed); the checkpoint
# pictures end up in $shotDir. Deterministic mode is on unless the scenario says "deterministic": false.
function Invoke-GameRun($sc, $log, $shotDir) {
    Get-ChildItem $OutDir -Filter 'test_shot_*.bmp' | Remove-Item -Force
    $env:PS2X_TEST_MODE = '1'
    $env:PS2X_TEST_INPUT = $sc.input
    $env:PS2X_TEST_FRAMES = [string]$sc.frames
    $env:PS2X_TEST_SHOTS = (@($sc.shots) -join ',')
    $env:PS2X_WATCHDOG_SEC = '15'
    $env:PS2X_DETERMINISTIC = '1'
    if ($sc.PSObject.Properties['deterministic'] -and -not $sc.deterministic) { $env:PS2X_DETERMINISTIC = '0' }
    if ($sc.renderer -eq 'cpu') { $env:PS2X_GS_RENDERER = 'cpu' } else { Remove-Item Env:PS2X_GS_RENDERER -ErrorAction SilentlyContinue }

    # The game paces itself to ~60 frames per second; allow 3x plus start-up before giving up.
    $timeoutSec = [int]($sc.frames / 60 * 3) + 120
    $started = Get-Date
    $proc = Start-Process -FilePath 'cmd.exe' -ArgumentList "/c `"`"$Exe`" `"$Elf`" > `"$log`" 2>&1`"" `
        -WorkingDirectory $Root -WindowStyle Minimized -PassThru
    $finished = $proc.WaitForExit($timeoutSec * 1000)
    if (-not $finished) {
        & taskkill.exe /T /F /PID $proc.Id | Out-Null
        $code = 'timeout'
    }
    else {
        $proc.WaitForExit()
        $code = $proc.ExitCode
    }
    $seconds = [math]::Round(((Get-Date) - $started).TotalSeconds)
    Write-Host "  run took $seconds s, exit code $code, log $log"
    New-Item -ItemType Directory -Force $shotDir | Out-Null
    Get-ChildItem $OutDir -Filter 'test_shot_*.bmp' | Move-Item -Destination $shotDir -Force
    return $code
}

# The "[det] vsync=N cycle=C ee=H" fingerprints of a deterministic run.
function Get-DetLines($log) {
    if (-not (Test-Path $log)) { return @() }
    return @(Get-Content $log | ForEach-Object { if ($_ -match '\[det\] (vsync=\d+ cycle=\d+ ee=[0-9a-f]+)') { $Matches[1] } })
}

$spec = Get-Content (Join-Path $Root 'tests\scenarios.json') -Raw | ConvertFrom-Json
if ($buildOk) {
    foreach ($sc in $spec.scenarios) {
        if ($Only -and $sc.name -ne $Only) { continue }
        Write-Host "=== Scenario $($sc.name): $($sc.description) ==="
        $dir = Join-Path $TestsOut $sc.name
        if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
        New-Item -ItemType Directory -Force $dir | Out-Null
        $log = Join-Path $dir 'run.log'
        $exitCode = Invoke-GameRun $sc $log $dir

        $lines = @()
        if (Test-Path $log) { $lines = Get-Content $log }
        # [threads] lines: tick, gameFps, spu2 peak, iopcd host/fail. Not anchored: stdout logs without a line
        # break can end up in front of a stderr line.
        $threadStats = foreach ($l in $lines) {
            if ($l -match '\[threads\] tick=(\d+) gameFps=([\d.]+)') {
                $o = [pscustomobject]@{ Tick = [int]$Matches[1]; Fps = [double]$Matches[2]; Peak = 0; Host = 0; Fail = 0 }
                if ($l -match 'peak=(-?\d+)') { $o.Peak = [int]$Matches[1] }
                if ($l -match 'iopcd reads=\d+ sectors=\d+ host=(\d+) fail=(\d+)') { $o.Host = [int]$Matches[1]; $o.Fail = [int]$Matches[2] }
                $o
            }
        }
        $threadStats = @($threadStats)
        $shotFiles = @(Get-ChildItem $dir -Filter 'test_shot_*.bmp' | ForEach-Object {
                [pscustomobject]@{ Tick = [int]($_.BaseName -replace 'test_shot_', ''); Path = $_.FullName } } | Sort-Object Tick)

        foreach ($check in $sc.checks) {
            switch ($check.type) {
                'exit_code' {
                    $hang = ($lines | Select-String -Pattern '^\[test\] WATCHDOG' -SimpleMatch:$false | Select-Object -First 1)
                    $detail = "exit code $exitCode"
                    if ($hang) { $detail += "; " + $hang.Line + " (stacks in the log)" }
                    Write-CheckResult $sc.name $check ([string]$exitCode -eq [string]$check.expect) $detail
                }
                'log_absent' {
                    $hit = $lines | Select-String -Pattern $check.pattern | Select-Object -First 1
                    if ($hit) { Write-CheckResult $sc.name $check $false ("line " + $hit.LineNumber + ": " + $hit.Line.Substring(0, [math]::Min(160, $hit.Line.Length))) }
                    else { Write-CheckResult $sc.name $check $true 'not in the log' }
                }
                'shot' {
                    $shot = $shotFiles | Where-Object { $_.Tick -ge $check.tick } | Select-Object -First 1
                    if (-not $shot) { Write-CheckResult $sc.name $check $false "no picture at tick $($check.tick) (the run ended earlier?)"; break }
                    $var = [ShotMath]::Variance($shot.Path)
                    $ok = $var -ge $check.min_variance
                    $detail = "tick $($shot.Tick) variance {0:N0} (min {1})" -f $var, $check.min_variance
                    $golden = Join-Path $GoldenDir $check.golden
                    if ($UpdateGolden) {
                        New-Item -ItemType Directory -Force $GoldenDir | Out-Null
                        Copy-Item $shot.Path $golden -Force
                        $detail += "; golden updated"
                    }
                    elseif (Test-Path $golden) {
                        $diff = [ShotMath]::MeanDiff($shot.Path, $golden)
                        if ($diff -lt 0) { $ok = $false; $detail += "; size differs from golden" }
                        else {
                            if ($diff -gt $check.max_diff) { $ok = $false }
                            $detail += "; diff to golden {0:N1} (max {1})" -f $diff, $check.max_diff
                        }
                    }
                    else { $detail += "; no golden yet (run with -UpdateGolden)" }
                    Write-CheckResult $sc.name $check $ok $detail
                }
                'fps' {
                    $sel = @($threadStats | Where-Object { $_.Tick -gt $check.after_tick })
                    if ($sel.Count -eq 0) { Write-CheckResult $sc.name $check $false "no [threads] lines after tick $($check.after_tick)"; break }
                    $avg = ($sel | Measure-Object -Property Fps -Average).Average
                    Write-CheckResult $sc.name $check ($avg -ge $check.min) ("average gameFps {0:N1} after tick {1} (min {2})" -f $avg, $check.after_tick, $check.min)
                }
                'iopcd' {
                    $hostReads = ($threadStats | Measure-Object -Property Host -Sum).Sum
                    $fails = ($threadStats | Measure-Object -Property Fail -Sum).Sum
                    Write-CheckResult $sc.name $check (($hostReads -ge $check.min_host) -and ($fails -le $check.max_fail)) "host reads $hostReads, failures $fails"
                }
                'audio_peak' {
                    $sel = @($threadStats | Where-Object { $_.Tick -gt $check.after_tick })
                    $peak = 0
                    if ($sel.Count) { $peak = ($sel | Measure-Object -Property Peak -Maximum).Maximum }
                    Write-CheckResult $sc.name $check ($peak -ge $check.min) "max spu2 peak $peak after tick $($check.after_tick) (min $($check.min), full scale 32767)"
                }
                'deterministic' {
                    # Run the same scenario again and compare the per-second state fingerprints.
                    Write-Host '  second run for the determinism check'
                    $log2 = Join-Path $dir 'run2.log'
                    $exit2 = Invoke-GameRun $sc $log2 (Join-Path $dir 'run2')
                    $a = Get-DetLines $log
                    $b = Get-DetLines $log2
                    if ($a.Count -eq 0) { Write-CheckResult $sc.name $check $false 'no [det] lines (is PS2X_DETERMINISTIC on?)'; break }
                    $n = [math]::Min($a.Count, $b.Count)
                    $split = -1
                    for ($i = 0; $i -lt $n; $i++) { if ($a[$i] -ne $b[$i]) { $split = $i; break } }
                    if ($split -ge 0) {
                        Write-CheckResult $sc.name $check $false ("runs split at fingerprint $($split + 1) of $n`n  run 1: $($a[$split])`n  run 2: $($b[$split])")
                    }
                    elseif ($a.Count -ne $b.Count) {
                        Write-CheckResult $sc.name $check $false "same for $n fingerprints, then one run stopped early ($($a.Count) vs $($b.Count); run 2 exit $exit2)"
                    }
                    else { Write-CheckResult $sc.name $check $true "$n fingerprints identical (last: $($a[$n - 1]))" }
                }
                default { Add-Result $sc.name $check.type 'FAIL' "unknown check type '$($check.type)'" }
            }
        }
    }
}

# ---- 3. Report ------------------------------------------------------------------------------------
$failCount = @($results | Where-Object { $_.Status -eq 'FAIL' }).Count
$overall = 'PASS'
if ($failCount -gt 0) { $overall = 'FAIL' }
$report = New-Object System.Text.StringBuilder
[void]$report.AppendLine("Rayman Revolution test report - $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')")
[void]$report.AppendLine("Overall: $overall ($failCount FAIL)")
[void]$report.AppendLine('')
foreach ($r in $results) {
    [void]$report.AppendLine(("{0,-10} {1,-24} {2}" -f $r.Status, $r.Scenario, $r.Check))
    [void]$report.AppendLine("           " + ($r.Detail -replace "`n", "`n           "))
}
[void]$report.AppendLine('')
[void]$report.AppendLine('KNOWN-FAIL = expected failure with a known cause (does not fail the run).')
[void]$report.AppendLine('FIXED?     = a known failure passed; check it and remove known_fail from tests\scenarios.json.')
[void]$report.AppendLine('Per-scenario logs and pictures: out\tests\<scenario>\')
Set-Content -Path $ReportPath -Value $report.ToString() -Encoding UTF8
Write-Host ''
Write-Host "Overall: $overall  (report: $ReportPath)" -ForegroundColor $(if ($overall -eq 'PASS') { 'Green' } else { 'Red' })
if ($overall -eq 'PASS') { exit 0 } else { exit 1 }
