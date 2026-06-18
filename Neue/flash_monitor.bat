@echo off
REM LebensSpur flash + monitor zinciri (en cok kullanilan akis).
REM Build degisikligi sonrasi: bu bat tek tik ile flash + monitor acar.
REM Cihaz baska porttaysa SET LS_PORT=COM<N>. Monitorden cikis: Ctrl + ]

setlocal
set MSYSTEM=
cd /d "%~dp0"

if "%LS_PORT%"=="" set LS_PORT=COM10

call C:\Espressif\frameworks\esp-idf-v5.5.2\export.bat
if errorlevel 1 (
    echo [HATA] ESP-IDF environment yuklenemedi.
    pause
    exit /b 1
)

echo.
echo Flash + monitor on %LS_PORT%...
idf.py -p %LS_PORT% flash monitor

endlocal
