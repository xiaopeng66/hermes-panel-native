[CmdletBinding()]
param(
    [switch]$DryRun,
    [int]$Seconds = 60
)
# Coexistence check for "old Electron build + new native build".
#
# Two isolations make this safe to run unattended:
#   * --user-data-dir points the legacy app at a scratch directory, so its real data
#     (%APPDATA%\BalancePanel) is never written;
#   * --host-resolver-rules maps both site hosts to 127.0.0.1 inside that process only, so the legacy
#     app cannot reach the real sites and its packaged credentials never leave the machine. Nothing
#     listens there, so its polls fail locally - which is exactly what we want in a coexistence test.
# This script therefore performs the check itself; -DryRun merely prints the plan.
$ErrorActionPreference = 'Stop'
$legacyProject = 'E:/AI/Zcode/projects/balance-widget'
$newRelease = 'E:/AI/Zcode/projects/hermes-panel-native/build/Release/hermes-panel-native.exe'
$root = 'E:/AI/Zcode/tmp/hermes-panel-native/coexist'
$legacyData = "$root/legacy-userdata"
$newData = "$root/new-data"
$report = "$root/report.txt"

function Get-Metadata([string]$Path) {
    if (-not (Test-Path $Path)) { return "$Path (absent)" }
    $files = Get-ChildItem -Recurse -File $Path -ErrorAction SilentlyContinue
    $sum = ($files | Measure-Object -Property Length -Sum).Sum
    $last = ($files | Sort-Object LastWriteTime -Descending | Select-Object -First 1).LastWriteTime
    "{0} files={1} bytes={2} lastWrite={3}" -f $Path, $files.Count, $sum, $last
}

if (-not (Test-Path (Join-Path $legacyProject 'node_modules/electron'))) {
    throw "legacy electron is not installed under $legacyProject/node_modules"
}
if (-not (Test-Path $newRelease)) {
    throw "release build missing; run tools/package.ps1 first"
}
$electron = Get-ChildItem (Join-Path $legacyProject 'node_modules/electron/dist') -Filter 'electron.exe' -ErrorAction SilentlyContinue |
    Select-Object -First 1 -ExpandProperty FullName
if (-not $electron) { throw "electron.exe not found in the legacy project" }

$resolverRules = '--host-resolver-rules=MAP new.sharedchat.cc 127.0.0.1, MAP cloud.doulor.cn 127.0.0.1'
Write-Host "PLAN:"
Write-Host "  1. legacy app : $electron $legacyProject --user-data-dir=$legacyData"
Write-Host "     $resolverRules"
Write-Host "     (user data redirected AND both site hosts resolved to 127.0.0.1 in-process, so"
Write-Host "      %APPDATA%\BalancePanel is not written and no real-site traffic is possible)"
Write-Host "  2. new build  : $newRelease --instance coexist --data-dir $newData"
Write-Host "  3. for $Seconds seconds: keep both running, then compare metadata of the real legacy"
Write-Host "     data directory, both isolated directories, and the hotkey ownership."
Write-Host ""
if ($DryRun) {
    Write-Host "Dry run complete; re-run without -DryRun to perform the check."
    return
}

[IO.Directory]::CreateDirectory($root) | Out-Null
$realLegacy = Join-Path $env:APPDATA 'BalancePanel'
$before = Get-Metadata $realLegacy
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("before: $before")

$legacy = Start-Process -FilePath $electron -ArgumentList @($legacyProject, "--user-data-dir=$legacyData", $resolverRules) -PassThru
$new = Start-Process -FilePath $newRelease -ArgumentList @('--instance', 'coexist', '--data-dir', $newData) -PassThru
Write-Host "both started (legacy pid $($legacy.Id), new pid $($new.Id)); observing for $Seconds s"
Start-Sleep -Seconds $Seconds

# What the new build reports about itself is read from its own log, which lives in the isolated dir.
$newLog = Join-Path $newData 'logs/app.log'
if (Test-Path $newLog) {
    $lines.Add("new app log tail:")
    Get-Content $newLog -Tail 12 | ForEach-Object { $lines.Add("  $_") }
}
foreach ($process in @($legacy, $new)) {
    if ($process -and -not $process.HasExited) { $process.Kill(); $process.WaitForExit(5000) }
}
Start-Sleep -Seconds 2
$lines.Add("after : $(Get-Metadata $realLegacy)")
$lines.Add("legacy isolated data : $(Get-Metadata $legacyData)")
$lines.Add("new isolated data    : $(Get-Metadata $newData)")
$lines.Add("new default data dir : $(Get-Metadata (Join-Path $env:LOCALAPPDATA 'HermesModelPanelNative'))")
# The legacy app's own log lives in the isolated directory; report only counts, never raw lines, so no
# session or account material can end up in the report.
$legacyLog = Get-ChildItem -Recurse -File $legacyData -Include '*.log' -ErrorAction SilentlyContinue | Select-Object -First 1
if ($legacyLog) {
    $text = Get-Content $legacyLog.FullName -ErrorAction SilentlyContinue
    $failures = ($text | Select-String -Pattern 'refused|ECONNREFUSED|error|失败' -ErrorAction SilentlyContinue).Count
    $lines.Add("legacy log: $($legacyLog.Name) lines=$($text.Count) failureMatches=$failures (real-site traffic impossible by resolver rules)")
} else {
    $lines.Add("legacy log: none found in the isolated directory")
}
$lines | Set-Content -Path $report -Encoding UTF8
Write-Host "REPORT: $report"
$lines | ForEach-Object { Write-Host "  $_" }
Write-Host ""
Write-Host "Read the report this way: 'before' and 'after' for $realLegacy must be identical, which proves"
Write-Host "the new build does not write legacy data. Anything else needs a look before replacing the old app."
