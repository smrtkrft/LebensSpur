# 🚨 SmartKraft LebensSpur Protocol

**[English](#english)** | **[Deutsch](#deutsch)** | **[Türkçe](#turkish)**

---

<a name="english"></a>
## 🇬🇧 English

### 🚨 SmartKraft LebensSpur Protocol

**LebensSpur Protocol** is an automatic message transmission system that requires periodic postponement actions. If postponement is not performed, your pre-prepared critical messages and important files are automatically sent to designated trusted contacts.

The system guarantees delivery of vital information, passwords, documents, or instructions to trusted individuals in emergency situations. It provides uninterrupted automatic transmission through a three-tier WiFi infrastructure, programmable alarm system, and relay control.

### 📖 What Does It Do?

The LebensSpur Protocol waits for you to postpone using the physical or virtual button within the set time period. If postponement is not done:

1. **Early Warning System:** Before the countdown ends, warning emails are sent to your email address as many times as you configured
2. **LebensSpur Protocol Activation:** If no action is taken by the end of the period, the protocol is activated and critical messages are automatically sent to your designated contacts/groups

### 💡 Who Is It Designed For?

- 👴 **Elderly living alone** - Regular check-in mechanism
- 🏥 **People with chronic conditions** - Emergency support for conditions like heart disease, epilepsy
- 🔐 **Critical information holders** - Secure transfer of important information
- 🛡️ **Security-requiring situations** - Anyone making emergency plans

### 🎯 How Does It Work?

1. **Setup** → WiFi connection, timer settings, email configuration, static IP assignment
2. **Usage** → Press the physical or virtual button before the alarm time, reset the countdown
3. **Early Warning** → If no postponement is made, you receive the configured number of warning emails
4. **LebensSpur Activation** → If no action is taken by the end, the protocol activates and sends critical messages

### ✨ Features

- ⏱️ **Flexible Timer System:** Configurable countdown from 1 minute to 60 days
- 📧 **Multiple Email Support:** Automatic email sending to 3 different groups
- 🔘 **Dual Button System:** Physical button (on board) and web-based virtual button
- 🌐 **Full-Featured Web Interface:** Modern web panel for all settings and controls
- 📱 **API Endpoint Support:** Mobile integration for virtual button
- 🔄 **Automatic OTA Updates:** Seamless firmware updates via GitHub
- 🌍 **Multi-language Support:** Turkish, English, and German interface
- ⚠️ **Early Warning System:** Configurable number of warnings before protocol activation

### 🛠️ Hardware Requirements

- **Microcontroller:** ESP32-C6 (RISC-V processor, WiFi integrated)
- **Power Supply:** 230V AC or 5V DC (USB-C)
- **Physical Button:** Output pins on board (optional - virtual button alternative available)
- **Relay Module:** Output pins on board (optional, maximum 5V 30mA - **WARNING:** Exceeding limit damages device)

### 🚀 Initial Setup

#### 1. WiFi Configuration

On first boot, the device creates an access point named **"SmartKraft-LebensSpur"**:

1. Connect to **SmartKraft-LebensSpur** network from WiFi networks
2. Go to `http://192.168.4.1` in your browser
3. Enter your WiFi network name (SSID) and password
4. Set static IP address (recommended)

#### 2. Timer Configuration

Set the countdown duration from the web interface:
- **Minimum:** 1 minute
- **Maximum:** 60 days
- **Example usage:** 7 days (weekly check)

#### 3. Email Configuration

Enter your SMTP server information:
- **Supported services:** ProtonMail, Gmail, Outlook, custom SMTP servers
- **Recipient groups:** Up to 3 different groups can be added
- **Early warning count:** You can set as many alarms as desired (COUNTDOWN > ALARM COUNT)

### 🌐 Web Interface

You can access the web panel from your device's static IP address:

```
http://[device-ip-address]
```

#### Menu Pages

- 🏠 **Home Page:** System status, countdown, and postpone button
- ⏱️ **Timer Settings:** Countdown duration and early warning configuration
- 📧 **Mail Settings:** SMTP configuration and recipient groups
- 📡 **WiFi Settings:** Network configuration and static IP settings
- 🔑 **API Settings:** Virtual button API endpoint configuration
- 🧪 **Test Page:** ⚠️ *In current version only for test group - Will be removed in future versions*

### 🔧 Technical Details

#### Hardware
- **Processor:** ESP32-C6 (RISC-V, 160 MHz)
- **Flash Memory:** 4MB (3MB APP / 1MB SPIFFS partitioning)
- **WiFi:** 2.4 GHz 802.11 b/g/n
- **Power Input:** 5V USB-C or 230V AC

#### Software
- **OTA Update:** GitHub repository-based automatic updates
- **Firmware Version:** v1.0.0
- **Partition Scheme:** SmartKraft OTA (Dual APP + SPIFFS)

### 📝 License

This project is licensed under **GNU Affero General Public License v3.0 (AGPL-3.0)**.

See [LICENSE](LICENSE) file for details.

### 🏢 About SmartKraft

**SmartKraft** is an open-source community that develops prototypes and conducts R&D work according to needs. All developments and experimental work are shared with open-source code.

**Website:** [smartkraft.ch](https://smartkraft.ch)

#### Developer

**SEU // Emek Ulaş Suna**

---

**© 2025 SmartKraft. All rights reserved.**

**Curiosity develops, development saves lives** 🛡️

---

<a name="deutsch"></a>
## 🇩🇪 Deutsch

### 🚨 SmartKraft LebensSpur-Protokoll

**LebensSpur Protokoll** ist ein automatisches Nachrichtenübermittlungssystem, das regelmäßige Verlängerungsaktionen erfordert. Wenn keine Verlängerung durchgeführt wird, werden Ihre vorbereiteten kritischen Nachrichten und wichtigen Dateien automatisch an festgelegte Vertrauenspersonen gesendet.

Das System garantiert die Zustellung lebenswichtiger Informationen, Passwörter, Dokumente oder Anweisungen an Vertrauenspersonen in Notfallsituationen. Es bietet unterbrechungsfreie automatische Übertragung durch eine dreistufige WiFi-Infrastruktur, programmierbares Alarmsystem und Relaissteuerung.

### 📖 Wofür ist es gedacht?

Das LebensSpur-Protokoll wartet darauf, dass Sie innerhalb der festgelegten Zeit mit der physischen oder virtuellen Taste verlängern. Wenn keine Verlängerung erfolgt:

1. **Frühwarnsystem:** Vor Ablauf des Countdowns werden so viele Warn-E-Mails an Ihre E-Mail-Adresse gesendet, wie Sie konfiguriert haben
2. **LebensSpur-Protokoll-Aktivierung:** Wenn bis zum Ende des Zeitraums keine Aktion durchgeführt wird, wird das Protokoll aktiviert und kritische Nachrichten werden automatisch an Ihre festgelegten Kontakte/Gruppen gesendet

### 💡 Für wen ist es konzipiert?

- 👴 **Alleinlebende ältere Menschen** - Regelmäßiger Check-in-Mechanismus
- 🏥 **Personen mit chronischen Erkrankungen** - Notfallunterstützung bei Erkrankungen wie Herzerkrankungen, Epilepsie
- 🔐 **Inhaber kritischer Informationen** - Sichere Übertragung wichtiger Informationen
- 🛡️ **Sicherheitsrelevante Situationen** - Jeder, der Notfallpläne erstellt

### 🎯 Wie funktioniert es?

1. **Einrichtung** → WiFi-Verbindung, Timer-Einstellungen, E-Mail-Konfiguration, statische IP-Zuweisung
2. **Verwendung** → Drücken Sie die physische oder virtuelle Taste vor der Alarmzeit, setzen Sie den Countdown zurück
3. **Frühwarnung** → Wenn keine Verlängerung erfolgt, erhalten Sie die konfigurierte Anzahl von Warn-E-Mails
4. **LebensSpur-Aktivierung** → Wenn bis zum Ende keine Aktion erfolgt, wird das Protokoll aktiviert und sendet kritische Nachrichten

### ✨ Funktionen

- ⏱️ **Flexibles Timer-System:** Konfigurierbarer Countdown von 1 Minute bis 60 Tage
- 📧 **Mehrfache E-Mail-Unterstützung:** Automatischer E-Mail-Versand an 3 verschiedene Gruppen
- 🔘 **Duales Tastensystem:** Physische Taste (auf der Platine) und webbasierte virtuelle Taste
- 🌐 **Voll ausgestattete Web-Oberfläche:** Modernes Web-Panel für alle Einstellungen und Steuerungen
- 📱 **API-Endpoint-Unterstützung:** Mobile Integration für virtuelle Taste
- 🔄 **Automatische OTA-Updates:** Nahtlose Firmware-Updates über GitHub
- 🌍 **Mehrsprachige Unterstützung:** Türkisch, Englisch und Deutsch Benutzeroberfläche
- ⚠️ **Frühwarnsystem:** Konfigurierbare Anzahl von Warnungen vor Protokollaktivierung

### 🛠️ Hardware-Anforderungen

- **Mikrocontroller:** ESP32-C6 (RISC-V Prozessor, WiFi integriert)
- **Stromversorgung:** 230V AC oder 5V DC (USB-C)
- **Physische Taste:** Ausgangspins auf der Platine (optional - virtuelle Taste als Alternative verfügbar)
- **Relaismodul:** Ausgangspins auf der Platine (optional, maximal 5V 30mA - **WARNUNG:** Überschreitung des Limits beschädigt das Gerät)

### 🚀 Ersteinrichtung

#### 1. WiFi-Konfiguration

Beim ersten Start erstellt das Gerät einen Access Point namens **"SmartKraft-LebensSpur"**:

1. Verbinden Sie sich mit dem **SmartKraft-LebensSpur** Netzwerk aus den WiFi-Netzwerken
2. Gehen Sie in Ihrem Browser zu `http://192.168.4.1`
3. Geben Sie Ihren WiFi-Netzwerknamen (SSID) und Passwort ein
4. Stellen Sie eine statische IP-Adresse ein (empfohlen)

#### 2. Timer-Konfiguration

Legen Sie die Countdown-Dauer über die Web-Oberfläche fest:
- **Minimum:** 1 Minute
- **Maximum:** 60 Tage
- **Beispielverwendung:** 7 Tage (wöchentliche Kontrolle)

#### 3. E-Mail-Konfiguration

Geben Sie Ihre SMTP-Serverinformationen ein:
- **Unterstützte Dienste:** ProtonMail, Gmail, Outlook, benutzerdefinierte SMTP-Server
- **Empfängergruppen:** Bis zu 3 verschiedene Gruppen können hinzugefügt werden
- **Frühwarnanzahl:** Sie können so viele Alarme wie gewünscht einstellen (COUNTDOWN > ALARMANZAHL)

### 🌐 Web-Oberfläche

Sie können über die statische IP-Adresse Ihres Geräts auf das Web-Panel zugreifen:

```
http://[gerät-ip-adresse]
```

#### Menüseiten

- 🏠 **Startseite:** Systemstatus, Countdown und Verlängerungstaste
- ⏱️ **Timer-Einstellungen:** Countdown-Dauer und Frühwarnungskonfiguration
- 📧 **Mail-Einstellungen:** SMTP-Konfiguration und Empfängergruppen
- 📡 **WiFi-Einstellungen:** Netzwerkkonfiguration und statische IP-Einstellungen
- 🔑 **API-Einstellungen:** Konfiguration des virtuellen Tasten-API-Endpoints
- 🧪 **Testseite:** ⚠️ *In aktueller Version nur für Testgruppe - Wird in zukünftigen Versionen entfernt*

### 🔧 Technische Details

#### Hardware
- **Prozessor:** ESP32-C6 (RISC-V, 160 MHz)
- **Flash-Speicher:** 4MB (3MB APP / 1MB SPIFFS-Partitionierung)
- **WiFi:** 2.4 GHz 802.11 b/g/n
- **Stromeingang:** 5V USB-C oder 230V AC

#### Software
- **OTA-Update:** GitHub-Repository-basierte automatische Updates
- **Firmware-Version:** v1.0.0
- **Partitionsschema:** SmartKraft OTA (Dual APP + SPIFFS)

### 📝 Lizenz

Dieses Projekt ist unter **GNU Affero General Public License v3.0 (AGPL-3.0)** lizenziert.

Siehe [LICENSE](LICENSE) Datei für Details.

### 🏢 Über SmartKraft

**SmartKraft** ist eine Open-Source-Community, die Prototypen entwickelt und F&E-Arbeit nach Bedarf durchführt. Alle Entwicklungen und experimentellen Arbeiten werden mit Open-Source-Code geteilt.

**Website:** [smartkraft.ch](https://smartkraft.ch)

#### Entwickler

**SEU // Emek Ulaş Suna**

---

**© 2025 SmartKraft. Alle Rechte vorbehalten.**

**Neugier entwickelt, Entwicklung rettet Leben** 🛡️

---

<a name="turkish"></a>
## 🇹🇷 Türkçe

### 🚨 SmartKraft LebensSpur Protokolü

**LebensSpur Protokolü**, belirli aralıklarla erteleme işlemi yapılmasını gerektiren otomatik mesaj iletim sistemidir. Erteleme yapılmazsa, önceden hazırladığınız kritik mesajlar ve önemli dosyalar belirlediğiniz güvenilir kişilere otomatik olarak gönderilir. 

Sistem, acil durumlarda hayati bilgilerin, şifrelerin, belgelerin veya talimatların güvenilir kişilere ulaşmasını garanti eder. Üç katmanlı WiFi altyapısı, programlanabilir alarm sistemi ve röle kontrolü ile kesintisiz otomatik iletim sağlar.

Sistem, acil durumlarda hayati bilgilerin, şifrelerin, belgelerin veya talimatların güvenilir kişilere ulaşmasını garanti eder. Üç katmanlı WiFi altyapısı, programlanabilir alarm sistemi ve röle kontrolü ile kesintisiz otomatik iletim sağlar.

### 📖 Ne İşe Yarar?

LebensSpur Protokolü, belirlediğiniz süre zarfında fiziksel veya sanal buton ile erteleme yapmanızı bekler. Erteleme yapılmazsa:

1. **Erken Uyarı Sistemi:** Geri sayım bitiminden önce, ayarladığınız alarm sayısı kadar e-posta adresinize uyarı gönderilir
2. **LebensSpur Protokolü Aktivasyonu:** Süre sonunda hiçbir işlem yapılmamışsa, protokol devreye girer ve belirlediğiniz kişilere/gruplara kritik mesajlar otomatik olarak gönderilir

### 💡 Kimler İçin Tasarlandı?

- 👴 **Evde tek yaşayan yaşlılar** - Düzenli kontrol mekanizması
- 🏥 **Kronik hastalığı olanlar** - Kalp, epilepsi gibi hastalıklarda acil durum desteği
- 🔐 **Kritik bilgi sahipleri** - Önemli bilgilerin güvenli aktarımı
- 🛡️ **Güvenlik gerektiren durumlar** - Acil durum planlaması yapan herkes

### 🎯 Nasıl Çalışır?

1. **Kurulum** → WiFi bağlantısı, timer ayarı, e-posta yapılandırması, statik IP ataması
2. **Kullanım** → Alarm süresi dolmadan önce fiziksel veya sanal butona bas, geri sayımı sıfırla
3. **Erken Uyarı** → Erteleme yapılmazsa belirlediğiniz sayıda uyarı e-postası gelir
4. **LebensSpur Aktivasyonu** → Süre sonunda işlem yapılmazsa protokol devreye girer, kritik mesajlar gönderilir

### ✨ Özellikler

- ⏱️ **Esnek Timer Sistemi:** 1 dakika ile 60 gün arasında ayarlanabilir geri sayım
- 📧 **Çoklu E-posta Desteği:** 3 farklı gruba otomatik e-posta gönderimi
- 🔘 **Çift Buton Sistemi:** Fiziksel buton (Bord üzerinde) ve web tabanlı sanal buton
- 🌐 **Tam Özellikli Web Arayüzü:** Tüm ayarlar ve kontroller için modern web paneli
- 📱 **API Endpoint Desteği:** Sanal buton için mobil entegrasyon
- 🔄 **Otomatik OTA Güncelleme:** GitHub üzerinden kesintisiz firmware güncellemeleri
- 🌍 **Çoklu Dil Desteği:** Türkçe, İngilizce ve Almanca arayüz
- ⚠️ **Erken Uyarı Sistemi:** Protokol devreye girmeden önce ayarlanabilir sayıda uyarı

### 🛠️ Donanım Gereksinimleri

- **Mikrodenetleyici:** ESP32-C6 (RISC-V işlemci, WiFi entegreli)
- **Güç Kaynağı:** 230V AC veya 5V DC (USB-C)
- **Fiziksel Buton:** Bord üzerinde cikis pinleri (isteğe bağlı - sanal buton alternatifi mevcut)
- **Röle Modülü:** Bord üzerinde cikis pinleri (isteğe bağlı, maksimum 5V 30mA - **DİKKAT:** Limit aşımı cihaza zarar verir)

### 🚀 İlk Kullanım

#### 1. WiFi Yapılandırması

Cihaz ilk açılışta **"SmartKraft-LebensSpur"** adında bir access point oluşturur:

1. WiFi ağlarından **SmartKraft-LebensSpur** ağına bağlanın
2. Tarayıcınızda `http://192.168.4.1` adresine gidin
3. WiFi ağ adınızı (SSID) ve şifrenizi girin
4. Statik IP adresi ayarlayın (önerilen)

#### 2. Timer Yapılandırması

Web arayüzünden geri sayım süresini belirleyin:
- **Minimum:** 1 dakika
- **Maksimum:** 60 gün
- **Örnek kullanım:** 7 gün (haftalık kontrol)

#### 3. E-posta Yapılandırması

SMTP sunucu bilgilerinizi girin:
- **Desteklenen servisler:** ProtonMail, Gmail, Outlook, özel SMTP sunucuları
- **Alıcı grupları:** 3 farklı gruba kadar eklenebilir
- **Erken uyarı sayısı:** İstediğiniz sayıda alarm ayarlayabilirsiniz (GERI SAYIM > ALARM SAYISI)

### 🌐 Web Arayüzü

Cihazınızın statik IP adresinden web paneline erişebilirsiniz:

```
http://[cihaz-ip-adresi]
```

#### Menü Sayfaları

- 🏠 **Ana Sayfa:** Sistem durumu, geri sayım ve erteleme butonu
- ⏱️ **Timer Ayarları:** Geri sayım süresi ve erken uyarı yapılandırması
- 📧 **Mail Ayarları:** SMTP yapılandırması ve alıcı grupları
- 📡 **WiFi Ayarları:** Ağ yapılandırması ve statik IP ayarları
- 🔑 **API Ayarları:** Sanal buton API endpoint yapılandırması
- 🧪 **Test Sayfası:** ⚠️ *Mevcut sürümde sadece test grubu için - Sonraki sürümlerde kaldırılacak*

### 🔧 Teknik Detaylar

#### Donanım
- **İşlemci:** ESP32-C6 (RISC-V, 160 MHz)
- **Flash Bellek:** 4MB (3MB APP / 1MB SPIFFS bölümleme)
- **WiFi:** 2.4 GHz 802.11 b/g/n
- **Güç Girişi:** 5V USB-C veya 230V AC

#### Yazılım
- **OTA Güncelleme:** GitHub repository tabanlı otomatik güncelleme
- **Firmware Sürümü:** v1.0.0
- **Partition Scheme:** SmartKraft OTA (Dual APP + SPIFFS)

### 📝 Lisans

Bu proje **GNU Affero General Public License v3.0 (AGPL-3.0)** altında lisanslanmıştır.

Detaylar için [LICENSE](LICENSE) dosyasına bakın.

### 🏢 SmartKraft Hakkında

**SmartKraft**, ihtiyaçlar doğrultusunda prototipler geliştiren ve AR-GE çalışmaları yapan açık kaynak topluluğudur. Tüm geliştirmeler ve deneysel çalışmalar açık kaynak kod ile paylaşılır.

**Web Sitesi:** [smartkraft.ch](https://smartkraft.ch)

#### Geliştirici

**SEU // Emek Ulaş Suna**

---

**© 2025 SmartKraft. Tüm hakları saklıdır.**

**Merak etmek geliştirir, geliştirmek hayat kurtarır** 🛡️
