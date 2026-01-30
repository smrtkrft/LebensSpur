#include "web_handlers.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <esp_wifi.h>  // WiFi güç yönetimi için

// FIRMWARE_VERSION artık config_store.h'da tanımlı (tek noktada yönetim)

// ============================================
// EXTERN DEĞIŞKENLER (SmartKraft_LebensSpur.ino'dan)
// ============================================
extern float currentTemperature;
extern float maxTemperature;
extern float minTemperature;
extern const char* getThermalStateString();
extern bool wifiDisabledByThermal;

// i18n language files
#include "i18n_en.h"
#include "i18n_de.h"
#include "i18n_tr.h"

// Embedded HTML content
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en" id="htmlRoot">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>SmartKraft LebensSpur Control Panel</title>
    <style>*{margin:0;padding:0;box-sizing:border-box}body{font-family:monospace;background:#000;color:#fff;line-height:1.4;font-size:14px}a{color:#fff}.container{max-width:820px;margin:0 auto;padding:16px}.header{text-align:center;margin-bottom:20px;padding-bottom:12px;border-bottom:1px solid #333}.header h1{font-size:1.8em;font-weight:normal;letter-spacing:2px}.device-id{color:#777;font-size:.9em;margin-top:4px}.status-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:12px;margin-bottom:20px;border:1px solid #333;padding:12px}.status-card{text-align:center}.status-label{color:#666;font-size:.8em;margin-bottom:4px;text-transform:uppercase}.status-value{font-size:1.2em;color:#fff;min-height:1.2em}.timer-readout{text-align:center;border:1px solid #333;padding:18px;margin-bottom:16px}.timer-readout .value{font-size:2.6em;letter-spacing:2px}.timer-readout .label{color:#777;margin-top:6px;font-size:.85em}.button-bar{display:flex;flex-wrap:wrap;gap:8px;justify-content:center;margin-bottom:24px}button{background:transparent;border:1px solid #555;color:#fff;padding:10px 18px;font-family:monospace;cursor:pointer;text-transform:uppercase;letter-spacing:1px;transition:background .2s}button:hover{background:#222}.btn-danger{border-color:#f00;color:#f00}.btn-danger:hover{background:#f00;color:#000}.btn-success{border-color:#fff;color:#fff}.btn-success:hover{background:#fff;color:#000}.btn-warning{border-color:#ff0;color:#ff0}.btn-warning:hover{background:#ff0;color:#000}.tabs{display:flex;flex-wrap:wrap;border-bottom:1px solid #333;margin-bottom:8px}.tab{flex:1;min-width:140px;border:1px solid #333;border-bottom:none;background:#000;color:#666;padding:10px;cursor:pointer;text-align:center;font-size:.9em}.tab+.tab{margin-left:4px}.tab.active{color:#fff;border-color:#fff}.tab-content{border:1px solid #333;padding:20px}.tab-pane{display:none}.tab-pane.active{display:block}.form-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:12px}.form-group{display:flex;flex-direction:column;gap:6px;margin-bottom:16px}label{font-size:.85em;color:#ccc;text-transform:uppercase;letter-spacing:1px}input[type="text"],input[type="number"],input[type="password"],input[type="email"],textarea,select{width:100%;padding:10px;background:#000;border:1px solid #333;color:#fff;font-family:monospace}textarea{resize:vertical;min-height:100px}.checkbox{display:flex;align-items:center;gap:8px;font-size:.9em;color:#ccc}.section-title{border-bottom:1px solid #333;padding-bottom:6px;margin-top:8px;margin-bottom:12px;font-size:1em;letter-spacing:1px;text-transform:uppercase}.attachments{border:1px solid #333;padding:12px;margin-bottom:16px}.attachments table{width:100%;border-collapse:collapse;font-size:.85em}.attachments th,.attachments td{border-bottom:1px solid #222;padding:6px;text-align:left}.attachments th{color:#888;text-transform:uppercase;letter-spacing:1px}.file-upload{border:1px dashed #555;padding:20px;text-align:center;margin-bottom:12px;cursor:pointer}.file-upload:hover{background:#111}.alert{display:none;margin-bottom:12px;padding:10px;border:1px solid #333;font-size:.85em}.alert.success{border-color:#fff;color:#fff}.alert.error{border-color:#f00;color:#f00}.list{border:1px solid #333;padding:10px;max-height:180px;overflow-y:auto;font-size:.85em}.list-item{border-bottom:1px solid #222;padding:6px 0;display:flex;justify-content:space-between;align-items:center}.list-item:last-child{border-bottom:none}.badge{display:inline-block;padding:2px 6px;font-size:1.5em;border:1px solid #333;margin-left:6px}.connection-indicator{position:fixed;top:12px;right:12px;border:1px solid #333;padding:6px 10px;font-size:.8em;z-index:20;background:#000;max-width:280px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.connection-indicator.online{border-color:#fff;color:#fff}.connection-indicator.offline{border-color:#f00;color:#f00}.lang-selector{position:fixed;top:12px;left:12px;z-index:21;background:#000;border:1px solid #333;padding:6px;display:flex;gap:4px}.lang-btn{background:transparent;border:1px solid #555;color:#888;padding:4px 10px;font-family:monospace;cursor:pointer;font-size:.75em;letter-spacing:1px;transition:all .2s;min-width:40px}.lang-btn:hover{background:#222;border-color:#fff;color:#fff}.lang-btn.active{border-color:#fff;color:#fff;font-weight:bold}.accordion{border:1px solid #333;margin-bottom:12px}.accordion-header{background:#111;border-bottom:1px solid #333;padding:12px 16px;cursor:pointer;display:flex;justify-content:space-between;align-items:center;text-transform:uppercase;letter-spacing:1px;font-size:.9em;transition:background .2s}.accordion-header:hover{background:#1a1a1a}.accordion-header.active{background:#0a0a0a;color:#fff}.accordion-toggle{font-size:1.2em;transition:transform .3s}.accordion-header.active .accordion-toggle{transform:rotate(180deg);color:#fff}.accordion-content{max-height:0;overflow:hidden;transition:max-height .3s ease;background:#0a0a0a}.accordion-content.active{max-height:2000px;padding:16px;border-top:1px solid #fff}.preset-btn{display:inline-block;padding:8px 16px;margin:4px;border:1px solid #555;background:#111;color:#ccc;cursor:pointer;text-align:center;font-size:.85em;transition:all .2s}.preset-btn:hover{background:#222;border-color:#fff}.preset-btn.active{border-color:#fff;background:#fff;color:#000}.ap-info-box{text-align:center;padding:20px;margin-bottom:20px}.ap-info-row{text-align:center;margin:8px 0;font-size:1.1em}.ap-info-label{display:block;color:#888;font-size:.9em;margin-bottom:4px}.ap-info-value{display:block;color:#fff;font-weight:bold;letter-spacing:1px}.smtp-info-box{text-align:center;padding:20px;margin-bottom:20px}.smtp-info-row{display:flex;justify-content:center;align-items:center;gap:12px;margin:8px 0;font-size:1em}.smtp-info-label{color:#888;font-size:.85em;min-width:120px;text-align:right}.smtp-info-value{color:#fff;letter-spacing:1px}.toggle-switch{position:relative;display:inline-block;width:60px;height:30px}.toggle-switch input{opacity:0;width:0;height:0}.toggle-slider{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background:#333;border:2px solid #555;transition:.3s;border-radius:30px}.toggle-slider:before{position:absolute;content:"";height:20px;width:20px;left:3px;bottom:3px;background:#666;transition:.3s;border-radius:50%}input:checked+.toggle-slider{background:#000;border-color:#fff}input:checked+.toggle-slider:before{transform:translateX(30px);background:#fff}.toggle-container{display:flex;justify-content:center;align-items:center;gap:12px;margin:24px 0}.toggle-label{font-size:1em;letter-spacing:1px;text-transform:uppercase;color:#ccc;transition:color .3s}.toggle-status{font-size:.85em;letter-spacing:1px;color:#666;min-width:80px;transition:color .3s}input:checked~.toggle-status{color:#fff}input:checked~.toggle-label{color:#fff}@media (max-width:600px){.lang-selector{top:8px;left:8px;font-size:.7em;padding:4px;gap:2px}.lang-btn{padding:2px 6px;min-width:32px;font-size:.65em}.connection-indicator{top:48px;right:8px;left:8px;max-width:none;font-size:.7em;padding:4px 8px}.tabs{display:grid;grid-template-columns:1fr 1fr;gap:4px}.tab{min-width:0;margin:0!important;font-size:.8em;padding:12px 8px}.tab.active{background:#fff;color:#000;font-weight:bold}.button-bar{flex-wrap:nowrap!important;gap:4px!important}.button-bar>div{gap:4px!important;min-width:0}.button-bar button{padding:8px 10px!important;font-size:.7em!important;min-width:0;letter-spacing:0}.ap-info-row{font-size:1em}.ap-info-label,.smtp-info-label{text-align:center;min-width:auto}.smtp-info-row{flex-direction:column;align-items:stretch;gap:4px}.smtp-info-label{text-align:left;min-width:auto;font-size:.75em;margin-bottom:2px}.toggle-container{flex-direction:column;gap:8px;align-items:stretch;margin:16px 0}.toggle-label{font-size:.85em;text-align:center;order:1}.toggle-switch{order:2;align-self:center}.toggle-status{order:3;text-align:center;min-width:auto}}</style>
</head>
<body>
    <div id="mainApp" style="display:block;">
    <div class="container">
        <div class="header">
            <h1 data-i18n="header.title">SMARTKRAFT LEBENSSPUR</h1>
            <div class="device-id">
                <span id="deviceId">-</span> / <span id="firmwareVersion">-</span>
            </div>
        </div>

        <!-- Dil Seçimi ve Durum Bilgileri -->
        <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:16px; gap:12px;">
            <div class="lang-selector" style="position:static; border:1px solid #333; padding:6px; display:flex; gap:4px;">
                <button class="lang-btn active" data-lang="en">EN</button>
                <button class="lang-btn" data-lang="de">DE</button>
                <button class="lang-btn" data-lang="tr">TR</button>
            </div>
            <div id="connectionStatus" class="connection-indicator offline" style="position:static; flex:1; text-align:right;" data-i18n="status.connecting">Checking connection...</div>
        </div>

        <div class="status-grid">
            <div class="status-card">
                <div class="status-label" data-i18n="status.timerStatus">Timer Status</div>
                <div class="status-value" id="timerStatus">-</div>
            </div>
            <div class="status-card">
                <div class="status-label" data-i18n="status.remainingTime">Remaining Time</div>
                <div class="status-value" id="remainingTime">-</div>
            </div>
            <div class="status-card">
                <div class="status-label" data-i18n="status.nextAlarm">Next Alarm</div>
                <div class="status-value" id="nextAlarm">-</div>
            </div>
            <div class="status-card">
                <div class="status-label">Wi-Fi</div>
                <div class="status-value" id="wifiStatus">-</div>
            </div>
        </div>

        <div class="timer-readout">
            <div class="value" id="timerDisplay">00:00:00</div>
            <div class="label" data-i18n="status.countdown">Countdown</div>
            
            <div class="button-bar" style="justify-content:space-between; gap:6px; margin-top:16px; margin-bottom:0;">
                <div style="display:flex; gap:6px; flex:1;">
                    <button id="btnStart" style="border:1px solid #fff; color:#fff; background:#000; flex-shrink:0;" onclick="startTimer()" data-i18n="buttons.start">Start</button>
                    <button id="btnPause" style="display:none; border:1px solid #fff; color:#fff; background:#000; flex-shrink:0;" onclick="pauseTimer()" data-i18n="buttons.pause">Pause</button>
                    <button id="btnResume" style="display:none; border:1px solid #fff; color:#fff; background:#000; flex-shrink:0;" onclick="resumeTimer()" data-i18n="buttons.resume">Resume</button>
                    <button id="btnReset" style="border:1px solid #fff; color:#fff; background:#000; flex-shrink:0;" onclick="resetTimer()" data-i18n="buttons.reset">Reset</button>
                </div>
                <button id="btnPhysical" style="border:1px solid #fff; color:#fff; background:#000; flex-shrink:0; white-space:nowrap;" onclick="virtualButton()" data-i18n="buttons.virtualButton">Virtual Button</button>
            </div>
        </div>

        <div class="tabs">
            <div class="tab active" data-tab="alarmTab" data-i18n="tabs.alarm">Alarm Settings</div>
            <div class="tab" data-tab="mailTab" data-i18n="tabs.mail">Mail Settings</div>
            <div class="tab" data-tab="wifiTab" data-i18n="tabs.wifi">Connection Settings</div>
            <div class="tab" data-tab="infoTab" data-i18n="tabs.info">Info</div>
        </div>

        <div class="tab-content">
            <div id="alarmTab" class="tab-pane active">
                <div id="alarmAlert" class="alert"></div>
                <div class="section-title" data-i18n="alarm.sectionCountdown">Countdown Parameters</div>
                <div class="form-grid">
                    <div class="form-group">
                        <label data-i18n="alarm.unitLabel">Time Unit</label>
                        <select id="timerUnit">
                            <option value="minutes" data-i18n="alarm.unitMinutes">Minutes</option>
                            <option value="hours" data-i18n="alarm.unitHours">Hours</option>
                            <option value="days" data-i18n="alarm.unitDays">Days</option>
                        </select>
                    </div>
                    <div class="form-group">
                        <label data-i18n="alarm.totalLabel">Total Duration (1-60)</label>
                        <input type="number" id="timerTotal" min="1" max="60" value="7">
                    </div>
                    <div class="form-group">
                        <label data-i18n="alarm.alarmsLabel">Number of Alarms (0-10)</label>
                        <input type="number" id="timerAlarms" min="0" max="10" value="3">
                    </div>
                </div>
                
                <div style="display:flex; justify-content:center; margin-top:16px;">
                    <button onclick="saveTimerSettings()" data-i18n="buttons.save" style="width:50%; min-width:200px;">Save</button>
                </div>

                <div class="section-title" data-i18n="alarm.sectionAlarms">Alarm Schedule</div>
                <div class="list" id="alarmSchedule">-</div>
            </div>

            <div id="mailTab" class="tab-pane">
                <div id="mailAlert" class="alert"></div>

                <!-- MAİL ENTEGRASYONU -->
                <div class="accordion">
                    <div class="accordion-header" onclick="toggleAccordion(this)">
                        <span data-i18n="mail.sectionSMTP">SMTP Settings</span>
                        <span class="accordion-toggle">v</span>
                    </div>
                    <div class="accordion-content">
                        <!-- SMTP Bilgileri -->
                        <div class="smtp-info-box">
                            <div class="smtp-info-row">
                                <span class="smtp-info-label" data-i18n="mail.server">SMTP Server</span>
                                <input type="text" id="smtpServer" data-i18n="mail.serverPlaceholder" placeholder="smtp.gmail.com" style="flex:1;max-width:400px;padding:8px;background:#000;border:1px solid #333;color:#fff;font-family:monospace;">
                            </div>
                            <div class="smtp-info-row">
                                <span class="smtp-info-label" data-i18n="mail.port">Port</span>
                                <input type="number" id="smtpPort" value="465" style="flex:1;max-width:400px;padding:8px;background:#000;border:1px solid #333;color:#fff;font-family:monospace;">
                            </div>
                            <div class="smtp-info-row">
                                <span class="smtp-info-label" data-i18n="mail.username">Username</span>
                                <input type="email" id="smtpUsername" data-i18n="mail.usernamePlaceholder" placeholder="user@example.com" style="flex:1;max-width:400px;padding:8px;background:#000;border:1px solid #333;color:#fff;font-family:monospace;">
                            </div>
                            <div class="smtp-info-row">
                                <span class="smtp-info-label" data-i18n="mail.password">Password</span>
                                <input type="password" id="smtpPassword" data-i18n="mail.passwordPlaceholder" placeholder="App Password" style="flex:1;max-width:400px;padding:8px;background:#000;border:1px solid #333;color:#fff;font-family:monospace;">
                            </div>
                        </div>
                        
                        <!-- SMTP Kullanım Açıklaması -->
                        <div style="color:#888;font-size:.9em;line-height:1.6;margin-top:20px;padding:16px;border:1px solid #333;border-radius:4px;" data-i18n="mail.smtpInfoText">
                            1. Enter your email provider's SMTP server (e.g., smtp.gmail.com, smtp-mail.outlook.com)<br>2. Use port <strong>465</strong> (SSL/TLS)<br>3. Enter your email address as username<br>4. Generate an app-specific password from your email provider (not your regular password)
                        </div>
                    </div>
                </div>

                <!-- ERKEN UYARI SİSTEMİ -->
                <div class="accordion">
                    <div class="accordion-header" onclick="toggleAccordion(this)">
                        <span data-i18n="mail.sectionWarning">Early Warning Message</span>
                        <span class="accordion-toggle">v</span>
                    </div>
                    <div class="accordion-content">
                        <div class="form-group">
                            <label data-i18n="mail.warningSubject">Subject</label>
                            <input type="text" id="warningSubject" data-i18n="mail.warningSubjectPlaceholder" placeholder="Early Warning from SmartKraft LebensSpur">
                        </div>
                        <div class="form-group">
                            <label data-i18n="mail.warningBody">Message Body</label>
                            <textarea id="warningBody" data-i18n="mail.warningBodyPlaceholder" placeholder="Warning message content...">Device: {DEVICE_ID}
Time: {TIMESTAMP}
Remaining: {REMAINING}

This is a SmartKraft LebensSpur early warning message.</textarea>
                        </div>
                        <div style="font-size:0.7em; color:#666; margin-bottom:12px;">
                            <span data-i18n="mail.placeholders">Use {DEVICE_ID}, {TIMESTAMP}, {REMAINING}, %ALARM_INDEX%, %TOTAL_ALARMS%, %REMAINING%</span>
                        </div>
                        <div class="form-group">
                            <label data-i18n="mail.warningUrl">Trigger URL (GET)</label>
                            <input type="text" id="warningUrl" data-i18n="mail.warningUrlPlaceholder" placeholder="https://example.com/api/warning">
                        </div>
                        <button id="btnTestWarning" class="btn-warning" style="width:100%;" data-i18n="mail.testWarning">Test Warning Mail</button>
                    </div>
                </div>

                <!-- LEBENSSPUR PROTOKOLÜ - MAIL GROUPS -->
                <div class="accordion">
                    <div class="accordion-header" onclick="toggleAccordion(this)">
                        <span data-i18n="mail.sectionFinalGroups">Final Message Groups (LebensSpur Protocol)</span>
                        <span class="accordion-toggle">v</span>
                    </div>
                    <div class="accordion-content">
                        <div style="font-size:0.85em; color:#888; margin-bottom:16px; line-height:1.5;">
                            <span data-i18n="mail.groupsHelp">Create up to 3 mail groups. Each group has its own recipients, message, files and URL trigger. Click on a group to edit.</span>
                        </div>
                        
                        <!-- Mail Grup Listesi -->
                        <div id="mailGroupsList" style="border:1px solid #333; background:#0a0a0a;">
                            <!-- Gruplar dinamik olarak buraya yüklenecek -->
                        </div>
                        
                        <!-- Yeni Grup Ekle Butonu -->
                        <button onclick="addMailGroup()" style="width:100%; margin-top:12px; border:1px dashed #555;" data-i18n="mail.addGroup">
                            + Add New Mail Group
                        </button>
                    </div>
                </div>

                <div style="margin-top:20px;">
                    <button id="btnSaveMail" style="width:100%;" data-i18n="buttons.save">Save</button>
                </div>
            </div>

            <div id="wifiTab" class="tab-pane">
                <div id="wifiAlert" class="alert"></div>
                
                <!-- 🔌 CUSTOM API ENDPOINT SETTINGS -->
                <div class="accordion">
                    <div class="accordion-header" onclick="toggleAccordion(this)">
                        <span data-i18n="api.sectionAPI">Custom API Endpoint</span>
                        <span class="accordion-toggle">▼</span>
                    </div>
                    <div class="accordion-content">
                        <div class="toggle-container" style="margin-bottom:24px;">
                            <span class="toggle-label" data-i18n="api.enabled">ENABLE CUSTOM API ENDPOINT</span>
                            <label class="toggle-switch">
                                <input type="checkbox" id="apiEnabled" onchange="updateToggleStatus(this, 'apiEnabledStatus')">
                                <span class="toggle-slider"></span>
                            </label>
                            <span class="toggle-status" id="apiEnabledStatus" data-i18n="api.inactive">Deaktif</span>
                        </div>
                        
                        <div class="form-group">
                            <label data-i18n="api.endpoint">Custom Endpoint Path</label>
                            <div style="display:flex; gap:8px; align-items:center;">
                                <span style="color:#888; font-family:monospace; white-space:nowrap;">http://[IP]/api/</span>
                                <input type="text" id="apiEndpoint" data-i18n="api.endpointPlaceholder" placeholder="trigger" 
                                       oninput="updateAPIPreview()" style="flex:1;">
                            </div>
                        </div>
                        
                        <div class="form-group">
                            <label data-i18n="api.preview">Preview URL</label>
                            <div id="apiPreview" style="padding:12px; background:#000; border:1px solid #fff; color:#fff; font-family:monospace; font-size:0.85em; word-break:break-all;">
                                http://192.168.1.100/api/trigger
                            </div>
                        </div>
                        
                        <div class="toggle-container" style="margin-bottom:24px;">
                            <span class="toggle-label" data-i18n="api.requireToken">REQUIRE AUTHORIZATION TOKEN</span>
                            <label class="toggle-switch">
                                <input type="checkbox" id="apiRequireToken" onchange="toggleAPIToken(); updateToggleStatus(this, 'apiRequireTokenStatus')">
                                <span class="toggle-slider"></span>
                            </label>
                            <span class="toggle-status" id="apiRequireTokenStatus" data-i18n="api.inactive">Deaktif</span>
                        </div>
                        
                        <div class="form-group" id="apiTokenGroup" style="display:none;">
                            <label data-i18n="api.token">Authorization Token</label>
                            <input type="text" id="apiToken" data-i18n="api.tokenPlaceholder" placeholder="your-secret-token">
                            <div style="font-size:0.7em; color:#666; font-style:italic; margin-top:4px;">
                                <span data-i18n="api.tokenHelp">Include in Authorization header: curl -H "Authorization: your-token" http://IP/api/endpoint</span>
                            </div>
                        </div>
                        
                        <button onclick="saveAPISettings()" data-i18n="buttons.save">Save</button>
                        
                        <div style="margin-top:16px; padding:12px; border:1px solid #333; background:#0a0a0a;">
                            <div style="font-size:0.85em; color:#fff; margin-bottom:8px; font-weight:bold;" data-i18n="api.examples">Usage Examples:</div>
                            <div style="font-size:0.75em; color:#888; font-family:monospace; line-height:1.6;">
                                <div style="margin-bottom:8px;">
                                    <div style="color:#ccc;" data-i18n="api.exampleCurl">cURL:</div>
                                    <code id="apiExampleCurl" style="color:#fff;">curl -X POST http://192.168.1.100/api/trigger</code>
                                </div>
                                <div style="margin-bottom:8px;">
                                    <div style="color:#ccc;" data-i18n="api.exampleHA">Home Assistant:</div>
                                    <code id="apiExampleHA" style="color:#fff;">rest_command:<br>  &nbsp;trigger_ls:<br>  &nbsp;&nbsp;url: "http://192.168.1.100/api/trigger"<br>  &nbsp;&nbsp;method: POST</code>
                                </div>
                                <div>
                                    <div style="color:#ccc;" data-i18n="api.exampleNode">Node-RED:</div>
                                    <code id="apiExampleNode" style="color:#fff;">[http request] → POST → http://192.168.1.100/api/trigger</code>
                                </div>
                            </div>
                        </div>
                    </div>
                </div>
                
                <!-- 1️⃣ ACCESS POINT (AP) ACCORDION -->
                <div class="accordion">
                    <div class="accordion-header" onclick="toggleAccordion(this)">
                        <span data-i18n="wifi.sectionAP">Access Point Settings</span>
                        <span class="accordion-toggle">▼</span>
                    </div>
                    <div class="accordion-content">
                        <!-- AP Bilgileri -->
                        <div class="ap-info-box">
                            <div class="ap-info-row">
                                <span class="ap-info-label" data-i18n="wifi.apNetworkName">Ağ Adı</span>
                                <span class="ap-info-value">LS-<span id="apChipId">XXXXXXXXXXXX</span></span>
                            </div>
                            <div class="ap-info-row">
                                <span class="ap-info-label" data-i18n="wifi.apPassword">Şifre</span>
                                <span class="ap-info-value">smartkraft123</span>
                            </div>
                            <div class="ap-info-row">
                                <span class="ap-info-label" data-i18n="wifi.apIPAddress">IP Adresi</span>
                                <span class="ap-info-value">192.168.4.1</span>
                            </div>
                            <div class="ap-info-row">
                                <span class="ap-info-label" data-i18n="wifi.apMDNS">mDNS</span>
                                <span class="ap-info-value">ls-<span id="apMdnsChipId">xxxxxxxxxxxx</span>.local</span>
                            </div>
                        </div>
                        
                        <!-- Toggle Switch -->
                        <div class="toggle-container">
                            <span class="toggle-label" data-i18n="wifi.apModeToggleLabel">ERİŞİM NOKTASI (AP) MODU</span>
                            <label class="toggle-switch">
                                <input type="checkbox" id="apModeEnabled" checked onchange="updateToggleStatus(this, 'apModeStatus')">
                                <span class="toggle-slider"></span>
                            </label>
                            <span class="toggle-status" id="apModeStatus" data-i18n="wifi.apModeActive">Aktif</span>
                        </div>
                        
                        <!-- AP Mode Açıklama -->
                        <div style="color:#888;font-size:.9em;line-height:1.6;margin-top:20px;padding:16px;border:1px solid #333;border-radius:4px;" data-i18n="wifi.apModeDescription">
                            Yapılandırma erişimi için bir WiFi ağı oluşturur. İlk kurulumda varsayılan olarak açıktır. Bağımsız (sadece AP) veya ana ağınızla eşzamanlı (Dual Mode: AP+STA) çalışabilir. Ana WiFi'nize bağlandığında her iki ağ da esnek erişim için aktif kalır. Sadece ana ağ bağlantısını kullanmak istiyorsanız AP modunu kapatabilirsiniz.
                        </div>
                    </div>
                </div>
                
                <!-- 2️⃣ PRIMARY WIFI (SSID1) ACCORDION -->
                <div class="accordion" style="margin-top:16px;">
                    <div class="accordion-header" onclick="toggleAccordion(this)">
                        <span data-i18n="wifi.sectionSTA">Primary WiFi Network (SSID 1)</span>
                        <span class="accordion-toggle">▼</span>
                    </div>
                    <div class="accordion-content">
                        <div class="form-grid">
                            <div class="form-group">
                                <label data-i18n="wifi.staSSID">Target Network (SSID)</label>
                                <input type="text" id="wifiPrimarySsid" data-i18n="wifi.staSSIDPlaceholder" placeholder="Your WiFi network name">
                            </div>
                            <div class="form-group">
                                <label data-i18n="wifi.staPassword">Password</label>
                                <input type="password" id="wifiPrimaryPassword" data-i18n="wifi.staPasswordPlaceholder" placeholder="Network password">
                            </div>
                            <div class="form-group">
                                <label data-i18n="wifi.staDHCP">Statik IP</label>
                                <div style="display:flex; align-items:center; gap:12px; height:42px;">
                                    <label class="toggle-switch">
                                        <input type="checkbox" id="primaryStaticEnabled" onchange="updateToggleStatus(this, 'primaryStaticStatus')">
                                        <span class="toggle-slider"></span>
                                    </label>
                                    <span class="toggle-status" id="primaryStaticStatus" data-i18n="wifi.inactive">Deaktif</span>
                                </div>
                            </div>
                        </div>
                        <div class="form-grid">
                            <div class="form-group">
                                <label data-i18n="wifi.staIP">IP Address</label>
                                <input type="text" id="primaryIP" placeholder="192.168.1.100">
                            </div>
                            <div class="form-group">
                                <label data-i18n="wifi.staGateway">Gateway</label>
                                <input type="text" id="primaryGateway" placeholder="192.168.1.1">
                            </div>
                            <div class="form-group">
                                <label data-i18n="wifi.staSubnet">Subnet Mask</label>
                                <input type="text" id="primarySubnet" placeholder="255.255.255.0">
                            </div>
                            <div class="form-group">
                                <label data-i18n="wifi.staDNS">Primary DNS</label>
                                <input type="text" id="primaryDNS" placeholder="192.168.1.1">
                            </div>
                            <div class="form-group">
                                <label data-i18n="wifi.mdnsHostname">mDNS Hostname (.local)</label>
                                <input type="text" id="primaryMDNS" placeholder="ls" maxlength="32" pattern="[a-zA-Z0-9-]*">
                                <small style="color:#888;font-size:0.85em;display:block;margin-top:4px;" data-i18n="wifi.mdnsHelp">Sadece hostname girin (.local yazmayın). Örn: "ls" → "ls.local"</small>
                            </div>
                        </div>
                    </div>
                </div>
                
                <!-- 3️⃣ BACKUP WIFI (SSID2) ACCORDION -->
                <div class="accordion" style="margin-top:16px;">
                    <div class="accordion-header" onclick="toggleAccordion(this)">
                        <span>Backup WiFi Network (SSID 2)</span>
                        <span class="accordion-toggle">▼</span>
                    </div>
                    <div class="accordion-content">
                        <div class="form-grid">
                            <div class="form-group">
                                <label data-i18n="wifi.staSSID">Target Network (SSID)</label>
                                <input type="text" id="wifiSecondarySsid" data-i18n="wifi.staSSIDPlaceholder" placeholder="Your WiFi network name">
                            </div>
                            <div class="form-group">
                                <label data-i18n="wifi.staPassword">Password</label>
                                <input type="password" id="wifiSecondaryPassword" data-i18n="wifi.staPasswordPlaceholder" placeholder="Network password">
                            </div>
                            <div class="form-group">
                                <label data-i18n="wifi.staDHCP">Statik IP</label>
                                <div style="display:flex; align-items:center; gap:12px; height:42px;">
                                    <label class="toggle-switch">
                                        <input type="checkbox" id="secondaryStaticEnabled" onchange="updateToggleStatus(this, 'secondaryStaticStatus')">
                                        <span class="toggle-slider"></span>
                                    </label>
                                    <span class="toggle-status" id="secondaryStaticStatus" data-i18n="wifi.inactive">Deaktif</span>
                                </div>
                            </div>
                        </div>
                        <div class="form-grid">
                            <div class="form-group">
                                <label data-i18n="wifi.staIP">IP Address</label>
                                <input type="text" id="secondaryIP" placeholder="192.168.1.101">
                            </div>
                            <div class="form-group">
                                <label data-i18n="wifi.staGateway">Gateway</label>
                                <input type="text" id="secondaryGateway" placeholder="192.168.1.1">
                            </div>
                            <div class="form-group">
                                <label data-i18n="wifi.staSubnet">Subnet Mask</label>
                                <input type="text" id="secondarySubnet" placeholder="255.255.255.0">
                            </div>
                            <div class="form-group">
                                <label data-i18n="wifi.staDNS">Primary DNS</label>
                                <input type="text" id="secondaryDNS" placeholder="192.168.1.1">
                            </div>
                            <div class="form-group">
                                <label data-i18n="wifi.mdnsHostname">mDNS Hostname (.local)</label>
                                <input type="text" id="secondaryMDNS" placeholder="ls" maxlength="32" pattern="[a-zA-Z0-9-]*">
                                <small style="color:#888;font-size:0.85em;display:block;margin-top:4px;" data-i18n="wifi.mdnsHelp">Sadece hostname girin (.local yazmayın). Örn: "ls" → "ls.local"</small>
                            </div>
                        </div>
                    </div>
                </div>
                
                <!-- 4️⃣ ACİL DURUM BAĞLANTI (Normal - Accordion DEĞİL) -->
                <div style="border-top:1px solid #333; margin:28px 0 0; padding-top:24px;">
                    <div style="display:flex; align-items:center; justify-content:center; gap:12px; margin-bottom:16px;">
                        <div style="width:28px; height:28px; border:2px solid #ffa500; border-radius:4px; display:flex; align-items:center; justify-content:center; font-weight:900; font-size:18px; color:#ffa500; flex-shrink:0;">!</div>
                        <div style="font-size:1.1em; font-weight:700; letter-spacing:1px; color:#fff;" data-i18n="wifi.emergencyTitle">EMERGENCY INTERNET CONNECTION</div>
                    </div>
                    <div class="toggle-container" style="margin:0 0 18px 40px;">
                        <span class="toggle-label" data-i18n="wifi.emergencyCheckbox">ALLOW UNSECURED NETWORKS</span>
                        <label class="toggle-switch">
                            <input type="checkbox" id="wifiAllowOpen" onchange="updateToggleStatus(this, 'wifiAllowOpenStatus')">
                            <span class="toggle-slider"></span>
                        </label>
                        <span class="toggle-status" id="wifiAllowOpenStatus" data-i18n="wifi.inactive">Deaktif</span>
                    </div>
                    <div style="border:1px solid #444; padding:18px; background:#0a0a0a; font-size:0.85em; line-height:1.7; color:#bbb; margin-left:40px;">
                        <div style="margin-bottom:14px;">
                            <strong style="color:#fff; font-size:0.95em;" data-i18n="wifi.emergencyWhen">When does it work?</strong>
                            <ul style="margin:8px 0 0 20px; padding:0;">
                                <li style="margin:4px 0;" data-i18n="wifi.emergencyWhen1">AP mode is off</li>
                                <li style="margin:4px 0;" data-i18n="wifi.emergencyWhen2">Primary and backup networks cannot be connected (or there is no internet access)</li>
                            </ul>
                        </div>
                        <div style="margin-bottom:14px;">
                            <strong style="color:#fff; font-size:0.95em;" data-i18n="wifi.emergencyHow">How does it work?</strong>
                            <div style="margin-top:8px; white-space:pre-line;" data-i18n="wifi.emergencyHowText">The device scans nearby unsecured (open) WiFi networks and temporarily connects to send emails by checking internet access. If WiFi is connected but there is no internet, it automatically switches to another network.</div>
                        </div>
                        <div style="margin-bottom:10px;">
                            <span style="color:#ff4444; font-weight:700;" data-i18n="wifi.emergencyProtocol">LebensSpur Data Protocol:</span>
                            <span style="margin-left:6px;" data-i18n="wifi.emergencyProtocolText">Email is NEVER lost! It tries indefinitely until internet access is available.</span>
                        </div>
                        <div style="font-style:italic; font-size:0.9em; color:#888; border-top:1px solid #222; padding-top:12px; margin-top:12px; white-space:pre-line;" data-i18n="wifi.emergencyNote">Note: Since it poses a security risk, it is recommended to use only in critical situations. Mail connection is TLS/SSL encrypted.</div>
                    </div>
                </div>
                <div class="button-bar" style="justify-content:center; margin-top:30px;">
                    <button onclick="saveWiFiSettings()" data-i18n="buttons.save">Save</button>
                    <button class="btn-warning" onclick="scanNetworks()" data-i18n="buttons.scan">Scan</button>
                    <button class="btn-danger" onclick="factoryReset()" data-i18n="buttons.factoryReset">Factory Reset</button>
                    <button onclick="rebootDevice()" data-i18n="buttons.reboot">Reboot</button>
                </div>
                <div class="section-title" style="margin-top:34px;">Bulunan Ağlar</div>
                <div class="list" id="wifiScanResults">-</div>
            </div>

            <div id="infoTab" class="tab-pane">
                <div class="section-title" style="margin-top:0;" data-i18n="info.title">SmartKraft LebensSpur User Guide</div>
                <div style="font-size:0.9em; line-height:1.6; color:#ccc; margin-bottom:20px;" data-i18n="info.description">
                    SmartKraft LebensSpur (Life Trace) is an intelligent countdown timer with automatic email delivery, emergency WiFi fallback, and relay control for critical timing scenarios.
                </div>

                <div class="section-title" data-i18n="info.quickStart">Quick Start</div>
                <div style="font-size:0.85em; line-height:1.6; color:#ccc; margin-bottom:20px;">
                    <div style="margin-bottom:12px;">
                        <strong data-i18n="info.step1Title">1. Set Timer Duration</strong><br>
                        <span data-i18n="info.step1Text" style="color:#999;">Go to Alarm Settings → Choose time unit (minutes/hours/days) → Set total duration (1-60) → Set number of alarms (0-10) → Save</span>
                    </div>
                    <div style="margin-bottom:12px;">
                        <strong data-i18n="info.step2Title">2. Configure Email</strong><br>
                        <span data-i18n="info.step2Text" style="color:#999;">Go to Mail Settings → Enter SMTP server (ProtonMail or Gmail) → Add recipients → Customize warning/final message → Upload attachments (optional) → Test → Save</span>
                    </div>
                    <div style="margin-bottom:12px;">
                        <strong data-i18n="info.step3Title">3. Setup WiFi</strong><br>
                        <span data-i18n="info.step3Text" style="color:#999;">Go to Connection Settings → Configure Access Point mode → Add primary WiFi network → Add backup WiFi (optional) → Enable emergency open networks (optional) → Save</span>
                    </div>
                    <div style="margin-bottom:12px;">
                        <strong data-i18n="info.step4Title">4. Start Timer</strong><br>
                        <span data-i18n="info.step4Text" style="color:#999;">Click Start button → Timer begins countdown → Alarms trigger at scheduled intervals → Final relay triggers when time expires</span>
                    </div>
                </div>

                <div class="section-title" data-i18n="info.featuresTitle">Key Features</div>
                <div style="font-size:0.85em; line-height:1.6; color:#ccc; margin-bottom:20px;">
                    <div style="margin-bottom:8px;">
                        <strong data-i18n="info.feature1">→ Virtual Button:</strong> <span data-i18n="info.feature1Text" style="color:#999;">Reset timer remotely via web interface or custom API endpoint</span>
                    </div>
                    <div style="margin-bottom:8px;">
                        <strong data-i18n="info.feature2">→ Virtual Button API:</strong> <span data-i18n="info.feature2Text" style="color:#999;">Create custom HTTP endpoint for home automation (Home Assistant, Node-RED)</span>
                    </div>
                    <div style="margin-bottom:8px;">
                        <strong data-i18n="info.feature3">→ Emergency WiFi:</strong> <span data-i18n="info.feature3Text" style="color:#999;">Automatically connects to open networks if primary/backup WiFi fails</span>
                    </div>
                    <div style="margin-bottom:8px;">
                        <strong data-i18n="info.feature4">→ Email Attachments:</strong> <span data-i18n="info.feature4Text" style="color:#999;">Total storage: 900KB for all mail groups combined</span>
                    </div>
                    <div style="margin-bottom:8px;">
                        <strong data-i18n="info.feature5">→ Multi-language:</strong> <span data-i18n="info.feature5Text" style="color:#999;">Interface available in English, German, and Turkish</span>
                    </div>
                    <div style="margin-bottom:8px;">
                        <strong data-i18n="info.feature6">→ mDNS Hostname:</strong> <span data-i18n="info.feature6Text" style="color:#999;">Custom device name for each WiFi network (easy .local access)</span>
                    </div>
                </div>

                <div class="section-title" data-i18n="info.apiTitle">Virtual Button API Setup</div>
                <div style="font-size:0.85em; line-height:1.6; color:#ccc; margin-bottom:20px;">
                    <span data-i18n="info.apiText1">Go to Connection Settings → Virtual Button API Endpoint → Enable → Set endpoint name (e.g., "trigger") → Optional: Enable token authentication → Save</span><br><br>
                    <span data-i18n="info.apiText2">Example usage:</span>
                    <div style="background:#0a0a0a; border:1px solid #333; padding:8px; margin-top:8px; font-family:monospace; font-size:0.8em; color:#fff;">
                        curl -X POST http://192.168.1.100/api/trigger
                    </div>
                </div>

                <div class="section-title" data-i18n="info.securityTitle">Security & Privacy</div>
                <div style="font-size:0.85em; line-height:1.6; color:#ccc; margin-bottom:20px;">
                    <div style="margin-bottom:6px;" data-i18n="info.security1">• All data stored locally on device (no cloud)</div>
                    <div style="margin-bottom:6px;" data-i18n="info.security2">• Email connections encrypted with TLS/SSL</div>
                    <div style="margin-bottom:6px;" data-i18n="info.security3">• Optional token authentication for API</div>
                    <div style="margin-bottom:6px;" data-i18n="info.security4">• Factory reset deletes all settings permanently</div>
                </div>

                <div class="section-title" data-i18n="info.technicalTitle">Technical Specifications</div>
                <div style="font-size:0.85em; line-height:1.6; color:#ccc; margin-bottom:20px;">
                    <div style="margin-bottom:6px;" data-i18n="info.tech1">• Processor: ESP32-C6 (RISC-V, WiFi 6)</div>
                    <div style="margin-bottom:6px;" data-i18n="info.tech2">• Storage: LittleFS filesystem</div>
                    <div style="margin-bottom:6px;" data-i18n="info.tech3">• WiFi: Dual mode (AP + STA)</div>
                    <div style="margin-bottom:6px;" data-i18n="info.tech4">• Power: USB-C 5V DC or 230V AC</div>
                    <div style="margin-bottom:6px;" data-i18n="info.tech5">• Output: URL API trigger and onboard relay pins (max 5V / 30mA) with physical button support</div>
                </div>

                <div style="border-top:1px solid #222; margin:28px 0 24px 0;"></div>

                <div style="display:flex; justify-content:center;">
                    <div style="display:inline-flex; align-items:center; gap:16px; padding:12px 20px; background:linear-gradient(180deg, #0d0d0d 0%, #080808 100%); border:1px solid #2a2a2a; border-radius:8px; box-shadow:0 2px 8px rgba(0,0,0,0.3);">
                        <div style="display:flex; align-items:center; gap:8px;">
                            <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="#666" stroke-width="2"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/></svg>
                            <span id="currentFirmwareVersion" style="font-size:0.85em; color:#fff; font-weight:600; letter-spacing:0.5px;">-</span>
                        </div>
                        <div style="width:1px; height:20px; background:#333;"></div>
                        <button id="otaCheckBtn" onclick="checkOTAUpdate()" style="padding:6px 14px; background:linear-gradient(180deg, #1a1a1a 0%, #111 100%); color:#aaa; border:1px solid #333; cursor:pointer; font-size:0.75em; font-weight:500; transition:all 0.2s; border-radius:4px; text-transform:uppercase; letter-spacing:0.5px;" onmouseover="this.style.background='linear-gradient(180deg, #fff 0%, #ddd 100%)';this.style.color='#000';this.style.borderColor='#fff'" onmouseout="this.style.background='linear-gradient(180deg, #1a1a1a 0%, #111 100%)';this.style.color='#aaa';this.style.borderColor='#333'">
                            <span data-i18n="info.otaButton">Check Updates</span>
                        </button>
                    </div>
                </div>
                <div id="otaStatus" style="text-align:center; margin-top:10px; font-size:0.75em; color:#666; min-height:14px;"></div>

                <div style="border-top:1px solid #333; padding-top:20px; margin-top:24px; text-align:center;">
                    <div style="margin-bottom:8px; font-size:0.9em;" data-i18n="info.supportTitle">Support and Documentation</div>
                    <div style="margin-bottom:12px; font-size:0.85em; color:#888;" data-i18n="info.supportText">For detailed user manual, example scenarios and updates:</div>
                    <div style="display:flex; justify-content:center; gap:12px; flex-wrap:wrap;">
                        <a href="https://smartkraft.ch/lebensspur" target="_blank" rel="noopener noreferrer" style="display:inline-block; padding:8px 20px; background:#fff; color:#000; border:1px solid #fff; border-radius:4px; text-decoration:none; font-weight:500; font-size:0.9em; transition:all 0.3s;">
                            SmartKraft.ch/LebensSpur
                        </a>
                        <a href="https://github.com/smrtkrft/LebensSpur_protocol" target="_blank" rel="noopener noreferrer" style="display:inline-block; padding:8px 20px; background:#fff; color:#000; border:1px solid #fff; border-radius:4px; text-decoration:none; font-weight:500; font-size:0.9em; transition:all 0.3s;">
                            GitHub-LebensSpur
                        </a>
                    </div>
                </div>

                <div style="border-top:1px solid #333; padding-top:16px; margin-top:30px; text-align:center; font-size:0.75em; color:#666;">
                    <div>SmartKraft LebensSpur <span id="footerVersion">v1.0.4</span> • Open Source Hardware/Software</div>
                    <div style="margin-top:4px;">© 2025 SmartKraft Systems</div>
                </div>
            </div>
        </div>
    </div>

    <script>
        // i18n System
        let i18nData = {};
        let currentLang = localStorage.getItem('lang') || 'en';

        async function loadLanguage(lang) {
            try {
                const response = await fetch(`/api/i18n?lang=${lang}`);
                i18nData = await response.json();
                currentLang = lang;
                localStorage.setItem('lang', lang);
                document.getElementById('htmlRoot').setAttribute('lang', lang);
                applyTranslations();
                updateLangButtons();
            } catch (error) {
                console.error('Failed to load language:', error);
            }
        }

        function applyTranslations() {
            document.querySelectorAll('[data-i18n]').forEach(el => {
                const key = el.getAttribute('data-i18n');
                const translation = getTranslation(key);
                if (translation) {
                    if (el.tagName === 'INPUT' && el.type !== 'checkbox' && el.type !== 'radio') {
                        el.placeholder = translation;
                    } else {
                        // white-space:pre-line olan elementlerde \n korunur
                        // Diğerlerinde \n → <br> dönüşümü
                        const style = window.getComputedStyle(el);
                        if (style.whiteSpace === 'pre-line' || style.whiteSpace === 'pre-wrap') {
                            el.textContent = translation;
                        } else {
                            el.innerHTML = translation.replace(/\n/g, '<br>');
                        }
                    }
                }
            });
            
            // Update dynamic content
            updateStatusDisplay();
        }

        function getTranslation(key) {
            const keys = key.split('.');
            let value = i18nData;
            for (const k of keys) {
                if (value && typeof value === 'object') {
                    value = value[k];
                } else {
                    return null;
                }
            }
            return value;
        }

        function switchLanguage(lang) {
            loadLanguage(lang);
            
            // Tüm toggle durumlarını güncelle
            const toggles = [
                { checkboxId: 'apModeEnabled', statusId: 'apModeStatus' },
                { checkboxId: 'apiEnabled', statusId: 'apiEnabledStatus' },
                { checkboxId: 'apiRequireToken', statusId: 'apiRequireTokenStatus' },
                { checkboxId: 'primaryStaticEnabled', statusId: 'primaryStaticStatus' },
                { checkboxId: 'secondaryStaticEnabled', statusId: 'secondaryStaticStatus' },
                { checkboxId: 'wifiAllowOpen', statusId: 'wifiAllowOpenStatus' },
                { checkboxId: 'modalGroupEnabled', statusId: 'modalGroupEnabledStatus' }
            ];
            
            toggles.forEach(({ checkboxId, statusId }) => {
                const checkbox = document.getElementById(checkboxId);
                if (checkbox) {
                    updateToggleStatus(checkbox, statusId);
                }
            });
        }

        function updateLangButtons() {
            document.querySelectorAll('.lang-btn').forEach(btn => {
                btn.classList.toggle('active', btn.getAttribute('data-lang') === currentLang);
            });
        }

        function t(key) {
            return getTranslation(key) || key;
        }
        
        function updateStatusDisplay() {
            const s = state.status;
            if (!s) return;
            
            const isPaused = s.paused;
            const isRunning = s.timerActive && !s.paused;
            const isStopped = !s.timerActive;
            
            let statusText = t('timerStates.idle');
            if (isPaused) statusText = t('timerStates.paused');
            else if (isRunning) statusText = t('timerStates.running');
            else if (s.timerActive) statusText = t('timerStates.completed');
            
            const timerStatusEl = document.getElementById('timerStatus');
            if (timerStatusEl) timerStatusEl.textContent = statusText;
            
            // NOT: Sıcaklık göstergesi KALDIRILDI
        }

        // Initialize i18n and app on page load
        document.addEventListener('DOMContentLoaded', () => {
            console.log('[INIT] DOM loaded, starting app...');
            // i18n'yi paralel yükle, init()'i bloke etme
            loadLanguage(currentLang);
            init();
        });

        const state = {
            timer: {},
            status: {},
            mail: { attachments: [] },
            wifi: {}
        };

        async function api(path, options = {}) {
            const defaultHeaders = options.headers || {};
            
            if (options.body && !(options.body instanceof FormData)) {
                defaultHeaders['Content-Type'] = 'application/json';
                options.body = JSON.stringify(options.body);
            }
            options.headers = defaultHeaders;
            
            // Timeout ekle (15 saniye)
            const controller = new AbortController();
            const timeoutId = setTimeout(() => controller.abort(), 15000);
            options.signal = controller.signal;
            
            try {
                const response = await fetch(path, options);
                clearTimeout(timeoutId);
                
                if (!response.ok) {
                    const text = await response.text();
                    throw new Error(text || response.statusText);
                }
                if (response.status === 204) return null;
                const text = await response.text();
                try { return JSON.parse(text); } catch { return text; }
            } catch (error) {
                clearTimeout(timeoutId);
                if (error.name === 'AbortError') {
                    throw new Error(t('errors.timeout'));
                }
                throw error;
            }
        }

        function showAlert(id, message, type = 'success') {
            const el = document.getElementById(id);
            if (!el) return;
            el.textContent = message;
            el.className = `alert ${type}`;
            el.style.display = 'block';
            setTimeout(() => { el.style.display = 'none'; }, 4000);
        }

        function openTab(event, id) {
            console.log('[TAB] Opening:', id);
            const tabs = document.querySelectorAll('.tab');
            const panes = document.querySelectorAll('.tab-pane');
            
            console.log('[TAB] Found', tabs.length, 'tabs and', panes.length, 'panes');
            
            tabs.forEach(tab => tab.classList.remove('active'));
            panes.forEach(pane => pane.classList.remove('active'));
            
            if (event && event.currentTarget) {
                event.currentTarget.classList.add('active');
            }
            
            const targetPane = document.getElementById(id);
            if (targetPane) {
                targetPane.classList.add('active');
                console.log('[TAB] Activated pane:', id);
            } else {
                console.error('[TAB] Pane not found:', id);
            }
        }

        function toggleAccordion(header) {
            header.classList.toggle('active');
            const content = header.nextElementSibling;
            content.classList.toggle('active');
        }

        function updateToggleStatus(checkbox, statusElementId) {
            const statusElement = document.getElementById(statusElementId);
            if (statusElement) {
                const lang = document.getElementById('htmlRoot').getAttribute('lang') || 'en';
                if (checkbox.checked) {
                    if (lang === 'tr') statusElement.textContent = 'Aktif';
                    else if (lang === 'de') statusElement.textContent = 'Aktiv';
                    else statusElement.textContent = 'Active';
                } else {
                    if (lang === 'tr') statusElement.textContent = 'Deaktif';
                    else if (lang === 'de') statusElement.textContent = 'Inaktiv';
                    else statusElement.textContent = 'Inactive';
                }
            }
        }

        async function sendWarningTest() {
            console.log('[MAIL TEST] Warning test başlatıldı...');
            try {
                const result = await api('/api/mail/test', {
                    method: 'POST',
                    body: { testType: 'warning' }
                });
                
                console.log('[MAIL TEST] Warning başarılı:', result);
                showAlert('mailAlert', t('mail.testSuccess'), 'success');
            } catch (e) {
                console.error('[MAIL TEST] Warning exception:', e);
                showAlert('mailAlert', t('mail.testError') + ': ' + e.message, 'error');
            }
        }

        async function sendFinalTest() {
            if (!confirm(t('mail.testFinalConfirm'))) {
                return;
            }
            
            console.log('[MAIL TEST] Final test başlatıldı...');
            try {
                const result = await api('/api/mail/test', {
                    method: 'POST',
                    body: { testType: 'ls' }
                });
                
                console.log('[MAIL TEST] Final başarılı:', result);
                showAlert('mailAlert', t('mail.testSuccess'), 'success');
            } catch (e) {
                console.error('[MAIL TEST] Final exception:', e);
                showAlert('mailAlert', t('mail.testError') + ': ' + e.message, 'error');
            }
        }

        function formatDuration(seconds) {
            const days = Math.floor(seconds / 86400);
            const hours = Math.floor((seconds % 86400) / 3600);
            const minutes = Math.floor((seconds % 3600) / 60);
            const secs = seconds % 60;
            if (days > 0) {
                return `${days}g ${hours.toString().padStart(2,'0')}sa ${minutes.toString().padStart(2,'0')}dk`;
            }
            return `${hours.toString().padStart(2,'0')}:${minutes.toString().padStart(2,'0')}:${secs.toString().padStart(2,'0')}`;
        }

        function updateStatusView() {
            const s = state.status;
            const connection = document.getElementById('connectionStatus');
            const deviceIdEl = document.getElementById('deviceId');
            const firmwareVersionEl = document.getElementById('firmwareVersion');
            const footerVersionEl = document.getElementById('footerVersion');
            const currentFwVersionEl = document.getElementById('currentFirmwareVersion');
            
            if (s.deviceId && deviceIdEl) {
                deviceIdEl.textContent = s.deviceId;
            }
            
            if (s.firmwareVersion && firmwareVersionEl) {
                firmwareVersionEl.textContent = s.firmwareVersion;
            }
            
            // Footer'daki version'u da güncelle
            if (s.firmwareVersion && footerVersionEl) {
                footerVersionEl.textContent = s.firmwareVersion;
            }
            
            // OTA bölümündeki version'u güncelle
            if (s.firmwareVersion && currentFwVersionEl) {
                currentFwVersionEl.textContent = s.firmwareVersion;
            }
            
            if (connection) {
                if (s.wifiConnected) {
                    const flags = [];
                    if (s.apModeEnabled) flags.push('AP');
                    if (s.allowOpenNetworks) flags.push('OPEN-FALLBACK');
                    
                    // mDNS hostname göster (varsa)
                    let mdnsInfo = '';
                    if (s.hostname && s.hostname !== s.ip) {
                        mdnsInfo = ` | ${s.hostname}.local`;
                    }
                    
                    connection.textContent = `Wi-Fi: ${s.ssid || '-'} (${s.ip || '-'})${mdnsInfo} ${flags.length? '['+flags.join(',')+']':''}`;
                    connection.classList.add('online');
                    connection.classList.remove('offline');
                } else {
                    connection.textContent = `Wi-Fi: ${t('status.offline')}`;
                    connection.classList.remove('online');
                    connection.classList.add('offline');
                }
            }

            // Update button visibility based on timer state
            const btnStart = document.getElementById('btnStart');
            const btnPause = document.getElementById('btnPause');
            const btnResume = document.getElementById('btnResume');
            
            if (btnStart && btnPause && btnResume) {
                const isStopped = !s.timerActive;
                const isPaused = s.paused;
                const isRunning = s.timerActive && !s.paused;

                btnStart.style.display = isStopped ? 'inline-block' : 'none';
                btnPause.style.display = isRunning ? 'inline-block' : 'none';
                btnResume.style.display = isPaused ? 'inline-block' : 'none';
            }

            updateStatusDisplay();
            
            const remainingTimeEl = document.getElementById('remainingTime');
            const timerDisplayEl = document.getElementById('timerDisplay');
            const nextAlarmEl = document.getElementById('nextAlarm');
            const wifiStatusEl = document.getElementById('wifiStatus');
            
            if (remainingTimeEl) remainingTimeEl.textContent = formatDuration(s.remainingSeconds || 0);
            if (timerDisplayEl) timerDisplayEl.textContent = formatDuration(s.remainingSeconds || 0);

            if (nextAlarmEl) {
                if (s.alarms && s.alarms.length > s.nextAlarmIndex) {
                    const nextOffset = s.alarms[s.nextAlarmIndex];
                    const total = s.totalSeconds || 0;
                    const elapsed = total - (s.remainingSeconds || 0);
                    const remainingToNext = Math.max(nextOffset - elapsed, 0);
                    nextAlarmEl.textContent = formatDuration(remainingToNext);
                } else {
                    nextAlarmEl.textContent = '-';
                }
            }

            if (wifiStatusEl) {
                const wifiStatus = s.wifiConnected ? `${s.ssid || 'N/A'} (${s.ip || '-'})` : t('status.offline');
                wifiStatusEl.textContent = wifiStatus;
            }

            const scheduleEl = document.getElementById('alarmSchedule');
            if (scheduleEl) {
                if (!s.alarms || s.alarms.length === 0) {
                    scheduleEl.innerHTML = '<span data-i18n="messages.noAlarms">No alarms configured</span>';
                    applyTranslations();
                } else {
                    const totalSeconds = s.totalSeconds || 0;
                    const elapsed = totalSeconds - (s.remainingSeconds || 0);
                    scheduleEl.innerHTML = s.alarms.map((offset, idx) => {
                        const remaining = Math.max(offset - elapsed, 0);
                        return `<div class="list-item">Alarm ${idx + 1}<span class="badge">${formatDuration(remaining)}</span></div>`;
                    }).join('');
                }
            }
        }

        let connectionRetryCount = 0;
        const MAX_RETRIES = 3;

        async function loadStatus() {
            try {
                const controller = new AbortController();
                const timeoutId = setTimeout(() => controller.abort(), 10000); // 10 saniye timeout
                
                const response = await fetch('/api/status', {
                    signal: controller.signal,
                    headers: { 'Cache-Control': 'no-cache' }
                });
                clearTimeout(timeoutId);
                
                if (!response.ok) throw new Error('HTTP ' + response.status);
                
                state.status = await response.json();
                updateStatusView();
                connectionRetryCount = 0; // Başarılı, retry sayacını sıfırla
                
                // Bağlantı başarılı - status göster
                const connection = document.getElementById('connectionStatus');
                connection.classList.remove('offline');
                connection.classList.add('online');
            } catch (err) {
                console.error('Status load error:', err);
                connectionRetryCount++;
                
                // AP modda veya ilk yüklemede hata mesajını gizle
                const connection = document.getElementById('connectionStatus');
                if (connectionRetryCount >= MAX_RETRIES) {
                    // Çok fazla hata - muhtemelen AP modu, status'u gizle
                    connection.style.display = 'none';
                }
            }
        }

        async function loadTimerSettings() {
            try {
                state.timer = await api('/api/timer');
                const unitEl = document.getElementById('timerUnit');
                const totalEl = document.getElementById('timerTotal');
                const alarmsEl = document.getElementById('timerAlarms');
                const enabledEl = document.getElementById('timerEnabled');
                
                if (unitEl) unitEl.value = state.timer.unit;
                if (totalEl) totalEl.value = state.timer.totalValue;
                if (alarmsEl) alarmsEl.value = state.timer.alarmCount;
                if (enabledEl) enabledEl.checked = state.timer.enabled;
            } catch (err) {
                console.error('[TIMER SETTINGS] Load error:', err);
            }
        }

        async function saveTimerSettings() {
            try {
                const unitEl = document.getElementById('timerUnit');
                const totalEl = document.getElementById('timerTotal');
                const alarmsEl = document.getElementById('timerAlarms');
                const enabledEl = document.getElementById('timerEnabled');
                
                if (!unitEl || !totalEl || !alarmsEl) {
                    throw new Error('Required form elements not found');
                }
                
                const payload = {
                    unit: unitEl.value,
                    totalValue: Number(totalEl.value),
                    alarmCount: Number(alarmsEl.value),
                    enabled: enabledEl ? enabledEl.checked : true  // Default true if element missing
                };
                
                await api('/api/timer', { method: 'PUT', body: payload });
                showAlert('alarmAlert', t('alarm.saveSuccess'));
                await loadStatus();
            } catch (err) {
                console.error('[TIMER SETTINGS] Save error:', err);
                showAlert('alarmAlert', t('alarm.saveError') + ': ' + (err.message || ''), 'error');
            }
        }

        async function startTimer() {
            try {
                await api('/api/timer/start', { method: 'POST' });
                await loadStatus();
            } catch (err) {
                console.error('[TIMER] Start error:', err);
                showAlert('alarmAlert', 'Start error: ' + err.message, 'error');
            }
        }
        
        async function pauseTimer() {
            try {
                await api('/api/timer/stop', { method: 'POST' });
                await loadStatus();
            } catch (err) {
                console.error('[TIMER] Pause error:', err);
                showAlert('alarmAlert', 'Pause error: ' + err.message, 'error');
            }
        }
        
        async function resumeTimer() {
            try {
                await api('/api/timer/resume', { method: 'POST' });
                await loadStatus();
            } catch (err) {
                console.error('[TIMER] Resume error:', err);
                showAlert('alarmAlert', 'Resume error: ' + err.message, 'error');
            }
        }
        
        async function resetTimer() {
            try {
                await api('/api/timer/reset', { method: 'POST' });
                latchRelayDisplay(false);
                await loadStatus();
            } catch (err) {
                console.error('[TIMER] Reset error:', err);
                showAlert('alarmAlert', 'Reset error: ' + err.message, 'error');
            }
        }
        
        async function virtualButton() {
            try {
                await api('/api/timer/virtual-button', { method: 'POST' });
                latchRelayDisplay(false);
                await loadStatus();
            } catch (err) {
                console.error('[TIMER] Virtual button error:', err);
                showAlert('alarmAlert', 'Virtual button error: ' + err.message, 'error');
            }
        }

        function collectRecipients() {
            const raw = document.getElementById('mailRecipients').value;
            const list = raw.split(/[\n,]/).map(x => x.trim()).filter(Boolean);
            return Array.from(new Set(list)).slice(0, 10);
        }

        function updateAttachmentTable() {
            // Deprecated: Attachments artık mail grupları içinde yönetiliyor
            // Bu fonksiyon geriye dönük uyumluluk için boş bırakıldı
            return;
        }

        function toggleAttachment(index, field, value) {
            // Deprecated: Attachments artık mail grupları içinde yönetiliyor
            return;
        }

        async function uploadAttachment(event) {
            const file = event.target.files[0];
            if (!file) return;
            
            // Dosya boyutu kontrolü (300 KB = 307200 bytes)
            if (file.size > 307200) {
                showAlert('mailAlert', 'Dosya boyutu 300 KB\'dan büyük olamaz!', 'error');
                event.target.value = '';
                return;
            }
            
            const form = new FormData();
            form.append('file', file);
            
            try {
                const response = await fetch(`/api/upload?groupIndex=${currentEditingGroupIndex}`, { method: 'POST', body: form });
                
                if (!response.ok) {
                    const result = await response.json();
                    throw new Error(result.message || t('mail.uploadError'));
                }
                
                const result = await response.json();
                
                // Mail settings'i yeniden yükle
                await loadMailSettings();
                
                // Modal açıksa, dosya listesini güncelle
                if (currentEditingGroupIndex >= 0 && mailGroups[currentEditingGroupIndex]) {
                    // Yeni yüklenen dosyayı gruba ekle
                    if (!mailGroups[currentEditingGroupIndex].attachments) {
                        mailGroups[currentEditingGroupIndex].attachments = [];
                    }
                    if (!mailGroups[currentEditingGroupIndex].attachments.includes(result.path)) {
                        mailGroups[currentEditingGroupIndex].attachments.push(result.path);
                    }
                    updateModalAttachmentsList(mailGroups[currentEditingGroupIndex].attachments);
                }
                
                showAlert('mailAlert', t('mail.uploadSuccess'));
            } catch (err) {
                showAlert('mailAlert', err.message || t('mail.uploadError'), 'error');
            } finally {
                event.target.value = '';
            }
        }

        async function deleteAttachment(path) {
            try {
                await api(`/api/attachments?path=${encodeURIComponent(path)}`, { method: 'DELETE' });
                await loadMailSettings();
                showAlert('mailAlert', t('mail.deleteSuccess'));
            } catch (err) {
                showAlert('mailAlert', err.message || t('mail.deleteError'), 'error');
            }
        }

        async function loadMailSettings() {
            try {
                state.mail = await api('/api/mail');
                document.getElementById('smtpServer').value = state.mail.smtpServer || '';
                document.getElementById('smtpPort').value = state.mail.smtpPort || 465;
                document.getElementById('smtpUsername').value = state.mail.username || '';
                document.getElementById('smtpPassword').value = state.mail.password || '';
                document.getElementById('warningSubject').value = state.mail.warning?.subject || '';
                document.getElementById('warningBody').value = state.mail.warning?.body || '';
                document.getElementById('warningUrl').value = state.mail.warning?.getUrl || '';
                
                // Load mail groups
                mailGroups = state.mail.mailGroups || [];
                renderMailGroups();
                
                updateAttachmentTable();
            } catch (err) {
                console.error(err);
            }
        }

        async function saveMailSettings() {
            try {
                const payload = {
                    smtpServer: document.getElementById('smtpServer').value,
                    smtpPort: Number(document.getElementById('smtpPort').value),
                    username: document.getElementById('smtpUsername').value,
                    password: document.getElementById('smtpPassword').value,
                    recipients: [], // Deprecated - now using mailGroups
                    warning: {
                        subject: document.getElementById('warningSubject').value,
                        body: document.getElementById('warningBody').value,
                        getUrl: document.getElementById('warningUrl').value
                    },
                    final: {
                        subject: '', // Deprecated - now in mailGroups
                        body: '',
                        getUrl: ''
                    },
                    attachments: state.mail.attachments || [],
                    mailGroups: mailGroups || []
                };
                await api('/api/mail', { method: 'PUT', body: payload });
                showAlert('mailAlert', t('mail.saveSuccess'));
            } catch (err) {
                showAlert('mailAlert', t('mail.saveError') + ': ' + (err.message || ''), 'error');
            }
        }

        async function loadWiFiSettings() {
            try {
                state.wifi = await api('/api/wifi');
                const w = state.wifi;
                const map = {
                    wifiPrimarySsid: w.primarySSID,
                    wifiPrimaryPassword: w.primaryPassword,
                    wifiSecondarySsid: w.secondarySSID,
                    wifiSecondaryPassword: w.secondaryPassword,
                    primaryIP: w.primaryIP,
                    primaryGateway: w.primaryGateway,
                    primarySubnet: w.primarySubnet,
                    primaryDNS: w.primaryDNS,
                    primaryMDNS: w.primaryMDNS,
                    secondaryIP: w.secondaryIP,
                    secondaryGateway: w.secondaryGateway,
                    secondarySubnet: w.secondarySubnet,
                    secondaryDNS: w.secondaryDNS,
                    secondaryMDNS: w.secondaryMDNS
                };
                Object.keys(map).forEach(id => { const el = document.getElementById(id); if (el) el.value = map[id] || ''; });
                document.getElementById('wifiAllowOpen').checked = !!w.allowOpenNetworks;
                updateToggleStatus(document.getElementById('wifiAllowOpen'), 'wifiAllowOpenStatus');
                document.getElementById('apModeEnabled').checked = !!w.apModeEnabled;
                updateToggleStatus(document.getElementById('apModeEnabled'), 'apModeStatus');
                document.getElementById('primaryStaticEnabled').checked = !!w.primaryStaticEnabled;
                updateToggleStatus(document.getElementById('primaryStaticEnabled'), 'primaryStaticStatus');
                
                // AP Chip ID'yi göster (tam 12 karakter)
                const status = await api('/api/status');
                if (status && status.chipId) {
                    // status.chipId doğrudan 12 karakter: "8EFE12345678"
                    const chipId = status.chipId;
                    const apChipIdEl = document.getElementById('apChipId');
                    if (apChipIdEl) apChipIdEl.textContent = chipId;
                    const apMdnsChipIdEl = document.getElementById('apMdnsChipId');
                    if (apMdnsChipIdEl) apMdnsChipIdEl.textContent = chipId.toLowerCase();
                }
                document.getElementById('secondaryStaticEnabled').checked = !!w.secondaryStaticEnabled;
                updateToggleStatus(document.getElementById('secondaryStaticEnabled'), 'secondaryStaticStatus');
            } catch (err) { console.error(err); }
        }

        async function saveWiFiSettings() {
            try {
                // mDNS değerlerini temizle (.local suffix'i kaldır)
                const primaryMDNS = document.getElementById('primaryMDNS').value.replace('.local', '').trim();
                const secondaryMDNS = document.getElementById('secondaryMDNS').value.replace('.local', '').trim();
                
                const payload = {
                    primarySSID: document.getElementById('wifiPrimarySsid').value,
                    primaryPassword: document.getElementById('wifiPrimaryPassword').value,
                    secondarySSID: document.getElementById('wifiSecondarySsid').value,
                    secondaryPassword: document.getElementById('wifiSecondaryPassword').value,
                    allowOpenNetworks: document.getElementById('wifiAllowOpen').checked,
                    apModeEnabled: document.getElementById('apModeEnabled').checked,
                    primaryStaticEnabled: document.getElementById('primaryStaticEnabled').checked,
                    primaryIP: document.getElementById('primaryIP').value,
                    primaryGateway: document.getElementById('primaryGateway').value,
                    primarySubnet: document.getElementById('primarySubnet').value,
                    primaryDNS: document.getElementById('primaryDNS').value,
                    primaryMDNS: primaryMDNS,
                    secondaryStaticEnabled: document.getElementById('secondaryStaticEnabled').checked,
                    secondaryIP: document.getElementById('secondaryIP').value,
                    secondaryGateway: document.getElementById('secondaryGateway').value,
                    secondarySubnet: document.getElementById('secondarySubnet').value,
                    secondaryDNS: document.getElementById('secondaryDNS').value,
                    secondaryMDNS: secondaryMDNS
                };
                console.log('WiFi kaydet payload:', payload);
                await api('/api/wifi', { method: 'PUT', body: payload });
                showAlert('wifiAlert', t('wifi.saveSuccess'));
            } catch (err) { 
                console.error('WiFi kayıt hatası:', err);
                showAlert('wifiAlert', t('wifi.saveError') + ': ' + (err.message || ''), 'error'); 
            }
        }

        async function scanNetworks() {
            try {
                const result = await api('/api/wifi/scan');
                const target = document.getElementById('wifiScanResults');
                if (!result.networks || result.networks.length === 0) { target.innerHTML = 'Ağ bulunamadı'; }
                else {
                    target.innerHTML = result.networks.map(net => `<div class="list-item">${net.ssid || '<adı yok>'}<span class="badge">${net.open ? 'ŞİFRESİZ' : 'ŞİFRELİ'}</span>${net.current ? '<span class=\"badge\">AKTİF</span>' : ''}</div>`).join('');
                }
            } catch (err) { showAlert('wifiAlert', err.message || 'Taramada hata', 'error'); }
        }
        
        // Custom API Endpoint Functions
        async function loadAPISettings() {
            try {
                const data = await api('/api/settings');
                document.getElementById('apiEnabled').checked = data.enabled || false;
                updateToggleStatus(document.getElementById('apiEnabled'), 'apiEnabledStatus');
                document.getElementById('apiEndpoint').value = data.endpoint || '';
                document.getElementById('apiRequireToken').checked = data.requireToken || false;
                updateToggleStatus(document.getElementById('apiRequireToken'), 'apiRequireTokenStatus');
                document.getElementById('apiToken').value = data.token || '';
                toggleAPIToken();
                updateAPIPreview();
            } catch (err) {
                console.error('[API SETTINGS] Load error:', err);
            }
        }
        
        async function saveAPISettings() {
            try {
                const payload = {
                    enabled: document.getElementById('apiEnabled').checked,
                    endpoint: document.getElementById('apiEndpoint').value,
                    requireToken: document.getElementById('apiRequireToken').checked,
                    token: document.getElementById('apiToken').value
                };
                await api('/api/settings', { method: 'PUT', body: payload });
                showAlert('wifiAlert', 'API settings saved successfully!', 'success');
                updateAPIPreview();
            } catch (err) {
                console.error('[API SETTINGS] Save error:', err);
                showAlert('wifiAlert', 'Failed to save API settings: ' + (err.message || ''), 'error');
            }
        }
        
        function toggleAPIToken() {
            const requireToken = document.getElementById('apiRequireToken').checked;
            const tokenGroup = document.getElementById('apiTokenGroup');
            if (tokenGroup) {
                tokenGroup.style.display = requireToken ? 'flex' : 'none';
            }
            updateAPIPreview();
        }
        
        function updateAPIPreview() {
            const endpoint = document.getElementById('apiEndpoint').value || 'trigger';
            const requireToken = document.getElementById('apiRequireToken').checked;
            const token = document.getElementById('apiToken').value;
            
            // Get current IP (try to extract from status or use placeholder)
            let currentIP = '192.168.1.100';
            if (window.lastStatus && window.lastStatus.network && window.lastStatus.network.ip) {
                currentIP = window.lastStatus.network.ip;
            }
            
            const fullUrl = `http://${currentIP}/api/${endpoint}`;
            
            // Update preview
            const previewEl = document.getElementById('apiPreview');
            if (previewEl) {
                previewEl.textContent = fullUrl;
            }
            
            // Update examples
            const curlExample = requireToken 
                ? `curl -X POST -H "Authorization: ${token}" ${fullUrl}`
                : `curl -X POST ${fullUrl}`;
            const curlEl = document.getElementById('apiExampleCurl');
            if (curlEl) curlEl.textContent = curlExample;
            
            const haExample = requireToken
                ? `rest_command:\n  trigger_ls:\n    url: "${fullUrl}"\n    method: POST\n    headers:\n      Authorization: "${token}"`
                : `rest_command:\n  trigger_ls:\n    url: "${fullUrl}"\n    method: POST`;
            const haEl = document.getElementById('apiExampleHA');
            if (haEl) haEl.innerHTML = haExample.replace(/\n/g, '<br>  ');
            
            const nodeExample = `[http request] → POST → ${fullUrl}${requireToken ? ' (Auth: ' + token + ')' : ''}`;
            const nodeEl = document.getElementById('apiExampleNode');
            if (nodeEl) nodeEl.textContent = nodeExample;
        }
        
        // ⚠️ YENİ: Accordion Toggle
        function toggleAccordion(header) {
            header.classList.toggle('active');
            const content = header.nextElementSibling;
            content.classList.toggle('active');
        }

        async function factoryReset() {
            if(!confirm(t('info.factoryResetConfirm'))) return;
            try {
                await api('/api/factory-reset', { method: 'POST' });
                location.reload();
            } catch(e){ 
                showAlert('wifiAlert', e.message || t('errors.unknown'),'error'); 
            }
        }

        async function rebootDevice() {
            if(!confirm(t('info.rebootConfirm'))) return;
            try {
                await api('/api/reboot', { method: 'POST' });
                showAlert('wifiAlert', t('info.rebootSuccess'), 'success');
            } catch(e){ 
                showAlert('wifiAlert', e.message || t('errors.unknown'),'error'); 
            }
        }

        async function checkOTAUpdate() {
            const btn = document.getElementById('otaCheckBtn');
            const status = document.getElementById('otaStatus');
            
            btn.disabled = true;
            btn.style.opacity = '0.5';
            status.textContent = t('info.otaChecking') || 'Checking...';
            status.style.color = '#888';
            
            try {
                const result = await api('/api/ota/check', { method: 'POST' });
                if (result.status === 'updating') {
                    status.textContent = t('info.otaUpdating') || 'Update found! Restarting...';
                    status.style.color = '#4CAF50';
                } else if (result.status === 'ok') {
                    status.textContent = t('info.otaNoUpdate') || 'No update available';
                    status.style.color = '#888';
                } else {
                    status.textContent = result.message || 'Error';
                    status.style.color = '#f44336';
                }
            } catch(e) {
                status.textContent = e.message || t('errors.unknown');
                status.style.color = '#f44336';
            } finally {
                btn.disabled = false;
                btn.style.opacity = '1';
            }
        }

        function bindStaticIpToggles(){
            function toggle(prefix){
                const en = document.getElementById(prefix+"StaticEnabled")?.checked;
                ["IP","Gateway","Subnet","DNS"].forEach(s=>{
                    const el = document.getElementById(prefix.toLowerCase()+s);
                    if(el) el.disabled = !en;
                });
            }
            const p = document.getElementById('primaryStaticEnabled');
            const s = document.getElementById('secondaryStaticEnabled');
            if(p) p.addEventListener('change', ()=>toggle('primary'));
            if(s) s.addEventListener('change', ()=>toggle('secondary'));
            toggle('primary');
            toggle('secondary');
        }

        let initialized = false; // Global flag to prevent double init

        async function init() {
            if (initialized) {
                console.warn('[INIT] Already initialized, skipping...');
                return;
            }
            initialized = true;
            
            console.log('[INIT] Starting initialization...');
            
            // 1. DİL BUTONLARINI KUR (en önce - global)
            console.log('[INIT] Setting up language buttons...');
            document.querySelectorAll('.lang-btn').forEach(btn => {
                const lang = btn.getAttribute('data-lang');
                console.log('[LANG] Attaching listener to:', lang);
                btn.addEventListener('click', function(e) {
                    console.log('[LANG] Switching to:', lang);
                    e.preventDefault();
                    switchLanguage(lang);
                });
            });
            
            // 2. TAB SİSTEMİNİ KUR
            console.log('[INIT] Setting up tab navigation...');
            document.querySelectorAll('.tab').forEach((tab, index) => {
                const tabId = tab.getAttribute('data-tab');
                console.log('[TAB] Attaching listener to tab', index, ':', tabId);
                tab.addEventListener('click', function(e) {
                    console.log('[TAB] Click event on', tabId);
                    e.preventDefault();
                    e.stopPropagation();
                    openTab(e, tabId);
                }, true); // Use capture phase
            });
            
            // 3. İlk tab'ı aktif et
            console.log('[INIT] Activating first tab...');
            const firstTab = document.querySelector('.tab[data-tab="alarmTab"]');
            if (firstTab) {
                openTab({ currentTarget: firstTab }, 'alarmTab');
            }
            
            // 4. MAIL TEST BUTONLARINI KUR
            console.log('[INIT] Setting up mail test buttons...');
            const btnTestWarning = document.getElementById('btnTestWarning');
            const btnSaveMail = document.getElementById('btnSaveMail');
            
            if (btnTestWarning) {
                // Disable during test to prevent double-click
                btnTestWarning.addEventListener('click', async function(e) {
                    e.preventDefault();
                    if (btnTestWarning.disabled) {
                        console.log('[BUTTON] Test Warning - Already running, ignored');
                        return;
                    }
                    console.log('[BUTTON] Test Warning clicked');
                    btnTestWarning.disabled = true;
                    try {
                        await sendWarningTest();
                    } finally {
                        setTimeout(() => { btnTestWarning.disabled = false; }, 1000);
                    }
                });
            }
            
            if (btnSaveMail) {
                btnSaveMail.addEventListener('click', function(e) {
                    e.preventDefault();
                    console.log('[BUTTON] Save Mail clicked');
                    saveMailSettings();
                });
            }
            
            // 5. Diğer ayarları yükle (paralel, bloke etmeden)
            console.log('[INIT] Loading settings...');
            document.getElementById('deviceId').textContent = "";
            document.getElementById('firmwareVersion').textContent = "";
            loadStatus(); // async ama await etme
            loadTimerSettings(); // async ama await etme
            loadMailSettings(); // async ama await etme - Mail groups da burada yüklenecek
            loadWiFiSettings(); // async ama await etme
            loadAPISettings(); // async ama await etme - Load custom API settings
            bindStaticIpToggles();
            
            // 6. Düzenli status güncelleme (daha hızlı - responsive UI için)
            console.log('[INIT] Setting up status polling...');
            let statusInterval = setInterval(loadStatus, 2000); // 3000ms → 2000ms
            
            // Page Visibility API - Sayfa arka plandayken polling'i durdur
            document.addEventListener('visibilitychange', function() {
                if (document.hidden) {
                    // Sayfa arka planda - interval'i durdur (memory save)
                    if (statusInterval) {
                        clearInterval(statusInterval);
                        statusInterval = null;
                    }
                } else {
                    // Sayfa ön planda - interval'i yeniden başlat
                    if (!statusInterval) {
                        loadStatus(); // Hemen bir kez çalıştır
                        statusInterval = setInterval(loadStatus, 2000); // 3000ms → 2000ms
                    }
                }
            });
            
            console.log('[INIT] Initialization complete!');
        }

        // init() will be called from DOMContentLoaded
        
        // Mail Groups Management
        let mailGroups = [];
        let currentEditingGroupIndex = -1;
        
        function addMailGroup() {
            currentEditingGroupIndex = -1;
            document.getElementById('mailGroupModalTitle').textContent = 'Add Mail Group';
            
            // Clear form
            document.getElementById('modalGroupName').value = '';
            document.getElementById('modalGroupEnabled').checked = true;
            updateToggleStatus(document.getElementById('modalGroupEnabled'), 'modalGroupEnabledStatus');
            document.getElementById('modalGroupRecipients').value = '';
            document.getElementById('modalGroupSubject').value = 'SmartKraft LebensSpur Final';
            document.getElementById('modalGroupBody').value = '[!] LEBENSSPUR PROTOCOL ACTIVE [!]\n\nDevice: {DEVICE_ID}\nTime: {TIMESTAMP}\n\nTimer completed.';
            document.getElementById('modalGroupUrl').value = '';
            
            document.getElementById('mailGroupModal').style.display = 'block';
        }
        
        function editMailGroup(index) {
            currentEditingGroupIndex = index;
            const group = mailGroups[index];
            document.getElementById('mailGroupModalTitle').textContent = 'Edit Mail Group';
            document.getElementById('modalGroupName').value = group.name;
            document.getElementById('modalGroupEnabled').checked = group.enabled;
            updateToggleStatus(document.getElementById('modalGroupEnabled'), 'modalGroupEnabledStatus');
            document.getElementById('modalGroupRecipients').value = group.recipients.join('\n');
            document.getElementById('modalGroupSubject').value = group.subject;
            document.getElementById('modalGroupBody').value = group.body;
            document.getElementById('modalGroupUrl').value = group.getUrl;
            
            // Dosyaları göster
            updateModalAttachmentsList(group.attachments || []);
            
            document.getElementById('mailGroupModal').style.display = 'block';
        }
        
        function updateModalAttachmentsList(attachments) {
            const container = document.getElementById('modalAttachmentsList');
            if (!container) return;
            
            if (!attachments || attachments.length === 0) {
                container.innerHTML = '';
                return;
            }
            
            container.innerHTML = `
                <div style="border:1px solid #333; padding:8px; margin-top:8px;">
                    <div style="font-size:0.8em; color:#888; margin-bottom:8px;">📎 Uploaded Files:</div>
                    ${attachments.filter(a => a.trim()).map((path, idx) => {
                        const fileName = path.split('/').pop().split('_').slice(1).join('_'); // Remove timestamp prefix
                        return `
                            <div style="display:flex; justify-content:space-between; align-items:center; padding:4px 0; border-bottom:1px solid #222;">
                                <span style="font-size:0.75em; color:#ccc;">${fileName}</span>
                                <button onclick="deleteAttachmentFromGroup(${currentEditingGroupIndex}, '${path}')" 
                                        style="background:transparent; border:1px solid #f00; color:#f00; padding:2px 8px; cursor:pointer; font-size:0.7em;">
                                    Delete
                                </button>
                            </div>
                        `;
                    }).join('')}
                </div>
            `;
        }
        
        async function deleteAttachmentFromGroup(groupIndex, path) {
            if (!confirm('Delete this file?')) return;
            
            try {
                // Backend'de sil
                await api(`/api/attachments?path=${encodeURIComponent(path)}`, { method: 'DELETE' });
                
                // Local state'i güncelle
                if (mailGroups[groupIndex]) {
                    mailGroups[groupIndex].attachments = mailGroups[groupIndex].attachments.filter(a => a !== path);
                    updateModalAttachmentsList(mailGroups[groupIndex].attachments);
                }
                
                showAlert('mailAlert', 'File deleted successfully');
            } catch (err) {
                showAlert('mailAlert', 'Error deleting file: ' + err.message, 'error');
            }
        }
        
        function closeMailGroupModal() {
            document.getElementById('mailGroupModal').style.display = 'none';
        }
        
        function saveMailGroup() {
            const name = document.getElementById('modalGroupName').value.trim();
            const enabled = document.getElementById('modalGroupEnabled').checked;
            const recipients = document.getElementById('modalGroupRecipients').value
                .split('\n')
                .map(r => r.trim())
                .filter(r => r.length > 0)
                .slice(0, 10);
            const subject = document.getElementById('modalGroupSubject').value.trim();
            const body = document.getElementById('modalGroupBody').value.trim();
            const getUrl = document.getElementById('modalGroupUrl').value.trim();
            
            // Attachments artık upload ile yönetiliyor, mevcut attachments'ı koru
            const attachments = (currentEditingGroupIndex >= 0 && mailGroups[currentEditingGroupIndex]) 
                ? mailGroups[currentEditingGroupIndex].attachments 
                : [];
            
            if (!name) {
                alert(t('mail.groupNameRequired'));
                return;
            }
            
            if (recipients.length === 0) {
                alert(t('mail.groupRecipientsRequired'));
                return;
            }
            
            const groupData = {
                name: name,
                enabled: enabled,
                recipients: recipients,
                subject: subject,
                body: body,
                getUrl: getUrl,
                attachments: attachments
            };
            
            if (currentEditingGroupIndex >= 0) {
                mailGroups[currentEditingGroupIndex] = groupData;
            } else {
                if (mailGroups.length >= 3) {
                    alert(t('mail.groupMaxReached'));
                    return;
                }
                mailGroups.push(groupData);
            }
            
            // Save all mail settings (including groups)
            closeMailGroupModal();
            renderMailGroups();
            // Auto-save will happen when user clicks main Save button
        }
        
        function deleteMailGroup(index) {
            if (!confirm('Delete this mail group?')) return;
            
            mailGroups.splice(index, 1);
            renderMailGroups();
            // Auto-save will happen when user clicks main Save button
        }
        
        function renderMailGroups() {
            const container = document.getElementById('mailGroupsList');
            
            if (mailGroups.length === 0) {
                container.innerHTML = '<div style="padding:20px; text-align:center; color:#666;"><span data-i18n="messages.noMailGroups">No mail groups yet. Click "Add New Mail Group" to create one.</span></div>';
                applyTranslations();
                return;
            }
            
            container.innerHTML = mailGroups.map((group, index) => {
                const statusBadge = group.enabled 
                    ? `<span style="color:#0f0; font-size:0.7em;">● ${t('messages.active')}</span>` 
                    : `<span style="color:#666; font-size:0.7em;">○ DISABLED</span>`;
                
                const recipientCount = group.recipients.length || 0;
                const attachmentCount = group.attachments.filter(a => a.trim()).length || 0;
                
                return `
                    <div onclick="editMailGroup(${index})" style="border-bottom:1px solid #333; padding:16px; cursor:pointer; transition:background 0.2s;" 
                         onmouseover="this.style.background='#111'" onmouseout="this.style.background='transparent'">
                        <div style="display:flex; justify-content:space-between; align-items:center;">
                            <div style="flex:1;">
                                <div style="font-size:1em; font-weight:bold; margin-bottom:4px;">
                                    ${group.name || t('mail.groupUnnamed')} ${statusBadge}
                                </div>
                                <div style="font-size:0.75em; color:#888;">
                                    ${recipientCount} ${t('mail.groupRecipientCount')} • ${attachmentCount} ${t('mail.groupFileCount')}
                                </div>
                            </div>
                            <div style="font-size:1.5em; color:#666;">›</div>
                        </div>
                    </div>
                `;
            }).join('');
        }
        
        </script>

    <!-- Mail Groups Modal -->
    <div id="mailGroupModal" style="display:none; position:fixed; top:0; left:0; width:100%; height:100%; background:rgba(0,0,0,0.9); z-index:1000; overflow-y:auto; padding:40px 0;">
        <div style="max-width:700px; margin:0 auto; background:#000; border:2px solid #fff; padding:24px;">
            <!-- Modal Header -->
            <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:20px; border-bottom:1px solid #333; padding-bottom:12px;">
                <h3 id="mailGroupModalTitle" style="margin:0; font-size:1.2em; letter-spacing:1px;" data-i18n="mail.editGroupTitle">Edit Mail Group</h3>
                <button onclick="closeMailGroupModal()" style="background:transparent; border:1px solid #f00; color:#f00; padding:4px 12px; cursor:pointer;"><span data-i18n="buttons.close">✕ Close</span></button>
            </div>
            
            <!-- Modal Content -->
            <div style="padding-right:8px;">
                <!-- Grup İsmi -->
                <div class="form-group">
                    <label data-i18n="mail.groupName">Group Name</label>
                    <input type="text" id="modalGroupName" data-i18n="mail.groupNamePlaceholder" placeholder="e.g., Management, Technical Team, Emergency" style="width:100%;">
                </div>
                
                <!-- Grup Aktif/Pasif -->
                <div class="toggle-container" style="margin-bottom:24px;">
                    <span class="toggle-label" data-i18n="mail.groupEnabled">ENABLE THIS GROUP</span>
                    <label class="toggle-switch">
                        <input type="checkbox" id="modalGroupEnabled" onchange="updateToggleStatus(this, 'modalGroupEnabledStatus')">
                        <span class="toggle-slider"></span>
                    </label>
                    <span class="toggle-status" id="modalGroupEnabledStatus" data-i18n="mail.inactive">Deaktif</span>
                </div>
                
                <!-- Alıcılar -->
                <div class="form-group">
                    <label data-i18n="mail.sectionRecipients">Recipients</label>
                    <textarea id="modalGroupRecipients" data-i18n="mail.recipientsPlaceholder" placeholder="recipient1@example.com&#10;recipient2@example.com" style="min-height:80px; width:100%;"></textarea>
                </div>
                <div style="font-size:0.7em; color:#666; margin-bottom:12px;">
                    <span data-i18n="mail.recipientsHelpGroup">Enter email addresses (one per line, max 10)</span>
                </div>
                
                <!-- Subject -->
                <div class="form-group">
                    <label data-i18n="mail.finalSubject">Subject</label>
                    <input type="text" id="modalGroupSubject" data-i18n="mail.finalSubjectPlaceholder" placeholder="Final Notice from SmartKraft LebensSpur" style="width:100%;">
                </div>
                
                <!-- Body -->
                <div class="form-group">
                    <label data-i18n="mail.finalBody">Message Body</label>
                    <textarea id="modalGroupBody" data-i18n="mail.finalBodyPlaceholder" placeholder="Final message content..." style="min-height:120px; width:100%;">[!] LEBENSSPUR PROTOCOL ACTIVE [!]

Device: {DEVICE_ID}
Time: {TIMESTAMP}

Timer completed. Urgent action required.</textarea>
                </div>
                <div style="font-size:0.7em; color:#666; margin-bottom:12px;">
                    <span data-i18n="mail.placeholders">Use {DEVICE_ID}, {TIMESTAMP}, {REMAINING}</span>
                </div>
                
                <!-- URL Trigger -->
                <div class="form-group">
                    <label data-i18n="mail.finalUrl">Trigger URL (GET)</label>
                    <input type="text" id="modalGroupUrl" data-i18n="mail.finalUrlPlaceholder" placeholder="https://example.com/api/final" style="width:100%;">
                </div>
                
                <!-- Dosya Yükleme -->
                <div style="border:1px dashed #555; padding:16px; margin:16px 0; text-align:center; cursor:pointer;" onclick="document.getElementById('modalFileInput').click()">
                    <div style="color:#888; font-size:0.8em; margin-bottom:8px;" data-i18n="mail.uploadZone">📎 Click to upload file (max 300 KB per group, 900 KB total)</div>
                </div>
                <input type="file" id="modalFileInput" style="display:none" onchange="uploadAttachment(event)">
                
                <!-- Yüklenen Dosyalar -->
                <div id="modalAttachmentsList" style="margin-top:12px;"></div>
            </div>
            
            <!-- Modal Footer -->
            <div style="display:flex; gap:12px; justify-content:flex-end; border-top:1px solid #333; padding-top:16px; margin-top:16px;">
                <button onclick="deleteMailGroup(currentEditingGroupIndex); closeMailGroupModal();" style="padding:8px 16px; background:transparent; border:1px solid #f00; color:#f00; cursor:pointer;" data-i18n="buttons.delete">Delete Group</button>
                <button onclick="closeMailGroupModal()" style="padding:8px 16px; background:transparent; border:1px solid #555; color:#888; cursor:pointer;" data-i18n="buttons.cancel">Cancel</button>
                <button onclick="saveMailGroup()" style="padding:8px 16px; background:#fff; border:1px solid #fff; color:#000; cursor:pointer; font-weight:bold;" data-i18n="buttons.save">Save Changes</button>
            </div>
        </div>
    </div>

</body>
</html>
)rawliteral";

