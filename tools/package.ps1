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
# build/ 不在（刚清理过/新克隆）时先配置一次：package.ps1 必须是"一条命令就能出包"。
if (-not (Test-Path (Join-Path $build 'CMakeCache.txt'))) {
    Write-Host "  configuring (no build cache)"
    & $cmake '-S' $project '-B' $build '-DHERMES_BUILD_TESTS=OFF' | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw "configure failed with exit $LASTEXITCODE" }
}
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
# 版本号三处对齐（硬校验）：CMakeLists / app.manifest / hermes.rc，且 exe 的**固定字段**
# （VS_FIXEDFILEINFO，关于对话框读的就是它）也必须一致。以前只改 rc 的 VALUE 字符串，
# 固定字段停在旧版本 —— 关于对话框显示 1.1.6 而文件属性显示 1.1.8，真机可复现。
$cmakeFile = Join-Path $project 'CMakeLists.txt'
$cmakeVersion = [regex]::Match((Get-Content $cmakeFile -Raw), 'VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES').Groups[1].Value
if (-not $cmakeVersion) { throw 'cannot read project VERSION from CMakeLists.txt' }
$info = (Get-Item (Join-Path $target 'HermesModelPanelNative.exe')).VersionInfo
$fixedVersion = '{0}.{1}.{2}.{3}' -f $info.FileMajorPart, $info.FileMinorPart, $info.FileBuildPart, $info.FilePrivatePart
$stringVersion = $info.FileVersion
$manifestVersion = [regex]::Match((Get-Content (Join-Path $project 'src/app/app.manifest') -Raw),
    '<assemblyIdentity version="([0-9.]+)"').Groups[1].Value
foreach ($pair in @(@('CMakeLists', "$cmakeVersion.0"), @('fixed (rc FILEVERSION)', $fixedVersion),
                    @('string (rc VALUE)', $stringVersion), @('app.manifest', $manifestVersion))) {
    if ($pair[1] -ne "$cmakeVersion.0") {
        throw ("version mismatch: {0} is '{1}', CMakeLists says '{2}'" -f $pair[0], $pair[1], "$cmakeVersion.0")
    }
    Write-Host ("  version check {0} = {1}" -f $pair[0], $pair[1])
}
Write-Host "PACKAGE OK: $target (version '$stringVersion')"
Get-Content $manifest | ForEach-Object { Write-Host "  $_" }
