# LebensSpur

LebensSpur, ESP32-C6 üzerinde çalışan bir ölü adam anahtarıdır (dead man's switch). Kullanıcı geri sayımı belirli aralıklarla sıfırlar. Süre dolarsa cihaz önceden ayarlanmış iletileri ve eylemleri gönderir: ekli e-posta, Telegram, webhook, MQTT ve röle. Kurulum ve kullanım cihazın içindeki web arayüzünden yapılır.

- Donanım: ESP32-C6, 4 MB dahili flash, 16 MB harici SPI flash, röle çıkışı.
- Yazılım: ESP-IDF 5.5.2.
- Program ve web arayüzü yalnızca dahili flash'tadır. Harici flash yalnızca kullanıcı verisini tutar: olay günlüğü, ayar yedeği ve posta ekleri.

## Depo yapısı

```
code/                    ESP-IDF projesi (firmware + web arayüzü)
  CMakeLists.txt         proje dosyası; PROJECT_VER sürümün tek kaynağı
  sdkconfig.defaults     SDK ayarları
  partitions.csv         dahili flash bölüm tablosu (ota_0 / ota_1 / coredump)
  dependencies.lock      bileşen sürüm kilidi (mdns, littlefs)
  main/                  açılış sırası ve görev kurulumu
  components/platform/   donanım ve sistem katmanı (cfg, http, net_wifi, storage, ota, ...)
  components/app/        uygulama katmanı (core, api, channels)
  gui/                   web arayüzü kaynağı (HTML/CSS/JS, 6 dil); derlemede firmware'e gömülür
  tools/                 kod üreteci (cfg_schema.json -> C + arayüz şeması)
  test/host/             masaüstü birim testleri (saf C modüller)
  test/api/              cihaz testleri (API sözleşmesi, güvenlik)
  esp_bin/               derleme ve test betikleri; derleme çıktıları buraya yazılır
docs/                    tasarım, plan, araştırma ve süreç kayıtları
```

## Gereksinimler

- ESP-IDF 5.5.2. Varsayılan yol `~/esp/esp-idf-v5.5.2`; farklıysa `IDF_DIR=/yol` ile belirtilir.
- Python 3.
- Arayüz testleri için Google Chrome (headless).

## Derleme ve yükleme

```sh
./code/esp_bin/build.sh                                   # derle
./code/esp_bin/build.sh -p /dev/cu.usbmodem1101 flash     # yükle (port: ls /dev/cu.usbmodem*)
./code/esp_bin/build.sh -p /dev/cu.usbmodem1101 monitor   # seri izle (çıkış: Ctrl+])
./code/esp_bin/build.sh menuconfig
```

`build.sh`, verilen argümanları olduğu gibi `idf.py`'ye geçirir.

Derleme çıktıları git'e girmez. Nereye yazıldıkları:

| Çıktı | Yer |
|---|---|
| Derleme dosyaları | `code/esp_bin/build/` |
| Üretilen ayar dosyası | `code/esp_bin/sdkconfig` |
| İndirilen bileşenler | `code/managed_components/` |
| Sürüm adıyla kopyalanan firmware | `code/esp_bin/firmware/lebensspur_vX.Y.Z.bin` ve `.elf` |

İmaj sınırı 1,5 MiB'dir; imaj bunu aşarsa derleme hata verir.

## Testler

```sh
./code/esp_bin/build.sh test                                  # masaüstü birim testleri
./code/esp_bin/full_check.sh <cihaz-ip> <port> <çıktı-klasörü>  # tam denetim
```

Tam denetim yedi katmanı sırayla çalıştırır:

1. Birim testleri.
2. `-Werror` ile derleme ve cihaza yükleme.
3. API sözleşme testi.
4. Arayüz ekran ve JavaScript denetimi.
5. Arayüz ↔ cihaz gidiş-dönüş testi.
6. Güvenlik testi; cihaz parolasız ve parolalı durumda karşılaştırılır.
7. Zorlama ve gerçek koşullar: istek seli, canlı bağlantı yuvalarının tükenmesi, yarım bağlantılar, art arda ayar kaydı, sayaç uç değerleri, ek dosya sınırları, art arda yeniden başlatma, ayar yazılırken USB'den sert reset.

Fabrika ayarı testi (`code/test/api/test_factory.py`) tam denetimin dışındadır. Fabrika ayarı WiFi bilgisini sildiği için bu test, WiFi bilgisini derleme ortamından yeniden yazan geliştirme derlemesiyle çalışır:

```sh
LS_DEV_WIFI_SSID=... LS_DEV_WIFI_PASS=... ./code/esp_bin/build.sh reconfigure
LS_DEV_WIFI_SSID=... LS_DEV_WIFI_PASS=... ./code/esp_bin/build.sh -p <port> flash
python3 code/test/api/test_factory.py <cihaz-ip>
./code/esp_bin/build.sh reconfigure && ./code/esp_bin/build.sh -p <port> flash   # parolasız derlemeye dön
```

Özet `<çıktı-klasörü>/ozet.txt` dosyasına yazılır. Kural: her düzeltmeden sonra tam denetim baştan çalışır. Sonraki aşamaya ancak **arka arkaya iki tur hatasız** geçtikten sonra geçilir.

## Yayın (OTA)

Cihaz güncellemeleri bu deponun yayınlarından denetler (`releases/latest`). Kaynak kod ve yayınlar aynı depodadır.

1. `code/CMakeLists.txt` içindeki `PROJECT_VER` artırılır.
2. Firmware derlenir; tam denetim iki kez hatasız geçer.
3. Değişiklikler commit edilip gönderilir.
4. Yayın açılır:
   - Etiket: `vX.Y.Z`. Cihaz sürümü etiketten okur, bu yüzden etiket bu biçimde kalmalıdır.
   - Başlık: `LS VX.Y.Z`.
   - Dosyalar: `code/esp_bin/firmware/` altından kopyalanan `lebensspur.bin` ve `lebensspur.elf`.

Cihaz bir imajı yalnızca `project_name` değeri `lebensspur` ise ve sürümü çalışan sürümden yeniyse kabul eder. Yeni imaj ilk açılışta kendini onaylar; onaylanmazsa cihaz önceki imaja geri döner.

## Belgeler

- `docs/00-surec.md`: süreç adımları ve karar günlüğü.
- `docs/04-plan-son.md`: son mimari plan.
- `docs/07-adim6-sozlesme.md`: cihaz uçları (API) sözleşmesi ve OTA cihaz turu.
- `docs/yapilacaklar.md`: yapılacaklar listesi.