namespace {
// JSON capacity tanımları header'da
// constexpr size_t JSON_CAPACITY = 4096; // Bu satır artık gereksiz
constexpr size_t MAX_UPLOAD_SIZE = 307200; // 300 KB per group (total 900 KB for 3 groups)

struct UploadContext {
    File file;
    size_t written = 0;
    String storedPath;
    String originalName;
    String errorMessage = ""; // Hata mesajı

    void reset() {
        if (file) {
            file.close();
        }
        written = 0;
        storedPath = "";
        originalName = "";
        errorMessage = "";
    }
};

UploadContext uploadContext;
}

void WebInterface::begin(WebServer *srv,
                         ConfigStore *storePtr,
                         CountdownScheduler *sched,
                         MailAgent *mailAgent,
                         LebenSpurNetworkManager *netMgr,
                         const String &deviceIdentifier,
                         DNSServer *dns,
                         const String &apName,
                         OTAManager *ota) {
    server = srv;
    store = storePtr;
    scheduler = sched;
    mail = mailAgent;
    network = netMgr;
    deviceId = deviceIdentifier;
    dnsServer = dns;
    this->apName = apName; // AP name'i kaydet
    otaManager = ota;  // OTA Manager referansı

    server->on("/", HTTP_GET, [this]() { handleIndex(); });
    server->on("/api/status", HTTP_GET, [this]() { handleStatus(); });

    server->on("/api/timer", HTTP_GET, [this]() { handleTimerGet(); });
    server->on("/api/timer", HTTP_PUT, [this]() { handleTimerUpdate(); });
    server->on("/api/timer/start", HTTP_POST, [this]() { handleTimerStart(); });
    server->on("/api/timer/stop", HTTP_POST, [this]() { handleTimerStop(); });
    server->on("/api/timer/resume", HTTP_POST, [this]() { handleTimerResume(); });
    server->on("/api/timer/reset", HTTP_POST, [this]() { handleTimerReset(); });
    server->on("/api/timer/virtual-button", HTTP_POST, [this]() { handleVirtualButton(); });

    server->on("/api/mail", HTTP_GET, [this]() { handleMailGet(); });
    server->on("/api/mail", HTTP_PUT, [this]() { handleMailUpdate(); });
    server->on("/api/mail/test", HTTP_POST, [this]() { handleMailTest(); });

    server->on("/api/wifi", HTTP_GET, [this]() { handleWiFiGet(); });
    server->on("/api/wifi", HTTP_PUT, [this]() { handleWiFiUpdate(); });
    server->on("/api/wifi/scan", HTTP_GET, [this]() { handleWiFiScan(); });

    server->on("/api/attachments", HTTP_GET, [this]() { handleAttachmentList(); });
    server->on("/api/attachments", HTTP_DELETE, [this]() { handleAttachmentDelete(); });
    
    server->on("/api/i18n", HTTP_GET, [this]() { handleI18n(); });
    
    // Custom API endpoint settings
    server->on("/api/settings", HTTP_GET, [this]() { handleAPIGet(); });
    server->on("/api/settings", HTTP_PUT, [this]() { handleAPIUpdate(); });

    server->on("/api/logs", HTTP_GET, [this]() { handleLogs(); });
    server->on("/api/reboot", HTTP_POST, [this]() { handleReboot(); });
    server->on("/api/factory-reset", HTTP_POST, [this]() { handleFactoryReset(); });
    server->on("/api/ota/check", HTTP_POST, [this]() { handleOTACheck(); });

    server->on("/api/ip", HTTP_GET, [this]() {
        JsonDocument doc;
        doc["ip"] = WiFi.localIP().toString();
        doc["hostname"] = WiFi.getHostname();
        sendJson(doc);
    });

    server->on("/api/upload", HTTP_POST,
               [this]() {
                   JsonDocument doc;
                   if (uploadContext.errorMessage.length() > 0) {
                       // Hata oluştu
                       doc["status"] = "error";
                       doc["message"] = uploadContext.errorMessage;
                       String output;
                       serializeJson(doc, output);
                       server->send(400, "application/json", output);
                   } else if (!uploadContext.storedPath.length()) {
                       // Dosya yüklenmedi
                       doc["status"] = "error";
                       doc["message"] = "No file uploaded";
                       String output;
                       serializeJson(doc, output);
                       server->send(400, "application/json", output);
                   } else {
                       // Başarılı
                       doc["status"] = "ok";
                       doc["path"] = uploadContext.storedPath;
                       doc["name"] = uploadContext.originalName;
                       sendJson(doc);
                   }
                   uploadContext.reset();
               },
               [this]() { handleAttachmentUpload(); });
}

