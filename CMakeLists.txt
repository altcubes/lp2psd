cmake_minimum_required(VERSION 3.15)
project(lp2psd CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(lp2psd
    src/main.cpp
    src/layout.cpp
    src/style.cpp
    src/image.cpp
    src/psd_writer.cpp
)
target_include_directories(lp2psd PRIVATE src)

if(MSVC)
    target_compile_options(lp2psd PRIVATE /utf-8 /W3)
endif()

if(WIN32)
    target_link_libraries(lp2psd PRIVATE gdiplus comdlg32 shell32)
endif()
