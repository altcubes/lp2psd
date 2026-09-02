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
    src/textmetrics.cpp
)
target_include_directories(lp2psd PRIVATE src)

# Optional dbnet support (Japanese text-region detection via the m-i-t DBNet
# detector on ONNX Runtime). Enabled automatically when the onnxruntime
# prebuilt package is present under third_party/onnxruntime (see
# scripts/setup_dbnet.bat and docs/dbnet.md).
set(ONNXRUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime")
if(EXISTS "${ONNXRUNTIME_DIR}/include/onnxruntime_c_api.h")
    target_sources(lp2psd PRIVATE src/dbnet.cpp)
    target_compile_definitions(lp2psd PRIVATE LP2PSD_WITH_dbnet)
    target_include_directories(lp2psd PRIVATE "${ONNXRUNTIME_DIR}/include")
    message(STATUS "dbnet enabled (onnxruntime: ${ONNXRUNTIME_DIR})")
else()
    message(STATUS "dbnet disabled: third_party/onnxruntime not found")
endif()

if(MSVC)
    target_compile_options(lp2psd PRIVATE /utf-8 /W3)
endif()

if(WIN32)
    target_link_libraries(lp2psd PRIVATE gdiplus comdlg32 shell32)
endif()
