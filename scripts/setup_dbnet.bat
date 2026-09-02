@echo off
rem Downloads the optional dbnet dependencies:
rem   - onnxruntime prebuilt package -> third_party\onnxruntime\{include,lib}
rem   - DBNet detection model (ONNX) -> dbnet_detect.onnx (repo root)
rem     (exported from the m-i-t detector via scripts\export_dbnet_onnx.py;
rem     needs Python + torch, see docs/dbnet.md)
rem After running this, rebuild (build.bat); the script also copies
rem onnxruntime.dll next to the built exe. Then enable dbnet in config.json
rem ("dbnet"."enabled": true). See docs/dbnet.md for details.

setlocal
set ORT_VERSION=1.19.2
set ORT_URL=https://github.com/microsoft/onnxruntime/releases/download/v%ORT_VERSION%/onnxruntime-win-x64-%ORT_VERSION%.zip
rem Fallback mirror for slow/blocked access to github.com:
set ORT_URL_MIRROR=https://ghproxy.cn/%ORT_URL%

where curl >nul 2>nul || (echo [ERROR] curl not found & exit /b 1)
where powershell >nul 2>nul || (echo [ERROR] powershell not found & exit /b 1)

rem ---- onnxruntime ---------------------------------------------------------
if exist "third_party\onnxruntime\include\onnxruntime_c_api.h" (
    echo [OK] onnxruntime already present, skipping download
) else (
    echo Downloading onnxruntime %ORT_VERSION% ...
    if not exist third_party mkdir third_party
    curl --ssl-no-revoke -L -o third_party\ort.zip "%ORT_URL%" || curl --ssl-no-revoke -L -o third_party\ort.zip "%ORT_URL_MIRROR%" || (echo [ERROR] download failed & exit /b 1)
    for %%F in (third_party\ort.zip) do if %%~zF LSS 1000000 (echo [ERROR] ort.zip too small, download failed & exit /b 1)
    echo Extracting ...
    powershell -NoProfile -Command "Expand-Archive -Force 'third_party\ort.zip' 'third_party\ort_tmp'"
    if errorlevel 1 (echo [ERROR] extract failed & exit /b 1)
    if exist third_party\onnxruntime rmdir /s /q third_party\onnxruntime
    move "third_party\ort_tmp\onnxruntime-win-x64-%ORT_VERSION%" "third_party\onnxruntime" >nul || (echo [ERROR] layout unexpected & exit /b 1)
    rmdir /s /q third_party\ort_tmp
    del third_party\ort.zip
    echo [OK] third_party\onnxruntime ready
)

rem ---- detection model (DBNet ONNX, exported from m-i-t detector) -----------
if exist "dbnet_detect.onnx" (
    echo [OK] dbnet_detect.onnx already present, skipping export
) else (
    echo Exporting dbnet_detect.onnx ...
    where python >nul 2>nul || (echo [ERROR] python not found - see docs/dbnet.md for manual export & exit /b 1)
    python -c "import torch, torchvision" >nul 2>nul || (
        echo [ERROR] torch/torchvision not installed - run:
        echo     pip install torch torchvision --index-url https://download.pytorch.org/whl/cpu
        echo then rerun this script, or see docs/dbnet.md
        exit /b 1
    )
    python scripts\export_dbnet_onnx.py dbnet_detect.onnx || (echo [ERROR] export failed - see docs/dbnet.md & exit /b 1)
)

rem ---- place the DLL next to the built exe ----------------------------------
if exist "build\Release\lp2psd.exe" (
    if exist "third_party\onnxruntime\lib\onnxruntime.dll" (
        copy /y "third_party\onnxruntime\lib\onnxruntime.dll" "build\Release\" >nul
        echo [OK] onnxruntime.dll copied to build\Release\
    )
)

echo.
echo Done. Now rebuild, set config.json "dbnet"."enabled": true, and run.
endlocal
exit /b 0
