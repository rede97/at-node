/*
 * rathole_client.cpp - rathole reverse-tunnel client (plain TCP transport)
 * See rathole_client.h for the protocol wire format.
 *
 * RAM-conscious architecture: the rathole server keeps a standby pool of
 * TCP_POOL_SIZE=8 data channels per service (hardcoded in server.rs).
 * Standby channels are idle 99.9% of the time, so they are NOT tasks:
 * each tunnel has ONE manager task that owns the control channel plus up
 * to RATHOLE_POOL_SIZE pooled data-channel sockets (polled non-blocking).
 * A FreeRTOS task is spawned only when the server assigns a real visitor
 * (StartForwardTcp), and lives only for that forwarding session.
 */
#include <WiFi.h>
#include <Preferences.h>
#include <mbedtls/sha256.h>
#include "rathole_client.h"

#define RATHOLE_PROTO_VERSION   1
#define RATHOLE_HELLO_LEN       37
#define RATHOLE_AUTH_LEN        32
#define RATHOLE_CMD_LEN         4
#define RATHOLE_ACK_LEN         4
#define RATHOLE_HEARTBEAT_MS    45000   /* server interval 30s, timeout 40s; margin */
#define RATHOLE_PUMP_BUF        1460
/* The rathole server wants TCP_POOL_SIZE=8 standbys per service, but each
 * lwIP TCP socket costs ~2.4KB heap and the MQTT TLS session needs a large
 * transient allocation for its handshake. 2 standbys per tunnel keeps a
 * warm first-hit path within the RAM budget; the server retries dropped
 * CreateDataChannel requests when more visitors actually arrive. */
#define RATHOLE_POOL_SIZE       2

extern Preferences prefs;   /* shared "atnode" namespace, owned by the sketch */
void save_config(const String& key, const String& value);
bool mqtt_is_connected(void);

struct TunnelCfg {
    String server;    /* "host:port" of the rathole server */
    String token;
    String svc;       /* service name (identical on server side) */
    String local;     /* "host:port" to forward to */
    bool   auto_conn;
};

struct Tunnel {
    TunnelCfg       cfg;
    TaskHandle_t    task;
    WiFiClient      cli;                        /* control channel */
    WiFiClient*     pool[RATHOLE_POOL_SIZE];    /* standby data channels */
    volatile bool   want_run;
    volatile bool   connected;
    String          last_error;
    uint8_t         session_key[32];
    char            host[64];
    uint16_t        port;
};

static Tunnel        g_tun[RATHOLE_MAX_TUNNELS];
static volatile int  g_data_ch_count = 0;   /* pooled + forwarding, global */

/* --- helpers ------------------------------------------------------------ */

static void sha256_oneshot(const uint8_t* data, size_t len, uint8_t out[32])
{
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, data, len);
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
}

/* Split "host:port"; returns false on malformed input. */
static bool split_host_port(const String& s, char* host, size_t host_len, uint16_t* port)
{
    int colon = s.lastIndexOf(':');
    if (colon <= 0 || colon == (int)s.length() - 1) return false;
    String h = s.substring(0, colon);
    int p = s.substring(colon + 1).toInt();
    if (p <= 0 || p > 65535 || h.length() >= host_len) return false;
    strcpy(host, h.c_str());
    *port = (uint16_t)p;
    return true;
}

static bool read_exact(Client& c, uint8_t* buf, size_t len, uint32_t timeout_ms)
{
    size_t got = 0;
    uint32_t start = millis();
    while (got < len) {
        int avail = c.available();
        if (avail > 0) {
            int r = c.read(buf + got, min((size_t)avail, len - got));
            if (r <= 0) return false;
            got += r;
        } else {
            if (!c.connected()) return false;
            if ((uint32_t)(millis() - start) > timeout_ms) return false;
            delay(1);
        }
    }
    return true;
}

static bool write_all(Client& c, const uint8_t* buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        size_t w = c.write(buf + sent, len - sent);
        if (w == 0) {
            if (!c.connected()) return false;
            delay(1);
        } else {
            sent += w;
        }
    }
    return true;
}

