[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$project = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$build = Join-Path $project 'build'
$cmake = 'C:/Program Files/CMake/bin/cmake.exe'
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $project 'dist' }

Write-Host "SETUP: building $Configuration (app + setup + uninstaller)"
if (-not (Test-Path (Join-Path $build 'CMakeCache.txt'))) {
    & $cmake '-S' $project '-B' $build '-DHERMES_BUILD_TESTS=OFF' | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw "configure failed with exit $LASTEXITCODE" }
}
& $cmake '--build' $build '--config' $Configuration '--target' `
    hermes-panel-native hermes-panel-setup hermes-panel-uninstaller '--parallel' |
    ForEach-Object { Write-Host $_ }
if ($LASTEXITCODE -ne 0) { throw "build failed with exit $LASTEXITCODE" }

$version = [regex]::Match((Get-Content (Join-Path $project 'CMakeLists.txt') -Raw),
    'VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES').Groups[1].Value
if (-not $version) { throw 'cannot read project VERSION from CMakeLists.txt' }
$app = Join-Path $build "$Configuration/hermes-panel-native.exe"
$setup = Join-Path $build "$Configuration/hermes-panel-setup.exe"
$uninstaller = Join-Path $build "$Configuration/hermes-panel-uninstaller.exe"
$doc = Join-Path $project 'third_party/README.md'
foreach ($artifact in @($app, $setup, $uninstaller, $doc)) {
    if (-not (Test-Path $artifact)) { throw "missing build output: $artifact" }
}
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
$output = Join-Path $OutputDirectory "HermesModelPanelNative-Setup-$version.exe"
Copy-Item $setup $output -Force

# Append the payload: [app][uninstaller][third-party.md][footer]
# footer = magic "HNPAYL2\0" (8) + uint64 app size + uint64 uninstaller size + uint64 doc size
$parts = @($app, $uninstaller, $doc)
$sizes = @()
$stream = [IO.File]::Open($output, 'Append', 'Write')
try {
    foreach ($part in $parts) {
        $bytes = [IO.File]::ReadAllBytes($part)
        $stream.Write($bytes, 0, $bytes.Length)
        $sizes += [uint64]$bytes.Length
        Write-Host ("  payload {0} ({1:N0} bytes)" -f (Split-Path $part -Leaf), $bytes.Length)
    }
    $footer = New-Object byte[] 32
    # "HNPAYL2" + NUL: 8-byte magic, matching kPayloadMagic in installer/common.h.
    $magic = [byte[]](72, 78, 80, 65, 89, 76, 50, 0)
    [Array]::Copy($magic, 0, $footer, 0, 8)
    [BitConverter]::GetBytes($sizes[0]).CopyTo($footer, 8)
    [BitConverter]::GetBytes($sizes[1]).CopyTo($footer, 16)
    [BitConverter]::GetBytes($sizes[2]).CopyTo($footer, 24)
    $stream.Write($footer, 0, 32)
}
finally {
    $stream.Close()
}

$hash = (Get-FileHash $output -Algorithm SHA256).Hash.ToLower()
"$hash  $(Split-Path $output -Leaf)" | Set-Content -Path "$output.sha256" -Encoding ASCII
Write-Host ("SETUP OK: {0} ({1:N0} bytes)" -f $output, (Get-Item $output).Length)
Write-Host "  sha256 $hash"
