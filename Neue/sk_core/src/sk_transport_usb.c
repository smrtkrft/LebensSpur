// USB Serial/JTAG CLI transport · ESP-IDF v5.x.
//
// 2026-05-20: ANSI arrow-key history desteği eklendi (insan modu).
// Linenoise'a dönmeden kendi 16-slot circular buffer'ımızla. SKAPP'in
// JSON modu etkilenmez (ESC sequence sadece ilk byte != '{' iken).
//
// 2026-05-14 yeniden yazıldı (linenoise + stdio yığını çıkarıldı).
//
// Önceki sürüm linenoise + esp_console + VFS ile stdin/stdout üzerinden
// çalışıyordu. ESP-IDF'in ESP_CONSOLE_USB_SERIAL_JTAG=y kurulumu ile bu
// yığın, USB Serial/JTAG'i SKAPP'tan kullanılmaz hale getiriyordu: ESP-IDF
// boot'ta basit (driver'sız) VFS kuruyor, biz sonradan
// usb_serial_jtag_driver_install + use_driver ile FreeRTOS-driver path'ine
// geçiriyorduk; setvbuf ile linenoise tty algılaması bu geçişte yarım
// kalıyordu. Sonuç: SKAPP'a banner / prompt / cevap hiç ulaşmıyordu.
//
// Yeni sürüm: linenoise + stdio'yu tamamen atla, doğrudan
// `usb_serial_jtag_read_bytes` / `usb_serial_jtag_write_bytes` ile çalış.
// Hat-tabanlı (NDJSON-uyumlu) okuma + her satır için
// `sk_cli_dispatch_line` çağrısı. Echo + prompt yalnız "human" sürüm
// için (satır `{` ile başlamıyorsa); makine modunda (SKAPP) çıktı saf
// JSON kalır, request id eşleşmesi temiz çalışır.
//
// Kayıplar: TAB completion. SKAPP'in NDJSON history'si zaten host'ta;
// in-device history insan kullanıcılar (PuTTY / idf.py monitor) için
// mini 16-slot ring buffer ile sağlanır — yukarı/aşağı ok.

#include "sk_transport_usb.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sk_cli.h"
#include "sk_core.h"

static const char *TAG = "sk_transport_usb";

#define USB_LINE_BUF        1024   // host'tan gelen tek satır maks. uzunluk
#define USB_RX_BUFFER_SIZE  512    // driver iç RX FIFO
#define USB_TX_BUFFER_SIZE  2048   // driver iç TX FIFO (help / device.info 1-2 KB)
#define USB_WRITE_TICKS     pdMS_TO_TICKS(200)

static bool        s_initialized      = false;
static const char *s_prompt_override  = NULL;

// ---------------------------------------------------------------------
// Mini history (insan modu için)
// 16 slot × 256 byte = 4 KB statik RAM. Çoğu CLI komutu < 80 byte;
// 256 büyük rezerv. Daha uzun komutlar truncate (kabul edilebilir).
// ---------------------------------------------------------------------
#define HIST_MAX        16
#define HIST_LINE_MAX   256

static char s_hist[HIST_MAX][HIST_LINE_MAX];
static int  s_hist_count = 0;      // dolu slot sayısı (max HIST_MAX)
static int  s_hist_next  = 0;      // sıradaki yazılacak slot index

static void hist_push(const char *line)
{
    if (!line || !*line) return;
    // Son girilen ile aynıysa atla (ardışık dup'ları temizle)
    if (s_hist_count > 0) {
        int last = (s_hist_next - 1 + HIST_MAX) % HIST_MAX;
        if (strcmp(s_hist[last], line) == 0) return;
    }
    strncpy(s_hist[s_hist_next], line, HIST_LINE_MAX - 1);
    s_hist[s_hist_next][HIST_LINE_MAX - 1] = '\0';
    s_hist_next = (s_hist_next + 1) % HIST_MAX;
    if (s_hist_count < HIST_MAX) s_hist_count++;
}

// offset: 1 = en son komut, 2 = ondan önceki, ... s_hist_count = en eski
static const char *hist_get(int offset)
{
    if (offset < 1 || offset > s_hist_count) return NULL;
    int idx = (s_hist_next - offset + HIST_MAX) % HIST_MAX;
    return s_hist[idx];
}

static const char *active_prompt(void)
{
    return s_prompt_override ? s_prompt_override : sk_core_get_prompt();
}

