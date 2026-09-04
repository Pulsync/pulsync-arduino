/**
 * pulsync_portal — Captive portal implementation
 *
 * Uses ESP-IDF httpd server + soft-AP.
 * Serves a minimal HTML form for WiFi credential entry.
 * Implements DNS hijack for captive portal detection.
 */

#include "pulsync_portal.h"
#include "pulsync_wifi.h"
#include "pulsync_version.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "lwip/sockets.h"
#include "mbedtls/sha256.h"

static const char *TAG = "pulsync_portal";

/* ---------- HTML form ---------- */

static const char PORTAL_HTML[] =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>PulSync Setup</title>"
    "<style>"
    "*{margin:0;padding:0;box-sizing:border-box}"
    "body{font-family:-apple-system,sans-serif;padding:20px;background:#0B1120;color:#F1F5F9;min-height:100vh}"
    ".c{max-width:400px;margin:0 auto}"
    ".logo{text-align:center;margin-bottom:24px;font-size:1.4em;font-weight:700}"
    ".logo span{color:#F97316}"
    "form{background:#151F32;padding:24px;border-radius:12px;border:1px solid #1E2D45}"
    "label{display:block;margin-bottom:4px;font-size:0.8em;color:#94A3B8}"
    "input{width:100%;padding:11px 14px;margin-bottom:14px;border:1px solid #1E2D45;border-radius:8px;"
    "background:#1A2740;color:#F1F5F9;font-size:0.9em}"
    "input:focus{border-color:#F97316;outline:none}"
    ".row{display:flex;gap:8px;align-items:flex-end}"
    ".row input{flex:1;margin-bottom:0}"
    ".row .btn-s{padding:11px 14px;background:#1A2740;color:#94A3B8;border:1px solid #1E2D45;"
    "border-radius:8px;font-size:0.75em;cursor:pointer;white-space:nowrap}"
    ".row .btn-s:hover{border-color:#F97316;color:#F97316}"
    "button[type=submit]{width:100%;padding:13px;background:#F97316;color:#fff;border:none;"
    "border-radius:8px;font-size:1em;cursor:pointer;font-weight:600;margin-top:4px}"
    "button[type=submit]:hover{background:#EA580C}"
    ".note{text-align:center;color:#64748B;font-size:0.75em;margin-top:16px}"
    ".divider{border-top:1px solid #1E2D45;margin:16px 0;}"
    "#server-row{display:none}"
    "</style></head><body><div class='c'>"
    "<div class='logo'><span>Pul</span>Sync</div>"
    "<form method='POST' action='/save'>"
    "<label>WiFi Network</label>"
    "<div id='wifi-list'></div>"
    "<input type='text' name='ssid' id='ssid-input' placeholder='Your WiFi SSID' required>"
    "<button type='button' onclick='scanWifi(event)' id='scan-btn' style='width:100%;padding:10px;background:#1A2740;color:#94A3B8;"
    "border:1px solid #1E2D45;border-radius:8px;font-size:0.8em;cursor:pointer;margin-bottom:14px'>Scan Networks</button>"
    "<label>Password</label>"
    "<input type='password' name='pass' placeholder='WiFi Password'>"
    "<div class='divider'></div>"
    "<label>Pairing Code</label>"
    "<input type='text' name='code' placeholder='PUL-XXXXXX or PLF-XXXXXX' required>"
    "<label>Device Name <span style='color:#64748B'>(optional)</span></label>"
    "<input type='text' name='name' placeholder='e.g. Kitchen Sensor'>"
    "<div class='divider'></div>"
    "<div style='margin-bottom:14px'>"
    "<div style='display:flex;justify-content:space-between;align-items:center'>"
    "<label style='margin:0'>Server</label>"
    "<span class='btn-s' onclick=\"document.getElementById('server-row').style.display='block';this.style.display='none'\">Change</span>"
    "</div>"
    "<div style='font-size:0.8em;color:#F97316;margin-top:4px'>pulsync.in</div>"
    "<div id='server-row' style='margin-top:8px'>"
    "<input type='text' name='server' placeholder='pulsync.in' value=''>"
    "</div>"
    "</div>"
    "<button type='submit'>Connect &amp; Register</button>"
    "</form>"
    "<p class='note'>Device will connect to WiFi and register with your server.</p>"
    "</div>"
    "<script>"
    "try{"
    "var p=new URLSearchParams(location.search);"
    "if(p.get('code'))document.querySelector('[name=code]').value=p.get('code');"
    "if(p.get('server')){document.querySelector('[name=server]').value=p.get('server');"
    "document.getElementById('server-row').style.display='block';}"
    "if(p.get('name'))document.querySelector('[name=name]').value=p.get('name');"
    "}catch(e){}"
    "</script>"
    "<script>"
    "function scanWifi(e){"
    "var btn=document.getElementById('scan-btn');btn.textContent='Scanning...';"
    "fetch('/scan').then(function(r){return r.json()}).then(function(list){"
    "var h='';for(var i=0;i<list.length;i++){"
    "h+='<div class=wfi data-ssid=\"'+list[i].ssid+'\">'+list[i].ssid+' <small>'+list[i].rssi+'dBm</small></div>';}"
    "document.getElementById('wifi-list').innerHTML=h;btn.textContent='Scan Networks';"
    "var items=document.querySelectorAll('.wfi');for(var j=0;j<items.length;j++){"
    "items[j].onclick=function(){document.getElementById('ssid-input').value=this.getAttribute('data-ssid');document.getElementById('wifi-list').innerHTML='';};}"
    "}).catch(function(){btn.textContent='Scan failed';});"
    "}"
    "scanWifi();"
    "</script>"
    "<style>.wfi{padding:10px 14px;background:#1A2740;border:1px solid #1E2D45;border-radius:6px;margin-bottom:6px;cursor:pointer}.wfi:hover{border-color:#F97316}.wfi small{color:#64748B;float:right}</style>"
    "</body></html>";

static const char PORTAL_SUCCESS_HTML[] =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>PulSync - Saved</title>"
    "<style>"
    "body{font-family:-apple-system,sans-serif;margin:0;padding:40px;background:#0B1120;"
    "color:#F1F5F9;text-align:center;}"
    "h1{color:#22C55E;margin-bottom:12px;}"
    "p{color:#94A3B8;font-size:0.9em;}"
    "</style></head><body>"
    "<h1>&#x2713; Saved</h1>"
    "<p>Connecting to WiFi and registering device...</p>"
    "<p style='color:#64748B;margin-top:16px'>You can close this page.</p>"
    "</body></html>";

/* Login page for the gated config menu (enrolled device). AP is open; this is
 * an app-layer password prompt. On success the server sets a session cookie and
 * the menu becomes reachable. */
