cmake_minimum_required(VERSION 3.15)
project(psdgen CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- CLI: psdgen.exe ----
add_executable(psdgen
    src/main.cpp
    src/layout.cpp
    src/style.cpp
    src/image.cpp
    src/psd_writer.cpp
)
target_include_directories(psdgen PRIVATE src)

if(MSVC)
    target_compile_options(psdgen PRIVATE /utf-8 /W3)
endif()

if(WIN32)
    target_link_libraries(psdgen PRIVATE gdiplus comdlg32 shell32)
endif()

# ---- GUI: psdgen-gui.exe (native Win32 wrapper around psdgen.exe) ----
if(WIN32)
    add_executable(psdgen_gui WIN32 src/gui_main.cpp)
    set_target_properties(psdgen_gui PROPERTIES OUTPUT_NAME psdgen-gui)
    target_include_directories(psdgen_gui PRIVATE src)
    if(MSVC)
        target_compile_options(psdgen_gui PRIVATE /utf-8 /W3)
    endif()
    target_link_libraries(psdgen_gui PRIVATE user32 comdlg32 shell32 gdi32)
endif()