void WebInterface::startServer() {
    if (!server) return;
    
    // Kayıtlı WiFi ayarlarını kontrol et
    WiFiSettings wifiConfig = store->loadWiFiSettings();
    bool hasStoredWiFi = (wifiConfig.primarySSID.length() > 0);
    bool staConnected = false;
    
    if (hasStoredWiFi) {
        if (network && network->connectToKnown()) {
            staConnected = true;
        }
    }
    
    bool shouldStartAP = false;
    
    if (!hasStoredWiFi) {
        shouldStartAP = true;
    } else if (!staConnected && wifiConfig.apModeEnabled) {
        shouldStartAP = true;
    } else if (staConnected && wifiConfig.apModeEnabled) {
        shouldStartAP = true;
    }
    
    if (shouldStartAP && staConnected) {
        WiFi.mode(WIFI_AP_STA);
    } else if (shouldStartAP && !staConnected) {
        WiFi.mode(WIFI_AP);
    } else if (!shouldStartAP && staConnected) {
        WiFi.mode(WIFI_STA);
    }
    
    delay(100);
    
    if (shouldStartAP) {
        String chipIdStr = getOrCreateDeviceId();
        String apMdnsHostname = "ls-" + chipIdStr;
        WiFi.softAPsetHostname(apMdnsHostname.c_str());
        delay(50);
        
        WiFi.softAP(apName.c_str());
        delay(500);
        
        if (dnsServer) {
            dnsServer->start(53, "*", WiFi.softAPIP());
        }
        
        startAPModeMDNS();
    } else {
        if (dnsServer) {
            dnsServer->stop();
        }
    }
    
    server->onNotFound([this]() {
        String uri = server->uri();
        if (uri.startsWith("/api/")) {
            handleAPITrigger();
        } else {
            server->send(404, "text/plain", "Not Found");
        }
    });
    
    server->begin();
    disableWiFiPowerSave();
}