static const char PORTAL_LOGIN_HTML[] =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>PulSync Config — Login</title>"
    "<style>"
    "*{margin:0;padding:0;box-sizing:border-box}"
    "body{font-family:-apple-system,sans-serif;padding:20px;background:#0B1120;color:#F1F5F9;min-height:100vh;display:flex;align-items:center;justify-content:center}"
    ".c{max-width:360px;width:100%}"
    ".logo{text-align:center;margin-bottom:20px;font-size:1.4em;font-weight:700}"
    ".logo span{color:#F97316}"
    ".card{background:#151F32;padding:24px;border-radius:12px;border:1px solid #1E2D45}"
    "label{display:block;margin-bottom:4px;font-size:0.8em;color:#94A3B8}"
    "input{width:100%;padding:11px 14px;margin-bottom:14px;border:1px solid #1E2D45;border-radius:8px;"
    "background:#1A2740;color:#F1F5F9;font-size:0.9em}"
    "input:focus{border-color:#F97316;outline:none}"
    "button{width:100%;padding:13px;background:#F97316;color:#fff;border:none;border-radius:8px;font-size:1em;cursor:pointer;font-weight:600}"
    "button:hover{background:#EA580C}"
    ".msg{font-size:0.78em;margin-bottom:10px;min-height:14px;color:#EF4444;text-align:center}"
    ".note{text-align:center;color:#64748B;font-size:0.72em;margin-top:14px;line-height:1.5}"
    "</style></head><body><div class='c'>"
    "<div class='logo'><span>Pul</span>Sync <span style='color:#64748B;font-size:0.6em'>config</span></div>"
    "<div class='card'>"
    "<label>Config Password</label>"
    "<input type='password' id='pw' placeholder='Enter device config password' autofocus>"
    "<div id='msg' class='msg'></div>"
    "<button type='button' onclick='doLogin()'>Unlock</button>"
    "<p class='note'>This device is enrolled. Reveal its config password from "
    "your Pulsync dashboard. Lost it? Hold the device button to factory reset.</p>"
    "</div></div>"
    "<script>"
    "function doLogin(){var p=document.getElementById('pw').value;if(!p){return;}"
    "var m=document.getElementById('msg');m.textContent='Checking...';m.style.color='#94A3B8';"
    "fetch('/login',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'pw='+encodeURIComponent(p)})"
    ".then(function(r){return r.json()}).then(function(d){"
    "if(d&&d.ok){location.href='/';}else{m.style.color='#EF4444';m.textContent='Incorrect password';}})"
    ".catch(function(){m.style.color='#EF4444';m.textContent='Login failed';});}"
    "document.getElementById('pw').addEventListener('keydown',function(e){if(e.key==='Enter')doLogin();});"
    "</script></body></html>";

/* Config portal — management menu SPA.
 * Served in three parts so the firmware version is inserted at runtime:
 *   CONFIG_HTML  <version>  CONFIG_HTML_TAIL
 * The page is a small client-side menu: Networks / Server / Danger, each with
 * its own Save, plus an Exit button that closes the portal. All actions are
 * AJAX against /wifi, /wifi/save, /server/save, /scan, /factory-reset, /exit. */
static const char CONFIG_HTML[] =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>PulSync Config</title>"
    "<style>"
    "*{margin:0;padding:0;box-sizing:border-box}"
    "body{font-family:-apple-system,sans-serif;padding:20px;background:#0B1120;color:#F1F5F9;min-height:100vh}"
    ".c{max-width:420px;margin:0 auto}"
    ".logo{text-align:center;margin-bottom:16px;font-size:1.3em;font-weight:700}"
    ".logo span{color:#F97316}"
    ".card{background:#151F32;padding:18px;border-radius:12px;border:1px solid #1E2D45;margin-bottom:14px}"
    ".card h3{font-size:0.8em;text-transform:uppercase;letter-spacing:0.5px;color:#94A3B8;margin-bottom:12px;display:flex;justify-content:space-between;align-items:center}"
    "label{display:block;margin-bottom:4px;font-size:0.8em;color:#94A3B8}"
    "input{width:100%;padding:11px 14px;margin-bottom:12px;border:1px solid #1E2D45;border-radius:8px;"
    "background:#1A2740;color:#F1F5F9;font-size:0.9em}"
    "input:focus{border-color:#F97316;outline:none}"
    "button{padding:12px;background:#F97316;color:#fff;border:none;border-radius:8px;"
    "font-size:0.9em;cursor:pointer;font-weight:600;width:100%}"
    "button:hover{background:#EA580C}"
    ".btn-sec{background:#1A2740;color:#94A3B8;border:1px solid #1E2D45;font-weight:400}"
    ".btn-sec:hover{background:#22304d;color:#F1F5F9}"
    ".btn-danger{background:transparent;color:#EF4444;border:1px solid #EF4444}"
    ".btn-danger:hover{background:#EF4444;color:#fff}"
    ".btn-sm{width:auto;padding:6px 10px;font-size:0.75em}"
    ".net{display:flex;align-items:center;gap:8px;padding:9px 12px;background:#1A2740;border:1px solid #1E2D45;border-radius:8px;margin-bottom:6px}"
    ".net .nm{flex:1;font-size:0.88em;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
    ".net .pos{color:#64748B;font-size:0.7em;min-width:16px}"
    ".net .ic{background:#22304d;border:1px solid #1E2D45;color:#94A3B8;border-radius:6px;width:30px;height:30px;font-size:0.9em;padding:0;line-height:1}"
    ".net .ic:hover{border-color:#F97316;color:#F97316}"
    ".net .ic.rm:hover{border-color:#EF4444;color:#EF4444;background:#EF4444;color:#fff}"
    ".wfi{padding:9px 12px;background:#1A2740;border:1px solid #1E2D45;border-radius:6px;margin-bottom:6px;cursor:pointer;font-size:0.85em}"
    ".wfi:hover{border-color:#F97316}.wfi small{color:#64748B;float:right}"
    ".info{font-size:0.75em;color:#64748B;margin-bottom:6px}"
    ".info b{color:#F1F5F9}"
    ".row2{display:flex;gap:8px}.row2>*{flex:1}"
    ".msg{font-size:0.75em;margin:4px 0 8px;min-height:14px}"
    ".msg.ok{color:#22C55E}.msg.err{color:#EF4444}"
    ".full{margin-top:8px}"
    ".empty{color:#64748B;font-size:0.8em;padding:6px 0}"
    "</style></head><body><div class='c'>"
    "<div class='logo'><span>Pul</span>Sync <span style='color:#64748B;font-size:0.6em'>config</span></div>"
    "<div class='card'>"
    "<h3>Device</h3>"
    "<div class='info'>Firmware: <b>";