// Raw bytes -> driver. usb_writer'ın altındaki yapı taşı; CRLF
// normalizasyonu yapmaz. usb_writer ve write_str doğrudan bunu çağırır.
static void write_raw(const char *bytes, size_t len)
{
    if (!bytes || len == 0) return;
    size_t off = 0;
    while (off < len) {
        const int written = usb_serial_jtag_write_bytes(
            (const uint8_t *)(bytes + off),
            len - off,
            USB_WRITE_TICKS);
        if (written <= 0) {
            // Host okumadan TX buffer doldu + timeout; kalan byte'ı drop et.
            ESP_LOGW(TAG, "write_raw dropped %u bytes (host slow / disconnected)",
                     (unsigned)(len - off));
            return;
        }
        off += (size_t)written;
    }
}

// CLI dispatch + boot banner için writer. fwrite(stdout) yerine doğrudan
// driver'a yazar; VFS / setvbuf routing karmaşası tamamen atlanır.
//
// Eski sürümde VFS `ESP_LINE_ENDINGS_CRLF` TX modunda `\n` byte'larını
// otomatik `\r\n` yapıyordu; VFS bypass'tan sonra bu çeviri kayboldu ve
// PuTTY/TeraTerm gibi terminallerde imleç sadece bir satır aşağı iniyor
// (CR yok), sonraki yazı satırın ortasından başlıyor. Burada manuel
// CRLF normalizasyonu yapıyoruz: chunk içinde herhangi bir standalone
// `\n` (önünde `\r` olmayan) bulunursa öncesinde `\r` enjekte edilir.
// Zaten doğru CRLF sekansları aynen geçer.
static void usb_writer(const char *chunk, size_t len, void *user)
{
    (void)user;
    if (!chunk || len == 0) return;

    size_t i = 0;
    char prev = '\0';   // chunk'tan önceki son yazılan byte; geriye dönük
                         // CR algılaması için cross-call hatırlamadığımız
                         // (best-effort); tek bir dispatch içinde \r ve \n
                         // ayrı çağrılarda gelse de küçük bir CR fazlalığı
                         // PuTTY'de görsel artefakt yaratmaz.
    while (i < len) {
        // Sıradaki \n'yi bul; arada düz byte'ları topluca yaz.
        size_t j = i;
        while (j < len && chunk[j] != '\n') j++;
        if (j > i) {
            write_raw(chunk + i, j - i);
            prev = chunk[j - 1];
        }
        if (j < len) {
            // chunk[j] == '\n'. Önünde \r yoksa CR ekle.
            if (prev != '\r') {
                write_raw("\r", 1);
            }
            write_raw("\n", 1);
            prev = '\n';
            i = j + 1;
        } else {
            break;
        }
    }
}

static void write_str(const char *s)
{
    if (!s || !*s) return;
    usb_writer(s, strlen(s), NULL);
}

// İnsan modunda mevcut input satırını ekranda history'den gelen yeni
// satırla değiştir. ANSI `\r\x1b[K` (line başına git + satır sonuna kadar
// sil), sonra prompt + new_line yazılır. buf + *idx senkronize edilir.
static void replace_line(char *buf, size_t cap, size_t *idx, const char *new_line)
{
    write_str("\r\x1b[K");
    write_str(active_prompt());
    if (new_line && *new_line) write_str(new_line);

    size_t n = new_line ? strlen(new_line) : 0;
    if (n > cap - 1) n = cap - 1;
    if (n > 0) memcpy(buf, new_line, n);
    buf[n] = '\0';
    *idx = n;
}

