"""H2b step 1: switch the renderer to the shared icon module and wire it into the build."""
import pathlib
import re

# 1) panel.h: use the icon module instead of a private IconId enum
header = pathlib.Path("src/render/panel.h")
text = header.read_text(encoding="utf-8")
text = text.replace('''#include "app/panel_view.h"
#include "render/layout.h"
#include "render/theme.h"''','''#include "app/panel_view.h"
#include "render/icons.h"
#include "render/layout.h"
#include "render/theme.h"''')
text = re.sub(r"// Icon set from the old icons\.js[^\n]*\n(//[^\n]*\n)*enum class IconId \{(?:[^}]*)\};\n\n", "", text)
text = text.replace('''    void Icon(ID2D1RenderTarget* target,const D2D1_RECT_F& rect,IconId id,const D2D1_COLOR_F& colour,
        float thickness=1.6f);''','''    // Thin wrapper around DrawIcon: the panel always strokes with its own round-cap style.
    void Icon(ID2D1RenderTarget* target,const D2D1_RECT_F& rect,IconId id,const D2D1_COLOR_F& colour,
        float thickness=2.0f);''')
header.write_text(text, encoding="utf-8", newline="")

# 2) panel.cpp: replace the inline icon switch with the shared table
source = pathlib.Path("src/render/panel.cpp")
code = source.read_text(encoding="utf-8")
start = code.index("void PanelRenderer::Icon(ID2D1RenderTarget* target")
end = code.index("void PanelRenderer::Lamp(")
replacement = '''void PanelRenderer::Icon(ID2D1RenderTarget* target,const D2D1_RECT_F& rect,IconId id,const D2D1_COLOR_F& colour,
    float thickness) {
    // One implementation for every icon: the shapes come from render/icons.cpp as data, so the widget
    // tests can assert the same definitions the renderer draws (DESIGN §4.9 #35).
    DrawIcon(target,brush_.Get(),arcStyle_.Get(),rect,id,colour,thickness,scale_);
}
'''
code = code[:start] + replacement + code[end:]
source.write_text(code, encoding="utf-8", newline="")

# 3) CMake: icons.cpp joins the app and a new widget-tests target appears
cmake = pathlib.Path("CMakeLists.txt")
list_text = cmake.read_text(encoding="utf-8")
list_text = list_text.replace("src/render/layout.cpp src/render/panel.cpp",
                              "src/render/icons.cpp src/render/layout.cpp src/render/panel.cpp")
if "widget-tests" not in list_text:
    list_text = list_text.replace('''    add_executable(log-tests tests/log_tests.cpp src/data/log.cpp)''',
'''    add_executable(widget-tests tests/widget_tests.cpp src/render/icons.cpp src/app/panel_view.cpp src/render/theme.cpp)
    target_include_directories(widget-tests PRIVATE src)
    target_link_libraries(widget-tests PRIVATE d2d1 dwrite)
    add_test(NAME widget COMMAND widget-tests)
    add_executable(log-tests tests/log_tests.cpp src/data/log.cpp)''')
cmake.write_text(list_text, encoding="utf-8", newline="")
print("renderer switched to the icon module; widget-tests registered")
