cmake_minimum_required(VERSION 3.24)
project(HermesModelPanelNative VERSION 0.1.0 LANGUAGES CXX)
enable_language(RC)   # assets/hermes.rc：把 logo 编进 exe（Explorer/任务栏/窗口类都用它）

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "HermesModelPanelNative requires MSVC x64 on Windows")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options(/W4 /WX /utf-8 /permissive-)
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)

add_executable(hermes-panel-native WIN32 assets/hermes.rc src/app/main.cpp src/app/application.cpp src/app/application_data.cpp src/app/application_pages.cpp src/app/application_input.cpp src/app/application_control.cpp src/app/control_pipe.cpp src/app/panel_view.cpp src/data/settings.cpp src/data/log.cpp src/core/geometry_controller.cpp src/core/state.cpp src/render/icons.cpp src/render/widgets.cpp src/render/textfield.cpp src/render/surface.cpp src/data/helper.cpp src/data/state.cpp src/data/probe.cpp src/data/http.cpp src/data/latency.cpp src/data/profiles.cpp src/data/unc_snapshot.cpp src/render/panel.cpp src/render/theme.cpp src/render/layout.cpp)
# 界面全自绘，没有任何 WebView2/内容运行时（H2a 的剥离）：exe 是自包含的，不链接也不需要随附 DLL。
target_include_directories(hermes-panel-native PRIVATE third_party src)
target_link_libraries(hermes-panel-native PRIVATE hermes-geometry user32 shell32 shcore dwmapi d2d1 dwrite ole32 winhttp crypt32 imm32)
target_link_options(hermes-panel-native PRIVATE "/MANIFEST:EMBED" "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/app/app.manifest")
target_link_options(hermes-panel-native PRIVATE "/MAP:$<TARGET_FILE_DIR:hermes-panel-native>/app.map" "/MAPINFO:EXPORTS")
# DESIGN §8: the control plane (named pipe + COPYDATA commands) exists in debug builds only, so a
# shipped release cannot be driven by another local process.
target_compile_definitions(hermes-panel-native PRIVATE
    $<$<CONFIG:Debug>:BALANCE_CONTROL_PLANE=1>
    $<$<NOT:$<CONFIG:Debug>>:BALANCE_CONTROL_PLANE=0>)

add_library(hermes-geometry STATIC src/core/geometry.cpp)
target_include_directories(hermes-geometry PUBLIC src)



# 真实 wsl.exe 的桥探针：只读 ping/list，用来在没有界面的情况下看清退出码与耗时。
add_executable(wsl-probe tools/wsl_probe.cpp src/data/helper.cpp)
target_include_directories(wsl-probe PRIVATE third_party src)
target_link_libraries(wsl-probe PRIVATE user32)

# Fake wsl.exe for helper-bridge tests (INV-H5).
add_executable(fake-wsl tools/fake_wsl.cpp)
target_link_libraries(fake-wsl PRIVATE user32)

# Fake hermes helper for the page gates (INV-H5): speaks the real protocol from a fixture file.
add_executable(fake-hermes tools/fake_hermes.cpp)
target_include_directories(fake-hermes PRIVATE third_party)
target_link_libraries(fake-hermes PRIVATE user32)

# Widget gallery (H2b evidence): renders both themes' control sheet to PNG. Not a test target.
add_executable(widget-gallery tools/widget_gallery.cpp src/render/icons.cpp src/render/widgets.cpp src/render/theme.cpp)
# 图标像素门禁：每个图标都要"真的画出可见像素"（真机 bug：纯圆图标继承残留笔刷色 → 屏幕上消失）。
add_executable(icons-check tools/icons_check.cpp src/render/icons.cpp src/render/theme.cpp)
target_include_directories(icons-check PRIVATE src)
target_link_libraries(icons-check PRIVATE d2d1 dwrite windowscodecs ole32)
add_test(NAME icons COMMAND icons-check)
target_include_directories(widget-gallery PRIVATE src)
target_link_libraries(widget-gallery PRIVATE d2d1 dwrite windowscodecs ole32)

add_executable(geometry-probe tests/harness/probe.cpp)
target_include_directories(geometry-probe PRIVATE third_party)
target_link_libraries(geometry-probe PRIVATE user32 gdi32 windowscodecs ole32)

add_executable(environment-probe tests/harness/environment.cpp)
target_link_libraries(environment-probe PRIVATE user32 shcore)

# One-off drag repro used to diagnose the snap-band failure (kept until H2c clears it).
add_executable(drag-repro tools/drag_repro.cpp tests/harness/client.h)
target_include_directories(drag-repro PRIVATE third_party tests)
target_link_libraries(drag-repro PRIVATE user32 ws2_32)

