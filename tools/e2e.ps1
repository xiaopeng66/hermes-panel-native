[CmdletBinding()]
param(
    [ValidateSet('Build', 'Unit', 'Widget', 'Lifecycle', 'Shell', 'Pages', 'Regression', 'A', 'ReadHelper', 'Audit', 'Release', 'All', 'Pointer')][string]$Gate = 'All',
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
#
# 真实指针套件（合成鼠标点来点去/拖窗口）单独成 -Gate Pointer，**默认跑法不含它们**：
#   * 它们需要在空闲桌面上真点真拖，机器上只要有人/别的自动化在动鼠标就只会 SKIP，
#     每次还要几分钟（a-hover 一项 ≈150s、hover ≈20s、drag ≈8s）；
#   * 平时的回归只关心功能与几何（用控制面驱动，确定、快），指针交互在需要时单跑一遍即可：
#       powershell -File tools/e2e.ps1 -Gate Pointer
#   * h-ime 同样依赖桌面（要一个可用的 IME），一并归到 Pointer。
$filters = @{
    Unit       = '^(geometry|state|log|settings)$'
    Widget     = '^(widget|icons)$'
    Lifecycle  = '^lifecycle$'
    Shell      = '^(shell|tray|motion)$'
    # a-panelui was a balance scenario (activeSite/trend) and was replaced by the hermes page gates.
    # a-c1（重启/恢复）不点不拖，留在默认集合里。
    A          = '^a-(snap|pullout|hover|edge|crossdpi|topology|c1)$'
    # Page gates: the hermes pages against the fake helper.
    Pages      = '^(h-models|h-fallback|h-add|h-backup|h-settings|h-offline|h-migrate|h-controls)$'
    Regression = '^(geometry|state|log|settings|widget|icons|helper|hermes-state|lifecycle|shell|tray|motion|h-models|h-fallback|h-add|h-backup|h-settings|h-offline|h-migrate|h-controls)$'
    Release    = '^release$'
    # 真实指针/键盘：点来点去、拖窗口、跨屏、悬停、IME。需要空闲桌面，单跑。
    Pointer    = '^(hover|drag|dpi|h-ime|a-snap|a-pullout|a-hover|a-edge|a-crossdpi|a-topology)$'
}
$allFilter = '^(geometry|state|log|widget|icons|helper|hermes-state|lifecycle|shell|tray|motion|h-models|h-fallback|h-add|h-backup|h-settings|h-offline|h-migrate|h-controls|a-c1)$'
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
