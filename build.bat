@echo off
rem Build psdgen with CMake (Visual Studio 2022 recommended).
where cmake >nul 2>nul || (echo [ERROR] cmake not found & exit /b 1)
if not exist build mkdir build
cmake -S . -B build || exit /b 1
cmake --build build --config Release || exit /b 1
echo.
echo Build OK: build\Release\psdgen.exe
