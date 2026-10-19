# Hermes 模型面板（原生版）

WSL 里 Hermes 的模型 / 供应商 / 回退链管理悬浮窗。C++20 + 纯 Win32 + Direct2D/DirectWrite 手绘界面，单 exe 约 0.9 MB，无 Electron、无 WebView2、无运行时依赖。

## 功能

- **模型页**：三盏状态灯 + 当前模型铭牌，支持搜索、全部/收藏/最近过滤，按供应商分组，点行即切换。
- **回退链页**：梯级列表上移/下移/移除，追加条目，逐条探测连通性。
- **添加页**：新建供应商（API Key 掩码输入）、给已有供应商加模型、删除供应商（输名称确认）。
- **备份页**：每次写入前自动备份 `config.yaml` 与 `.env`，可筛选并一键恢复。
- **设置页**：WSL 发行版、WebUI 端口、刷新间隔、写入前 diff、自动重启网关、置顶、关闭行为、深/浅/跟随系统主题、呼出热键。
- **桌面集成**：托盘全菜单、热键呼出、贴边收起 + 悬停/点击滑出、跨屏拖拽、Per-Monitor DPI 自适应。

## 安全设计

- 配置写入只有一个入口：WSL 里的 `hermes-panel-apply.py`（写前备份、原子替换），面板自己从不直接写配置。
- API Key 只写 `.env`、界面不回显，日志不记录任何 Key/密码/token。

## 构建与门禁

- 构建：`cmake -S . -B build -G "Visual Studio 17 2022" -A x64`，然后 `cmake --build build --config Release --parallel`
- 门禁：`powershell -ExecutionPolicy Bypass -File tools/e2e.ps1 -Gate All`（单元/几何/生命周期/页面/外壳/回归/审计）

## 使用

- 需要 WSL + Hermes 环境；托盘左键呼出面板，右键出菜单，热键随时唤起。
- 下载发行版里的 `HermesModelPanelNative.exe` 直接运行即可，校验值见 `manifest.sha256`。
