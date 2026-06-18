@echo off
REM LebensSpur firmware flash to ESP32-C6 on COM10.
REM Cihaz baska porttaysa SET LS_PORT=COM<N> ile override yapilabilir.
REM Build calistirilmamissa once derler.

setlocal
set MSYSTEM=
cd /d "%~dp0"

if "%LS_PORT%"=="" set LS_PORT=COM10

call C:\Espressif\frameworks\esp-idf-v5.5.2\export.bat
if errorlevel 1 (
    echo.
    echo [HATA] ESP-IDF environment yuklenemedi.
    pause
    exit /b 1
)

echo.
echo Flashing to %LS_PORT%...
idf.py -p %LS_PORT% flash
set FLASH_EXIT=%ERRORLEVEL%

echo.
if %FLASH_EXIT% EQU 0 (
    echo [OK] Flash tamamlandi. Monitor icin monitor.bat
) else (
    echo [HATA] Flash basarisiz, exit=%FLASH_EXIT%.
)
pause
endlocal
