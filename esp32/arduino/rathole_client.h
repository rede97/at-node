/*
 * rathole_client.h - rathole reverse-tunnel client (plain TCP transport)
 *
 * Implements the client side of the rathole protocol (v1) for up to
 * RATHOLE_MAX_TUNNELS concurrent services. See ../rathole/src/protocol.rs
 * and client.rs for the reference implementation.
 *
 * Wire format (bincode, little-endian, fixed-size ints; enum = u32 variant):
 *   Hello::ControlChannelHello = u32(0) + u8(version=1) + sha256(service)[32]  (37B)
 *   Hello::DataChannelHello    = u32(1) + u8(version=1) + session_key[32]      (37B)
 *   Auth                       = sha256(token || nonce)[32]                    (32B)
 *   Ack                        = u32: 0=Ok 1=ServiceNotExist 2=AuthFailed       (4B)
 *   ControlChannelCmd          = u32: 0=CreateDataChannel 1=HeartBeat           (4B)
 *   DataChannelCmd             = u32: 0=StartForwardTcp 1=StartForwardUdp       (4B)
 *
 * Scope cuts (vs. full rathole): plain TCP transport only (no TLS/noise),
 * TCP forwarding only (no UDP), no hot reload, no proxy.
 */
#pragma once
#include <Arduino.h>

#define RATHOLE_MAX_TUNNELS    1   /* single SSH tunnel suffices (jump host); less RAM, less exposure */
#define RATHOLE_MAX_DATA_CH   20   /* pooled+forwarding cap; server pools 8 per service */

/* Load configs from NVS and start control tasks for tunnels with auto=1.
 * Call once from setup() after load_config(). Tasks wait for WiFi.     */
void rathole_init(void);

/* Set one config field (server/token/service/local/auto/retry), persist to NVS.
 * key: "server" | "token" | "service" | "local" | "auto" | "retry".
 * If the tunnel is running, it is restarted to pick up the change.     */
bool rathole_set(int idx, const String& key, const String& val);

/* Read one config field for form prefill (token is returned; guard who
 * can reach the HTTP page). */
String rathole_get(int idx, const String& key);

/* Global master switch (NVS "rathole_en", default on). When off, all
 * tunnels are stopped and none auto-start at boot; rathole_start()
 * refuses. When switched back on, tunnels with auto=1 start again.   */
void rathole_set_enabled(bool en);
bool rathole_is_enabled(void);

/* Start/stop the control channel task for tunnel idx (0-based). */
bool rathole_start(int idx);
void rathole_stop(int idx);

/* Stop the tunnel and wipe its NVS keys. */
void rathole_clear(int idx);

/* Per-tunnel status as a JSON object fragment. */
String rathole_status_json(int idx);
