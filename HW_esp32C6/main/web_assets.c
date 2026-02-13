/**
 * Web Assets - Gomulu HTML Sayfalari
 *
 * Login ve fallback index HTML firmware icinde gomulu.
 * Token-bazli auth: login sonrasi token localStorage'a kaydedilir,
 * her istekte Authorization: Bearer <token> olarak gonderilir.
 * Cookie fallback da desteklenir (Set-Cookie response'dan).
 */

#include "web_assets.h"
#include "file_manager.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "WEB_ASSETS";

#define WEB_ASSETS_VERSION "3.0.0"
#define VERSION_FILE FILE_MGR_WEB_PATH "/version.txt"

// Embedded setup.html (CMakeLists.txt'de EMBED_TXTFILES ile dahil)
extern const uint8_t setup_html_start[] asm("_binary_setup_html_start");
extern const uint8_t setup_html_end[] asm("_binary_setup_html_end");

// ============================================================================
// LOGIN PAGE - Siyah/beyaz monospace, sadece sifre, Bearer token
// ============================================================================
static const char LOGIN_HTML[] =
"<!DOCTYPE html>\n"
"<html lang=\"tr\">\n"
"<head>\n"
"<meta charset=\"UTF-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
"<title>LebensSpur</title>\n"
"<style>\n"
"*{margin:0;padding:0;box-sizing:border-box}\n"
"body{font-family:monospace;background:#000;color:#fff;min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px}\n"
".container{width:100%;max-width:400px}\n"
".header{text-align:center;margin-bottom:32px}\n"
".logo{width:80px;height:80px;margin:0 auto 16px;border:2px solid #fff;border-radius:50%;display:flex;align-items:center;justify-content:center}\n"
".logo svg{width:48px;height:48px}\n"
".header h1{font-size:1.6em;font-weight:normal;letter-spacing:3px}\n"
".header span{color:#666;font-size:.85em;display:block;margin-top:8px}\n"
".box{border:1px solid #333;padding:24px}\n"
".form-group{margin-bottom:16px}\n"
"label{display:block;margin-bottom:6px;color:#888;font-size:.85em;text-transform:uppercase;letter-spacing:1px}\n"
"input{width:100%;padding:12px;border:1px solid #333;background:#000;color:#fff;font-family:monospace;font-size:1em}\n"
"input:focus{outline:none;border-color:#fff}\n"
"button{width:100%;padding:12px;border:1px solid #fff;background:#000;color:#fff;font-family:monospace;font-size:1em;cursor:pointer;text-transform:uppercase;letter-spacing:2px;margin-top:8px}\n"
"button:hover{background:#fff;color:#000}\n"
".error{color:#f00;text-align:center;margin-top:12px;font-size:.85em;display:none}\n"
".lang{display:flex;justify-content:center;gap:8px;margin-top:20px}\n"
".lang button{width:auto;padding:6px 12px;border-color:#333;font-size:.75em}\n"
".lang button:hover{border-color:#fff}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<div class=\"container\">\n"
"<div class=\"header\">\n"
"<div class=\"logo\"><svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#fff\" stroke-width=\"2\"><circle cx=\"12\" cy=\"12\" r=\"10\"/><polyline points=\"12 6 12 12 16 14\"/></svg></div>\n"
"<h1>LEBENSSPUR</h1><span>Dead Man's Switch</span></div>\n"
"<div class=\"box\">\n"
"<form id=\"f\">\n"
"<div class=\"form-group\"><label id=\"l1\">SIFRE</label><input type=\"password\" id=\"p\" required></div>\n"
"<button type=\"submit\" id=\"l2\">GIRIS YAP</button>\n"
"<p class=\"error\" id=\"e\"></p>\n"
"</form>\n"
"</div>\n"
"<div class=\"lang\"><button onclick=\"setLang('tr')\">TR</button><button onclick=\"setLang('en')\">EN</button><button onclick=\"setLang('de')\">DE</button></div>\n"
"</div>\n"
"<script>\n"
"var t={tr:{l1:'SIFRE',l2:'GIRIS YAP',e1:'Giris basarisiz'},en:{l1:'PASSWORD',l2:'LOGIN',e1:'Login failed'},de:{l1:'PASSWORT',l2:'ANMELDEN',e1:'Anmeldung fehlgeschlagen'}};\n"
"function setLang(l){localStorage.setItem('lang',l);var s=t[l];document.getElementById('l1').textContent=s.l1;document.getElementById('l2').textContent=s.l2;}\n"
"setLang(localStorage.getItem('lang')||'tr');\n"
"document.getElementById('f').onsubmit=async function(e){e.preventDefault();var err=document.getElementById('e');err.style.display='none';\n"
"try{var r=await fetch('/api/login',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({password:document.getElementById('p').value})});\n"
"var d=await r.json();if(d.success&&d.token){localStorage.setItem('ls_token',d.token);location.href='/';}else{err.textContent=t[localStorage.getItem('lang')||'tr'].e1;err.style.display='block';}}\n"
"catch(ex){err.textContent='Connection error';err.style.display='block';}};\n"
"</script>\n"
"</body>\n"
"</html>\n";

