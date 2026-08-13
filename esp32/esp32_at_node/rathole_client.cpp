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
/* Heap guard: every reconnect cycle costs lwIP sockets that linger in
 * TIME_WAIT (2*MSL = 120s on ESP-IDF). Under reconnect churn the pile
 * starves the whole IP stack for minutes (measured: 22K -> 3.8K free,
 * all TCP/HTTP/ICMP dead until the pcbs expired). Refuse to add sockets
 * when tight and let TIME_WAITs drain instead. */
#define RATHOLE_MIN_FREE_HEAP   12000
/* The rathole server wants TCP_POOL_SIZE=8 standbys per service, but each
 * lwIP TCP socket costs ~2.4KB heap and the MQTT TLS session needs a large
 * transient allocation for its handshake. 1 standby per tunnel: the server
 * retries dropped CreateDataChannel requests when more visitors arrive, so
 * a warm first-hit is kept at half the RAM cost. Measured: each tunnel with
 * pool=2 costs ~10KB; two tunnels left <11KB free and lwIP/HTTP starved. */
#define RATHOLE_POOL_SIZE       1

extern Preferences prefs;   /* shared "atnode" namespace, owned by the sketch */
void save_config(const String& key, const String& value);
bool mqtt_is_connected(void);

struct TunnelCfg {
    String server;    /* "host:port" of the rathole server */
    String token;
    String svc;       /* service name (identical on server side) */
    String local;     /* "host:port" to forward to */
    bool   auto_conn;
    bool   enabled;   /* per-tunnel persisted switch (NVS tun<N>_en, default on) */
    uint8_t retry_s;  /* reconnect backoff base, seconds (1-60) */
};

static bool           g_rathole_enabled = true;   /* global master switch (NVS) */

struct Tunnel {
    TunnelCfg       cfg;
    TaskHandle_t    task;
    WiFiClient      cli;                        /* control channel — manager task only */
    WiFiClient*     pool[RATHOLE_POOL_SIZE];    /* standby data channels — manager task only */
    volatile bool   want_run;                   /* desired state; set/cleared by any task */
    volatile bool   reconfig;                   /* config changed: tear down + reconnect */
    volatile bool   connected;
    char            last_error[64];  /* fixed buffer: written by manager, read by HTTP/AT tasks; a String would race (heap free under concurrent read) */
    uint8_t         session_key[32];
    char            host[64];
    uint16_t        port;
};

static Tunnel        g_tun[RATHOLE_MAX_TUNNELS];
static volatile int  g_data_ch_count = 0;   /* pooled + forwarding, global */

static void set_err(Tunnel& t, const char* e)
{
    strncpy(t.last_error, e, sizeof(t.last_error) - 1);
    t.last_error[sizeof(t.last_error) - 1] = '\0';
}

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
        /* SSH echo is many small writes; Nagle would add ~40ms+ jitter per
         * unacked segment. The remote side is already setNoDelay. */
        local.setNoDelay(true);
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
    /* A forward session costs ~7KB (socket + task stack + pump buffer);
     * refuse when tight — the server retries the visitor. */
    if (ESP.getFreeHeap() < RATHOLE_MIN_FREE_HEAP) goto fail;
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
    if (ESP.getFreeHeap() < RATHOLE_MIN_FREE_HEAP) return;   /* let TIME_WAITs drain */
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
static bool control_handshake(Tunnel& t, const TunnelCfg& cfg)
{
    if (!split_host_port(cfg.server, t.host, sizeof(t.host), &t.port)) {
        set_err(t, "bad server addr");
        return false;
    }
    if (!t.cli.connect(t.host, t.port)) {
        set_err(t, "tcp connect failed");
        return false;
    }
    t.cli.setNoDelay(true);

    /* Hello: ControlChannelHello(version, sha256(service name)) */
    uint8_t digest[32];
    sha256_oneshot((const uint8_t*)cfg.svc.c_str(), cfg.svc.length(), digest);
    uint8_t hello[RATHOLE_HELLO_LEN];
    build_hello(hello, 0, digest);
    if (!write_all(t.cli, hello, sizeof(hello))) {
        set_err(t, "write hello failed");
        return false;
    }

    /* Server hello carries a random nonce */
    uint8_t sh[RATHOLE_HELLO_LEN];
    if (!read_exact(t.cli, sh, sizeof(sh), 10000)) {
        set_err(t, "read hello failed");
        return false;
    }
    if (le_u32(sh) != 0 || sh[4] != RATHOLE_PROTO_VERSION) {
        set_err(t, "bad server hello");
        return false;
    }

    /* Auth: sha256(token || nonce) — also the data-channel session key */
    String concat = cfg.token;
    for (int i = 5; i < RATHOLE_HELLO_LEN; i++) concat += (char)sh[i];
    sha256_oneshot((const uint8_t*)concat.c_str(), concat.length(), t.session_key);
    if (!write_all(t.cli, t.session_key, RATHOLE_AUTH_LEN)) {
        set_err(t, "write auth failed");
        return false;
    }

    uint8_t ack[RATHOLE_ACK_LEN];
    if (!read_exact(t.cli, ack, sizeof(ack), 10000)) {
        set_err(t, "read ack failed");
        return false;
    }
    uint32_t a = le_u32(ack);
    if (a != 0) {
        set_err(t, (a == 1) ? "service not exist" : "auth failed");
        return false;
    }
    return true;
}

