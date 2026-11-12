# 安装版（installer）

原生安装器 + 卸载器：单文件 `Setup.exe`，不依赖 Inno Setup / NSIS / .NET。

## 构建

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/build-installer.ps1
```

产物：

- `dist/HermesModelPanelNative-Setup-<版本>.exe` —— 安装包（setup.exe + 主程序 + 卸载器 + 第三方声明，尾部 32 字节 footer 记录长度）
- `dist/HermesModelPanelNative-Setup-<版本>.exe.sha256`

`installer/common.h/.cpp` 是两者共用的基础设施（payload 读取、注册表、快捷方式、关闭正在运行的面板、递归删除等）；`setup.cpp` 与 `uninstall.cpp` 只放各自的 UI 和流程。

## 安装器

默认装到 `D:\自制小工具\HermesModelPanelNative`，安装时可选：

- 开机自动启动（HKCU `...\CurrentVersion\Run`，值为 `"<安装目录>\HermesModelPanelNative.exe" --startup`，登录后启动到托盘）
- 桌面快捷方式 / 开始菜单快捷方式
- 安装不会删除目标目录已有的 `data`（设置/档案/日志）

「应用和功能」里的卸载入口指向安装目录的 `uninstall.exe`。

### 静默参数

```
--silent | /S            无界面安装
--dir=<路径> | /DIR=<路径>  安装目录（默认 D:\自制小工具\HermesModelPanelNative）
--no-autostart           不写开机自启项（已有项会被移除）
--no-desktop             不建桌面快捷方式（已有快捷方式会被删除）
--no-startmenu           不建开始菜单快捷方式（已有文件夹会被删除）
--launch                 安装完启动面板（普通启动，显示贴条/面板）
--import-data=<目录>      把旧便携版 data 目录里的 settings.json / profiles.json 导入目标 data（只在目标没有 settings.json 时）
```

退出码：0 成功，1 失败（失败原因写在 `%TEMP%\HermesModelPanelNative-setup.log`）。

## 卸载器

GUI 默认勾选「保留设置与日志（data 文件夹）」。静默：

```
uninstall.exe --silent          # 保留 data
uninstall.exe --silent --purge  # 连安装目录和全部数据一起删
```

卸载器会把自身复制到 `%TEMP%` 再启动副本（正在运行的 exe 不能删除/重命名），副本完成删除后用一个隐藏的 `cmd` 延迟自删。

## 说明

- 安装/卸载都是 `asInvoker`（HKCU，无需管理员）；装到 `Program Files` 这类受保护目录会因权限失败并给出提示。
- 升级 = 直接再跑一次新的 Setup：覆盖 exe/卸载器，保留 `data`，刷新快捷方式、自启项与卸载登记。
