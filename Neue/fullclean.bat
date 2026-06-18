@echo off
REM LebensSpur build dizinini tamamen temizle.
REM sdkconfig'i de siler; bir sonraki build sdkconfig.defaults'tan yeniden uretir.
REM CMakeLists / partitions.csv / sdkconfig.defaults degisikliginden sonra kullan.

setlocal
set MSYSTEM=
cd /d "%~dp0"

call C:\Espressif\frameworks\esp-idf-v5.5.2\export.bat
if errorlevel 1 (
    echo [HATA] ESP-IDF environment yuklenemedi.
    pause
    exit /b 1
)

idf.py fullclean
echo.
if exist sdkconfig (
    del /q sdkconfig
    echo sdkconfig silindi.
)
echo [OK] Build dizini temizlendi.
pause
endlocal
