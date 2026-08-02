cmake_minimum_required(VERSION 3.15)
project(psdgen CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(psdgen
    src/main.cpp
    src/psd_writer.cpp
)
target_include_directories(psdgen PRIVATE src)

if(MSVC)
    target_compile_options(psdgen PRIVATE /utf-8 /W3)
endif()

if(WIN32)
    target_link_libraries(psdgen PRIVATE gdiplus)
endif()
