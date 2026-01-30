# 🔧 Open Source Usage Instructions (For Developers)

**[English](#english)** | **[Türkçe](#turkish)**

---

<a name="english"></a>
## 🇬🇧 English

## 📋 Table of Contents

- [Overview](#overview-en)
- [Requirements](#requirements-en)
- [Installation Steps](#installation-steps-en)
- [Libraries](#libraries-en)
- [Partition Scheme (OTA Configuration)](#partition-scheme-en)
- [GPIO Pin Configuration](#gpio-pin-configuration-en)
- [Critical Notes and Common Errors](#critical-notes-en)
- [First Compilation and Upload](#first-compilation-en)
- [OTA Update Configuration](#ota-update-configuration-en)

---

<a name="overview-en"></a>
## 🎯 Overview

This project is a **LebensSpur Protocol** implementation developed for **ESP32-C6**. It includes OTA (Over-The-Air) updates, SPIFFS file system, and multi-language support.

**⚠️ IMPORTANT:** This code is optimized ONLY for **ESP32-C6**. It will not work or may cause issues on other ESP32 variants.

---

<a name="requirements-en"></a>
## 📦 Requirements

### Hardware
- **ESP32-C6** microcontroller (Seeed XIAO ESP32C6 recommended)
- **4MB Flash memory** (minimum)
- USB-C cable (for programming)
- Optional: Button, Relay module

### Software
- **Arduino IDE** 2.x or later
- **ESP32 Board Package** v3.0.0 or later
- **Git** (to clone the code)

---

<a name="installation-steps-en"></a>
## 🚀 Installation Steps

### 1. Arduino IDE Setup

#### Board Manager Configuration
Arduino IDE → Preferences → Additional Boards Manager URLs:
```
https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_dev_index.json
```

#### ESP32 Board Package Installation
Tools → Board → Boards Manager → Search "ESP32" → **esp32 by Espressif Systems** → Install (v3.0.0+)

### 2. Download Code

```bash
git clone https://github.com/smrtkrft/LebensSpur_protocol.git
cd LebensSpur_protocol
```

### 3. Open in Arduino IDE

Open `SmartKraft_LebensSpur/SmartKraft_LebensSpur.ino` file with Arduino IDE.

---

<a name="libraries-en"></a>
## 📚 Libraries

### Built-in Libraries (Included with ESP32 Package)
- `WiFi.h` - WiFi connection management
- `WebServer.h` - Web server operations
- `SPIFFS.h` - File system
- `Preferences.h` - Configuration for NVS (Non-Volatile Storage)
- `Update.h` - OTA update
- `HTTPClient.h` - HTTP requests (for OTA)
- `esp_task_wdt.h` - Watchdog timer

### External Libraries (Manual installation required)

#### 1. ArduinoJson (v6.x)
```
Sketch → Include Library → Manage Libraries → Search "ArduinoJson" → Install v6.21.0 or later
```
**⚠️ WARNING:** DO NOT USE v7.x! Code is written for v6.x.

#### 2. ESP Mail Client (Mobizt)
```
Library Manager → Search "ESP Mail Client" → Install version developed by Mobizt (v3.4.0+)
```

**Library Verification:**
```cpp
// If these lines compile successfully, libraries are correctly installed
#include <ArduinoJson.h>
#include <ESP_Mail_Client.h>
```

---

<a name="partition-scheme-en"></a>
## ⚙️ Partition Scheme (OTA Configuration)

### ❌ WRONG Partition Selection = Bootloop!

Remember that ESP32-C6 has **4MB flash**. Dual APP partition is required for OTA.

### ✅ CORRECT Setting (Critical!)

⚠️ **IMPORTANT:** Standard Arduino IDE installation DOES NOT include "SmartKraft OTA" partition scheme. You MUST add it manually.

#### Manual Partition Addition (Mandatory!)

Device will enter BOOTLOOP if you don't follow these steps!

1. **Copy partitions.csv file:**

Windows:
```powershell
Copy-Item "partitions.csv" "$env:LOCALAPPDATA\Arduino15\packages\esp32\hardware\esp32\3.x.x\tools\partitions\smartkraft_ota.csv"
```

Linux/Mac:
```bash
cp partitions.csv ~/.arduino15/packages/esp32/hardware/esp32/3.x.x/tools/partitions/smartkraft_ota.csv
```

*(Replace 3.x.x with your installed version)*

2. **Edit boards.txt file:**

File path:
- Windows: `%LOCALAPPDATA%\Arduino15\packages\esp32\hardware\esp32\3.x.x\boards.txt`
- Linux/Mac: `~/.arduino15/packages/esp32/hardware/esp32/3.x.x/boards.txt`

**Lines to add** (to XIAO_ESP32C6 section):
```
XIAO_ESP32C6.menu.PartitionScheme.smartkraft=SmartKraft OTA (1.5MB APP/1MB SPIFFS)
XIAO_ESP32C6.menu.PartitionScheme.smartkraft.build.partitions=smartkraft_ota
XIAO_ESP32C6.menu.PartitionScheme.smartkraft.upload.maximum_size=1572864
```

3. **Restart Arduino IDE**

4. **Select Partition Scheme:**

In Arduino IDE:
```
Tools → Board → ESP32 Arduino → XIAO_ESP32C6
Tools → Partition Scheme → "SmartKraft OTA (1.5MB APP/1MB SPIFFS)"
```

You will now see "SmartKraft OTA" option in the list.

### Partition Structure

```
# Name,   Type, SubType, Offset,  Size
nvs,      data, nvs,     0x9000,  0x5000
otadata,  data, ota,     0xe000,  0x2000
app0,     app,  ota_0,   0x10000, 0x180000   # 1.5MB
app1,     app,  ota_1,   0x190000,0x180000   # 1.5MB (for OTA)
spiffs,   data, spiffs,  0x310000,0xF0000    # 960KB
```

**⚠️ Why Important:**
- **app0 + app1:** Dual boot for OTA updates
- **spiffs:** Web files and configurations
- **Total:** Full 4MB flash usage
- **Without app1:** OTA won't work
- **Without spiffs:** Web interface and settings will be lost

---

<a name="gpio-pin-configuration-en"></a>
## 📍 GPIO Pin Configuration

### Used Pins

```cpp
// Defined in SmartKraft_LebensSpur.ino

#define BUTTON_PIN 21       // Physical button (optional)
#define RELAY_PIN  18       // Relay output (optional)
```

### Pin Details

| Pin | Function | Type | Description |
|-----|----------|------|-------------|
| **GPIO21** | Button Input | INPUT_PULLUP | Physical postpone button (optional) |
| **GPIO18** | Relay Output | OUTPUT | Relay control (optional, Max 5V 30mA) |

### ⚠️ GPIO Critical Notes

1. **GPIO21 (Button):**
   - In `INPUT_PULLUP` mode
   - Pulls to GND when button is pressed
   - No external pull-up resistor needed
   - Pin can be left empty if not using physical button (virtual button will be used)

2. **GPIO18 (Relay):**
   - **MAXIMUM 5V 30mA** - Exceeding this will damage ESP32-C6!
   - Use **transistor driver** for high current relays (BC547, 2N2222 etc.)
   - Pin can be left empty if not using relay

3. **Pins NOT to Use:**
   - GPIO0, GPIO8, GPIO9: Boot and JTAG pins
   - Can create conflicts, do not use

### Pin Modification

If you want to use different pins, in `SmartKraft_LebensSpur.ino`:

```cpp
#define BUTTON_PIN 21  // Your desired pin number
#define RELAY_PIN  18  // Your desired pin number
```

**Then:**
```cpp
void setup() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(RELAY_PIN, OUTPUT);
}
```

---

<a name="critical-notes-en"></a>
## 🚨 Critical Notes and Common Errors

### ❌ ISSUE 1: Bootloop - Continuous Reset

**Cause:**
- Wrong partition scheme selected
- 2MB APP partition selected (doesn't fit in 4MB flash)

**Solution:**
- Select "SmartKraft OTA (1.5MB APP/1MB SPIFFS)"
- Manually add partition scheme (instructions above)
- If you won't use mail integration and only trigger URL without uploading files, you can select 2MB APP without SPIFFS area.

### ❌ ISSUE 2: "Sketch too large" Error

**Cause:**
- Code size exceeds 1.5MB
- Too many debug outputs

**Solution:**
```
Tools → Core Debug Level → "None"
Tools → Optimize → "Optimize for size (-Os)"
```

### ❌ ISSUE 3: SPIFFS Mount Failed

**Cause:**
- SPIFFS not formatted on first upload
- Partition scheme doesn't include SPIFFS

**Solution:**
```cpp
// SPIFFS.format() is called on first upload (present in code)
// Manual format:
SPIFFS.format();
```

### ❌ ISSUE 4: OTA Update Not Working

**Cause:**
- No app1 partition
- Wrong GitHub URL
- No WiFi connection

**Solution:**
- Use dual partition scheme
- Check GitHub URL in `network_manager.cpp`
- Verify WiFi connection

### ❌ ISSUE 5: Mail Not Sending

**Cause:**
- ESP_Mail_Client library missing/wrong version
- Wrong SMTP settings
- Watchdog timeout (long mail sending process//~100KB/Second)

**Solution:**
- Use Mobizt ESP_Mail_Client v3.4.0+
- Test SMTP settings
- Increase watchdog timeout (present in code)

### ❌ ISSUE 6: Web Interface Not Opening

**Cause:**
- No SPIFFS files
- No WiFi connection
- Wrong IP address

**Solution:**
- Check IP address from Serial Monitor
- Verify SPIFFS is properly mounted
- Use `192.168.4.1` in AP mode

---

<a name="first-compilation-en"></a>
## 🔨 First Compilation and Upload

### Step 1: Board Settings

```
Tools → Board → ESP32 Arduino → XIAO_ESP32C6
Tools → Partition Scheme → SmartKraft OTA (1.5MB APP/1MB SPIFFS)
Tools → Flash Size → 4MB
Tools → Flash Mode → QIO
Tools → Flash Frequency → 80MHz
Tools → Upload Speed → 921600
Tools → Core Debug Level → None (or Error)
```

### Step 2: Port Selection

```
Tools → Port → (COM port where your ESP32-C6 is connected)
```

### Step 3: Compilation (Verify)

```
Sketch → Verify/Compile
```

**Successful compilation output:**
```
Sketch uses XXXXX bytes (XX%) of program storage space.
Global variables use XXXXX bytes (XX%) of dynamic memory.
```

**⚠️ If program storage exceeds 90%:**
- Remove unnecessary debug code
- Core Debug Level → None
- Optimize → Size

### Step 4: Upload

```
Sketch → Upload
```

**First upload time:** ~30-60 seconds

### Step 5: Serial Monitor Check

```
Tools → Serial Monitor → 115200 baud
```

**Successful boot output:**
```
SmartKraft LebensSpur v1.0.0
[WiFi] AP mode active: SmartKraft-LebensSpur
[SPIFFS] File system initialized
[Web] Server started: 192.168.4.1
[OTA] Automatic update active
[READY] System ready
```

---

<a name="ota-update-configuration-en"></a>
## 🔄 OTA Update Configuration

### GitHub Repository Setup

In `network_manager.cpp` file:

```cpp
const char* GITHUB_REPO_OWNER = "smrtkrft";           // Your GitHub username
const char* GITHUB_REPO_NAME = "LebensSpur_protocol";        // Your repository name
const char* GITHUB_FIRMWARE_FILE = "firmware.bin";    // Release asset name
```

### Firmware Binary Creation

After compiling with Arduino IDE:

**Windows:**
```
%TEMP%\arduino\sketches\[sketch-folder]\SmartKraft_LebensSpur.ino.bin
```

**Linux/Mac:**
```
/tmp/arduino/sketches/[sketch-folder]/SmartKraft_LebensSpur.ino.bin
```

Upload this file to GitHub Release as `firmware.bin`.

### GitHub Release Creation

1. GitHub repository → Releases → Create a new release
2. Tag: `v1.0.1` (version number)
3. Title: "SmartKraft LebensSpur v1.0.1"
4. Upload: `firmware.bin` file
5. Publish release

### OTA Check

Device automatically checks every 24 hours. For manual check, press "OTA Update Check" button from web interface.

### OTA Update Process

```
1. Latest release is checked from GitHub API
2. Version number is compared
3. If new version exists, firmware.bin is downloaded
4. Written to app1 partition
5. Boot partition is changed
6. Device restarts
7. New firmware boots from app1
```

**⚠️ During OTA:**
- Do not cut device power
- Do not disconnect WiFi
- Process takes ~1-2 minutes

---

## 🛠️ Development Tips

### Debug Mode

During development:
```
Tools → Core Debug Level → Debug
```

For production:
```
Tools → Core Debug Level → None
```

### Serial Monitor Commands

Serial commands available in code:
```
status     - Show system status
reset      - Reset settings
restart    - Restart device
format     - Format SPIFFS (careful!)
```

### Memory Optimization

```cpp
// Use F() macro instead of String
Serial.println(F("Static text"));  // ✅ Saves SRAM
Serial.println("Static text");      // ❌ Uses SRAM

// Use PROGMEM
const char text[] PROGMEM = "Long text...";
```

### Watchdog Timer

For long operations like mail sending:
```cpp
esp_task_wdt_reset();  // Reset watchdog
```

---

## 📞 Support and Contribution

- **Issues:** https://github.com/smrtkrft/LebensSpur_protocol/issues
- **Pull Requests:** Welcome!
- **License:** AGPL-3.0 (you must share forks and modifications)

---

## ✅ Quick Checklist

Check before uploading:

- [ ] ESP32 Board Package v3.0.0+ installed
- [ ] ArduinoJson v6.x installed (NOT v7!)
- [ ] ESP_Mail_Client (Mobizt) installed
- [ ] Board: XIAO_ESP32C6 selected
- [ ] Partition: SmartKraft OTA selected
- [ ] Flash Size: 4MB
- [ ] Port correctly selected
- [ ] Libraries compile successfully
- [ ] GPIO pins match hardware

---

**© 2025 SmartKraft | AGPL-3.0 License**

---

<a name="turkish"></a>
## 🇹🇷 Türkçe

## 📋 İçindekiler

- [Genel Bakış](#genel-bakış-tr)
- [Gereksinimler](#gereksinimler-tr)
- [Kurulum Adımları](#kurulum-adımları-tr)
- [Kütüphaneler](#kütüphaneler-tr)
- [Partition Scheme (OTA Yapılandırması)](#partition-scheme-tr)
- [GPIO Pin Yapılandırması](#gpio-pin-yapılandırması-tr)
- [Kritik Notlar ve Yaygın Hatalar](#kritik-notlar-tr)
- [İlk Derleme ve Yükleme](#ilk-derleme-tr)
- [OTA Güncelleme Yapılandırması](#ota-güncelleme-yapılandırması-tr)

---

<a name="genel-bakış-tr"></a>

<a name="genel-bakış-tr"></a>
## 🎯 Genel Bakış

Bu proje **ESP32-C6** için geliştirilmiş bir **LebensSpur Protokolü** uygulamasıdır. OTA (Over-The-Air) güncellemesi, SPIFFS dosya sistemi ve çoklu dil desteği içerir.

**⚠️ ÖNEMLİ:** Bu kod yalnızca **ESP32-C6** için optimize edilmiştir. Diğer ESP32 varyantlarında çalışmaz veya sorun çıkarabilir.

---

<a name="gereksinimler-tr"></a>
## 📦 Gereksinimler

### Donanım
- **ESP32-C6** mikrodenetleyici (Seeed XIAO ESP32C6 önerilir)
- **4MB Flash bellek** (minimum)
- USB-C kablosu (programlama için)
- Opsiyonel: Buton, Röle modülü

### Yazılım
- **Arduino IDE** 2.x veya üzeri
- **ESP32 Board Package** v3.0.0 veya üzeri
- **Git** (kod çekmek için)

---

<a name="kurulum-adımları-tr"></a>
## 🚀 Kurulum Adımları

### 1. Arduino IDE Kurulumu

#### Board Manager Ayarı
Arduino IDE → Preferences → Additional Boards Manager URLs:
```
https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_dev_index.json
```

#### ESP32 Board Package Kurulumu
Tools → Board → Boards Manager → "ESP32" ara → **esp32 by Espressif Systems** → Install (v3.0.0+)

### 2. Kodu İndirme

```bash
git clone https://github.com/smrtkrft/LebensSpur_protocol.git
cd LebensSpur_protocol
```

### 3. Arduino IDE'de Açma

`SmartKraft_LebensSpur/SmartKraft_LebensSpur.ino` dosyasını Arduino IDE ile açın.

---

<a name="kütüphaneler-tr"></a>
## 📚 Kütüphaneler

### Dahili Kütüphaneler (ESP32 Package ile gelir)
- `WiFi.h` - WiFi bağlantı yönetimi
- `WebServer.h` - Web sunucu işlemleri
- `SPIFFS.h` - Dosya sistemi
- `Preferences.h` - NVS (Non-Volatile Storage) için yapılandırma
- `Update.h` - OTA güncelleme
- `HTTPClient.h` - HTTP istekleri (OTA için)
- `esp_task_wdt.h` - Watchdog timer

### Harici Kütüphaneler (Manuel kurulum gerekli)

#### 1. ArduinoJson (v6.x)
```
Sketch → Include Library → Manage Libraries → "ArduinoJson" ara → Install v6.21.0 veya üzeri
```
**⚠️ DİKKAT:** v7.x KULLANMAYIN! Kod v6.x için yazılmıştır.

#### 2. ESP Mail Client (Mobizt)
```
Library Manager → "ESP Mail Client" ara → Mobizt tarafından geliştirilen versiyonu Install (v3.4.0+)
```

**Kütüphane Doğrulama:**
```cpp
// Bu satırlar başarılı compile oluyorsa kütüphaneler doğru yüklenmiş demektir
#include <ArduinoJson.h>
#include <ESP_Mail_Client.h>
```

---

<a name="partition-scheme-tr"></a>
## ⚙️ Partition Scheme (OTA Yapılandırması)

### ❌ YANLIŞ Partition Seçimi = Bootloop!

ESP32-C6'nın **4MB flash** olduğunu unutmayın. OTA için dual APP partition gerekir.

### ✅ DOĞRU Ayar (Kritik!)

⚠️ **ÖNEMLİ:** Standart Arduino IDE kurulumunda "SmartKraft OTA" partition scheme'i yoktur. Manuel olarak eklemeniz GEREKMEKTEDİR.

#### Manuel Partition Ekleme (Zorunlu!)

Bu adımları yapmadan cihaz BOOTLOOP'a girecektir!

1. **partitions.csv dosyasını kopyalayın:**

Windows:
```powershell
Copy-Item "partitions.csv" "$env:LOCALAPPDATA\Arduino15\packages\esp32\hardware\esp32\3.x.x\tools\partitions\smartkraft_ota.csv"
```

Linux/Mac:
```bash
cp partitions.csv ~/.arduino15/packages/esp32/hardware/esp32/3.x.x/tools/partitions/smartkraft_ota.csv
```

*(3.x.x yerine kurulu sürümünüzü yazın)*

2. **boards.txt dosyasını düzenleyin:**

Dosya yolu:
- Windows: `%LOCALAPPDATA%\Arduino15\packages\esp32\hardware\esp32\3.x.x\boards.txt`
- Linux/Mac: `~/.arduino15/packages/esp32/hardware/esp32/3.x.x/boards.txt`

**Eklenecek satırlar** (XIAO_ESP32C6 bölümüne):
```
XIAO_ESP32C6.menu.PartitionScheme.smartkraft=SmartKraft OTA (1.5MB APP/1MB SPIFFS)
XIAO_ESP32C6.menu.PartitionScheme.smartkraft.build.partitions=smartkraft_ota
XIAO_ESP32C6.menu.PartitionScheme.smartkraft.upload.maximum_size=1572864
```

3. **Arduino IDE'yi yeniden başlatın**

4. **Partition Scheme'i seçin:**

Arduino IDE'de:
```
Tools → Board → ESP32 Arduino → XIAO_ESP32C6
Tools → Partition Scheme → "SmartKraft OTA (1.5MB APP/1MB SPIFFS)"
```

Artık listede "SmartKraft OTA" seçeneğini göreceksiniz.

### Partition Yapısı

```
# Name,   Type, SubType, Offset,  Size
nvs,      data, nvs,     0x9000,  0x5000
otadata,  data, ota,     0xe000,  0x2000
app0,     app,  ota_0,   0x10000, 0x180000   # 1.5MB
app1,     app,  ota_1,   0x190000,0x180000   # 1.5MB (OTA için)
spiffs,   data, spiffs,  0x310000,0xF0000    # 960KB
```

**⚠️ Neden Önemli:**
- **app0 + app1:** OTA güncellemesi için dual boot
- **spiffs:** Web dosyaları ve yapılandırmalar
- **Toplam:** 4MB flash tam kullanımı
- **app1 olmadan:** OTA çalışmaz
- **spiffs olmadan:** Web arayüzü ve ayarlar kaybolur

---

<a name="gpio-pin-yapılandırması-tr"></a>
## 📍 GPIO Pin Yapılandırması

### Kullanılan Pinler

```cpp
// SmartKraft_LebensSpur.ino içinde tanımlı

#define BUTTON_PIN 21       // Fiziksel buton (isteğe bağlı)
#define RELAY_PIN  18       // Röle çıkışı (isteğe bağlı)
```

### Pin Detayları

| Pin | Fonksiyon | Tip | Açıklama |
|-----|-----------|-----|----------|
| **GPIO21** | Buton Girişi | INPUT_PULLUP | Fiziksel erteleme butonu (opsiyonel) |
| **GPIO18** | Röle Çıkışı | OUTPUT | Röle kontrolü (opsiyonel, Max 5V 30mA) |

### ⚠️ GPIO Kritik Notlar

1. **GPIO21 (Buton):**
   - `INPUT_PULLUP` modunda
   - Buton basıldığında GND'ye çeker
   - Harici pull-up direncine gerek yok
   - Fiziksel buton kullanmıyorsanız pin boş bırakılabilir (sanal buton kullanılır)

2. **GPIO18 (Röle):**
   - **MAKSİMUM 5V 30mA** - Daha fazlası ESP32-C6'ya zarar verir!
   - Yüksek akım röle için **transistör sürücü** kullanın (BC547, 2N2222 vb.)
   - Röle kullanmıyorsanız pin boş bırakılabilir

3. **Kullanılmaması Gereken Pinler:**
   - GPIO0, GPIO8, GPIO9: Boot ve JTAG pinleri
   - Conflict yaratabilir, kullanmayın

### Pin Değiştirme

Farklı pin kullanmak isterseniz `SmartKraft_LebensSpur.ino` içinde:

```cpp
#define BUTTON_PIN 21  // İstediğiniz pin numarası
#define RELAY_PIN  18  // İstediğiniz pin numarası
```

**Sonra:**
```cpp
void setup() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(RELAY_PIN, OUTPUT);
}
```

---

<a name="kritik-notlar-tr"></a>
## 🚨 Kritik Notlar ve Yaygın Hatalar

### ❌ SORUN 1: Bootloop - Sürekli Reset

**Neden:**
- Yanlış partition scheme seçilmiş
- 2MB APP partition seçilmiş (4MB flash'a sığmaz)

**Çözüm:**
- "SmartKraft OTA (1.5MB APP/1MB SPIFFS)" seçin
- Partition scheme'i manuel ekleyin (yukarıdaki talimatlar)
- Mail entegrasyonu yapmayacaksaniz , dosya yüklenmeyecegi sadece URL tetikleme yapacaginiz icin  Spiffs alani olmadan 2MB APP secebilirsiniz.

### ❌ SORUN 2: "Sketch too large" Hatası

**Neden:**
- Kod boyutu 1.5MB'ı aşmış
- Debug çıktıları çok fazla

**Çözüm:**
```
Tools → Core Debug Level → "None"
Tools → Optimize → "Optimize for size (-Os)"
```

### ❌ SORUN 3: SPIFFS Mount Failed

**Neden:**
- İlk yüklemede SPIFFS formatlanmamış
- Partition scheme SPIFFS içermiyor

**Çözüm:**
```cpp
// İlk yüklemede SPIFFS.format() çağrılır (kod içinde mevcut)
// Manuel format:
SPIFFS.format();
```

### ❌ SORUN 4: OTA Güncelleme Çalışmıyor

**Neden:**
- app1 partition yok
- GitHub URL yanlış
- WiFi bağlantısı yok

**Çözüm:**
- Dual partition scheme kullanın
- `network_manager.cpp` içinde GitHub URL'i kontrol edin
- WiFi bağlantısını doğrulayın

### ❌ SORUN 5: Mail Gönderilmiyor

**Neden:**
- ESP_Mail_Client kütüphanesi eksik/yanlış versiyon
- SMTP ayarları yanlış
- Watchdog timeout (uzun süren mail gönderimi//~100KB/Saniye)

**Çözüm:**
- Mobizt ESP_Mail_Client v3.4.0+ kullanın
- SMTP ayarlarını test edin
- Watchdog timeout süresini artırın (kod içinde mevcut)

### ❌ SORUN 6: Web Arayüzü Açılmıyor

**Neden:**
- SPIFFS dosyaları yok
- WiFi bağlantısı yok
- Yanlış IP adresi

**Çözüm:**
- Serial Monitor'den IP adresini kontrol edin
- SPIFFS'in doğru mount edildiğini kontrol edin
- AP modunda `192.168.4.1` kullanın

---

<a name="ilk-derleme-tr"></a>
## 🔨 İlk Derleme ve Yükleme

### Adım 1: Board Ayarları

```
Tools → Board → ESP32 Arduino → XIAO_ESP32C6
Tools → Partition Scheme → SmartKraft OTA (1.5MB APP/1MB SPIFFS)
Tools → Flash Size → 4MB
Tools → Flash Mode → QIO
Tools → Flash Frequency → 80MHz
Tools → Upload Speed → 921600
Tools → Core Debug Level → None (veya Error)
```

### Adım 2: Port Seçimi

```
Tools → Port → (ESP32-C6'nızın bağlı olduğu COM portu)
```

### Adım 3: Derleme (Verify)

```
Sketch → Verify/Compile
```

**Başarılı derleme çıktısı:**
```
Sketch uses XXXXX bytes (XX%) of program storage space.
Global variables use XXXXX bytes (XX%) of dynamic memory.
```

**⚠️ Program storage %90'ı geçerse:**
- Gereksiz debug kodlarını kaldırın
- Core Debug Level → None
- Optimize → Size

### Adım 4: Yükleme (Upload)

```
Sketch → Upload
```

**İlk yükleme süresi:** ~30-60 saniye

### Adım 5: Serial Monitor Kontrolü

```
Tools → Serial Monitor → 115200 baud
```

**Başarılı boot çıktısı:**
```
SmartKraft DMF v1.0.0
[WiFi] AP modu aktif: SmartKraft-DMF
[SPIFFS] Dosya sistemi başlatıldı
[Web] Sunucu başlatıldı: 192.168.4.1
[OTA] Otomatik güncelleme aktif
[READY] Sistem hazır
```

---

<a name="ota-güncelleme-yapılandırması-tr"></a>
## 🔄 OTA Güncelleme Yapılandırması

### GitHub Repository Ayarı

`network_manager.cpp` dosyasında:

```cpp
const char* GITHUB_REPO_OWNER = "smrtkrft";           // GitHub kullanıcı adınız
const char* GITHUB_REPO_NAME = "DMF_protocol";        // Repository adınız
const char* GITHUB_FIRMWARE_FILE = "firmware.bin";    // Release asset adı
```

### Firmware Binary Oluşturma

Arduino IDE ile compile ettikten sonra:

**Windows:**
```
%TEMP%\arduino\sketches\[sketch-folder]\SmartKraft_DMF.ino.bin
```

**Linux/Mac:**
```
/tmp/arduino/sketches/[sketch-folder]/SmartKraft_DMF.ino.bin
```

Bu dosyayı GitHub Release'e `firmware.bin` adıyla yükleyin.

### GitHub Release Oluşturma

1. GitHub repository → Releases → Create a new release
2. Tag: `v1.0.1` (versiyon numarası)
3. Title: "SmartKraft DMF v1.0.1"
4. Upload: `firmware.bin` dosyası
5. Publish release

### OTA Kontrolü

Cihaz her 24 saatte bir otomatik kontrol eder. Manuel kontrol için web arayüzünden "OTA Güncelleme Kontrol" butonuna basın.

### OTA Güncelleme Süreci

```
1. GitHub API'den son release kontrol edilir
2. Versiyon numarası karşılaştırılır
3. Yeni versiyon varsa firmware.bin indirilir
4. app1 partition'a yazılır
5. boot partition değiştirilir
6. Cihaz yeniden başlatılır
7. Yeni firmware app1'den boot olur
```

**⚠️ OTA Sırasında:**
- Cihazın gücünü kesmeyin
- WiFi bağlantısını kesmeyin
- İşlem ~1-2 dakika sürer

---

## 🛠️ Geliştirme İpuçları

### Debug Modu

Geliştirme sırasında:
```
Tools → Core Debug Level → Debug
```

Production için:
```
Tools → Core Debug Level → None
```

### Serial Monitor Komutları

Kod içinde serial komutlar mevcut:
```
status     - Sistem durumunu göster
reset      - Ayarları sıfırla
restart    - Cihazı yeniden başlat
format     - SPIFFS'i formatla (dikkatli!)
```

### Bellek Optimizasyonu

```cpp
// String yerine F() macro kullanın
Serial.println(F("Sabit metin"));  // ✅ SRAM'den tasarruf
Serial.println("Sabit metin");      // ❌ SRAM kullanır

// PROGMEM kullanın
const char text[] PROGMEM = "Uzun metin...";
```

### Watchdog Timer

Mail gönderimi gibi uzun işlemler için:
```cpp
esp_task_wdt_reset();  // Watchdog'u resetle
```

---

## 📞 Destek ve Katkı

- **Issues:** https://github.com/smrtkrft/DMF_protocol/issues
- **Pull Requests:** Hoş geldiniz!
- **License:** AGPL-3.0 (fork ve değişiklikleri paylaşmalısınız)

---

## ✅ Hızlı Kontrol Listesi

Yüklemeden önce kontrol edin:

- [ ] ESP32 Board Package v3.0.0+ kurulu
- [ ] ArduinoJson v6.x kurulu (v7 DEĞİL!)
- [ ] ESP_Mail_Client (Mobizt) kurulu
- [ ] Board: XIAO_ESP32C6 seçili
- [ ] Partition: SmartKraft OTA seçili
- [ ] Flash Size: 4MB
- [ ] Port doğru seçildi
- [ ] Kütüphaneler compile oluyor
- [ ] GPIO pinleri donanıma uygun

---

**© 2025 SmartKraft | AGPL-3.0 License**