static const char CONFIG_HTML_TAIL[] =
    "</b></div>"
    "<div class='info'>Status: <b>Enrolled</b></div>"
    "</div>"

    /* --- Networks --- */
    "<div class='card'>"
    "<h3>WiFi Networks <span style='color:#64748B;font-weight:400' id='ncount'></span></h3>"
    "<div id='net-list'><div class='empty'>Loading...</div></div>"
    "<div id='net-msg' class='msg'></div>"
    "<div id='add-box' style='margin-top:10px;border-top:1px solid #1E2D45;padding-top:12px'>"
    "<label>Add network</label>"
    "<div id='scan-list'></div>"
    "<input type='text' id='a-ssid' placeholder='SSID'>"
    "<input type='password' id='a-pass' placeholder='Password (blank = open)'>"
    "<div class='row2'>"
    "<button type='button' class='btn-sec' onclick='scanWifi()' id='scan-btn'>Scan</button>"
    "<button type='button' onclick='addNet()'>Add</button>"
    "</div></div>"
    "<button type='button' class='full' onclick='saveNets()'>Save Networks</button>"
    "</div>"

    /* --- Server --- */
    "<div class='card'>"
    "<h3>Server</h3>"
    "<input type='text' id='srv' placeholder='host:port'>"
    "<div id='srv-msg' class='msg'></div>"
    "<button type='button' onclick='saveSrv()'>Save Server</button>"
    "</div>"

    /* --- Re-pair --- */
    "<div class='card'>"
    "<h3>Re-pair Device</h3>"
    "<p class='info'>Enter a new pairing code to move this device to a different "
    "project or account. The current registration is cleared and the device "
    "re-enrolls with a fresh token.</p>"
    "<input type='text' id='rp-code' placeholder='PUL-XXXXXX or PLF-XXXXXX'>"
    "<div id='rp-msg' class='msg'></div>"
    "<button type='button' onclick='doRepair()'>Re-pair</button>"
    "</div>"

    /* --- Danger --- */
    "<div class='card'>"
    "<h3 style='color:#EF4444'>Danger Zone</h3>"
    "<p class='info'>Erases all settings (WiFi, credentials, config). Device needs full re-setup.</p>"
    "<button type='button' class='btn-danger' onclick=\"if(confirm('Erase all settings?'))location.href='/factory-reset'\">Factory Reset</button>"
    "</div>"

    "<button type='button' class='btn-sec' onclick='doExit()'>Exit &amp; Reconnect</button>"
    "</div>"

    "<script>"
    /* in-memory working list of {ssid, hasPw, newPw} */
    "var nets=[];"
    "function esc(s){return (s||'').replace(/[<>&\"]/g,function(c){return {'<':'&lt;','>':'&gt;','&':'&amp;','\"':'&quot;'}[c];});}"
    "function setMsg(id,t,ok){var e=document.getElementById(id);e.textContent=t;e.className='msg '+(ok?'ok':'err');}"
    "function renderNets(){var el=document.getElementById('net-list');document.getElementById('ncount').textContent='('+nets.length+'/3)';"
    "if(!nets.length){el.innerHTML='<div class=empty>No networks saved.</div>';return;}"
    "var h='';for(var i=0;i<nets.length;i++){h+='<div class=net><span class=pos>'+(i+1)+'</span><span class=nm>'+esc(nets[i].ssid)+'</span>'"
    "+'<button class=\"ic\" onclick=\"mv('+i+',-1)\" '+(i==0?'disabled':'')+'>&uarr;</button>'"
    "+'<button class=\"ic\" onclick=\"mv('+i+',1)\" '+(i==nets.length-1?'disabled':'')+'>&darr;</button>'"
    "+'<button class=\"ic rm\" onclick=\"rm('+i+')\">&times;</button></div>';}"
    "el.innerHTML=h;}"
    "function mv(i,d){var j=i+d;if(j<0||j>=nets.length)return;var t=nets[i];nets[i]=nets[j];nets[j]=t;renderNets();}"
    "function rm(i){nets.splice(i,1);renderNets();}"
    "function addNet(){var s=document.getElementById('a-ssid').value.trim();if(!s){setMsg('net-msg','Enter an SSID',0);return;}"
    "if(nets.length>=3){setMsg('net-msg','Max 3 networks. Remove one first.',0);return;}"
    "for(var i=0;i<nets.length;i++){if(nets[i].ssid===s){setMsg('net-msg','Already in list',0);return;}}"
    "nets.push({ssid:s,hasPw:false,newPw:document.getElementById('a-pass').value});"
    "document.getElementById('a-ssid').value='';document.getElementById('a-pass').value='';"
    "document.getElementById('scan-list').innerHTML='';setMsg('net-msg','',1);renderNets();}"
    "function loadNets(){fetch('/wifi').then(function(r){return r.json()}).then(function(l){"
    "nets=l.map(function(x){return {ssid:x.ssid,hasPw:true,newPw:''};});renderNets();}).catch(function(){setMsg('net-msg','Load failed',0);});}"
    "function saveNets(){var body='n='+nets.length;for(var i=0;i<nets.length;i++){"
    "body+='&ssid'+i+'='+encodeURIComponent(nets[i].ssid)+'&pw'+i+'='+encodeURIComponent(nets[i].hasPw?'':(nets[i].newPw||''));}"
    "fetch('/wifi/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})"
    ".then(function(r){return r.json()}).then(function(l){nets=l.map(function(x){return {ssid:x.ssid,hasPw:true,newPw:''};});renderNets();setMsg('net-msg','Saved',1);})"
    ".catch(function(){setMsg('net-msg','Save failed',0);});}"
    "function saveSrv(){var v=document.getElementById('srv').value.trim();"
    "fetch('/server/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'server='+encodeURIComponent(v)})"
    ".then(function(){setMsg('srv-msg','Saved',1);}).catch(function(){setMsg('srv-msg','Save failed',0);});}"
    "function doRepair(){var c=document.getElementById('rp-code').value.trim();"
    "if(!c){setMsg('rp-msg','Enter a pairing code',0);return;}"
    "if(!confirm('Re-pair with '+c+'? This clears the current registration.'))return;"
    "setMsg('rp-msg','Re-pairing...',1);"
    "fetch('/re-pair',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'code='+encodeURIComponent(c)})"
    ".then(function(r){return r.json()}).then(function(d){"
    "if(d&&d.ok){document.body.innerHTML='<div style=\"text-align:center;padding:60px;font-family:sans-serif\"><h2 style=\"color:#22C55E\">Re-pairing</h2><p style=\"color:#94A3B8\">Device is re-enrolling with the new code. You can close this page.</p></div>';}"
    "else{setMsg('rp-msg',(d&&d.error)||'Re-pair failed',0);}})"
    ".catch(function(){setMsg('rp-msg','Re-pair failed',0);});}"
    "function scanWifi(){var b=document.getElementById('scan-btn');b.textContent='Scanning...';"
    "fetch('/scan').then(function(r){return r.json()}).then(function(l){var h='';"
    "for(var i=0;i<l.length;i++){h+='<div class=wfi data-ssid=\"'+esc(l[i].ssid)+'\">'+esc(l[i].ssid)+' <small>'+l[i].rssi+'dBm</small></div>';}"
    "document.getElementById('scan-list').innerHTML=h;b.textContent='Scan';"
    "var it=document.querySelectorAll('#scan-list .wfi');for(var j=0;j<it.length;j++){it[j].onclick=function(){"
    "document.getElementById('a-ssid').value=this.getAttribute('data-ssid');document.getElementById('scan-list').innerHTML='';};}"
    "}).catch(function(){b.textContent='Scan failed';});}"
    "function doExit(){fetch('/exit',{method:'GET'});document.body.innerHTML='<div style=\"text-align:center;padding:60px;font-family:sans-serif\"><h2 style=\"color:#22C55E\">Done</h2><p style=\"color:#94A3B8\">Reconnecting to WiFi. You can close this page.</p></div>';}"
    "loadNets();"
    "</script>"
    "</body></html>";