void WebInterface::loop() {
    if (!server) return;

    if (dnsServer) {
        dnsServer->processNextRequest();
    }
    
    server->handleClient();
    
    if (millis() - lastStatusPush > 2000) {
        broadcastStatus();
        lastStatusPush = millis();
    }
    
    static unsigned long lastWiFiCheck = 0;
    static bool wasConnected = false;
    
    if (millis() - lastWiFiCheck > 60000) {
        wifi_mode_t mode = WiFi.getMode();
        wl_status_t status = WiFi.status();
        
        if (mode == WIFI_STA || mode == WIFI_AP_STA) {
            bool nowConnected = (status == WL_CONNECTED);
            
            if (nowConnected) {
                String currentSSID = WiFi.SSID();
                
                if (network) {
                    bool shouldSwitch = network->checkForBetterNetwork(currentSSID);
                    if (shouldSwitch) {
                        network->connectToKnown();
                    }
                }
            } else {
                if (network) {
                    network->connectToKnown();
                }
            }
            
            wasConnected = nowConnected;
        }
        
        lastWiFiCheck = millis();
    }
}

void WebInterface::broadcastStatus() {
    if (!server) return;
}

void WebInterface::handleIndex() {
    // Static content için cache header'ları
    server->sendHeader("Cache-Control", "public, max-age=3600"); // 1 saat cache
    server->sendHeader("Connection", "keep-alive");
    server->send_P(200, "text/html", INDEX_HTML);
}

