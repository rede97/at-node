/*
 * ap_portal.cpp — AP mode WiFi provisioning portal
 *
 * Captive portal for first-time WiFi configuration.
 * Responsive HTML page for mobile and desktop.
 */

#include "ap_portal.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>

/* AP configuration */
#define AP_SSID_PREFIX   "AT-NODE-"
#define AP_PASSWORD      "ATNODECFG"
#define AP_IP            IPAddress(192, 168, 4, 1)
#define AP_NETMASK       IPAddress(255, 255, 255, 0)

/* Button trigger configuration */
#define AP_TRIGGER_PIN   10
#define AP_TRIGGER_MS    3000

/* DNS server for captive portal */
#define DNS_PORT         53

extern String g_device_name;
extern String g_hostname;
extern void save_config(const String& key, const String& value);

static WebServer   g_ap_http(80);
static DNSServer   g_ap_dns;
static bool        g_ap_active = false;
static String      g_ap_ssid;

/* --- HTML page -------------------------------------------------------- */
static const char* AP_PAGE_HTML = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>AT-Node Setup</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
    background: #f0f2f5; color: #333; padding: 16px;
  }
  .card {
    max-width: 420px; margin: 40px auto; background: #fff;
    border-radius: 12px; box-shadow: 0 2px 12px rgba(0,0,0,.08);
    padding: 24px;
  }
  h1 { font-size: 20px; margin-bottom: 16px; text-align: center; }
  .field { margin-bottom: 16px; }
  label { display: block; font-size: 14px; margin-bottom: 6px; color: #555; }
  input, select {
    width: 100%; padding: 10px 12px; border: 1px solid #ddd;
    border-radius: 8px; font-size: 16px;
  }
  button {
    width: 100%; padding: 12px; background: #007aff; color: #fff;
    border: none; border-radius: 8px; font-size: 16px; cursor: pointer;
  }
  button:active { background: #0056b3; }
  .note { font-size: 13px; color: #888; text-align: center; margin-top: 16px; }
  .scan { margin-bottom: 8px; }
  .scan a { color: #007aff; text-decoration: none; }
</style>
</head>
<body>
<div class="card">
  <h1>AT-Node WiFi Setup</h1>
  <form method="POST" action="/save">
    <div class="field">
      <label for="ssid">WiFi Network</label>
      <input id="ssid" name="ssid" list="ssidlist" required>
      <datalist id="ssidlist"></datalist>
    </div>
    <div class="field">
      <label for="pass">Password</label>
      <input id="pass" name="pass" type="password" required>
    </div>
    <button type="submit">Save &amp; Reboot</button>
  </form>
  <p class="note">Device will restart and join the selected network.</p>
</div>
<script>
  fetch('/scan').then(r => r.json()).then(list => {
    const dl = document.getElementById('ssidlist');
    list.forEach(s => {
      const o = document.createElement('option');
      o.value = s;
      dl.appendChild(o);
    });
  }).catch(() => {});
</script>
</body>
</html>
)HTML";

/* --- HTTP handlers ---------------------------------------------------- */
static void handle_root(void)
{
    g_ap_http.send(200, "text/html", AP_PAGE_HTML);
}

static void handle_scan(void)
{
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_FAILED) {
        WiFi.scanNetworks(true);
        g_ap_http.send(202, "application/json", "[]");
        return;
    }
    String json = "[";
    for (int i = 0; i < n; i++) {
        if (i) json += ",";
        json += "\"" + WiFi.SSID(i) + "\"";
    }
    json += "]";
    g_ap_http.send(200, "application/json", json);
    WiFi.scanDelete();
}

static void handle_save(void)
{
    String ssid = g_ap_http.arg("ssid");
    String pass = g_ap_http.arg("pass");
    if (ssid.length() == 0) {
        g_ap_http.send(400, "text/plain", "SSID required");
        return;
    }
    save_config("wifi_ssid", ssid);
    save_config("wifi_pass", pass);

    String html = "<html><head><meta charset=\"utf-8\">"
                  "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
                  "<title>Saved</title></head><body style=\"font-family:sans-serif;text-align:center;padding:40px;\">"
                  "<h2>Saved!</h2><p>Rebooting to join <b>" + ssid + "</b>...</p></body></html>";
    g_ap_http.send(200, "text/html", html);
    delay(2000);
    ESP.restart();
}

static void handle_not_found(void)
{
    /* Redirect all unknown paths to root for captive portal */
    g_ap_http.sendHeader("Location", "http://" + AP_IP.toString());
    g_ap_http.send(302, "text/plain", "");
}

/* --- public API ------------------------------------------------------- */
void ap_portal_start(void)
{
    if (g_ap_active) return;

    g_ap_ssid = String(AP_SSID_PREFIX) + g_device_name;
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(AP_IP, AP_IP, AP_NETMASK);
    WiFi.softAP(g_ap_ssid.c_str(), AP_PASSWORD);

    /* Start DNS server for captive portal */
    g_ap_dns.start(DNS_PORT, "*", AP_IP);

    g_ap_http.on("/", HTTP_GET, handle_root);
    g_ap_http.on("/scan", HTTP_GET, handle_scan);
    g_ap_http.on("/save", HTTP_POST, handle_save);
    g_ap_http.onNotFound(handle_not_found);
    g_ap_http.begin();

    /* Start WiFi scan in background */
    WiFi.scanNetworks(true);

    g_ap_active = true;
    Serial.printf("AP mode started: %s (pwd: %s)\n", g_ap_ssid.c_str(), AP_PASSWORD);
    Serial.printf("Connect to http://%s\n", AP_IP.toString().c_str());
}

void ap_portal_stop(void)
{
    if (!g_ap_active) return;
    g_ap_http.stop();
    g_ap_dns.stop();
    WiFi.softAPdisconnect(true);
    g_ap_active = false;
}

bool ap_portal_active(void)
{
    return g_ap_active;
}

void ap_portal_poll(void)
{
    if (!g_ap_active) return;
    g_ap_dns.processNextRequest();
    g_ap_http.handleClient();
}

bool ap_portal_check_button(void)
{
    pinMode(AP_TRIGGER_PIN, INPUT_PULLUP);
    uint32_t start = millis();
    while (millis() - start < AP_TRIGGER_MS) {
        if (digitalRead(AP_TRIGGER_PIN) == LOW) {
            delay(50);  /* debounce */
            if (digitalRead(AP_TRIGGER_PIN) == LOW) {
                Serial.println("AP trigger: button held, starting AP mode");
                ap_portal_start();
                return true;
            }
        }
        delay(10);
    }
    return false;
}