// Tek satırı oku. Blok-blok (her byte için portMAX_DELAY) — boş USB
// kanalı CPU yakmaz, ilk byte gelene kadar görev uyur.
//
// Hat sonu (`\n` veya `\r`) gelene kadar byte topla; ilk byte `{` ise
// makine modu işaretli, echo + sonraki prompt bastırılır (SKAPP'a saf
// JSON gider). İnsan modunda her byte echo edilir, satır sonunda CRLF
// basılır. Backspace (0x08 / 0x7F) görsel olarak ele alınır.
//
// İnsan modunda yukarı/aşağı ok (ESC `[A` / `[B`) history navigation:
//   - browse_offset = 0 ise edit buffer aktif (yeni komut)
//   - up: offset++ → eski komutu yükle ve göster
//   - down: offset-- → daha yeni komutu / boş satır
//   - başka bir karakter girince browse iptal (offset = 0)
//
// `*out_machine` döner: satır makine modunda mı.
static size_t read_line(char *buf, size_t cap, bool *out_machine)
{
    size_t idx = 0;
    bool first_byte = true;
    bool machine_mode = false;
    int  hist_browse = 0;   // 0 = aktif edit; 1+ = history offset

    while (idx < cap - 1) {
        uint8_t c;
        const int n = usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY);
        if (n <= 0) continue;

        if (first_byte) {
            machine_mode = (c == '{');
            first_byte = false;
        }

        // ESC sequence: insan modunda ANSI ok tuşları
        if (c == 0x1B && !machine_mode) {
            uint8_t s1, s2;
            // CSI introducer ve final byte için kısa timeout (Alt+letter
            // gibi sequence değilse ESC yalnız geldi say, atla).
            if (usb_serial_jtag_read_bytes(&s1, 1, pdMS_TO_TICKS(50)) <= 0) continue;
            if (s1 != '[' && s1 != 'O') continue;   // sadece CSI / SS3
            if (usb_serial_jtag_read_bytes(&s2, 1, pdMS_TO_TICKS(50)) <= 0) continue;

            if (s2 == 'A') {          // up
                if (hist_browse < s_hist_count) {
                    hist_browse++;
                    const char *h = hist_get(hist_browse);
                    if (h) replace_line(buf, cap, &idx, h);
                }
            } else if (s2 == 'B') {   // down
                if (hist_browse > 0) {
                    hist_browse--;
                    const char *h = (hist_browse > 0) ? hist_get(hist_browse) : "";
                    replace_line(buf, cap, &idx, h);
                }
            }
            // sağ/sol (C/D) ve diğerleri yok say — line-edit eski sürüm
            // gibi append-only kalır. (İleride lineedit eklenebilir.)
            continue;
        }

        if (c == '\n' || c == '\r') {
            if (!machine_mode) {
                usb_writer("\r\n", 2, NULL);   // CRLF terminal echosu
            }
            buf[idx] = '\0';
            *out_machine = machine_mode;
            return idx;
        }
        if (c == 0x08 || c == 0x7F) {           // BS / DEL
            if (idx > 0) {
                idx--;
                buf[idx] = '\0';
                if (!machine_mode) {
                    usb_writer("\b \b", 3, NULL); // görsel sil
                }
            }
            hist_browse = 0;   // edit başladı, browse'tan çık
            continue;
        }
        if (c < 0x20) continue;                 // diğer control chars

        if (!machine_mode) {
            usb_writer((const char *)&c, 1, NULL);
        }
        buf[idx++] = (char)c;
        hist_browse = 0;   // edit başladı, browse'tan çık
    }

    // Overflow: satır cap'i aştı. Truncate edip yine de dispatch et.
    buf[idx] = '\0';
    *out_machine = machine_mode;
    return idx;
}

static void cli_task(void *arg)
{
    (void)arg;

    // İnsan kullanıcısı için açılış banner'ı + ilk prompt. SKAPP bunları
    // alır ama JSON olmadığı için cli_client._onLine'da jsonDecode fail
    // edip sessiz drop edilir (request id matching etkilenmez).
    sk_core_write_banner(usb_writer, NULL);
    write_str(active_prompt());

    char line[USB_LINE_BUF];
    while (1) {
        bool machine = false;
        const size_t len = read_line(line, sizeof(line), &machine);
        if (len > 0) {
            sk_cli_dispatch_line(line, usb_writer, NULL);
            // İnsan modunda komutu history'ye ekle. Makine modunda SKAPP
            // kendi history'sini host tarafında tutuyor, JSON satırları
            // kaydetmek anlamsız + 16 slotu boğar.
            if (!machine) hist_push(line);
        }
        // İnsan modunda dispatch sonrası bir sonraki komut için prompt
        // bas. Makine modunda prompt göndermek SKAPP read buffer'ı kirletir.
        if (!machine) {
            write_str(active_prompt());
        }
    }
}

esp_err_t sk_transport_usb_init(const sk_transport_usb_cfg_t *cfg)
{
    if (s_initialized) return ESP_OK;

    int stack = 8192;
    int prio  = 4;
    if (cfg) {
        if (cfg->prompt)            s_prompt_override = cfg->prompt;  // contract: static lifetime
        if (cfg->task_stack    > 0) stack = cfg->task_stack;
        if (cfg->task_priority > 0) prio  = cfg->task_priority;
        // cfg->history_size artık kullanılmıyor (linenoise gitti).
    }

    usb_serial_jtag_driver_config_t drv_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    drv_cfg.rx_buffer_size = USB_RX_BUFFER_SIZE;
    drv_cfg.tx_buffer_size = USB_TX_BUFFER_SIZE;
    esp_err_t err = usb_serial_jtag_driver_install(&drv_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "usb_serial_jtag_driver_install: %s", esp_err_to_name(err));
        return err;
    }

    BaseType_t ok = xTaskCreate(cli_task, "sk_cli_usb", stack, NULL, prio, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "cli_task create failed");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "USB Serial/JTAG CLI ready (direct driver, prompt=\"%s\")",
             active_prompt());
    return ESP_OK;
}