/* ---------- State ---------- */

static pulsync_portal_state_t s_state = PULSYNC_PORTAL_STOPPED;
static pulsync_portal_mode_t s_mode = PULSYNC_PORTAL_MODE_ENROLL;
static pulsync_portal_creds_cb_t s_creds_cb = NULL;
static pulsync_portal_reset_cb_t s_reset_cb = NULL;
static pulsync_portal_wifi_save_cb_t s_wifi_save_cb = NULL;
static pulsync_portal_server_save_cb_t s_server_save_cb = NULL;
static pulsync_portal_exit_cb_t s_exit_cb = NULL;
static pulsync_portal_repair_cb_t s_repair_cb = NULL;

/* Snapshot of saved networks for the /wifi endpoint (SSIDs only exposed). */
static pulsync_wifi_cred_t s_wifi_list[PULSYNC_WIFI_MAX_CREDS];
static int s_wifi_list_count = 0;
static httpd_handle_t s_server = NULL;
static esp_netif_t *s_ap_netif = NULL;
static char s_ap_ssid[32] = {0};
static int s_dns_socket = -1;
static TaskHandle_t s_dns_task = NULL;

/* ---------- Config-menu password gate ----------
 * s_gate_hash holds the SHA-256 hex of the config password (server-issued).
 * The gate is active only in CONFIG mode when the device is enrolled and a hash
 * is present. s_session_token is a random cookie value minted on successful
 * login; a request is authed if its Cookie carries this token. */
static char s_gate_hash[72] = {0};       /* SHA-256 hex (64) + null, room to spare */
static bool s_gate_enrolled = false;
static char s_session_token[33] = {0};   /* 32 hex chars + null; empty = no session */
#define PULSYNC_PORTAL_COOKIE_NAME "ps_cfg"

/* ---------- URL decode helper ---------- */

static void url_decode(const char *src, char *dst, size_t dst_maxlen) {
    size_t di = 0;
    while (*src && di < dst_maxlen - 1) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], 0 };
            dst[di++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            dst[di++] = ' ';
            src++;
        } else {
            dst[di++] = *src++;
        }
    }
    dst[di] = '\0';
}

/* ---------- Password gate helpers ---------- */

/** Compute SHA-256 of `in` and write it as lowercase hex (64 chars + null).
 *  Uses the one-shot mbedtls_sha256(), which has a stable signature across
 *  mbedtls 2.28 (ESP-IDF 4.x) and 3.x (ESP-IDF 5.x / Arduino-ESP32 3.x).
 *  Last arg 0 = SHA-256 (not SHA-224). */
static void sha256_hex(const char *in, size_t in_len, char out_hex[65]) {
    uint8_t digest[32];
    mbedtls_sha256((const unsigned char *)in, in_len, digest, 0);
    static const char *hexd = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out_hex[i * 2]     = hexd[(digest[i] >> 4) & 0xF];
        out_hex[i * 2 + 1] = hexd[digest[i] & 0xF];
    }
    out_hex[64] = '\0';
}

/** Length-independent-ish constant-time string compare (avoids early-out on
 *  the first differing byte). Both are hex strings of known length. */
static bool ct_streq(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < la; i++) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

/** True if the portal's sensitive actions should be gated.
 *
 * Keyed on ENROLLMENT + a stored password hash, NOT on portal mode. Rationale:
 * an enrolled device that loses WiFi re-opens the portal in ENROLL mode, and
 * gating on CONFIG mode alone would leave /wifi/save, /server/save, /re-pair
 * etc. reachable by anyone on the open AP (deauth → join → hijack). A fresh,
 * unenrolled device has no hash (and s_gate_enrolled is false), so it stays
 * fully open — the pairing code is its gate. The enroll FORM (root page) is
 * still served in ENROLL mode; only the mutating actions demand the session. */
static bool gate_active(void) {
    return s_gate_enrolled && s_gate_hash[0] != '\0';
}

/** True if the request carries a valid session cookie (or gate not active). */
static bool request_authed(httpd_req_t *req) {
    if (!gate_active()) return true;          /* nothing to gate */
    if (s_session_token[0] == '\0') return false;  /* no session minted yet */

    size_t clen = httpd_req_get_hdr_value_len(req, "Cookie");
    if (clen == 0 || clen > 512) return false;
    char cookie[520];
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie)) != ESP_OK) {
        return false;
    }
    /* Look for "<name>=<session token>" anywhere in the Cookie header. */
    char needle[64];
    int nl = snprintf(needle, sizeof(needle), "%s=%s",
                      PULSYNC_PORTAL_COOKIE_NAME, s_session_token);
    if (nl <= 0) return false;
    return strstr(cookie, needle) != NULL;
}

/** Mint a fresh random session token (32 hex chars). */
static void mint_session(void) {
    static const char *hexd = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        s_session_token[i] = hexd[esp_random() & 0xF];
    }
    s_session_token[32] = '\0';
}

/** Serve the login page (200 OK). */
static esp_err_t send_login_page(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PORTAL_LOGIN_HTML, strlen(PORTAL_LOGIN_HTML));
    return ESP_OK;
}

/** Reject a gated API request that lacks a valid session (401 JSON). Returns
 *  true if the request was rejected (caller should return ESP_OK immediately),
 *  false if the request is authed / not gated and may proceed. */
