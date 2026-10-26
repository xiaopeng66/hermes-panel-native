cmake_minimum_required(VERSION 3.24)
project(HermesModelPanelNative VERSION 1.1.8 LANGUAGES CXX)
enable_language(RC)   # assets/hermes.rc：把 logo 编进 exe（Explorer/任务栏/窗口类都用它）

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "HermesModelPanelNative requires MSVC x64 on Windows")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options(/W4 /WX /utf-8 /permissive-)
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)

add_executable(hermes-panel-native WIN32 assets/hermes.rc src/app/main.cpp src/app/application.cpp src/app/application_data.cpp src/app/application_pages.cpp src/app/application_input.cpp src/app/application_control.cpp src/app/control_pipe.cpp src/app/panel_view.cpp src/data/settings.cpp src/data/log.cpp src/core/geometry_controller.cpp src/core/state.cpp src/render/icons.cpp src/render/widgets.cpp src/render/textfield.cpp src/ui/native_field.cpp src/ui/busy_bar.cpp src/ui/spin_glyph.cpp src/render/surface.cpp src/data/helper.cpp src/data/state.cpp src/data/probe.cpp src/data/http.cpp src/data/latency.cpp src/data/profiles.cpp src/data/unc_snapshot.cpp src/render/panel.cpp src/render/theme.cpp src/render/layout.cpp)
# 界面全自绘，没有任何 WebView2/内容运行时（H2a 的剥离）：exe 是自包含的，不链接也不需要随附 DLL。
target_include_directories(hermes-panel-native PRIVATE third_party src)
target_link_libraries(hermes-panel-native PRIVATE hermes-geometry user32 shell32 shcore dwmapi d2d1 dwrite ole32 winhttp crypt32 imm32)
target_link_options(hermes-panel-native PRIVATE "/MANIFEST:EMBED" "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/app/app.manifest")
target_link_options(hermes-panel-native PRIVATE "/MAP:$<TARGET_FILE_DIR:hermes-panel-native>/app.map" "/MAPINFO:EXPORTS")
# DESIGN §8: the control plane (named pipe + COPYDATA commands) exists in debug builds only, so a
# shipped release cannot be driven by another local process.
# 宏名是 HERMES_CONTROL_PLANE（与 src/app/control_plane.h 一致）：以前这里还在定义旧名
# BALANCE_CONTROL_PLANE，源码根本不读它——Debug/Release 是否带控制面实际由 control_plane.h
# 里的 _DEBUG 兜底决定。名字对上之后，这份按配置的定义才是真正生效的那一处。
target_compile_definitions(hermes-panel-native PRIVATE
    $<$<CONFIG:Debug>:HERMES_CONTROL_PLANE=1>
    $<$<NOT:$<CONFIG:Debug>>:HERMES_CONTROL_PLANE=0>)

add_library(hermes-geometry STATIC src/core/geometry.cpp)
target_include_directories(hermes-geometry PUBLIC src)



# 真实 wsl.exe 的桥探针：只读 ping/list，用来在没有界面的情况下看清退出码与耗时。
add_executable(wsl-probe tools/wsl_probe.cpp src/data/helper.cpp)
target_include_directories(wsl-probe PRIVATE third_party src)
target_link_libraries(wsl-probe PRIVATE user32)

# Widget gallery (H2b evidence): renders both themes' control sheet to PNG. Not a test target.
add_executable(widget-gallery tools/widget_gallery.cpp src/render/icons.cpp src/render/widgets.cpp src/render/theme.cpp)
target_include_directories(widget-gallery PRIVATE src)
target_link_libraries(widget-gallery PRIVATE d2d1 dwrite windowscodecs ole32)

# 2026-10-26（用户要求）：测试门禁整套移除 —— tests/、tools/e2e.ps1、tools/full_audit.py、
# 夹具（fake-wsl / fake-hermes / icons-check / geometry-probe / unc-parity 驱动）都已从仓库摘掉
# （备份在 E:/AI/Zcode/backups/hermes-panel-native-gates-20261026）。本文件只留产品目标：
# hermes-panel-native（应用）、hermes-geometry（几何库）、wsl-probe 与 widget-gallery（开发工具）。