void WebInterface::handleStatus() {
    // ===== HEALTH TRACKING: İstek geldi =====
    lastRequestTime = millis();
    requestCounter++;
    
    // Cache kontrol - performance optimization
    unsigned long now = millis();
    if (now - lastStatusCache < STATUS_CACHE_DURATION && !cachedStatusResponse.isEmpty()) {
        server->send(200, "application/json", cachedStatusResponse);
        return;
    }
    
    // Performans optimizasyonu: Orta boyut JSON capacity kullan  
    JsonDocument doc;
    ScheduleSnapshot snap = scheduler->snapshot();
    
    // Core timer bilgileri
    doc["timerActive"] = snap.timerActive;
    doc["paused"] = scheduler->isPaused();
    doc["remainingSeconds"] = snap.remainingSeconds;
    doc["nextAlarmIndex"] = snap.nextAlarmIndex;
    doc["finalTriggered"] = snap.finalTriggered;
    doc["totalSeconds"] = scheduler->totalSeconds();
    
    // Alarmları her zaman gönder (timer aktif olmasa bile)
    if (snap.totalAlarms > 0) {
        JsonArray alarms = doc["alarms"].to<JsonArray>();
        for (uint8_t i = 0; i < snap.totalAlarms; ++i) {
            alarms.add(snap.alarmOffsets[i]);
        }
    }
    
    // Network status - hızlı kontrol
    bool connected = network->isConnected();
    doc["wifiConnected"] = connected;
    if (connected) {
        doc["ssid"] = network->currentSSID();
        doc["ip"] = network->currentIP().toString();
        doc["hostname"] = WiFi.getHostname(); // mDNS hostname
    }
    
    doc["deviceId"] = deviceId;
    doc["chipId"] = getOrCreateDeviceId(); // Benzersiz 12 karakter ID
    doc["macAddress"] = getChipIdHex(); // Orijinal MAC (referans)
    doc["firmwareVersion"] = FIRMWARE_VERSION; // Dinamik version bilgisi
    doc["freeHeap"] = ESP.getFreeHeap(); // Memory monitoring
    
    // NOT: Termal bilgiler KALDIRILDI
    
    // WiFi config bilgileri sadece gerekirse
    WiFiSettings wifi = network->getConfig();
    doc["allowOpenNetworks"] = wifi.allowOpenNetworks;
    doc["apModeEnabled"] = wifi.apModeEnabled;
    doc["primaryStaticEnabled"] = wifi.primaryStaticEnabled;
    doc["secondaryStaticEnabled"] = wifi.secondaryStaticEnabled;
    
    // Response'u cache'le
    cachedStatusResponse = "";
    serializeJson(doc, cachedStatusResponse);
    lastStatusCache = now;
    
    server->send(200, "application/json", cachedStatusResponse);
}