// ============================================================================
// INDEX PAGE - Fallback (harici flash'ta GUI yoksa)
// Bearer token ile API cagirilari
// ============================================================================
static const char INDEX_HTML[] =
"<!DOCTYPE html>\n"
"<html lang=\"tr\">\n"
"<head>\n"
"<meta charset=\"UTF-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
"<title>LebensSpur Panel</title>\n"
"<style>\n"
"*{margin:0;padding:0;box-sizing:border-box}\n"
"body{font-family:monospace;background:#000;color:#fff;line-height:1.4;font-size:14px}\n"
".c{max-width:800px;margin:0 auto;padding:16px}\n"
".hdr{text-align:center;margin-bottom:20px;padding-bottom:12px;border-bottom:1px solid #333}\n"
".hdr h1{font-size:1.6em;font-weight:normal;letter-spacing:3px}\n"
".did{color:#666;font-size:.85em;margin-top:6px}\n"
".sg{display:grid;grid-template-columns:repeat(4,1fr);gap:12px;margin-bottom:20px;border:1px solid #333;padding:12px}\n"
".sc{text-align:center}\n"
".sl{color:#666;font-size:.75em;text-transform:uppercase;letter-spacing:1px}\n"
".sv{font-size:1.1em;color:#fff;margin-top:4px}\n"
".tr{text-align:center;border:1px solid #333;padding:24px;margin-bottom:20px}\n"
".tv{font-size:3em;letter-spacing:2px;font-weight:normal}\n"
".tl{color:#666;margin-top:8px;font-size:.85em;text-transform:uppercase}\n"
".bb{display:flex;gap:8px;justify-content:center;margin-top:20px}\n"
"button{background:#000;border:1px solid #555;color:#fff;padding:10px 20px;font-family:monospace;cursor:pointer;text-transform:uppercase;letter-spacing:1px}\n"
"button:hover{background:#222;border-color:#fff}\n"
".bp{border-color:#fff}\n"
".bp:hover{background:#fff;color:#000}\n"
".ir{display:flex;justify-content:space-between;padding:8px 0;border-bottom:1px solid #222}\n"
".il{color:#666}.iv{color:#fff}\n"
"@media(max-width:600px){.sg{grid-template-columns:1fr 1fr}}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<div class=\"c\">\n"
"<div class=\"hdr\"><h1>LEBENSSPUR</h1><div class=\"did\" id=\"did\">---</div></div>\n"
"<div class=\"sg\">\n"
"<div class=\"sc\"><div class=\"sl\">DURUM</div><div class=\"sv\" id=\"ss\">-</div></div>\n"
"<div class=\"sc\"><div class=\"sl\">KALAN</div><div class=\"sv\" id=\"sr\">-</div></div>\n"
"<div class=\"sc\"><div class=\"sl\">RESET</div><div class=\"sv\" id=\"sn\">-</div></div>\n"
"<div class=\"sc\"><div class=\"sl\">TETIK</div><div class=\"sv\" id=\"st\">-</div></div>\n"
"</div>\n"
"<div class=\"tr\">\n"
"<div class=\"tv\" id=\"tm\">00:00:00</div>\n"
"<div class=\"tl\">GERI SAYIM</div>\n"
"<div class=\"bb\">\n"
"<button class=\"bp\" onclick=\"resetTimer()\">SIFIRLA</button>\n"
"<button onclick=\"logout()\">CIKIS</button>\n"
"</div>\n"
"</div>\n"
"<div style=\"border:1px solid #333;padding:16px;margin-bottom:16px\">\n"
"<div class=\"ir\"><span class=\"il\">UPTIME</span><span class=\"iv\" id=\"up\">-</span></div>\n"
"<div class=\"ir\"><span class=\"il\">HEAP</span><span class=\"iv\" id=\"hp\">-</span></div>\n"
"<div class=\"ir\"><span class=\"il\">WIFI</span><span class=\"iv\" id=\"wi\">-</span></div>\n"
"</div>\n"
"</div>\n"
"<script>\n"
"var T=localStorage.getItem('ls_token')||'';\n"
"function hdr(){return{headers:{'Authorization':'Bearer '+T,'Content-Type':'application/json'}};}\n"
"function fmt(s){var h=Math.floor(s/3600),m=Math.floor((s%3600)/60),c=s%60;return String(h).padStart(2,'0')+':'+String(m).padStart(2,'0')+':'+String(c).padStart(2,'0');}\n"
"function fmem(b){return b<1024?b+'B':b<1048576?(b/1024).toFixed(1)+'KB':(b/1048576).toFixed(1)+'MB';}\n"
"async function load(){\n"
"try{var r=await fetch('/api/timer/status',hdr());if(r.status==401){location.href='/login.html';return;}\n"
"var d=await r.json();var st=['PASIF','AKTIF','UYARI','TETIKLENDI','DURAKLATILDI'];\n"
"document.getElementById('tm').textContent=fmt(d.remaining_seconds||0);\n"
"document.getElementById('ss').textContent=st[d.state]||'-';\n"
"document.getElementById('sr').textContent=fmt(d.remaining_seconds||0);\n"
"document.getElementById('sn').textContent=d.reset_count||0;\n"
"document.getElementById('st').textContent=d.trigger_count||0;\n"
"}catch(e){}\n"
"try{var r=await fetch('/api/device/info',hdr());var d=await r.json();\n"
"document.getElementById('did').textContent=d.device_id+' / v'+d.firmware_version;\n"
"var up=Math.floor((d.uptime_ms||0)/1000);var h=Math.floor(up/3600);var m=Math.floor((up%3600)/60);\n"
"document.getElementById('up').textContent=h+'s '+m+'dk';\n"
"document.getElementById('hp').textContent=fmem(d.heap_free||0);\n"
"document.getElementById('wi').textContent=d.wifi_connected?(d.sta_ip||'bagli'):'bagli degil';\n"
"}catch(e){}}\n"
"async function resetTimer(){try{await fetch('/api/timer/reset',Object.assign({method:'POST'},hdr()));load();}catch(e){}}\n"
"async function logout(){localStorage.removeItem('ls_token');try{await fetch('/api/logout',Object.assign({method:'POST'},hdr()));}catch(e){}location.href='/login.html';}\n"
"if(!T){location.href='/login.html';}else{load();setInterval(load,5000);}\n"
"</script>\n"
"</body>\n"
"</html>\n";