static bool gate_reject_api(httpd_req_t *req) {
    if (request_authed(req)) return false;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Locked — log in first\"}");
    return true;
}

/** Parse form body "ssid=xxx&pass=yyy&code=zzz&name=www&server=sss" */
static bool parse_form_body(const char *body, char *ssid, size_t ssid_len,
                            char *pass, size_t pass_len,
                            char *code, size_t code_len,
                            char *name, size_t name_len,
                            char *server, size_t server_len) {
    ssid[0] = '\0';
    pass[0] = '\0';
    code[0] = '\0';
    name[0] = '\0';
    server[0] = '\0';

    const char *p = body;
    while (p && *p) {
        const char *eq = strchr(p, '=');
        if (!eq) break;

        const char *amp = strchr(eq + 1, '&');
        size_t key_len = (size_t)(eq - p);
        size_t val_len = amp ? (size_t)(amp - eq - 1) : strlen(eq + 1);

        char val_raw[128] = {0};
        if (val_len >= sizeof(val_raw)) val_len = sizeof(val_raw) - 1;
        memcpy(val_raw, eq + 1, val_len);

        if (key_len == 4 && strncmp(p, "ssid", 4) == 0) {
            url_decode(val_raw, ssid, ssid_len);
        } else if (key_len == 4 && strncmp(p, "pass", 4) == 0) {
            url_decode(val_raw, pass, pass_len);
        } else if (key_len == 4 && strncmp(p, "code", 4) == 0) {
            url_decode(val_raw, code, code_len);
        } else if (key_len == 4 && strncmp(p, "name", 4) == 0) {
            url_decode(val_raw, name, name_len);
        } else if (key_len == 6 && strncmp(p, "server", 6) == 0) {
            url_decode(val_raw, server, server_len);
        }

        if (amp) {
            p = amp + 1;
        } else {
            break;
        }
    }

    return ssid[0] != '\0';
}

/* ---------- HTTP handlers ---------- */

static esp_err_t handler_get_root(httpd_req_t *req) {
    /* Gated config menu: show the login page until the session is authed. */
    if (gate_active() && !request_authed(req)) {
        return send_login_page(req);
    }
    httpd_resp_set_type(req, "text/html");
    if (s_mode == PULSYNC_PORTAL_MODE_CONFIG) {
        /* Send in three chunks so the firmware version is resolved at runtime. */
        httpd_resp_send_chunk(req, CONFIG_HTML, HTTPD_RESP_USE_STRLEN);
        httpd_resp_send_chunk(req, pulsync_version_get(), HTTPD_RESP_USE_STRLEN);
        httpd_resp_send_chunk(req, CONFIG_HTML_TAIL, HTTPD_RESP_USE_STRLEN);
        httpd_resp_send_chunk(req, NULL, 0);  /* end response */
    } else {
        httpd_resp_send(req, PORTAL_HTML, strlen(PORTAL_HTML));
    }
    return ESP_OK;
}

/* Factory reset handler (config mode) */
static esp_err_t handler_factory_reset(httpd_req_t *req) {
    /* Gated: the portal's factory reset sits behind the config password. The
     * physical button remains the ungated master reset for lost-password
     * recovery. */
    if (gate_active() && !request_authed(req)) {
        return send_login_page(req);
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, "<html><body style='font-family:sans-serif;background:#0B1120;color:#F1F5F9;text-align:center;padding:40px'><h1 style='color:#EF4444'>Factory Reset</h1><p>Device is erasing settings and restarting...</p></body></html>");
    if (s_reset_cb) {
        /* Delay slightly so response is sent first */
        vTaskDelay(pdMS_TO_TICKS(500));
        s_reset_cb();
    }
    return ESP_OK;
}

static esp_err_t handler_post_save(httpd_req_t *req) {
    /* Enroll-form submit. Gated when the device is already enrolled (an enrolled
     * device that dropped WiFi re-opens this form) — a fresh device is not
     * gated, so first-time setup stays open. */
    if (gate_active() && !request_authed(req)) {
        return send_login_page(req);
    }
    char body[512] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char ssid[33] = {0};
    char pass[65] = {0};
    char code[16] = {0};
    char name[33] = {0};
    char server[128] = {0};

    if (!parse_form_body(body, ssid, sizeof(ssid), pass, sizeof(pass),
                         code, sizeof(code), name, sizeof(name),
                         server, sizeof(server))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Portal submit: SSID='%s' code='%s' name='%s' server='%s'",
             ssid, code, name, server[0] ? server : "(default)");

    /* Send success page first */
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PORTAL_SUCCESS_HTML, strlen(PORTAL_SUCCESS_HTML));

    s_state = PULSYNC_PORTAL_CREDENTIALS_RECEIVED;

    /* Notify callback with all fields */
    if (s_creds_cb) {
        s_creds_cb(ssid, pass, server, code, name);
    }

    return ESP_OK;
}

/* Captive portal detection — redirect all unknown requests to root, preserving query params */
static esp_err_t handler_catch_all(httpd_req_t *req) {
    /* Build redirect URL preserving query string */
    char location[256] = "http://192.168.4.1/";
    const char *query_start = strchr(req->uri, '?');
    if (query_start) {
        snprintf(location, sizeof(location), "http://192.168.4.1/%s", query_start);
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* WiFi scan endpoint — returns JSON array of nearby networks */
static esp_err_t handler_get_scan(httpd_req_t *req) {
    if (gate_reject_api(req)) return ESP_OK;
    /* Trigger a scan (blocking, takes ~1-3 seconds) */
    wifi_scan_config_t scan_config = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };
    esp_wifi_scan_start(&scan_config, true);

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 20) ap_count = 20;  /* cap for memory */

    wifi_ap_record_t *ap_list = (wifi_ap_record_t *)malloc(ap_count * sizeof(wifi_ap_record_t));
    if (!ap_list) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }

    esp_wifi_scan_get_ap_records(&ap_count, ap_list);

    /* Build JSON response */
    char *json = (char *)malloc(ap_count * 64 + 16);
    if (!json) {
        free(ap_list);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }

    int pos = 0;
    pos += sprintf(json + pos, "[");
    for (int i = 0; i < ap_count; i++) {
        if (i > 0) pos += sprintf(json + pos, ",");
        const char *auth = "open";
        if (ap_list[i].authmode == WIFI_AUTH_WPA2_PSK) auth = "wpa2";
        else if (ap_list[i].authmode == WIFI_AUTH_WPA_PSK) auth = "wpa";
        else if (ap_list[i].authmode == WIFI_AUTH_WPA3_PSK) auth = "wpa3";
        else if (ap_list[i].authmode != WIFI_AUTH_OPEN) auth = "secured";
        pos += sprintf(json + pos, "{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":\"%s\"}",
                       (char *)ap_list[i].ssid, ap_list[i].rssi, auth);
    }
    pos += sprintf(json + pos, "]");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);

    free(json);
    free(ap_list);
    return ESP_OK;
}