void WebInterface::handleTimerGet() {
    JsonDocument doc; // Küçük response
    auto settings = scheduler->settings();
    
    // Dakika/Saat/Gün seçimi
    if (settings.unit == TimerSettings::MINUTES) {
        doc["unit"] = "minutes";
    } else if (settings.unit == TimerSettings::HOURS) {
        doc["unit"] = "hours";
    } else {
        doc["unit"] = "days";
    }
    
    doc["totalValue"] = settings.totalValue;
    doc["alarmCount"] = settings.alarmCount;
    doc["enabled"] = settings.enabled;
    sendJson(doc);
}

void WebInterface::handleTimerUpdate() {
    if (!server->hasArg("plain")) {
        server->send(400, "application/json", "{\"error\":\"JSON bekleniyor\"}");
        return;
    }
    
    String body = server->arg("plain");
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, body);
    
    if (error) {
        server->send(400, "application/json", "{\"error\":\"JSON parse error\"}");
        return;
    }
    
    try {
        TimerSettings settings = scheduler->settings();
        String unit = doc["unit"].as<String>();
        
        if (unit == "minutes") {
            settings.unit = TimerSettings::MINUTES;
        } else if (unit == "hours") {
            settings.unit = TimerSettings::HOURS;
        } else {
            settings.unit = TimerSettings::DAYS;
        }
        
        settings.totalValue = doc["totalValue"].as<uint16_t>();
        settings.totalValue = constrain(settings.totalValue, (uint16_t)1, (uint16_t)60);
        settings.alarmCount = doc["alarmCount"].as<uint8_t>();
        settings.alarmCount = constrain(settings.alarmCount, (uint8_t)0, (uint8_t)MAX_ALARMS);
        settings.enabled = doc["enabled"].as<bool>();

        scheduler->configure(settings);
        server->send(200, "application/json", "{\"status\":\"ok\"}");
        
    } catch (...) {
        server->send(500, "application/json", "{\"error\":\"Internal server error\"}");
    }
}