// ============================================================================
// Getter fonksiyonlari
// ============================================================================

const char *web_assets_get_login_html(void)
{
    return LOGIN_HTML;
}

const char *web_assets_get_index_html(void)
{
    return INDEX_HTML;
}

const char *web_assets_get_setup_html(void)
{
    return (const char *)setup_html_start;
}

bool web_assets_installed(void)
{
    return file_manager_exists(VERSION_FILE);
}

const char *web_assets_get_version(void)
{
    return WEB_ASSETS_VERSION;
}

esp_err_t web_assets_create_defaults(void)
{
    ESP_LOGI(TAG, "Varsayilan web dosyalari olusturuluyor...");

    esp_err_t ret = file_manager_mkdir(FILE_MGR_WEB_PATH);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Web dizini olusturulamadi");
        return ret;
    }

    // login.html
    ret = file_manager_write(FILE_MGR_WEB_PATH "/login.html",
                             LOGIN_HTML, strlen(LOGIN_HTML));
    if (ret != ESP_OK) return ret;

    // index.html
    ret = file_manager_write(FILE_MGR_WEB_PATH "/index.html",
                             INDEX_HTML, strlen(INDEX_HTML));
    if (ret != ESP_OK) return ret;

    // Version dosyasi
    ret = file_manager_write_string(VERSION_FILE, WEB_ASSETS_VERSION);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "Web dosyalari olusturuldu (v%s)", WEB_ASSETS_VERSION);
    return ESP_OK;
}

esp_err_t web_assets_deploy(bool force)
{
    if (!force && web_assets_installed()) {
        char ver[16] = {0};
        if (file_manager_read_string(VERSION_FILE, ver, sizeof(ver)) == ESP_OK) {
            if (strcmp(ver, WEB_ASSETS_VERSION) == 0) {
                ESP_LOGI(TAG, "Web assets guncel (v%s)", ver);
                return ESP_OK;
            }
            ESP_LOGI(TAG, "Web assets guncelleniyor: v%s -> v%s", ver, WEB_ASSETS_VERSION);
        }
    }

    return web_assets_create_defaults();
}