/* The manager task owns t.cli / t.pool for the tunnel's entire lifetime —
 * ONLY this task ever touches them. Stop/reconfig from other tasks is
 * signalled via flags (want_run / reconfig), never by calling t.cli.stop()
 * from another task: doing so freed the WiFiClient RX buffer under
 * read_exact() and panicked (NetworkClient::available() on NULL _rxBuffer,
 * Guru Meditation load access fault at 0x14). */
static void manager_task(void* pv)
{
    int idx = (int)(intptr_t)pv;
    Tunnel& t = g_tun[idx];
    char tag[8];
    snprintf(tag, sizeof(tag), "tun%d", idx + 1);

    uint32_t backoff_ms = 0;
    for (;;) {
        if (!t.want_run) {              /* parked: stopped via flags */
            backoff_ms = 0;
            delay(100);
            continue;
        }
        const uint32_t backoff_base = (uint32_t)t.cfg.retry_s * 1000;
        if (backoff_ms < backoff_base) backoff_ms = backoff_base;

        /* Boot grace: the MQTT TLS handshake needs large contiguous heap
         * blocks that tunnel sockets fragment, so hold off until MQTT is
         * connected (or 30s max) before creating pool sockets. */
        if (millis() < 30000 && !mqtt_is_connected()) {
            delay(250);
            continue;
        }
        if (WiFi.status() != WL_CONNECTED) {
            delay(1000);
            continue;
        }
        if (ESP.getFreeHeap() < RATHOLE_MIN_FREE_HEAP) {
            set_err(t, "low heap, draining");
            delay(2000);
            continue;
        }

        t.reconfig = false;
        /* Snapshot the config: rathole_set() rewrites t.cfg Strings from
         * the HTTP/AT task while we run; using a copy for the whole
         * connect cycle avoids a heap free under our feet. retry_s is a
         * plain uint8 and safe to re-read above. */
        TunnelCfg cfg = t.cfg;
        if (!control_handshake(t, cfg)) {
            t.cli.stop();
            Serial.printf("[rathole] %s handshake failed: %s\n",
                          tag, t.last_error);
        } else {
            t.connected = true;
            set_err(t, "");
            Serial.printf("[rathole] %s control channel up (%s -> %s)\n",
                          tag, cfg.svc.c_str(), cfg.local.c_str());

            uint8_t  cmd[RATHOLE_CMD_LEN];
            size_t   cmd_len = 0;
            uint32_t last_rx = millis();
            uint32_t up_since = millis();

            while (t.want_run && !t.reconfig) {
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
                    set_err(t, "control channel closed");
                    break;
                }
                if ((uint32_t)(millis() - last_rx) > RATHOLE_HEARTBEAT_MS) {
                    set_err(t, "heartbeat timeout");
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
                        start_forward(i, c, cfg);
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
            if (!t.want_run || t.reconfig) set_err(t, "");
            Serial.printf("[rathole] %s control channel down: %s\n",
                          tag, t.last_error);
            if (millis() - up_since > 3000) backoff_ms = backoff_base;
        }
        /* Sliced backoff: stop/reconfig takes effect within ~100ms instead
         * of waiting out a backoff that can reach 60s. */
        for (uint32_t w = 0; w < backoff_ms && t.want_run && !t.reconfig; w += 100) {
            delay(100);
        }
        backoff_ms = min(backoff_ms * 2, (uint32_t)60000);
    }
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
    c.enabled   = prefs.getString(nvs_key(idx, "en", key, sizeof(key)), "1") == "1";
    c.retry_s   = (uint8_t)constrain(prefs.getString(nvs_key(idx, "retry", key, sizeof(key)), "1").toInt(), 1, 60);
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
    prefs.begin("atnode", true);
    g_rathole_enabled = prefs.getString("rathole_en", "1") == "1";
    prefs.end();
    for (int i = 0; i < RATHOLE_MAX_TUNNELS; i++) {
        g_tun[i].task = NULL;
        g_tun[i].want_run = false;
        g_tun[i].reconfig = false;
        g_tun[i].connected = false;
        for (int s = 0; s < RATHOLE_POOL_SIZE; s++) g_tun[i].pool[s] = NULL;
        load_one(i);
        if (g_rathole_enabled && g_tun[i].cfg.enabled &&
            g_tun[i].cfg.auto_conn && configured(i)) {
            rathole_start(i);
        }
    }
}

void rathole_set_enabled(bool en)
{
    g_rathole_enabled = en;
    save_config("rathole_en", en ? "1" : "0");
    if (!en) {
        for (int i = 0; i < RATHOLE_MAX_TUNNELS; i++) rathole_stop(i);
        Serial.println("[rathole] globally disabled");
    } else {
        for (int i = 0; i < RATHOLE_MAX_TUNNELS; i++) {
            if (g_tun[i].cfg.enabled && g_tun[i].cfg.auto_conn) rathole_start(i);
        }
        Serial.println("[rathole] globally enabled");
    }
}

bool rathole_is_enabled(void)
{
    return g_rathole_enabled;
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
    else if (key == "enable")   { c.enabled = (val == "1" || val == "true");   field = "en"; }
    else if (key == "retry")    {
        int r = constrain(val.toInt(), 1, 60);
        if (r <= 0) return false;
        c.retry_s = (uint8_t)r;
        field = "retry";
    }
    else return false;

    char nk[16];
    save_config(nvs_key(idx, field, nk, sizeof(nk)),
                key == "auto"  ? String(c.auto_conn ? "1" : "0")
              : key == "enable" ? String(c.enabled ? "1" : "0")
              : key == "retry" ? String(c.retry_s) : val);

    /* per-tunnel switch: off stops immediately (flag; manager unwinds itself) */
    if (key == "enable" && !c.enabled) {
        rathole_stop(idx);
        return true;
    }

    /* running tunnel: ask the manager to reconnect with the new config.
     * Coalesces when several fields are set in one request. */
    if (key != "auto" && g_tun[idx].want_run) {
        g_tun[idx].reconfig = true;
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
    if (key == "enable")   return c.enabled ? "1" : "0";
    if (key == "retry")    return String(c.retry_s);
    return "";
}

bool rathole_start(int idx)
{
    if (idx < 0 || idx >= RATHOLE_MAX_TUNNELS) return false;
    Tunnel& t = g_tun[idx];
    if (!g_rathole_enabled) {
        set_err(t, "globally disabled");
        return false;
    }
    if (!t.cfg.enabled) {
        set_err(t, "tunnel disabled");
        return false;
    }
    if (!configured(idx)) {
        set_err(t, "not configured");
        return false;
    }
    t.reconfig = true;   /* wake a parked/backing-off manager immediately */
    t.want_run = true;
    if (t.task) return true;   /* persistent manager already up */
    char name[8];
    snprintf(name, sizeof(name), "ratm%d", idx);
    if (xTaskCreate(manager_task, name, 3072, (void*)(intptr_t)idx, 1, &t.task) != pdPASS) {
        t.want_run = false;
        t.task = NULL;
        set_err(t, "task create failed");
        return false;
    }
    return true;
}

void rathole_stop(int idx)
{
    if (idx < 0 || idx >= RATHOLE_MAX_TUNNELS) return;
    Tunnel& t = g_tun[idx];
    /* Flags only — the manager task tears down its own sockets within
     * ~100ms. NEVER t.cli.stop() from here: that frees the WiFiClient RX
     * buffer under the manager's read_exact() and panics. */
    t.want_run = false;
    t.reconfig = true;
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
    prefs.remove(nvs_key(idx, "en", key, sizeof(key)));
    prefs.remove(nvs_key(idx, "retry", key, sizeof(key)));
    prefs.end();
    g_tun[idx].cfg = TunnelCfg();
    set_err(g_tun[idx], "");
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
    j += ",\"retry\":" + String(t.cfg.retry_s);
    j += ",\"master\":" + String(g_rathole_enabled ? "true" : "false");
    j += ",\"enabled\":" + String(t.cfg.enabled ? "true" : "false");
    j += ",\"running\":" + String(t.want_run ? "true" : "false");
    j += ",\"connected\":" + String(t.connected ? "true" : "false");
    j += ",\"pool\":" + String(pooled);
    j += ",\"data_channels\":" + String(g_data_ch_count);
    j += ",\"free_heap\":" + String(ESP.getFreeHeap());
    j += ",\"last_error\":\"" + String(t.last_error) + "\"";
    j += "}";
    return j;
}