/* GET /wifi — return saved networks as JSON (SSIDs + position, no passwords) */
static esp_err_t handler_get_wifi(httpd_req_t *req) {
    if (gate_reject_api(req)) return ESP_OK;
    char json[PULSYNC_WIFI_MAX_CREDS * 64 + 8];
    int pos = 0;
    pos += sprintf(json + pos, "[");
    for (int i = 0; i < s_wifi_list_count; i++) {
        if (i > 0) pos += sprintf(json + pos, ",");
        /* SSID is <=32 chars; escape double-quotes/backslashes defensively. */
        char safe[PULSYNC_WIFI_STORE_SSID_MAXLEN * 2];
        int sp = 0;
        for (const char *c = s_wifi_list[i].ssid; *c && sp < (int)sizeof(safe) - 2; c++) {
            if (*c == '"' || *c == '\\') safe[sp++] = '\\';
            safe[sp++] = *c;
        }
        safe[sp] = '\0';
        pos += sprintf(json + pos, "{\"ssid\":\"%s\",\"pos\":%d}", safe, i);
    }
    pos += sprintf(json + pos, "]");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

/* Extract a single urlencoded field value from a form body into out. */
static bool form_field(const char *body, const char *key, char *out, size_t out_len) {
    out[0] = '\0';
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        const char *eq = strchr(p, '=');
        if (!eq) break;
        size_t this_klen = (size_t)(eq - p);
        const char *amp = strchr(eq + 1, '&');
        size_t vlen = amp ? (size_t)(amp - eq - 1) : strlen(eq + 1);
        if (this_klen == klen && strncmp(p, key, klen) == 0) {
            char raw[128] = {0};
            if (vlen >= sizeof(raw)) vlen = sizeof(raw) - 1;
            memcpy(raw, eq + 1, vlen);
            url_decode(raw, out, out_len);
            return true;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return false;
}

/* POST /wifi/save — replace the whole network list.
 * Body: n=<count>&ssid0=..&pw0=..&ssid1=..&pw1=..  (pw empty = keep current) */
static esp_err_t handler_post_wifi_save(httpd_req_t *req) {
    if (gate_reject_api(req)) return ESP_OK;
    char body[1024] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char nbuf[8] = {0};
    int n = 0;
    if (form_field(body, "n", nbuf, sizeof(nbuf))) n = atoi(nbuf);
    if (n < 0) n = 0;
    if (n > PULSYNC_WIFI_MAX_CREDS) n = PULSYNC_WIFI_MAX_CREDS;

    pulsync_wifi_cred_t creds[PULSYNC_WIFI_MAX_CREDS];
    memset(creds, 0, sizeof(creds));
    int count = 0;
    for (int i = 0; i < n; i++) {
        char kssid[12], kpw[12];
        snprintf(kssid, sizeof(kssid), "ssid%d", i);
        snprintf(kpw, sizeof(kpw), "pw%d", i);
        char ssid[PULSYNC_WIFI_STORE_SSID_MAXLEN] = {0};
        char pw[PULSYNC_WIFI_STORE_PASS_MAXLEN] = {0};
        form_field(body, kssid, ssid, sizeof(ssid));
        form_field(body, kpw, pw, sizeof(pw));
        if (ssid[0] == '\0') continue;
        strncpy(creds[count].ssid, ssid, PULSYNC_WIFI_STORE_SSID_MAXLEN - 1);
        strncpy(creds[count].password, pw, PULSYNC_WIFI_STORE_PASS_MAXLEN - 1);
        count++;
    }

    ESP_LOGI(TAG, "WiFi list save: %d network(s)", count);
    if (s_wifi_save_cb) s_wifi_save_cb(creds, count);

    /* Respond with the (possibly deduped/updated) list. */
    return handler_get_wifi(req);
}

/* POST /server/save — update server address. Body: server=host:port */
static esp_err_t handler_post_server_save(httpd_req_t *req) {
    if (gate_reject_api(req)) return ESP_OK;
    char body[256] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    body[received > 0 ? received : 0] = '\0';

    char server[128] = {0};
    form_field(body, "server", server, sizeof(server));
    ESP_LOGI(TAG, "Server save: '%s'", server);
    if (s_server_save_cb) s_server_save_cb(server);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* POST /login — config-menu password gate. Body: pw=<password>
 * Computes SHA-256(pw) and compares to the stored hash (offline check). On
 * success mints a session token and sets it as an HttpOnly cookie. */
static esp_err_t handler_post_login(httpd_req_t *req) {
    /* If the gate isn't active (unenrolled / no hash / enroll mode), there is
     * nothing to log into — treat as success so the client proceeds. */
    if (!gate_active()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":true}");
        return ESP_OK;
    }

    char body[160] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char pw[129] = {0};   /* generous headroom over the 12-char server password */
    form_field(body, "pw", pw, sizeof(pw));

    char entered_hash[65] = {0};
    sha256_hex(pw, strlen(pw), entered_hash);

    httpd_resp_set_type(req, "application/json");
    if (ct_streq(entered_hash, s_gate_hash)) {
        mint_session();
        char cookie[96];
        snprintf(cookie, sizeof(cookie),
                 "%s=%s; Path=/; HttpOnly; Max-Age=1800; SameSite=Lax",
                 PULSYNC_PORTAL_COOKIE_NAME, s_session_token);
        httpd_resp_set_hdr(req, "Set-Cookie", cookie);
        ESP_LOGI(TAG, "Config menu unlocked");
        httpd_resp_sendstr(req, "{\"ok\":true}");
    } else {
        ESP_LOGW(TAG, "Config login failed (bad password)");
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_sendstr(req, "{\"ok\":false}");
    }
    return ESP_OK;
}

/* POST /re-pair — re-enroll under a new pairing code. Body: code=PUL-XXXXXX
 * Clears the current token and triggers re-enrollment via the callback. The
 * server issues a fresh token (and config password) for the new pairing. */
static esp_err_t handler_post_repair(httpd_req_t *req) {
    if (gate_reject_api(req)) return ESP_OK;
    char body[128] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char code[24] = {0};
    form_field(body, "code", code, sizeof(code));
    /* Trim leading/trailing whitespace defensively. */
    if (code[0] == '\0') {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Missing pairing code\"}");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Re-pair requested: code='%s'", code);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");

    /* Defer teardown so the response flushes first, then hand off to the app
     * layer which clears the token, persists the code, and re-enrolls. */
    vTaskDelay(pdMS_TO_TICKS(300));
    if (s_repair_cb) s_repair_cb(code);
    return ESP_OK;
}

/* GET /exit — acknowledge, then stop portal + trigger reconnect. */
static esp_err_t handler_get_exit(httpd_req_t *req) {
    if (gate_reject_api(req)) return ESP_OK;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    /* Defer teardown so the response flushes first. */
    vTaskDelay(pdMS_TO_TICKS(300));
    pulsync_portal_stop();
    if (s_exit_cb) s_exit_cb();
    return ESP_OK;
}

/* ---------- DNS hijack task ---------- */

/**
 * Minimal DNS server that responds to all queries with the AP's IP (192.168.4.1).
 * This makes captive portal detection work on mobile devices.
 */
static void dns_hijack_task(void *param) {
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    s_dns_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_dns_socket < 0) {
        ESP_LOGE(TAG, "DNS socket failed");
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    if (bind(s_dns_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(s_dns_socket);
        s_dns_socket = -1;
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS hijack started on port 53");

    uint8_t buf[512];
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    while (s_state == PULSYNC_PORTAL_RUNNING) {
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(s_dns_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        int len = recvfrom(s_dns_socket, buf, sizeof(buf), 0,
                           (struct sockaddr *)&client_addr, &addr_len);
        if (len < 12) continue;  /* too short for DNS header */

        /* Build minimal DNS response: same ID, set QR bit, answer with 192.168.4.1 */
        buf[2] = 0x81;  /* QR=1, Opcode=0, AA=1, TC=0, RD=1 */
        buf[3] = 0x80;  /* RA=1, RCODE=0 */
        buf[6] = 0x00;  /* ANCOUNT = 1 */
        buf[7] = 0x01;

        /* Append answer: pointer to query name + A record + 192.168.4.1 */
        int resp_len = len;  /* keep original query */
        buf[resp_len++] = 0xC0;  /* name pointer */
        buf[resp_len++] = 0x0C;  /* offset to question name */
        buf[resp_len++] = 0x00; buf[resp_len++] = 0x01;  /* TYPE A */
        buf[resp_len++] = 0x00; buf[resp_len++] = 0x01;  /* CLASS IN */
        buf[resp_len++] = 0x00; buf[resp_len++] = 0x00;
        buf[resp_len++] = 0x00; buf[resp_len++] = 0x0A;  /* TTL 10s */
        buf[resp_len++] = 0x00; buf[resp_len++] = 0x04;  /* RDLENGTH 4 */
        buf[resp_len++] = 192; buf[resp_len++] = 168;
        buf[resp_len++] = 4;   buf[resp_len++] = 1;      /* 192.168.4.1 */

        sendto(s_dns_socket, buf, resp_len, 0,
               (struct sockaddr *)&client_addr, addr_len);
    }

    close(s_dns_socket);
    s_dns_socket = -1;
    s_dns_task = NULL;   /* clear before self-delete so a reopen can respawn */
    ESP_LOGI(TAG, "DNS hijack stopped");
    vTaskDelete(NULL);
}

/* ---------- Public API ---------- */

void pulsync_portal_init(void) {
    s_state = PULSYNC_PORTAL_STOPPED;

    /* Generate default AP SSID from MAC */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s%02X%02X",
             PULSYNC_PORTAL_AP_SSID_PREFIX, mac[4], mac[5]);

    ESP_LOGI(TAG, "Portal initialized (AP SSID: %s)", s_ap_ssid);
}

bool pulsync_portal_start(void) {
    if (s_state == PULSYNC_PORTAL_RUNNING) return true;

    s_state = PULSYNC_PORTAL_STARTING;
    /* Fresh session each time the portal opens — a prior unlock never carries
     * over to a new config session. */
    s_session_token[0] = '\0';
    ESP_LOGI(TAG, "Starting captive portal...");

    /* Create AP netif if needed */
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }
    if (!s_ap_netif) {
        ESP_LOGE(TAG, "AP netif create failed — cannot start portal");
        s_state = PULSYNC_PORTAL_STOPPED;
        return false;
    }

    /* The WiFi driver must be running before mode/config take effect. When the
     * portal is opened via the config button, WiFi is already started (STA is
     * connected), so this is a no-op — but if we're called before the STA path
     * ever started the driver, an unstarted driver silently drops the AP config
     * and no beacon is broadcast. Start it defensively; ESP_ERR_WIFI_NOT_STOPPED
     * just means it was already running. */
    esp_err_t serr = esp_wifi_start();
    if (serr != ESP_OK && serr != ESP_ERR_WIFI_NOT_STOPPED) {
        ESP_LOGW(TAG, "esp_wifi_start before AP config: %s", esp_err_to_name(serr));
    }

    /* Switch to APSTA mode (keep STA for background reconnect attempts). Check
     * the result — a failed mode switch is a common reason the AP never appears
     * yet everything else looks fine (httpd starts, no beacon is broadcast). */
    esp_err_t merr = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (merr != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode(APSTA) failed: %s", esp_err_to_name(merr));
        s_state = PULSYNC_PORTAL_STOPPED;
        return false;
    }

    /* Configure AP */
    wifi_config_t ap_config = {0};
    strncpy((char *)ap_config.ap.ssid, s_ap_ssid, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = strlen(s_ap_ssid);
    ap_config.ap.channel = PULSYNC_PORTAL_AP_CHANNEL;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ap_config.ap.max_connection = PULSYNC_PORTAL_AP_MAX_CONN;
    /* An open AP MUST advertise 0 as the beacon's auth requirement. Some IDF
     * versions require ssid_len set explicitly (done above) or they treat the
     * SSID as hidden. */

    esp_err_t cerr = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (cerr != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config(AP) failed: %s — AP will not broadcast",
                 esp_err_to_name(cerr));
        s_state = PULSYNC_PORTAL_STOPPED;
        return false;
    }
    ESP_LOGI(TAG, "AP configured: SSID='%s' (open, ch %d)",
             s_ap_ssid, PULSYNC_PORTAL_AP_CHANNEL);

    /* Start HTTP server. Defensively tear down any lingering instance first —
     * if a previous portal session left the httpd handle (and its listening
     * socket on port 80) alive, httpd_start would fail with EADDRINUSE (errno
     * 112). Clearing it makes start idempotent regardless of prior state. */
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        vTaskDelay(pdMS_TO_TICKS(100));  /* let the socket close */
    }

    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    http_config.server_port = PULSYNC_PORTAL_HTTP_PORT;
    http_config.uri_match_fn = httpd_uri_match_wildcard;
    http_config.max_uri_handlers = 14;

    /* Retry the bind a few times: if a prior portal session's listening socket
     * is still lingering (TIME_WAIT), the port frees within a second or two.
     * This avoids a hard EADDRINUSE (errno 112) failure on a quick re-open. */
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 4; attempt++) {
        err = httpd_start(&s_server, &http_config);
        if (err == ESP_OK) break;
        ESP_LOGW(TAG, "HTTP server start attempt %d failed: %s — retrying",
                 attempt + 1, esp_err_to_name(err));
        s_server = NULL;
        vTaskDelay(pdMS_TO_TICKS(750));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed after retries: %s", esp_err_to_name(err));
        s_server = NULL;
        s_state = PULSYNC_PORTAL_STOPPED;
        return false;
    }

    /* Register routes */
    httpd_uri_t uri_root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = handler_get_root
    };
    httpd_register_uri_handler(s_server, &uri_root);

    httpd_uri_t uri_save = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = handler_post_save
    };
    httpd_register_uri_handler(s_server, &uri_save);

    httpd_uri_t uri_scan = {
        .uri = "/scan",
        .method = HTTP_GET,
        .handler = handler_get_scan
    };
    httpd_register_uri_handler(s_server, &uri_scan);

    httpd_uri_t uri_reset = {
        .uri = "/factory-reset",
        .method = HTTP_GET,
        .handler = handler_factory_reset
    };
    httpd_register_uri_handler(s_server, &uri_reset);

    /* Config menu API endpoints */
    httpd_uri_t uri_wifi_get = {
        .uri = "/wifi",
        .method = HTTP_GET,
        .handler = handler_get_wifi
    };
    httpd_register_uri_handler(s_server, &uri_wifi_get);

    httpd_uri_t uri_wifi_save = {
        .uri = "/wifi/save",
        .method = HTTP_POST,
        .handler = handler_post_wifi_save
    };
    httpd_register_uri_handler(s_server, &uri_wifi_save);

    httpd_uri_t uri_server_save = {
        .uri = "/server/save",
        .method = HTTP_POST,
        .handler = handler_post_server_save
    };
    httpd_register_uri_handler(s_server, &uri_server_save);

    httpd_uri_t uri_repair = {
        .uri = "/re-pair",
        .method = HTTP_POST,
        .handler = handler_post_repair
    };
    httpd_register_uri_handler(s_server, &uri_repair);

    httpd_uri_t uri_login = {
        .uri = "/login",
        .method = HTTP_POST,
        .handler = handler_post_login
    };
    httpd_register_uri_handler(s_server, &uri_login);

    httpd_uri_t uri_exit = {
        .uri = "/exit",
        .method = HTTP_GET,
        .handler = handler_get_exit
    };
    httpd_register_uri_handler(s_server, &uri_exit);

    /* Catch-all for captive portal redirects */
    httpd_uri_t uri_catch = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = handler_catch_all
    };
    httpd_register_uri_handler(s_server, &uri_catch);

    /* Start DNS hijack for captive portal detection. Guard against a leftover
     * task from a previous session: stop() flips s_state (the task's loop
     * condition) but the task only self-deletes after its ~1s recv timeout, so
     * a quick reopen could otherwise spawn a second task racing on the shared
     * UDP socket. Only create if none is tracked. */
    if (s_dns_task == NULL) {
        xTaskCreate(dns_hijack_task, "pulsync_dns", 4096, NULL, 3, &s_dns_task);
    }

    s_state = PULSYNC_PORTAL_RUNNING;
    ESP_LOGI(TAG, "Captive portal running");
    return true;
}