void WebInterface::handleTimerStart() {
    // Start only if timer is stopped (not running or paused)
    if (scheduler->isStopped()) {
        scheduler->start();
        server->send(200, "application/json", "{\"status\":\"started\"}");
    } else {
        server->send(400, "application/json", "{\"error\":\"Timer is already running or paused\"}");
    }
}

void WebInterface::handleTimerStop() {
    // Stop is now "pause"
    if (scheduler->isActive()) {
        scheduler->pause();
        server->send(200, "application/json", "{\"status\":\"paused\"}");
    } else {
        server->send(400, "application/json", "{\"error\":\"Timer is not running\"}");
    }
}

void WebInterface::handleTimerResume() {
    // Resume from paused state
    if (scheduler->isPaused()) {
        scheduler->resume();
        server->send(200, "application/json", "{\"status\":\"resumed\"}");
    } else {
        server->send(400, "application/json", "{\"error\":\"Timer is not paused\"}");
    }
}

void WebInterface::handleTimerReset() {
    scheduler->reset();
    server->send(200, "application/json", "{\"status\":\"reset\"}");
}

void WebInterface::handleVirtualButton() {
    // Physical button simulation: reset and start atomically
    scheduler->reset();
    scheduler->start();
    server->send(200, "application/json", "{\"status\":\"virtual-button-pressed\"}");
}

void WebInterface::handleMailGet() {
    MailSettings mailSettings = mail->currentConfig();
    JsonDocument doc; // Mail settings büyük olabilir
    doc["smtpServer"] = mailSettings.smtpServer;
    doc["smtpPort"] = mailSettings.smtpPort;
    doc["username"] = mailSettings.username;
    doc["warning"]["subject"] = mailSettings.warning.subject;
    doc["warning"]["body"] = mailSettings.warning.body;
    doc["warning"]["getUrl"] = mailSettings.warning.getUrl;
    doc["final"]["subject"] = mailSettings.finalContent.subject;
    doc["final"]["body"] = mailSettings.finalContent.body;
    doc["final"]["getUrl"] = mailSettings.finalContent.getUrl;
    JsonArray recipients = doc["recipients"].to<JsonArray>();
    for (uint8_t i = 0; i < mailSettings.recipientCount; ++i) {
        recipients.add(mailSettings.recipients[i]);
    }
    JsonArray attachments = doc["attachments"].to<JsonArray>();
    for (uint8_t i = 0; i < mailSettings.attachmentCount; ++i) {
        JsonObject entry = attachments.add<JsonObject>();
        entry["displayName"] = mailSettings.attachments[i].displayName;
        entry["storedPath"] = mailSettings.attachments[i].storedPath;
        entry["size"] = mailSettings.attachments[i].size;
        entry["forWarning"] = mailSettings.attachments[i].forWarning;
        entry["forFinal"] = mailSettings.attachments[i].forFinal;
    }
    
    // Add Mail Groups
    JsonArray mailGroups = doc["mailGroups"].to<JsonArray>();
    for (uint8_t i = 0; i < mailSettings.mailGroupCount; ++i) {
        JsonObject group = mailGroups.add<JsonObject>();
        group["name"] = mailSettings.mailGroups[i].name;
        group["enabled"] = mailSettings.mailGroups[i].enabled;
        
        JsonArray groupRecipients = group["recipients"].to<JsonArray>();
        for (uint8_t j = 0; j < mailSettings.mailGroups[i].recipientCount; ++j) {
            groupRecipients.add(mailSettings.mailGroups[i].recipients[j]);
        }
        
        group["subject"] = mailSettings.mailGroups[i].subject;
        group["body"] = mailSettings.mailGroups[i].body;
        group["getUrl"] = mailSettings.mailGroups[i].getUrl;
        
        JsonArray groupAttachments = group["attachments"].to<JsonArray>();
        for (uint8_t j = 0; j < mailSettings.mailGroups[i].attachmentCount; ++j) {
            groupAttachments.add(mailSettings.mailGroups[i].attachments[j]);
        }
    }
    
    sendJson(doc);
}

void WebInterface::handleMailUpdate() {
    if (!server->hasArg("plain")) {
        server->send(400, "application/json", "{\"error\":\"JSON bekleniyor\"}");
        return;
    }
    JsonDocument doc; // Mail update büyük olabilir
    if (deserializeJson(doc, server->arg("plain"))) {
        server->send(400, "application/json", "{\"error\":\"JSON hata\"}");
        return;
    }

    MailSettings mailSettings = mail->currentConfig();
    mailSettings.smtpServer = doc["smtpServer"].as<String>();
    mailSettings.smtpPort = doc["smtpPort"] | 465;
    mailSettings.username = doc["username"].as<String>();
    
    // Sadece yeni şifre girildiyse güncelle
    String newPassword = doc["password"] | "";
    if (newPassword.length() > 0) {
        mailSettings.password = newPassword;
    }

    mailSettings.warning.subject = doc["warning"]["subject"].as<String>();
    mailSettings.warning.body = doc["warning"]["body"].as<String>();
    mailSettings.warning.getUrl = doc["warning"]["getUrl"].as<String>();

    mailSettings.finalContent.subject = doc["final"]["subject"].as<String>();
    mailSettings.finalContent.body = doc["final"]["body"].as<String>();
    mailSettings.finalContent.getUrl = doc["final"]["getUrl"].as<String>();

    if (doc["recipients"].is<JsonArray>()) {
        auto rec = doc["recipients"].as<JsonArray>();
        mailSettings.recipientCount = min((uint8_t)rec.size(), (uint8_t)MAX_RECIPIENTS);
        for (uint8_t i = 0; i < mailSettings.recipientCount; ++i) {
            mailSettings.recipients[i] = rec[i].as<String>();
        }
    }

    if (doc["attachments"].is<JsonArray>()) {
        auto attachments = doc["attachments"].as<JsonArray>();
        mailSettings.attachmentCount = min((uint8_t)attachments.size(), (uint8_t)MAX_ATTACHMENTS);
        for (uint8_t i = 0; i < mailSettings.attachmentCount; ++i) {
            auto entry = attachments[i];
            strlcpy(mailSettings.attachments[i].displayName, entry["displayName"].as<const char*>(), MAX_FILENAME_LEN);
            strlcpy(mailSettings.attachments[i].storedPath, entry["storedPath"].as<const char*>(), MAX_PATH_LEN);
            mailSettings.attachments[i].size = entry["size"].as<uint32_t>();
            mailSettings.attachments[i].forWarning = entry["forWarning"].as<bool>();
            mailSettings.attachments[i].forFinal = entry["forFinal"].as<bool>();
        }
    }

    // Handle Mail Groups
    if (doc["mailGroups"].is<JsonArray>()) {
        auto groups = doc["mailGroups"].as<JsonArray>();
        mailSettings.mailGroupCount = min((uint8_t)groups.size(), (uint8_t)MAX_MAIL_GROUPS);
        for (uint8_t i = 0; i < mailSettings.mailGroupCount; ++i) {
            auto group = groups[i];
            mailSettings.mailGroups[i].name = group["name"].as<String>();
            mailSettings.mailGroups[i].enabled = group["enabled"].as<bool>();
            
            // Recipients
            if (group["recipients"].is<JsonArray>()) {
                auto groupRecipients = group["recipients"].as<JsonArray>();
                mailSettings.mailGroups[i].recipientCount = min((uint8_t)groupRecipients.size(), (uint8_t)10);
                for (uint8_t j = 0; j < mailSettings.mailGroups[i].recipientCount; ++j) {
                    mailSettings.mailGroups[i].recipients[j] = groupRecipients[j].as<String>();
                }
            }
            
            mailSettings.mailGroups[i].subject = group["subject"].as<String>();
            mailSettings.mailGroups[i].body = group["body"].as<String>();
            mailSettings.mailGroups[i].getUrl = group["getUrl"].as<String>();
            
            // Attachments
            if (group["attachments"].is<JsonArray>()) {
                auto groupAttachments = group["attachments"].as<JsonArray>();
                mailSettings.mailGroups[i].attachmentCount = min((uint8_t)groupAttachments.size(), (uint8_t)5);
                for (uint8_t j = 0; j < mailSettings.mailGroups[i].attachmentCount; ++j) {
                    mailSettings.mailGroups[i].attachments[j] = groupAttachments[j].as<String>();
                }
            }
        }
    }

    mail->updateConfig(mailSettings);
    server->send(200, "application/json", "{\"status\":\"ok\",\"success\":true}");
}

void WebInterface::handleMailTest() {
    // WiFi kontrolü önce
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println(F("[MAIL TEST] HATA - WiFi yok"));
        server->send(503, "application/json", "{\"error\":\"WiFi required\"}");
        return;
    }
    
    // Request body'yi al
    String body = server->arg("plain");
    Serial.println(F("========== MAIL TEST BAŞLADI =========="));
    Serial.printf("[MAIL TEST] Body: %s\n", body.c_str());
    
    // JSON parse
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    
    if (err) {
        Serial.printf("[MAIL TEST] JSON HATA: %s\n", err.c_str());
        server->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }
    
    // testType oku (char array olarak direkt karşılaştır)
    const char* testTypeRaw = doc["testType"] | "warning";
    Serial.printf("[MAIL TEST] testType: '%s'\n", testTypeRaw);
    
    // Karşılaştırma (strcmp kullan - C-style string comparison)
    bool isLS = (strcmp(testTypeRaw, "ls") == 0);
    Serial.printf("[MAIL TEST] isLS: %s\n", isLS ? "TRUE" : "FALSE");
    
    // Mail gönder
    ScheduleSnapshot snap = scheduler->snapshot();
    String errorMsg;
    bool success = false;
    unsigned long start = millis();
    
    if (isLS) {
        Serial.println(F("[MAIL TEST] >>> LEBENSSPUR TEST ÇAĞRILIYOR <<<"));
        success = mail->sendFinalTest(snap, errorMsg);
    } else {
        Serial.println(F("[MAIL TEST] >>> WARNING TEST ÇAĞRILIYOR <<<"));
        success = mail->sendWarningTest(snap, errorMsg);
    }
    
    unsigned long elapsed = millis() - start;
    Serial.printf("[MAIL TEST] Sonuç: %s (%lu ms)\n", success ? "OK" : "FAIL", elapsed);
    Serial.println(F("========== MAIL TEST BİTTİ =========="));
    
    // Response
    if (success) {
        server->send(200, "application/json", "{\"status\":\"ok\"}");
    } else {
        Serial.printf("[MAIL TEST] Hata: %s\n", errorMsg.c_str());
        server->send(500, "application/json", "{\"error\":\"" + errorMsg + "\"}");
    }
}

void WebInterface::handleWiFiGet() {
    WiFiSettings wifi = network->getConfig();
    JsonDocument doc; // WiFi settings orta boyut
    doc["primarySSID"] = wifi.primarySSID;
    doc["primaryPassword"] = wifi.primaryPassword;
    doc["secondarySSID"] = wifi.secondarySSID;
    doc["secondaryPassword"] = wifi.secondaryPassword;
    doc["allowOpenNetworks"] = wifi.allowOpenNetworks;
    doc["apModeEnabled"] = wifi.apModeEnabled;
    doc["primaryStaticEnabled"] = wifi.primaryStaticEnabled;
    doc["primaryIP"] = wifi.primaryIP;
    doc["primaryGateway"] = wifi.primaryGateway;
    doc["primarySubnet"] = wifi.primarySubnet;
    doc["primaryDNS"] = wifi.primaryDNS;
    doc["primaryMDNS"] = wifi.primaryMDNS;
    doc["secondaryStaticEnabled"] = wifi.secondaryStaticEnabled;
    doc["secondaryIP"] = wifi.secondaryIP;
    doc["secondaryGateway"] = wifi.secondaryGateway;
    doc["secondarySubnet"] = wifi.secondarySubnet;
    doc["secondaryDNS"] = wifi.secondaryDNS;
    doc["secondaryMDNS"] = wifi.secondaryMDNS;
    sendJson(doc);
}

