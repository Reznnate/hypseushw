# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 HypseusHW contributors
#
# Baseline sweep with standalone Hypseus: run each game for N seconds, capture
# a window screenshot, the audio output (SDL disk driver) and the log tail.
#   sweep-hypseus.ps1 -Exe <hypseus.exe> -Data <Daphne folder> [-Games lair,ace]
# The exe's folder is its home (hypinput.ini, logs), so use a scratch copy of
# Hypseus, not a real installation.  Its DLLs must be on PATH or beside it.
param(
    [Parameter(Mandatory)][string]$Exe,
    [Parameter(Mandatory)][string]$Data,
    [string]$Out     = (Join-Path (Split-Path (Split-Path $PSScriptRoot)) "sweep"),
    [int]$Seconds    = 60,
    [string[]]$Games = @()
)

$table = [ordered]@{
    lair='lair.txt'; ace='ace.txt'; astron='astron.txt'; galaxy='galaxy.txt';
    badlands='badlands.txt'; bega='bega.txt'; cobra='cobraab.txt'; cobraab='cobraab.txt';
    roadblaster='roadblaster.txt'; esh='esh.txt'; gpworld='gpworld.txt';
    interstellar='interstellar.txt'; mach3='mach3.txt'; sdq='sdq.txt'; tq='tq.txt'; uvt='uvt.txt'
}
if ($Games.Count) { $sel = $Games } else { $sel = $table.Keys }

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;using System.Runtime.InteropServices;
public class SweepWin{[DllImport("user32.dll")]public static extern bool GetWindowRect(IntPtr h,out R r);
[DllImport("user32.dll")]public static extern bool SetForegroundWindow(IntPtr h);public struct R{public int L,T,Rt,B;}}
'@ -ErrorAction SilentlyContinue

$env:SDL_AUDIO_DRIVER = "disk"
New-Item -ItemType Directory -Force $Out | Out-Null
$home_ = Split-Path $Exe
$results = @()

foreach ($g in $sel) {
    $raw = Join-Path $Out "$g.raw"
    Remove-Item $raw -ErrorAction SilentlyContinue
    $env:SDL_DISKAUDIOFILE = $raw
    $ff = Join-Path $Data "framefile\$($table[$g])"
    $args_ = @($g, 'vldp', '-framefile', "`"$ff`"", '-romdir', "`"$Data\roms`"",
               '-fastboot', '-prefer_samples', '-noissues', '-x', '640', '-y', '480')
    $p = Start-Process -FilePath $Exe -WorkingDirectory $home_ -ArgumentList $args_ -PassThru `
         -RedirectStandardOutput (Join-Path $Out "$g.stdout.txt") -RedirectStandardError (Join-Path $Out "$g.stderr.txt")
    Start-Sleep $Seconds
    $p.Refresh()
    $alive = -not $p.HasExited
    if ($alive -and $p.MainWindowHandle -ne 0) {
        [SweepWin]::SetForegroundWindow($p.MainWindowHandle) | Out-Null; Start-Sleep -Milliseconds 500
        $r = New-Object SweepWin+R; [SweepWin]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
        $w = $r.Rt - $r.L; $h = $r.B - $r.T
        if ($w -gt 0 -and $h -gt 0) {
            $bmp = New-Object Drawing.Bitmap $w, $h
            [Drawing.Graphics]::FromImage($bmp).CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
            $bmp.Save((Join-Path $Out "$g.png")); $bmp.Dispose()
        }
    }
    if ($alive) { $p.CloseMainWindow() | Out-Null; Start-Sleep 2; if (-not $p.HasExited) { Stop-Process $p -Force } }

    # Audio: RMS of S16 samples
    $rms = 'n/a'
    if (Test-Path $raw) {
        $b = [IO.File]::ReadAllBytes($raw); $n = [int]($b.Length / 2); $sum = 0.0; $step = [Math]::Max(1, [int]($n / 200000))
        $cnt = 0; for ($i = 0; $i -lt $n; $i += $step) { $s = [BitConverter]::ToInt16($b, $i * 2); $sum += $s * $s; $cnt++ }
        if ($cnt) { $rms = [int][Math]::Sqrt($sum / $cnt) }
        Remove-Item $raw
    }
    $errs = (Get-Content (Join-Path $Out "$g.stdout.txt") -ErrorAction SilentlyContinue | Select-String 'ERROR|FATAL|abort' | Select -First 3) -join ' | '
    $results += [pscustomobject]@{ game = $g; alive = $alive; exit = $(if ($alive) { '' } else { $p.ExitCode }); audioRms = $rms; errors = $errs }
}
$results | Format-Table -AutoSize -Wrap | Out-String -Width 250
$results | Export-Csv (Join-Path $Out 'results.csv') -NoTypeInformation