void pulsync_portal_stop(void) {
    if (s_state == PULSYNC_PORTAL_STOPPED) return;

    ESP_LOGI(TAG, "Stopping captive portal");

    s_state = PULSYNC_PORTAL_STOPPED;
    s_session_token[0] = '\0';  /* invalidate any active config session */

    /* Stop HTTP server */
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }

    /* DNS task self-terminates when s_state changed above. It clears s_dns_task
     * right before deleting itself, so wait (bounded) for that to happen —
     * otherwise a quick reopen would see a stale handle and skip respawning the
     * DNS server, breaking captive-portal detection. The task's recv timeout is
     * ~1s, so cap the wait at ~1.5s. */
    for (int i = 0; i < 15 && s_dns_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (s_dns_task != NULL) {
        ESP_LOGW(TAG, "DNS task did not exit in time");
    }

    /* Switch back to STA only mode */
    esp_wifi_set_mode(WIFI_MODE_STA);

    ESP_LOGI(TAG, "Captive portal stopped");
}

pulsync_portal_state_t pulsync_portal_get_state(void) {
    return s_state;
}

bool pulsync_portal_is_active(void) {
    return s_state == PULSYNC_PORTAL_RUNNING;
}

void pulsync_portal_on_credentials(pulsync_portal_creds_cb_t cb) {
    s_creds_cb = cb;
}

