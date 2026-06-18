@echo off
REM LebensSpur serial monitor on COM10.
REM Cihaz baska porttaysa SET LS_PORT=COM<N> ile override yapilabilir.
REM Cikis: Ctrl + ] (kapali kose ayrac).

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
echo Monitor on %LS_PORT% --- Ctrl+] to exit
idf.py -p %LS_PORT% monitor

endlocal
