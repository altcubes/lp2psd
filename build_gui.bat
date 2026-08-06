@echo off
rem Build psdgen-gui.exe with CMake (same build tree as psdgen).
where cmake >nul 2>nul || (echo [ERROR] cmake not found & exit /b 1)
if not exist build mkdir build
cmake -S . -B build || exit /b 1
cmake --build build --config Release --target psdgen_gui || exit /b 1
echo.
echo Build OK: build\Release\psdgen-gui.exe