const char *pulsync_portal_get_ssid(void) {
    return s_ap_ssid;
}

void pulsync_portal_set_name(const char *suffix) {
    if (!suffix || suffix[0] == '\0') return;
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s%s",
             PULSYNC_PORTAL_AP_SSID_PREFIX, suffix);
}

void pulsync_portal_set_mode(pulsync_portal_mode_t mode) {
    s_mode = mode;
}

void pulsync_portal_on_factory_reset(pulsync_portal_reset_cb_t cb) {
    s_reset_cb = cb;
}

void pulsync_portal_on_wifi_save(pulsync_portal_wifi_save_cb_t cb) {
    s_wifi_save_cb = cb;
}

void pulsync_portal_on_server_save(pulsync_portal_server_save_cb_t cb) {
    s_server_save_cb = cb;
}

void pulsync_portal_on_repair(pulsync_portal_repair_cb_t cb) {
    s_repair_cb = cb;
}

void pulsync_portal_set_gate(const char *pw_hash_hex, bool enrolled) {
    if (pw_hash_hex) {
        strncpy(s_gate_hash, pw_hash_hex, sizeof(s_gate_hash) - 1);
        s_gate_hash[sizeof(s_gate_hash) - 1] = '\0';
    } else {
        s_gate_hash[0] = '\0';
    }
    s_gate_enrolled = enrolled;
}

void pulsync_portal_on_exit(pulsync_portal_exit_cb_t cb) {
    s_exit_cb = cb;
}

void pulsync_portal_set_wifi_list(const pulsync_wifi_cred_t *creds, int count) {
    if (count < 0) count = 0;
    if (count > PULSYNC_WIFI_MAX_CREDS) count = PULSYNC_WIFI_MAX_CREDS;
    memset(s_wifi_list, 0, sizeof(s_wifi_list));
    if (creds) {
        for (int i = 0; i < count; i++) s_wifi_list[i] = creds[i];
    }
    s_wifi_list_count = count;
}