include(CTest)
if(BUILD_TESTING)
    add_executable(shell-tests tests/shell_tests.cpp)
    target_include_directories(shell-tests PRIVATE third_party)
    target_link_libraries(shell-tests PRIVATE user32)
    add_test(NAME shell COMMAND shell-tests $<TARGET_FILE:hermes-panel-native>)
    set_tests_properties(shell PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77)
    add_executable(tray-tests tests/tray_tests.cpp)
    target_include_directories(tray-tests PRIVATE third_party)
    target_link_libraries(tray-tests PRIVATE user32)
    add_test(NAME tray COMMAND tray-tests $<TARGET_FILE:hermes-panel-native> $<TARGET_FILE:fake-hermes>)
    set_tests_properties(tray PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
    add_executable(hover-tests tests/hover_tests.cpp)
    target_include_directories(hover-tests PRIVATE third_party)
    target_link_libraries(hover-tests PRIVATE user32)
    add_test(NAME hover COMMAND hover-tests $<TARGET_FILE:hermes-panel-native>)
    set_tests_properties(hover PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77)
    add_executable(drag-tests tests/drag_tests.cpp)
    target_include_directories(drag-tests PRIVATE third_party)
    target_link_libraries(drag-tests PRIVATE user32)
    add_test(NAME drag COMMAND drag-tests $<TARGET_FILE:hermes-panel-native>)
    set_tests_properties(drag PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77)
    add_executable(motion-tests tests/motion_tests.cpp)
    target_include_directories(motion-tests PRIVATE third_party)
    target_link_libraries(motion-tests PRIVATE user32)
    add_test(NAME motion COMMAND motion-tests $<TARGET_FILE:hermes-panel-native>)
    set_tests_properties(motion PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
    add_executable(a-tests tests/a_tests.cpp)
    target_include_directories(a-tests PRIVATE third_party)
    target_link_libraries(a-tests PRIVATE user32 ws2_32)
    set(A_EVIDENCE "E:/AI/Zcode/tmp/hermes-panel-native/evidence")
    # The A gate covers the shell scenarios that do not depend on the (removed) balance data layer.
    # a-panelui exercised balance page clicks (activeSite/trend/compact) and moved to the hermes h-*
    # gates with H4-H6; PLAN §7 records the substitution.
    foreach(scenario snap pullout hover edge crossdpi topology c1)
        # 验收按 GOAL 的硬约束跑假助手夹具：真 WSL 冷启动会在拖拽中途多写一次几何。
        add_test(NAME a-${scenario} COMMAND a-tests --app $<TARGET_FILE:hermes-panel-native>
            --probe $<TARGET_FILE:geometry-probe> --evidence ${A_EVIDENCE} --scenario ${scenario}
            --helper $<TARGET_FILE:fake-hermes>)
    endforeach()
    set_tests_properties(a-snap a-pullout a-crossdpi a-topology a-c1 PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)
    set_tests_properties(a-edge PROPERTIES TIMEOUT 240 SKIP_RETURN_CODE 77)
    set_tests_properties(a-hover PROPERTIES TIMEOUT 400 SKIP_RETURN_CODE 77)

    add_executable(helper-tests tests/helper_tests.cpp src/data/helper.cpp)
    target_include_directories(helper-tests PRIVATE third_party src)
    target_link_libraries(helper-tests PRIVATE user32)
    add_test(NAME helper COMMAND helper-tests)
    set_tests_properties(helper PROPERTIES TIMEOUT 60)
    # unc-parity：C++ 行扫描器 vs apply.py `list` 输出逐字段对拍（DESIGN v1.7）。
    # 第一步 exe 把三种夹具（hermes 2/4 缩进、WebUI 0/2 缩进、边角）的快照 JSON 写到
    # tmp/unc-parity/；第二步 Python 驱动对同一夹具跑真 apply.py（WSL 内）逐字段比较。
    add_executable(unc-parity-tests tests/unc_parity_tests.cpp src/data/unc_snapshot.cpp)
    target_include_directories(unc-parity-tests PRIVATE third_party src)
    add_test(NAME unc-parity-snapshot COMMAND unc-parity-tests)
    set_tests_properties(unc-parity-snapshot PROPERTIES TIMEOUT 60)
    add_test(NAME unc-parity
        COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/unc_parity_drive.cmake)
    set_tests_properties(unc-parity PROPERTIES TIMEOUT 240 DEPENDS unc-parity-snapshot)
    add_executable(hermes-state-tests tests/state_tests2.cpp src/data/state.cpp)
    target_include_directories(hermes-state-tests PRIVATE third_party src)
    add_test(NAME hermes-state COMMAND hermes-state-tests)
    add_executable(h-models-tests tests/h_models_tests.cpp)
    target_include_directories(h-models-tests PRIVATE third_party tests)
    target_link_libraries(h-models-tests PRIVATE user32 ws2_32)
    add_test(NAME h-models COMMAND h-models-tests $<TARGET_FILE:hermes-panel-native>
        $<TARGET_FILE:fake-hermes>)
    set_tests_properties(h-models PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
    add_executable(h-fallback-tests tests/h_fallback_tests.cpp)
    target_include_directories(h-fallback-tests PRIVATE third_party tests)
    target_link_libraries(h-fallback-tests PRIVATE user32 ws2_32)
    add_test(NAME h-fallback COMMAND h-fallback-tests $<TARGET_FILE:hermes-panel-native>
        $<TARGET_FILE:fake-hermes>)
    set_tests_properties(h-fallback PROPERTIES TIMEOUT 120)
    add_executable(h-pages-tests tests/h_pages_tests.cpp src/render/theme.cpp)
    target_include_directories(h-pages-tests PRIVATE third_party tests src)
    target_link_libraries(h-pages-tests PRIVATE user32 ws2_32)
    foreach(scenario add backup settings)
        add_test(NAME h-${scenario} COMMAND h-pages-tests $<TARGET_FILE:hermes-panel-native>
            $<TARGET_FILE:fake-hermes> ${scenario})
    endforeach()
    set_tests_properties(h-add h-backup h-settings PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
    add_executable(h-shell-tests tests/h_shell_tests.cpp)
    target_include_directories(h-shell-tests PRIVATE third_party tests)
    target_link_libraries(h-shell-tests PRIVATE user32 ws2_32)
    add_test(NAME h-offline COMMAND h-shell-tests $<TARGET_FILE:hermes-panel-native>
        $<TARGET_FILE:fake-wsl> offline)
    add_test(NAME h-migrate COMMAND h-shell-tests $<TARGET_FILE:hermes-panel-native>
        $<TARGET_FILE:fake-hermes> migrate)
    set_tests_properties(h-offline h-migrate PROPERTIES TIMEOUT 120)
    # 逐控件点击门禁（每个控件的中心都要点得到，关键控件真点一遍）
    add_executable(h-controls-tests tests/h_controls_tests.cpp)
    target_include_directories(h-controls-tests PRIVATE third_party tests)
    target_link_libraries(h-controls-tests PRIVATE user32 ws2_32)
    add_test(NAME h-controls COMMAND h-controls-tests $<TARGET_FILE:hermes-panel-native>
        $<TARGET_FILE:fake-hermes>)
    set_tests_properties(h-controls PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)
    add_executable(h-ime-tests tests/h_ime_tests.cpp)
    target_include_directories(h-ime-tests PRIVATE third_party tests)
    target_link_libraries(h-ime-tests PRIVATE user32 ws2_32)
    add_test(NAME h-ime COMMAND h-ime-tests $<TARGET_FILE:hermes-panel-native> $<TARGET_FILE:fake-hermes>)
    set_tests_properties(h-ime PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
    add_executable(dpi-tests tests/dpi_tests.cpp)
    target_include_directories(dpi-tests PRIVATE third_party)
    target_link_libraries(dpi-tests PRIVATE user32)
    add_test(NAME dpi COMMAND dpi-tests $<TARGET_FILE:hermes-panel-native> $<TARGET_FILE:fake-hermes>)
    set_tests_properties(dpi PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
    add_executable(geometry-tests tests/geometry_tests.cpp)
    target_link_libraries(geometry-tests PRIVATE hermes-geometry)
    add_test(NAME geometry COMMAND geometry-tests)
    add_executable(widget-tests tests/widget_tests.cpp src/render/icons.cpp src/render/widgets.cpp src/render/textfield.cpp src/render/layout.cpp src/app/panel_view.cpp src/render/theme.cpp)
    target_include_directories(widget-tests PRIVATE src)
    target_link_libraries(widget-tests PRIVATE d2d1 dwrite)
    add_test(NAME widget COMMAND widget-tests)
    add_executable(log-tests tests/log_tests.cpp src/data/log.cpp)
    target_include_directories(log-tests PRIVATE src)
    add_test(NAME log COMMAND log-tests)
    # 设置持久化：单字段类型错误、极端值夹取、原子写、坏文件回落（第 5 轮迭代新增）。
    add_executable(settings-tests tests/settings_tests.cpp src/data/settings.cpp)
    target_include_directories(settings-tests PRIVATE src third_party)
    add_test(NAME settings COMMAND settings-tests)
    add_executable(state-tests tests/state_tests.cpp src/core/state.cpp)
    target_include_directories(state-tests PRIVATE src)
    add_test(NAME state COMMAND state-tests)
    add_executable(lifecycle-tests tests/lifecycle_tests.cpp)
    target_link_libraries(lifecycle-tests PRIVATE user32)
    add_test(NAME lifecycle COMMAND lifecycle-tests $<TARGET_FILE:hermes-panel-native>)
    set_tests_properties(lifecycle PROPERTIES TIMEOUT 20)
endif()
