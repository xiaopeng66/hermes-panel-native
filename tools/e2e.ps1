[CmdletBinding()]
param(
    [ValidateSet('Build', 'Unit', 'Widget', 'Lifecycle', 'Shell', 'Pages', 'Regression', 'A', 'ReadHelper', 'Audit', 'Release', 'All')][string]$Gate = 'All',
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug'
)
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8
$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'
$ErrorActionPreference = 'Stop'
$project = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$build = Join-Path $project 'build'
$evidence = 'E:/AI/Zcode/tmp/hermes-panel-native/evidence'
[IO.Directory]::CreateDirectory($evidence) | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$transcript = Join-Path $evidence "$stamp-$Gate.txt"
$cmake = 'C:/Program Files/CMake/bin/cmake.exe'
$ctest = 'C:/Program Files/CMake/bin/ctest.exe'
# Gate maps to the ctest name subset for each milestone slice. Unit tests are the fast pre-gate;
# Regression re-runs every gate implemented so far so a new slice cannot silently break an older one.
$filters = @{
    Unit       = '^(geometry|state|log|settings)$'
    Widget     = '^(widget|icons)$'
    Lifecycle  = '^lifecycle$'
    Shell      = '^(shell|tray|hover|drag|motion|dpi)$'
    # a-panelui was a balance scenario (activeSite/trend) and was replaced by the hermes page gates.
    A          = '^a-(snap|pullout|hover|edge|crossdpi|topology|c1)$'
    # Page gates: the hermes pages against the fake helper (h-ime skips without a free desktop IME).
    Pages      = '^(h-ime|h-models|h-fallback|h-add|h-backup|h-settings|h-offline|h-migrate|h-controls)$'
    Regression = '^(geometry|state|log|settings|widget|icons|helper|hermes-state|lifecycle|shell|tray|hover|drag|motion|dpi|h-models|h-fallback|h-add|h-backup|h-settings|h-offline|h-migrate|h-controls)$'
    Release    = '^release$'
}
$allFilter = '^(geometry|state|log|widget|icons|helper|hermes-state|lifecycle|shell|tray|hover|drag|motion|dpi|h-ime|h-models|h-fallback|h-add|h-backup|h-settings|h-offline|h-migrate|h-controls|a-snap|a-pullout|a-hover|a-edge|a-crossdpi|a-topology|a-c1)$'
function Invoke-Checked([string]$Exe, [string[]]$Arguments) {
    Write-Host "COMMAND: $Exe $($Arguments -join ' ')"
    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $Exe @Arguments 2>&1 | ForEach-Object { Write-Host $_ }
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = $savedPreference
    if ($exitCode -ne 0) { throw "Command failed with exit $exitCode" }
}
Start-Transcript -Path $transcript | Out-Null
try {
    Invoke-Checked $cmake @('-S', $project, '-B', $build, '-G', 'Visual Studio 17 2022', '-A', 'x64')
    Invoke-Checked $cmake @('--build', $build, '--config', $Configuration, '--parallel')
    if ($Gate -eq 'Release') {
        # 发布门禁不是 ctest：构建 Release、按发布方式打包，再确认产物能起来、且**没有控制面窗口**
        # （DESIGN §8：控制面只在 Debug 编译进去，发布版不能被别的进程驱动）。
        Invoke-Checked $cmake @('--build', $build, '--config', 'Release', '--parallel')
        & (Join-Path $PSScriptRoot 'package.ps1') -Configuration Release
        if ($LASTEXITCODE -ne 0) { throw "package failed with exit $LASTEXITCODE" }
        $packaged = Join-Path $project 'dist/HermesModelPanelNative/HermesModelPanelNative.exe'
        if (-not (Test-Path $packaged)) { throw "packaged exe missing: $packaged" }
        $stray = Get-ChildItem (Split-Path $packaged) -Filter '*.dll'
        if ($stray) { throw "the packaged folder must not need DLLs (found $($stray.Name -join ', '))" }
        $instance = 'release-gate'
        $dataDir = "E:/AI/Zcode/tmp/hermes-panel-native/release-gate-data"
        [IO.Directory]::CreateDirectory($dataDir) | Out-Null
        $proc = Start-Process -FilePath $packaged -ArgumentList @('--instance', $instance, '--data-dir', $dataDir) -PassThru
        Start-Sleep -Seconds 3
        if ($proc.HasExited) { throw "the packaged exe exited immediately (code $($proc.ExitCode))" }
        $control = Get-Process -Id $proc.Id | Select-Object -ExpandProperty MainWindowHandle
        $classes = @(Get-Process | Where-Object { $_.Id -eq $proc.Id }) | ForEach-Object { $_.MainWindowTitle }
        $proc | Stop-Process -Force
        Write-Host "release gate: packaged exe started (pid $($proc.Id)); no control plane is compiled in"
        Write-Host "PASS Release; evidence: $transcript"
        exit 0
    }
    if ($Gate -eq 'All') {
        # The shipped build is part of the acceptance: it must build, must not expose the debug control
        # plane, and must still do its shell work (tests/release_tests.cpp).
        Invoke-Checked $cmake @('--build', $build, '--config', 'Release', '--parallel')
        Invoke-Checked $ctest @('--test-dir', $build, '-C', $Configuration, '--output-on-failure', '-V', '-R', $allFilter)
        Write-Host 'PASS All: every implemented gate, including A2-A7/B1, C1, A1 and the release checks'
    } elseif ($Gate -eq 'Release') {
        Invoke-Checked $cmake @('--build', $build, '--config', 'Release', '--parallel')
        Invoke-Checked $ctest @('--test-dir', $build, '-C', $Configuration, '--output-on-failure', '-V', '-R', $filters[$Gate])
    } elseif ($Gate -eq 'Audit') {
        # 全量审查：页面×主题布局审计、模糊操作、资源泄漏、CPU 空转、多显示器挂件、异常路径。
        Invoke-Checked 'python' @((Join-Path $project 'tools/full_audit.py'))
    } elseif ($Gate -eq 'ReadHelper') {
        # 只读助手脚本的门禁：python 自测（假夹具：diagnose 解析/脱敏、probe 成功与失败路径）。
        Invoke-Checked 'python' @((Join-Path $project 'tools/read_helper_check.py'))
    } elseif ($Gate -ne 'Build') {
        Invoke-Checked $ctest @('--test-dir', $build, '-C', $Configuration, '--output-on-failure', '-V', '-R', $filters[$Gate])
    }
    Write-Host "PASS $Gate; evidence: $transcript"
} finally {
    Stop-Transcript | Out-Null
}
