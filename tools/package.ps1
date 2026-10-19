[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$OutputDirectory = 'E:/AI/Zcode/projects/hermes-panel-native/dist'
)
$ErrorActionPreference = 'Stop'
$project = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$build = Join-Path $project 'build'
$cmake = 'C:/Program Files/CMake/bin/cmake.exe'
$target = Join-Path $OutputDirectory 'HermesModelPanelNative'
Write-Host "PACKAGE: building $Configuration"
& $cmake '--build' $build '--config' $Configuration '--parallel' | ForEach-Object { Write-Host $_ }
if ($LASTEXITCODE -ne 0) { throw "build failed with exit $LASTEXITCODE" }
# A portable folder: the single executable plus the licence text of everything we ship.
# There is no WebView2 and no content runtime (H2a removed them): one self-contained exe.
# The debug control plane is compiled out of a release build, so the packaged binary cannot
# be driven by another process.
if (Test-Path $target) { Remove-Item -Recurse -Force $target }
[IO.Directory]::CreateDirectory($target) | Out-Null
$artifacts = @(
    @{ From = (Join-Path $build "$Configuration/hermes-panel-native.exe"); To = 'HermesModelPanelNative.exe' },
    @{ From = (Join-Path $project 'third_party/README.md'); To = 'THIRD-PARTY.md' }
)
foreach ($artifact in $artifacts) {
    if (-not (Test-Path $artifact.From)) { throw "missing build output: $($artifact.From)" }
    Copy-Item $artifact.From (Join-Path $target $artifact.To) -Force
    Write-Host ("  staged {0} ({1:N0} bytes)" -f $artifact.To, (Get-Item $artifact.From).Length)
}
$manifest = Join-Path $target 'manifest.sha256'
$lines = Get-ChildItem $target -File | Where-Object { $_.Name -ne 'manifest.sha256' } | ForEach-Object {
    "{0}  {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $_.Name
}
Set-Content -Path $manifest -Value $lines -Encoding ASCII
$version = (Get-Item (Join-Path $target 'HermesModelPanelNative.exe')).VersionInfo.FileVersion
Write-Host "PACKAGE OK: $target (version '$version')"
Get-Content $manifest | ForEach-Object { Write-Host "  $_" }