/* hello: u32 variant + u8 version + 32B digest */
static void build_hello(uint8_t out[RATHOLE_HELLO_LEN], uint32_t variant, const uint8_t digest[32])
{
    out[0] = (uint8_t)(variant & 0xFF);
    out[1] = (uint8_t)((variant >> 8) & 0xFF);
    out[2] = (uint8_t)((variant >> 16) & 0xFF);
    out[3] = (uint8_t)((variant >> 24) & 0xFF);
    out[4] = RATHOLE_PROTO_VERSION;
    memcpy(out + 5, digest, 32);
}

static uint32_t le_u32(const uint8_t* b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

/* --- forwarding (one task per real visitor) ------------------------------ */

struct FwdArgs {
    WiFiClient* remote;   /* pooled channel, already past the rathole handshake */
    char        lhost[64];
    uint16_t    lport;
};

static void forward_task(void* pv)
{
    FwdArgs* args = (FwdArgs*)pv;
    WiFiClient* remote = args->remote;
    WiFiClient local;

    do {
        if (!local.connect(args->lhost, args->lport)) {
            Serial.println("[rathole] fwd: local connect failed");
            break;
        }
        Serial.println("[rathole] fwd: forwarding");

        uint8_t* buf = (uint8_t*)malloc(RATHOLE_PUMP_BUF);
        if (!buf) break;
        uint32_t rx_total = 0, tx_total = 0;
        const char* cause = "remote closed";
        for (;;) {
            bool idle = true;
            /* WiFiClient::read() may spuriously return <=0 even when
             * available() reported data; only a real disconnect is fatal. */
            int a = remote->available();
            if (a > 0) {
                int r = remote->read(buf, min((size_t)a, (size_t)RATHOLE_PUMP_BUF));
                if (r > 0) {
                    if (!write_all(local, buf, r)) { cause = "local write fail"; break; }
                    rx_total += r;
                    idle = false;
                } else if (!remote->connected()) { cause = "remote read eof"; break; }
            }
            a = local.available();
            if (a > 0) {
                int r = local.read(buf, min((size_t)a, (size_t)RATHOLE_PUMP_BUF));
                if (r > 0) {
                    if (!write_all(*remote, buf, r)) { cause = "remote write fail"; break; }
                    tx_total += r;
                    idle = false;
                } else if (!local.connected()) { cause = "local read eof"; break; }
            }
            if (idle) {
                /* connected() flushes closed-but-buffered state; when both
                 * sides are drained and either is down, the session is over. */
                if (!remote->connected()) { cause = "remote closed"; break; }
                if (!local.connected())   { cause = "local closed";  break; }
                delay(1);
            }
        }
        free(buf);
        Serial.printf("[rathole] fwd: closed (%s) rx=%u tx=%u\n",
                      cause, (unsigned)rx_total, (unsigned)tx_total);
    } while (0);

    remote->stop();
    delete remote;
    local.stop();
    delete args;
    g_data_ch_count--;
    vTaskDelete(NULL);
}

static void start_forward(int slot_idx, WiFiClient* ch, const TunnelCfg& cfg)
{
    FwdArgs* args = new(std::nothrow) FwdArgs();
    if (!args) goto fail;
    args->remote = ch;
    if (!split_host_port(cfg.local, args->lhost, sizeof(args->lhost), &args->lport)) goto fail;
    if (xTaskCreate(forward_task, "ratfwd", 3072, args, 1, NULL) != pdPASS) {
        Serial.printf("[rathole] fwd: task create failed, free heap=%u\n",
                      (unsigned)ESP.getFreeHeap());
        goto fail;
    }
    return;
fail:
    delete args;
    ch->stop();
    delete ch;
    g_data_ch_count--;
    (void)slot_idx;
}

/* --- manager task (control channel + standby pool) ----------------------- */

/* Connect one standby data channel and add it to the pool. */
static void pool_fill_one(Tunnel& t, int slot)
{
    WiFiClient* c = new(std::nothrow) WiFiClient();
    if (!c) return;
    if (!c->connect(t.host, t.port)) {
        delete c;
        return;
    }
    c->setNoDelay(true);
    uint8_t hello[RATHOLE_HELLO_LEN];
    build_hello(hello, 1, t.session_key);   /* DataChannelHello(session_key) */
    if (!write_all(*c, hello, sizeof(hello))) {
        c->stop();
        delete c;
        return;
    }
    t.pool[slot] = c;
    g_data_ch_count++;
}

static void pool_drain(Tunnel& t)
{
    for (int i = 0; i < RATHOLE_POOL_SIZE; i++) {
        if (t.pool[i]) {
            t.pool[i]->stop();
            delete t.pool[i];
            t.pool[i] = NULL;
            g_data_ch_count--;
        }
    }
}

/* One connection attempt: TCP connect + rathole control handshake. */
static bool control_handshake(Tunnel& t)
{
    if (!split_host_port(t.cfg.server, t.host, sizeof(t.host), &t.port)) {
        t.last_error = "bad server addr";
        return false;
    }
    if (!t.cli.connect(t.host, t.port)) {
        t.last_error = "tcp connect failed";
        return false;
    }
    t.cli.setNoDelay(true);

    /* Hello: ControlChannelHello(version, sha256(service name)) */
    uint8_t digest[32];
    sha256_oneshot((const uint8_t*)t.cfg.svc.c_str(), t.cfg.svc.length(), digest);
    uint8_t hello[RATHOLE_HELLO_LEN];
    build_hello(hello, 0, digest);
    if (!write_all(t.cli, hello, sizeof(hello))) {
        t.last_error = "write hello failed";
        return false;
    }

    /* Server hello carries a random nonce */
    uint8_t sh[RATHOLE_HELLO_LEN];
    if (!read_exact(t.cli, sh, sizeof(sh), 10000)) {
        t.last_error = "read hello failed";
        return false;
    }
    if (le_u32(sh) != 0 || sh[4] != RATHOLE_PROTO_VERSION) {
        t.last_error = "bad server hello";
        return false;
    }

    /* Auth: sha256(token || nonce) — also the data-channel session key */
    String concat = t.cfg.token;
    for (int i = 5; i < RATHOLE_HELLO_LEN; i++) concat += (char)sh[i];
    sha256_oneshot((const uint8_t*)concat.c_str(), concat.length(), t.session_key);
    if (!write_all(t.cli, t.session_key, RATHOLE_AUTH_LEN)) {
        t.last_error = "write auth failed";
        return false;
    }

    uint8_t ack[RATHOLE_ACK_LEN];
    if (!read_exact(t.cli, ack, sizeof(ack), 10000)) {
        t.last_error = "read ack failed";
        return false;
    }
    uint32_t a = le_u32(ack);
    if (a != 0) {
        t.last_error = (a == 1) ? "service not exist" : "auth failed";
        return false;
    }
    return true;
}

static void manager_task(void* pv)
{
    int idx = (int)(intptr_t)pv;
    Tunnel& t = g_tun[idx];
    char tag[8];
    snprintf(tag, sizeof(tag), "tun%d", idx + 1);

    /* Boot grace: the MQTT TLS handshake needs large contiguous heap blocks
     * that tunnel sockets fragment, so hold off until MQTT is connected
     * (or 30s max) before creating pool sockets. */
    while (millis() < 30000 && !mqtt_is_connected() && t.want_run) {
        delay(250);
    }

    uint32_t backoff_ms = 1000;
    while (t.want_run) {
        if (WiFi.status() != WL_CONNECTED) {
            delay(1000);
            continue;
        }
        if (!control_handshake(t)) {
            t.cli.stop();
            Serial.printf("[rathole] %s handshake failed: %s\n",
                          tag, t.last_error.c_str());
        } else {
            t.connected = true;
            t.last_error = "";
            Serial.printf("[rathole] %s control channel up (%s -> %s)\n",
                          tag, t.cfg.svc.c_str(), t.cfg.local.c_str());

            uint8_t  cmd[RATHOLE_CMD_LEN];
            size_t   cmd_len = 0;
            uint32_t last_rx = millis();
            uint32_t up_since = millis();

            while (t.want_run) {
                /* control channel: accumulate one command */
                while (t.cli.available() > 0 && cmd_len < RATHOLE_CMD_LEN) {
                    int r = t.cli.read(cmd + cmd_len, RATHOLE_CMD_LEN - cmd_len);
                    if (r <= 0) break;
                    cmd_len += r;
                    last_rx = millis();
                }
                if (cmd_len == RATHOLE_CMD_LEN) {
                    cmd_len = 0;
                    if (le_u32(cmd) == 0) {          /* CreateDataChannel */
                        for (int i = 0; i < RATHOLE_POOL_SIZE; i++) {
                            if (!t.pool[i]) {
                                pool_fill_one(t, i);
                                break;
                            }
                        }
                        /* pool full: request dropped; server retries on demand */
                    }
                    /* le_u32(cmd) == 1: HeartBeat — last_rx already bumped */
                }
                if (!t.cli.connected() && t.cli.available() == 0) {
                    t.last_error = "control channel closed";
                    break;
                }
                if ((uint32_t)(millis() - last_rx) > RATHOLE_HEARTBEAT_MS) {
                    t.last_error = "heartbeat timeout";
                    break;
                }

                /* poll standby pool: activation or server-side close */
                for (int i = 0; i < RATHOLE_POOL_SIZE; i++) {
                    WiFiClient* c = t.pool[i];
                    if (!c) continue;
                    if (!c->connected() && c->available() == 0) {
                        /* server culled this standby */
                        c->stop();
                        delete c;
                        t.pool[i] = NULL;
                        g_data_ch_count--;
                        continue;
                    }
                    if (c->available() < RATHOLE_CMD_LEN) continue;
                    /* WiFiClient::available() can over-report; read with a
                     * short timeout and count what actually arrived. */
                    uint8_t  dcmd[RATHOLE_CMD_LEN];
                    size_t   got = 0;
                    uint32_t start = millis();
                    while (got < RATHOLE_CMD_LEN && millis() - start < 500) {
                        int r = c->read(dcmd + got, RATHOLE_CMD_LEN - got);
                        if (r > 0) got += r;
                        else if (!c->connected()) break;
                        else delay(1);
                    }
                    if (got == RATHOLE_CMD_LEN && le_u32(dcmd) == 0) {
                        /* StartForwardTcp: ownership moves to forward task */
                        t.pool[i] = NULL;
                        start_forward(i, c, t.cfg);
                    } else if (got == 0 && c->connected()) {
                        continue;   /* phantom report; retry next poll */
                    } else {
                        /* partial read desync or dead socket: drop it */
                        Serial.printf("[rathole] %s standby dropped (got=%u cmd=%lu)\n",
                                      tag, (unsigned)got, (unsigned long)le_u32(dcmd));
                        c->stop();
                        delete c;
                        t.pool[i] = NULL;
                        g_data_ch_count--;
                    }
                }
                delay(2);
            }

            t.connected = false;
            t.cli.stop();
            pool_drain(t);
            Serial.printf("[rathole] %s control channel down: %s\n",
                          tag, t.last_error.c_str());
            if (millis() - up_since > 3000) backoff_ms = 1000;
        }
        if (!t.want_run) break;
        delay(backoff_ms);
        backoff_ms = min(backoff_ms * 2, (uint32_t)15000);
    }
    pool_drain(t);
    t.connected = false;
    t.cli.stop();
    t.task = NULL;
    vTaskDelete(NULL);
}

/* --- NVS / API ----------------------------------------------------------- */

static const char* nvs_key(int idx, const char* field, char* buf, size_t len)
{
    snprintf(buf, len, "tun%d_%s", idx + 1, field);   /* <= 15 chars (NVS limit) */
    return buf;
}

static void load_one(int idx)
{
    char key[16];
    TunnelCfg& c = g_tun[idx].cfg;
    prefs.begin("atnode", true);
    c.server    = prefs.getString(nvs_key(idx, "server", key, sizeof(key)), "");
    c.token     = prefs.getString(nvs_key(idx, "token", key, sizeof(key)), "");
    c.svc       = prefs.getString(nvs_key(idx, "svc", key, sizeof(key)), "");
    c.local     = prefs.getString(nvs_key(idx, "local", key, sizeof(key)), "");
    c.auto_conn = prefs.getString(nvs_key(idx, "auto", key, sizeof(key)), "0") == "1";
    prefs.end();
}

static bool configured(int idx)
{
    const TunnelCfg& c = g_tun[idx].cfg;
    return c.server.length() > 0 && c.token.length() > 0 &&
           c.svc.length() > 0 && c.local.length() > 0;
}

void rathole_init(void)
{
    for (int i = 0; i < RATHOLE_MAX_TUNNELS; i++) {
        g_tun[i].task = NULL;
        g_tun[i].want_run = false;
        g_tun[i].connected = false;
        for (int s = 0; s < RATHOLE_POOL_SIZE; s++) g_tun[i].pool[s] = NULL;
        load_one(i);
        if (g_tun[i].cfg.auto_conn && configured(i)) {
            rathole_start(i);
        }
    }
}

bool rathole_set(int idx, const String& key, const String& val)
{
    if (idx < 0 || idx >= RATHOLE_MAX_TUNNELS) return false;
    TunnelCfg& c = g_tun[idx].cfg;
    const char* field;
    if (key == "server")        { c.server = val;  field = "server"; }
    else if (key == "token")    { c.token = val;   field = "token"; }
    else if (key == "service")  { c.svc = val;     field = "svc"; }
    else if (key == "local")    { c.local = val;   field = "local"; }
    else if (key == "auto")     { c.auto_conn = (val == "1" || val == "true"); field = "auto"; }
    else return false;

    char nk[16];
    save_config(nvs_key(idx, field, nk, sizeof(nk)),
                key == "auto" ? String(c.auto_conn ? "1" : "0") : val);

    /* restart a running tunnel so the change takes effect */
    if (g_tun[idx].task && key != "auto") {
        rathole_stop(idx);
        rathole_start(idx);
    }
    return true;
}

String rathole_get(int idx, const String& key)
{
    if (idx < 0 || idx >= RATHOLE_MAX_TUNNELS) return "";
    const TunnelCfg& c = g_tun[idx].cfg;
    if (key == "server")   return c.server;
    if (key == "token")    return c.token;
    if (key == "service")  return c.svc;
    if (key == "local")    return c.local;
    if (key == "auto")     return c.auto_conn ? "1" : "0";
    return "";
}

bool rathole_start(int idx)
{
    if (idx < 0 || idx >= RATHOLE_MAX_TUNNELS) return false;
    Tunnel& t = g_tun[idx];
    if (!configured(idx)) {
        t.last_error = "not configured";
        return false;
    }
    if (t.task) return true;   /* already running */
    t.want_run = true;
    char name[8];
    snprintf(name, sizeof(name), "ratm%d", idx);
    if (xTaskCreate(manager_task, name, 4096, (void*)(intptr_t)idx, 1, &t.task) != pdPASS) {
        t.want_run = false;
        t.task = NULL;
        t.last_error = "task create failed";
        return false;
    }
    return true;
}

void rathole_stop(int idx)
{
    if (idx < 0 || idx >= RATHOLE_MAX_TUNNELS) return;
    Tunnel& t = g_tun[idx];
    t.want_run = false;
    t.cli.stop();              /* unblock a pending read/connect */
    uint32_t start = millis();
    while (t.task && millis() - start < 3000) delay(10);
}

void rathole_clear(int idx)
{
    if (idx < 0 || idx >= RATHOLE_MAX_TUNNELS) return;
    rathole_stop(idx);
    char key[16];
    prefs.begin("atnode", false);
    prefs.remove(nvs_key(idx, "server", key, sizeof(key)));
    prefs.remove(nvs_key(idx, "token", key, sizeof(key)));
    prefs.remove(nvs_key(idx, "svc", key, sizeof(key)));
    prefs.remove(nvs_key(idx, "local", key, sizeof(key)));
    prefs.remove(nvs_key(idx, "auto", key, sizeof(key)));
    prefs.end();
    g_tun[idx].cfg = TunnelCfg();
    g_tun[idx].last_error = "";
}

String rathole_status_json(int idx)
{
    if (idx < 0 || idx >= RATHOLE_MAX_TUNNELS) return "{}";
    Tunnel& t = g_tun[idx];
    int pooled = 0;
    for (int i = 0; i < RATHOLE_POOL_SIZE; i++) {
        if (t.pool[i]) pooled++;
    }
    String j = "{";
    j += "\"id\":" + String(idx + 1);
    j += ",\"configured\":" + String(configured(idx) ? "true" : "false");
    j += ",\"server\":\"" + t.cfg.server + "\"";
    j += ",\"service\":\"" + t.cfg.svc + "\"";
    j += ",\"local\":\"" + t.cfg.local + "\"";
    j += ",\"auto\":" + String(t.cfg.auto_conn ? "true" : "false");
    j += ",\"running\":" + String(t.task ? "true" : "false");
    j += ",\"connected\":" + String(t.connected ? "true" : "false");
    j += ",\"pool\":" + String(pooled);
    j += ",\"data_channels\":" + String(g_data_ch_count);
    j += ",\"free_heap\":" + String(ESP.getFreeHeap());
    j += ",\"last_error\":\"" + t.last_error + "\"";
    j += "}";
    return j;
}