void WebInterface::handleWiFiUpdate() {
    if (!server->hasArg("plain")) { server->send(400, "application/json", "{\"error\":\"json\"}"); return; }
    JsonDocument doc; // WiFi update orta boyut
    if (deserializeJson(doc, server->arg("plain"))) { server->send(400, "application/json", "{\"error\":\"json\"}"); return; }
    
    WiFiSettings wifi = network->getConfig();
    wifi.primarySSID = doc["primarySSID"].as<String>();
    wifi.primaryPassword = doc["primaryPassword"].as<String>();
    wifi.secondarySSID = doc["secondarySSID"].as<String>();
    wifi.secondaryPassword = doc["secondaryPassword"].as<String>();
    wifi.allowOpenNetworks = doc["allowOpenNetworks"].as<bool>();
    wifi.apModeEnabled = doc["apModeEnabled"].as<bool>();
    wifi.primaryStaticEnabled = doc["primaryStaticEnabled"].as<bool>();
    wifi.primaryIP = doc["primaryIP"].as<String>();
    wifi.primaryGateway = doc["primaryGateway"].as<String>();
    wifi.primarySubnet = doc["primarySubnet"].as<String>();
    wifi.primaryDNS = doc["primaryDNS"].as<String>();
    wifi.primaryMDNS = doc["primaryMDNS"].as<String>();
    // .local suffix'ini kaldır (mDNS otomatik ekler)
    wifi.primaryMDNS.replace(".local", "");
    wifi.primaryMDNS.trim();
    Serial.printf("[WiFi] Primary mDNS ayarlandı: '%s'\n", wifi.primaryMDNS.c_str());
    
    wifi.secondaryStaticEnabled = doc["secondaryStaticEnabled"].as<bool>();
    wifi.secondaryIP = doc["secondaryIP"].as<String>();
    wifi.secondaryGateway = doc["secondaryGateway"].as<String>();
    wifi.secondarySubnet = doc["secondarySubnet"].as<String>();
    wifi.secondaryDNS = doc["secondaryDNS"].as<String>();
    wifi.secondaryMDNS = doc["secondaryMDNS"].as<String>();
    // .local suffix'ini kaldır (mDNS otomatik ekler)
    wifi.secondaryMDNS.replace(".local", "");
    wifi.secondaryMDNS.trim();
    Serial.printf("[WiFi] Secondary mDNS ayarlandı: '%s'\n", wifi.secondaryMDNS.c_str());
    
    // Ayarları kaydet
    network->setConfig(wifi);
    
    // Şu anki WiFi durumunu kontrol et
    bool isStaConnected = (WiFi.status() == WL_CONNECTED);
    
    // WiFi modunu ayarla
    if (wifi.apModeEnabled && isStaConnected) {
        // AP + STA (Dual mode)
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP(apName.c_str()); // Şifresiz AP
        if (dnsServer) dnsServer->start(53, "*", WiFi.softAPIP());
        startAPModeMDNS(); // AP Mode için mDNS başlat
        Serial.printf("[WiFi] AP modu açıldı (Dual mode): %s\n", apName.c_str());
    } else if (wifi.apModeEnabled && !isStaConnected) {
        // Sadece AP
        WiFi.mode(WIFI_AP);
        WiFi.softAP(apName.c_str()); // Şifresiz AP
        if (dnsServer) dnsServer->start(53, "*", WiFi.softAPIP());
        startAPModeMDNS(); // AP Mode için mDNS başlat
        Serial.printf("[WiFi] AP modu açıldı (Sadece AP): %s\n", apName.c_str());
    } else if (!wifi.apModeEnabled && isStaConnected) {
        // Sadece STA - AP'yi kapat
        if (dnsServer) dnsServer->stop();
        WiFi.mode(WIFI_STA);
        Serial.println(F("[WiFi] AP modu kapatıldı (Sadece STA)"));
    } else {
        // AP kapalı, STA da bağlı değil
        if (dnsServer) dnsServer->stop();
        WiFi.mode(WIFI_STA);
        Serial.println(F("[WiFi] AP modu kapatıldı, STA deneniyor"));
    }
    
    delay(100);
    
    // Yeni ayarlarla bağlantıyı dene
    network->ensureConnected(false);
    
    // mDNS'i yenile (eğer STA modunda bağlıysak, hostname değişmiş olabilir)
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println(F("[WiFi] mDNS yenileniyor..."));
        network->refreshMDNS();
    }
    
    server->send(204);
}

void WebInterface::handleWiFiScan() {
    auto list = network->scanNetworks();
    JsonDocument doc; // Network scan orta boyut
    JsonArray arr = doc["networks"].to<JsonArray>();
    String cur = WiFi.SSID();
    for (auto &net : list) {
        JsonObject item = arr.add<JsonObject>();
        item["ssid"] = net.ssid;
        item["rssi"] = net.rssi;
        item["open"] = net.open;
        item["current"] = (net.ssid == cur);
    }
    sendJson(doc);
}

void WebInterface::handleAttachmentList() {
    MailSettings mailSettings = mail->currentConfig();
    JsonDocument doc; // Attachment list orta boyut
    JsonArray arr = doc["attachments"].to<JsonArray>();
    for (uint8_t i = 0; i < mailSettings.attachmentCount; ++i) {
        JsonObject entry = arr.add<JsonObject>();
        entry["displayName"] = mailSettings.attachments[i].displayName;
        entry["storedPath"] = mailSettings.attachments[i].storedPath;
        entry["size"] = mailSettings.attachments[i].size;
        entry["forWarning"] = mailSettings.attachments[i].forWarning;
        entry["forFinal"] = mailSettings.attachments[i].forFinal;
    }
    sendJson(doc);
}

void WebInterface::handleAttachmentUpload() {
    HTTPUpload &upload = server->upload();
    
    if (upload.status == UPLOAD_FILE_START) {
        uploadContext.reset();
        
        // Klasörün var olduğundan emin ol
        if (!LittleFS.exists(store->dataFolder())) {
            LittleFS.mkdir(store->dataFolder());
        }
        
        String sanitized = upload.filename;
        sanitized.replace("..", "");
        sanitized.replace("/", "_");
        uploadContext.originalName = sanitized;
        String stored = store->dataFolder() + "/" + String(millis()) + "_" + sanitized;
        
        uploadContext.file = LittleFS.open(stored, "w");
        if (!uploadContext.file) {
            return;
        }
        uploadContext.storedPath = stored;
        uploadContext.written = 0;
        
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (!uploadContext.file) return;
        
        // Dosya boyutu kontrolü (300 KB per group)
        if (uploadContext.written + upload.currentSize > MAX_UPLOAD_SIZE) {
            uploadContext.file.close();
            LittleFS.remove(uploadContext.storedPath);
            uploadContext.errorMessage = "File size exceeds 300 KB limit";
            uploadContext.storedPath = "";
            return;
        }
        uploadContext.file.write(upload.buf, upload.currentSize);
        uploadContext.written += upload.currentSize;
        
    } else if (upload.status == UPLOAD_FILE_END) {
        if (!uploadContext.file) return;
        uploadContext.file.close();
        
        // Hangi gruba upload ediliyor?
        if (!server->hasArg("groupIndex")) {
            LittleFS.remove(uploadContext.storedPath);
            uploadContext.errorMessage = "Missing groupIndex parameter";
            uploadContext.storedPath = "";
            return;
        }
        
        int groupIndex = server->arg("groupIndex").toInt();
        MailSettings mailSettings = mail->currentConfig();
        
        // Geçerli grup index kontrolü
        if (groupIndex < 0 || groupIndex >= MAX_MAIL_GROUPS || groupIndex >= mailSettings.mailGroupCount) {
            LittleFS.remove(uploadContext.storedPath);
            uploadContext.errorMessage = "Invalid groupIndex";
            uploadContext.storedPath = "";
            return;
        }
        
        MailGroup &group = mailSettings.mailGroups[groupIndex];
        
        // Grup dosya sayısı kontrolü (max 5 per group)
        if (group.attachmentCount >= MAX_ATTACHMENTS_PER_GROUP) {
            LittleFS.remove(uploadContext.storedPath);
            uploadContext.errorMessage = "Group has reached maximum file count (5)";
            uploadContext.storedPath = "";
            return;
        }
        
        // Toplam dosya boyutu kontrolü (900 KB total for all groups)
        size_t totalSize = uploadContext.written;
        for (uint8_t i = 0; i < mailSettings.mailGroupCount; i++) {
            for (uint8_t j = 0; j < mailSettings.mailGroups[i].attachmentCount; j++) {
                String path = mailSettings.mailGroups[i].attachments[j];
                if (LittleFS.exists(path)) {
                    File f = LittleFS.open(path, "r");
                    if (f) {
                        totalSize += f.size();
                        f.close();
                    }
                }
            }
        }
        
        if (totalSize > 921600) { // 900 KB = 921600 bytes
            LittleFS.remove(uploadContext.storedPath);
            uploadContext.errorMessage = "Total storage exceeded 900 KB limit";
            uploadContext.storedPath = "";
            return;
        }
        
        // Dosyayı gruba ekle
        group.attachments[group.attachmentCount++] = uploadContext.storedPath;
        mail->updateConfig(mailSettings);
        
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        if (uploadContext.file) {
            uploadContext.file.close();
            LittleFS.remove(uploadContext.storedPath);
        }
        uploadContext.reset();
    }
}

void WebInterface::handleAttachmentDelete() {
    if (!server->hasArg("path")) {
        server->send(400, "application/json", "{\"error\":\"path parametresi yok\"}");
        return;
    }
    
    String path = server->arg("path");
    MailSettings mailSettings = mail->currentConfig();
    bool removed = false;
    
    // Mail grupları içinde dosyayı ara ve sil
    for (uint8_t groupIdx = 0; groupIdx < mailSettings.mailGroupCount; groupIdx++) {
        MailGroup &group = mailSettings.mailGroups[groupIdx];
        for (uint8_t i = 0; i < group.attachmentCount; i++) {
            if (path == group.attachments[i]) {
                // Dosyayı LittleFS'den sil
                LittleFS.remove(path);
                
                // Array'den kaldır (kaydır)
                for (uint8_t j = i; j + 1 < group.attachmentCount; j++) {
                    group.attachments[j] = group.attachments[j + 1];
                }
                group.attachmentCount--;
                removed = true;
                break;
            }
        }
        if (removed) break;
    }
    
    if (removed) {
        mail->updateConfig(mailSettings);
        server->send(200, "application/json", "{\"status\":\"deleted\"}");
    } else {
        server->send(404, "application/json", "{\"error\":\"dosya bulunamadı\"}");
    }
}

void WebInterface::handleLogs() {
    JsonDocument doc; // Logs küçük
    doc["heap"] = ESP.getFreeHeap();
    doc["uptime"] = millis() / 1000;
    doc["wifiStatus"] = WiFi.status();
    doc["heapInfo"]["total"] = ESP.getHeapSize();
    doc["heapInfo"]["free"] = ESP.getFreeHeap();
    doc["heapInfo"]["minFree"] = ESP.getMinFreeHeap();
    doc["heapInfo"]["maxAlloc"] = ESP.getMaxAllocHeap();
    sendJson(doc);
}

void WebInterface::handleI18n() {
    String lang = server->arg("lang");
    if (lang.isEmpty()) {
        lang = "en";
    }
    
    const char* i18nData = nullptr;
    
    if (lang == "en") {
        i18nData = I18N_EN;
    } else if (lang == "de") {
        i18nData = I18N_DE;
    } else if (lang == "tr") {
        i18nData = I18N_TR;
    } else {
        i18nData = I18N_EN; // default to English
    }
    
    server->send(200, "application/json", String(i18nData));
}

// Custom API endpoint handlers
void WebInterface::handleAPIGet() {
    APISettings settings = store->loadAPISettings();
    
    JsonDocument doc;
    doc["enabled"] = settings.enabled;
    doc["endpoint"] = settings.endpoint;
    doc["requireToken"] = settings.requireToken;
    doc["token"] = settings.token;
    
    sendJson(doc);
}

void WebInterface::handleAPIUpdate() {
    if (!server->hasArg("plain")) {
        server->send(400, "application/json", "{\"error\":\"No data\"}");
        return;
    }
    
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, server->arg("plain"));
    
    if (error) {
        server->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }
    
    APISettings settings;
    settings.enabled = doc["enabled"] | false;
    settings.endpoint = doc["endpoint"] | "";
    settings.requireToken = doc["requireToken"] | false;
    settings.token = doc["token"] | "";
    
    // Validate endpoint - must not be empty if enabled
    if (settings.enabled && settings.endpoint.isEmpty()) {
        server->send(400, "application/json", "{\"error\":\"Endpoint cannot be empty\"}");
        return;
    }
    
    // Save settings
    store->saveAPISettings(settings);
    
    JsonDocument response;
    response["status"] = "success";
    sendJson(response);
}

void WebInterface::handleAPITrigger() {
    // This will be called via onNotFound for dynamic endpoints
    // Check if the path matches the custom API endpoint
    APISettings settings = store->loadAPISettings();
    
    if (!settings.enabled) {
        server->send(404, "application/json", "{\"error\":\"Not found\"}");
        return;
    }
    
    String uri = server->uri();
    String expectedPath = "/api/" + settings.endpoint;
    
    if (uri != expectedPath) {
        server->send(404, "application/json", "{\"error\":\"Not found\"}");
        return;
    }
    
    // Check token if required
    if (settings.requireToken) {
        String providedToken = server->hasHeader("Authorization") ? server->header("Authorization") : "";
        if (providedToken != settings.token) {
            server->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
            return;
        }
    }
    
    // Trigger virtual button
    if (scheduler != nullptr) {
        scheduler->reset();
        scheduler->start();
        
        JsonDocument response;
        response["status"] = "triggered";
        response["endpoint"] = settings.endpoint;
        sendJson(response);
    } else {
        server->send(500, "application/json", "{\"error\":\"Scheduler not available\"}");
    }
}

void WebInterface::sendJson(const JsonDocument &doc) {
    String response;
    serializeJson(doc, response);
    
    // Performance optimizations - HTTP headers
    server->sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server->sendHeader("Pragma", "no-cache");
    server->sendHeader("Expires", "0");
    server->sendHeader("Connection", "keep-alive"); // Keep connection alive
    
    server->send(200, "application/json", response);
}

void WebInterface::handleFactoryReset() {
    store->eraseAll();
    JsonDocument doc; 
    doc["status"] = "reset"; 
    sendJson(doc);
    delay(1000);
    ESP.restart();
}

void WebInterface::handleReboot() {
    JsonDocument doc; 
    doc["status"] = "rebooting"; 
    sendJson(doc);
    delay(200);
    ESP.restart();
}

void WebInterface::handleOTACheck() {
    JsonDocument doc;
    
    if (!otaManager) {
        doc["status"] = "error";
        doc["message"] = "OTA Manager not initialized";
        sendJson(doc);
        return;
    }
    
    if (!network || !network->isConnected()) {
        doc["status"] = "error";
        doc["message"] = "WiFi not connected";
        sendJson(doc);
        return;
    }
    
    Serial.println(F("[OTA] Manuel güncelleme kontrolü başlatıldı..."));
    
    // Güncelleme kontrolü yap
    bool updateFound = otaManager->checkForUpdate();
    
    if (updateFound) {
        // Güncelleme bulundu ve uygulandı - cihaz yeniden başlatılacak
        doc["status"] = "updating";
        doc["message"] = "Update found, device will restart...";
    } else {
        // Güncelleme yok veya hata
        doc["status"] = "ok";
        doc["message"] = "No update available";
        doc["currentVersion"] = FIRMWARE_VERSION;
    }
    
    sendJson(doc);
}

// ============================================
// Helper Functions
// ============================================
// NOT: getOrCreateDeviceId() benzersiz ID döndürür, getChipIdHex() sadece MAC

void WebInterface::startAPModeMDNS() {
    String chipIdStr = getOrCreateDeviceId();  // Benzersiz ID
    String apMdnsHostname = "ls-" + chipIdStr;  // ls-XXXXXXXXXXXX
    
    // NOT: WiFi.softAPsetHostname() zaten setupWiFi() içinde çağrıldı
    
    MDNS.end(); // Önceki mDNS'i durdur
    delay(100);
    if (MDNS.begin(apMdnsHostname.c_str())) {
        MDNS.addService("http", "tcp", 80);
        MDNS.addServiceTxt("http", "tcp", "version", FIRMWARE_VERSION);
        MDNS.addServiceTxt("http", "tcp", "model", "SmartKraft-LebensSpur");
        MDNS.addServiceTxt("http", "tcp", "mode", "AP");
        Serial.printf("[mDNS] ✓ AP Mode başlatıldı: %s.local (HTTP service published)\n", apMdnsHostname.c_str());
        Serial.printf("[mDNS] ✓ AP IP: %s\n", WiFi.softAPIP().toString().c_str());
        Serial.printf("[mDNS] ✓ Mobil cihazda deneyin: http://%s.local\n", apMdnsHostname.c_str());
    } else {
        Serial.println(F("[mDNS] ✗ AP Mode başlatılamadı"));
    }
}

// ============================================
// WEB SERVER HEALTH CHECK
// ============================================

bool WebInterface::isHealthy() const {
    // Server null ise sağlıksız
    if (!server) return false;
    
    // WiFi bağlı değilse zaten istekler gelemez, bu normal
    // Sadece bağlıyken timeout kontrolü yap
    if (WiFi.status() != WL_CONNECTED && WiFi.getMode() != WIFI_AP && WiFi.getMode() != WIFI_AP_STA) {
        return true; // WiFi yok, ama bu web server hatası değil
    }
    
    // Son istekten bu yana HEALTH_TIMEOUT_MS geçtiyse unhealthy
    // Not: İlk başlangıçta lastRequestTime = 0 olacak, bu timeout'a neden olmamalı
    if (lastRequestTime == 0) {
        return true; // Henüz istek gelmedi, yeni başlatılmış
    }
    
    uint32_t elapsed = millis() - lastRequestTime;
    return elapsed < HEALTH_TIMEOUT_MS;
}

uint32_t WebInterface::getLastRequestTime() const {
    return lastRequestTime;
}

void WebInterface::resetHealthCounter() {
    lastRequestTime = millis();
    requestCounter = 0;
}
