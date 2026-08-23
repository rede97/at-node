/*
 * ssdp.cpp - SSDP/UPnP discovery responder (see ssdp.h).
 *
 * Reference: esp32_matrix/esp32_matrix.ino ssdpSetup/ssdpLoop.
 *
 * NOTE: Arduino.h (sdkconfig) must precede features.h — CONFIG_IDF_TARGET_*
 * come from the SDK config, and the .ino's implicit Arduino.h include does
 * not extend to .cpp files.
 */
#include <Arduino.h>
#include "features.h"

#if FEATURE_SSDP
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include "ssdp.h"

extern String g_device_name;

/* g_http is static inside arduino.ino (internal linkage); the sketch
 * hands us a pointer at ssdp_begin(). */
static WebServer* g_srv = nullptr;

static WiFiUDP   g_udp;
static const IPAddress SSDP_MCAST(239, 255, 255, 250);
static const uint16_t  SSDP_PORT = 1900;
static char     g_uuid[48];
static bool     g_active = false;
static uint32_t g_last_alive = 0;

/* UPnP device description document (presentationURL -> SPA at /). */
static void handle_description(void)
{
    String xml =
        "<?xml version=\"1.0\"?>\r\n"
        "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">\r\n"
        "  <specVersion><major>1</major><minor>0</minor></specVersion>\r\n"
        "  <device>\r\n"
        "    <deviceType>urn:schemas-upnp-org:device:Basic:1</deviceType>\r\n"
        "    <friendlyName>" + g_device_name + "</friendlyName>\r\n"
        "    <manufacturer>AT-Node</manufacturer>\r\n"
        "    <modelName>atnode-esp32</modelName>\r\n"
        "    <modelNumber>1.0</modelNumber>\r\n"
        "    <UDN>" + String(g_uuid) + "</UDN>\r\n"
        "    <presentationURL>http://" + WiFi.localIP().toString() + "/</presentationURL>\r\n"
        "  </device>\r\n"
        "</root>\r\n";
    g_srv->send(200, "text/xml", xml);
}

static void notify_alive(void)
{
    char msg[512];
    int n = snprintf(msg, sizeof(msg),
        "NOTIFY * HTTP/1.1\r\n"
        "HOST: %s:%u\r\n"
        "CACHE-CONTROL: max-age=1800\r\n"
        "LOCATION: http://%s:80/description.xml\r\n"
        "NT: upnp:rootdevice\r\n"
        "NTS: ssdp:alive\r\n"
        "SERVER: Arduino/1.0 UPnP/1.1 atnode-esp32/1.0\r\n"
        "USN: %s::upnp:rootdevice\r\n"
        "\r\n",
        SSDP_MCAST.toString().c_str(), SSDP_PORT,
        WiFi.localIP().toString().c_str(), g_uuid);
    g_udp.beginPacket(SSDP_MCAST, SSDP_PORT);
    g_udp.write((const uint8_t*)msg, n);
    g_udp.endPacket();
}

void ssdp_begin(WebServer* srv)
{
    if (g_active || WiFi.status() != WL_CONNECTED) return;
    g_srv = srv;
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(g_uuid, sizeof(g_uuid),
             "uuid:38323636-4558-4dda-9188-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    g_srv->on("/description.xml", HTTP_GET, handle_description);
    g_udp.beginMulticast(SSDP_MCAST, SSDP_PORT);
    g_active = true;
    for (int i = 0; i < 3; i++) {   /* alive x3: better discovery odds */
        notify_alive();
        delay(100);
    }
    g_last_alive = millis();
    Serial.printf("[SSDP] started, uuid=%s\n", g_uuid);
}

void ssdp_stop(void)
{
    if (!g_active) return;
    g_active = false;
    g_udp.stop();
    Serial.println("[SSDP] stopped");
}

void ssdp_loop(void)
{
    if (!g_active) return;
    if (WiFi.status() != WL_CONNECTED) {
        ssdp_stop();
        return;
    }
    if (millis() - g_last_alive > 900000) {   /* max-age 1800, renew at half */
        g_last_alive = millis();
        notify_alive();
    }
    int len = g_udp.parsePacket();
    if (len <= 0) return;
    char buf[512];
    int n = g_udp.read(buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = '\0';
    if (strncmp(buf, "M-SEARCH", 8) != 0) return;
    if (!strstr(buf, "ssdp:all") && !strstr(buf, "upnp:rootdevice") &&
        !strstr(buf, "Basic:1")) return;

    char resp[512];
    int rn = snprintf(resp, sizeof(resp),
        "HTTP/1.1 200 OK\r\n"
        "CACHE-CONTROL: max-age=1800\r\n"
        "EXT:\r\n"
        "LOCATION: http://%s:80/description.xml\r\n"
        "SERVER: Arduino/1.0 UPnP/1.1 atnode-esp32/1.0\r\n"
        "ST: upnp:rootdevice\r\n"
        "USN: %s::upnp:rootdevice\r\n"
        "\r\n",
        WiFi.localIP().toString().c_str(), g_uuid);
    g_udp.beginPacket(g_udp.remoteIP(), g_udp.remotePort());
    g_udp.write((const uint8_t*)resp, rn);
    g_udp.endPacket();
}
#endif /* FEATURE_SSDP */
