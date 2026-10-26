# Hermes 模型面板（原生版）

WSL 里 Hermes 的模型与供应商管理悬浮窗：C++20 + 纯 Win32 + Direct2D 手绘界面，单 exe、无 Electron、无 WebView2、无运行时依赖。

## 安装

需要 Windows 10/11，以及一台跑着 Hermes 的 WSL 发行版。

- 直接运行：下载发行版里的 `HermesModelPanelNative.exe`，校验值见发行版附带的 `manifest.sha256`。
- 从源码构建：`cmake -S . -B build -G "Visual Studio 17 2022" -A x64`，随后 `cmake --build build --config Release --parallel`。

## 用法

启动后常驻托盘：左键托盘图标呼出面板，右键出菜单，默认热键 `Ctrl+Alt+H` 呼出 / 收起。

- **模型页**：三盏状态灯与当前模型铭牌，支持搜索、全部 / 收藏 / 最近过滤，按供应商分组，点行即切换。
- **回退链页**：梯级列表上移 / 下移 / 移除，追加条目，逐条探测连通性。
- **添加页**：新建供应商、给已有供应商加模型、删除供应商。
- **备份页**：每次写入前自动备份 `config.yaml` 与 `.env`，可筛选并一键恢复。
- **设置页**：WSL 发行版、WebUI 端口、刷新间隔、写入前 diff、自动重启网关、置顶、关闭行为、主题、呼出热键。

设置与日志存放在 `%LOCALAPPDATA%\HermesModelPanelNative\`。

## 已知限制

- 只在 Windows + WSL 环境验证过；没有 WSL 的机器上只能看到离线页。
- 配置写入只有一个入口：WSL 里的 `hermes-panel-apply.py`（写前备份、原子替换）。面板自己不直接写配置文件，所以脱开 WSL 只能读、不能改。
- 面板是 Per-Monitor V2 自绘窗口，跨屏拖动与混合 DPI 环境下如遇异常，请附显示器布局一并报告。

## 安全

发现安全问题请用 GitHub 仓库「Security → Report a vulnerability」私下报告，不要在公开 Issue 里贴出可利用细节。

## 贡献

欢迎提 Issue 与 PR。

## 许可

未附带开源许可文件，如需在项目之外使用请先联系作者。

**第三方组件**：见 `third_party/README.md`。
