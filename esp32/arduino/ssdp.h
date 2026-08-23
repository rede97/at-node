/*
 * ssdp.h - SSDP/UPnP discovery (FEATURE_SSDP; requires FEATURE_HTTP)
 *
 * Wire behavior per the esp32_matrix reference sketch: join
 * 239.255.255.250:1900, NOTIFY ssdp:alive x3 at start + renewal every
 * MAX_AGE/2, unicast reply to M-SEARCH (ssdp:all / upnp:rootdevice /
 * Basic:1). UPnP device description at /description.xml carries
 * presentationURL = http://<ip>/ (the SPA) — that is the page Windows
 * opens with "View device webpage" in Explorer's Network view.
 *
 * Runtime tie: active only while the HTTP service is enabled
 * (set_http_enabled / wifi_services_up drive ssdp_begin/ssdp_stop).
 */
#pragma once
#include <Arduino.h>

class WebServer;

#if FEATURE_SSDP

/* Join the multicast group, register routes on srv, announce alive x3.
 * Call whenever the HTTP service comes up (idempotent; srv = &g_http). */
void ssdp_begin(WebServer* srv);

/* Leave the multicast group. Call when the HTTP service goes down. */
void ssdp_stop(void);

/* Pump: M-SEARCH replies + periodic alive renewal. Call from loop(). */
void ssdp_loop(void);

#endif /* FEATURE_SSDP */
