# esp_bin — build klasörü

Tüm ESP-IDF build çıktıları burada oluşur. `ALTE/` klasörüne hiçbir üretilen dosya yazılmaz.

| Yol | İçerik |
|---|---|
| `HW/CMakeLists.txt` | Sarmalayıcı proje. Kaynakları `ALTE/HW` ve `ALTE/GUI`'den alır. |
| `HW/build/` | idf.py çıktısı (bootloader, partition table, lebensspur.bin, elf, map) |
| `HW/sdkconfig` | Üretilen yapılandırma (git'e girmez) |
| `HW/managed_components/` | Component manager indirmeleri (git'e girmez) |
| `HW/dependencies.lock` | Bağımlılık sürüm kilidi (git'te tutulur) |
| `firmware/` | Sürüm adıyla kopyalanmış `lebensspur_vX.Y.Z.bin` dosyaları (git'e girmez) |
| `logs/` | Build/flash/seri log kayıtları (git'e girmez) |

## Kullanım

```sh
./esp_bin/build.sh                                   # build
./esp_bin/build.sh -p /dev/cu.usbmodem1101 flash     # yükle (port: ls /dev/cu.usbmodem*)
./esp_bin/build.sh -p /dev/cu.usbmodem1101 monitor   # seri izle (çıkış: Ctrl+])
./esp_bin/build.sh menuconfig
./esp_bin/build.sh fullclean
```

Not: ESP32-C6 USB-Serial/JTAG üzerinden görünür; port adı makineye göre değişir.

ESP-IDF varsayılan yolu `~/esp/esp-idf-v5.5.2`. Farklıysa `IDF_DIR=/yol ./esp_bin/build.sh`.

Firmware sürümü tek yerden gelir: `ALTE/HW/CMakeLists.txt` içindeki `PROJECT_VER`.
GUI sürümü `ALTE/GUI/config.json` ve `ext_flash.c` içindeki `WEB_FILES_VERSION` ile aynı tutulmalıdır.
