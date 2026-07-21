/********************************** (C) COPYRIGHT *******************************
 * File Name          : ble_dongle.c
 * Author             : at-node
 * Description        : BLE HID Host (dongle/receiver) — Central role task
 *
 *   State machine (adapted from WCH EVT Central example, specialized to
 *   HID keyboard host):
 *
 *     idle --AT+BT_SCAN--> scanning --GAP_DEVICE_INFO--> collect HID
 *       advertisers --GAP_DEVICE_DISCOVERY--> list via AT_Response
 *     idle --AT+BT_CONN--> connecting --GAP_LINK_ESTABLISHED-->
 *       GATT discovery: HID svc 0x1812
 *         -> Boot Key Input char 0x2A22 (value handle)
 *         -> Protocol Mode char 0x2A4E  (value handle)
 *         -> CCCD 0x2902 (first after boot char)
 *       write Protocol Mode = 0 (Boot protocol)
 *       write CCCD = 1 (notify enable)
 *     armed: ATT_HANDLE_VALUE_NOTI on boot handle -> forward 8-byte
 *       report to USB HID via kb_usb_send_report().
 ********************************************************************************/

#include "config.h"
#include "hws.h"
#include "ble_dongle.h"
#include "at_parser.h"

#if(defined(BLE_DONGLE)) && (BLE_DONGLE == TRUE)

/* Verbose diagnostics gate — see config.h BLE_DONGLE_DEBUG.
   ON: +BT_ADV/+BT_DISC raw dumps/+BT_GATT/+BT_RD/+BT_NTF over the AT
   channel (development). OFF: only user-facing results remain
   (+BT_SCAN/+BT_CONN/+BT_BOND/link-up-down events). */
#if(defined(BLE_DONGLE_DEBUG)) && (BLE_DONGLE_DEBUG == TRUE)
#define DGL_DBG(...)  AT_Response(__VA_ARGS__)
#define DGL_DBG_ON    1
#else
#define DGL_DBG(...)  ((void)0)
#define DGL_DBG_ON    0
#endif

/* USB keyboard report sender (hidkbd_usb.c) — dongle forwards here */
extern void kb_usb_send_report(uint8_t mods, uint8_t *keys, int count);

/* ===== Configuration ===== */

#define DGL_MAX_SCAN_RES     16      /* scan list size — RPA devices (rotating
                                        addresses) defeat dedupe and flood the
                                        list, so this must be generous */
#define DGL_NAME_LEN         20      /* captured device name (truncated) */
#define DGL_SCAN_SECONDS     5       /* default when AT+BT_SCAN has no arg */
#define DGL_LINK_TIMEOUT_MS  5000    /* establish-link watchdog */
#define DGL_AUTO_RETRY_MS    2000    /* auto-reconnect re-kick after a
                                        failed establish (stack busy) */

/* TMOS task events */
#define DGL_START_DEVICE_EVT   0x0001
#define DGL_SVC_DISC_EVT       0x0002
#define DGL_LINK_TIMEOUT_EVT   0x0004
#define DGL_SCAN_EVT           0x0008   /* deferred scan start (from AT task) */
#define DGL_CONN_EVT           0x0010   /* deferred connect    (from AT task) */
#define DGL_AUTO_EVT           0x0020   /* auto-reconnect kick / abort */

/* Pending requests — set by AT task, consumed by dongle task.
   GAP procedures (discovery / link establish) MUST be started from the
   dongle task's own context: the GAP layer routes result events to the
   TMOS task that started the procedure, and only the dongle task pumps
   GAP messages. Starting them from the AT task loses all events. */
static int8_t   dgl_pend_scan_sec = -1;
static int8_t   dgl_pend_conn_idx = -1;

/* GATT discovery sub-states */
enum {
    DISC_IDLE = 0,
    DISC_SVC,        /* finding HID service 0x1812 */
    DISC_BOOT_CHAR,  /* finding Boot Key Input char 0x2A22 */
    DISC_PROTO_CHAR, /* finding Protocol Mode char 0x2A4E */
    DISC_RPT_CHAR,   /* fallback: finding Report chars 0x2A4D */
    DISC_CCCD,       /* finding CCCD 0x2902 */
};

/* Dongle link states */
enum {
    DGL_IDLE = 0,
    DGL_SCANNING,
    DGL_CONNECTING,
    DGL_CONNECTED,   /* link up, GATT discovery in progress */
    DGL_ARMED,       /* boot report notify enabled, forwarding */
    DGL_AUTOCONN,    /* auto-reconnect: white-list establish pending */
};

/* Auto-reconnect control (AT+BT_AUTO). dgl_auto is the feature switch
   (default per config.h BLE_DONGLE_AUTO); dgl_auto_hold is a one-shot
   suppress set by a manual AT+BT_DISC so the link doesn't bounce back,
   cleared by AT+BT_AUTO=1 or any successful connection. */
#if(defined(BLE_DONGLE_AUTO)) && (BLE_DONGLE_AUTO == TRUE)
static uint8_t  dgl_auto = 1;
#else
static uint8_t  dgl_auto = 0;
#endif
static uint8_t  dgl_auto_hold = 0;

/* DIAG: StartDevice result, queryable via AT+BT_STATE */
static bStatus_t dgl_start_status = 0xFF;

/* Discovery retry budget — first attempt may race pairing or hit
   ATT_ERR_INSUFFICIENT_AUTHEN; retry a couple of times before giving up */
static uint8_t  dgl_disc_retries;

/* Passkey entry state — set when the keyboard displays a code and waits
   for us to input it (AT+BT_PASSKEY) */
static uint16_t dgl_passkey_conn = 0xFFFF;
static uint8_t  dgl_passkey_pending = 0;
static uint32_t dgl_passkey_preset = 123456;   /* default; most keyboards
                                                  accept the common static
                                                  PIN — user can override
                                                  via AT+BT_PASSKEY */

/* ===== State ===== */

static tmosTaskID dgl_task_id = INVALID_TASK_ID;
static uint8_t  dgl_state = DGL_IDLE;
static uint8_t  dgl_disc_state = DISC_IDLE;
static uint16_t dgl_conn_handle = GAP_CONNHANDLE_INIT;

/* Scan results */
typedef struct {
    uint8_t  addr[B_ADDR_LEN];
    uint8_t  addr_type;
    int8_t   rssi;
    char     name[DGL_NAME_LEN + 1];
} dgl_scan_rec_t;

static dgl_scan_rec_t dgl_scan_list[DGL_MAX_SCAN_RES];
static uint8_t  dgl_scan_count = 0;

/* Discovered handles */
static uint16_t dgl_svc_start, dgl_svc_end;
static uint16_t dgl_boot_hdl;    /* Boot Keyboard Input Report value handle */
static uint16_t dgl_proto_hdl;   /* Protocol Mode value handle */

/* Report-mode fallback (keyboards without Boot protocol): subscribe to
   every Report char's notify and forward 8-byte reports as the standard
   boot layout — most keyboards' main input report matches it. */
#define DGL_MAX_REPORTS  8
static uint16_t dgl_rpt_hdls[DGL_MAX_REPORTS];
static uint8_t  dgl_rpt_count;

/* CCCD write queue — ATT allows one outstanding write per connection,
   so CCCDs are enabled one at a time, advanced on ATT_WRITE_RSP. */
#define DGL_MAX_CCCD  8
static uint16_t dgl_cccd_list[DGL_MAX_CCCD];
static uint8_t  dgl_cccd_count, dgl_cccd_wr;
static uint8_t  dgl_proto_pending;   /* protocol-mode write awaiting RSP */

/* Post-arm DIAG: read each report char once — proves the chars are
   readable (pure notify gating) vs auth-gated (error). */
static uint8_t  dgl_rd_idx;

/* Reset all per-link state. Called when a link comes UP (stale data
   from a previous keyboard must never leak into the new discovery)
   and when it goes DOWN (stale handles would misfire the report
   fallback on reconnect, and a half-written CCCD queue would skip
   notify enables). */
static void dgl_reset_link_state(void)
{
    dgl_conn_handle  = GAP_CONNHANDLE_INIT;
    dgl_disc_state   = DISC_IDLE;
    dgl_disc_retries = 0;
    dgl_svc_start = dgl_svc_end = 0;
    dgl_boot_hdl = dgl_proto_hdl = 0;
    dgl_rpt_count = 0;
    dgl_cccd_count = dgl_cccd_wr = 0;
    dgl_cccd_list[0] = 0;
    dgl_proto_pending = 0;
    dgl_rd_idx = 0;
    dgl_passkey_pending = 0;
    dgl_passkey_conn = 0xFFFF;
}

/* Auto-reconnect: establish a link to the bonded keyboard's identity
   address (bond 0 in SNV). High duty cycle catches the short directed-
   advertising window (~1.28 s) a keyboard emits after power-up looking
   for its bonded host.

   NOTE: white-list establish (whiteList=TRUE + GAPBOND_AUTO_SYNC_WL/RL)
   was tried first and abandoned — on this stack the WL path started
   but never matched the advertiser (two-board test 2026-07-21: kbd
   AT+BT_DISC -> establish ran 25 s, no connect). Direct identity-
   address establish is the same call the manual AT+BT_CONN path uses
   and is proven. RPA keyboards may need the WL/RL path revisited
   (PLAN phase 3, RK test).
   MUST run in the dongle task context (GAP procedure — see the
   DGL_SCAN_EVT comment). Callers defer via DGL_AUTO_EVT. */
static void dgl_auto_kick(void)
{
    uint8_t bonds = 0;
    GAPBondMgr_GetParameter(GAPBOND_BOND_COUNT, &bonds);
    if (!dgl_auto || dgl_auto_hold || bonds == 0 || dgl_state != DGL_IDLE)
        return;

    gapBondRec_t rec;
    if (tmos_snv_read(calcNvID(0, GAP_BOND_REC_ID_OFFSET),
                      sizeof(rec), &rec) != SUCCESS) {
        DGL_DBG("+BT_AUTO: snv read err, retry");
        tmos_start_task(dgl_task_id, DGL_AUTO_EVT, MS1_TO_SYSTEM_TIME(DGL_AUTO_RETRY_MS));
        return;
    }
    bStatus_t st = GAPRole_CentralEstablishLink(TRUE /*highDutyCycle*/,
                                                FALSE /*direct*/,
                                                rec.publicAddrType,
                                                rec.publicAddr);
    if (st == SUCCESS) {
        dgl_state = DGL_AUTOCONN;
        AT_Response("+BT_AUTO: reconnecting (%d bonded)", bonds);
    } else {
        /* stack busy — retry shortly (bounded by the flag checks above) */
        DGL_DBG("+BT_AUTO: establish st=%d, retry", st);
        tmos_start_task(dgl_task_id, DGL_AUTO_EVT, MS1_TO_SYSTEM_TIME(DGL_AUTO_RETRY_MS));
    }
}

/* ===== Forward decls ===== */
static void dgl_event_cb(gapRoleEvent_t *pEvent);
static void dgl_rssi_cb(uint16_t connHandle, int8_t rssi);
static void dgl_mtu_cb(uint16_t connHandle, uint16_t maxTxOctets, uint16_t maxRxOctets);
static void dgl_pair_state_cb(uint16_t connHandle, uint8_t state, uint8_t status);
static void dgl_passcode_cb(uint8_t *deviceAddr, uint16_t connectionHandle,
                            uint8_t uiInputs, uint8_t uiOutputs);

/* GAP callbacks */
static gapCentralRoleCB_t dgl_role_cb = {
    dgl_rssi_cb,
    dgl_event_cb,
    dgl_mtu_cb,
};
static gapBondCBs_t dgl_bond_cb = {
    dgl_passcode_cb,
    dgl_pair_state_cb,
};

/* ===== Advertising data helpers ===== */

/* Check adv data for 16-bit service UUID 0x1812 (HID) and capture name.
   AD structure: [len][type][data...] repeated. */
static uint8_t dgl_adv_is_hid(const uint8_t *data, uint8_t len)
{
    uint8_t i = 0;
    while (i + 1 < len) {
        uint8_t ad_len = data[i];
        uint8_t ad_type = data[i + 1];
        if (ad_len == 0 || i + ad_len > len) break;
        if ((ad_type == 0x02 || ad_type == 0x03) && ad_len >= 3) {
            /* 16-bit service UUID list (partial/complete) */
            for (uint8_t j = 0; j + 1 < ad_len - 1; j += 2) {
                uint16_t uuid = BUILD_UINT16(data[i + 2 + j], data[i + 3 + j]);
                if (uuid == HID_SERV_UUID)
                    return 1;
            }
        }
        i += ad_len + 1;
    }
    return 0;
}

static void dgl_adv_get_name(const uint8_t *data, uint8_t len, char *out)
{
    uint8_t i = 0;
    out[0] = '\0';
    while (i + 1 < len) {
        uint8_t ad_len = data[i];
        uint8_t ad_type = data[i + 1];
        if (ad_len == 0 || i + ad_len > len) break;
        if (ad_type == 0x09 || ad_type == 0x08) {  /* complete/shortened name */
            uint8_t n = ad_len - 1;
            if (n > DGL_NAME_LEN) n = DGL_NAME_LEN;
            tmos_memcpy(out, &data[i + 2], n);
            out[n] = '\0';
            return;
        }
        i += ad_len + 1;
    }
}

static void dgl_add_scan_rec(gapDeviceInfoEvent_t *info)
{
    /* Inclusive filter: HID service UUID OR any device name.
       Many keyboards never put 0x1812 in advertising (only in GATT),
       so a strict HID filter misses them — name is how the user
       identifies their keyboard anyway. */
    uint8_t is_hid = dgl_adv_is_hid(info->pEvtData, info->dataLen);
    char name[DGL_NAME_LEN + 1];
    dgl_adv_get_name(info->pEvtData, info->dataLen, name);
    if (!is_hid && name[0] == '\0')
        return;

    /* dedupe by address — but merge: a scan response may carry the
       name that the bare advertisement lacked */
    for (uint8_t i = 0; i < dgl_scan_count; i++) {
        if (tmos_memcmp(info->addr, dgl_scan_list[i].addr, B_ADDR_LEN)) {
            if (name[0] && dgl_scan_list[i].name[0] == '\0')
                tmos_memcpy(dgl_scan_list[i].name, name, sizeof(name));
            dgl_scan_list[i].rssi = info->rssi;
            return;
        }
    }
    if (dgl_scan_count >= DGL_MAX_SCAN_RES) {
        /* List full (RPA spam defeats dedupe): evict the weakest entry if
           the new one is stronger — keeps the nearby keyboard discoverable
           in noisy RF environments. */
        uint8_t weak = 0;
        for (uint8_t i = 1; i < dgl_scan_count; i++)
            if (dgl_scan_list[i].rssi < dgl_scan_list[weak].rssi)
                weak = i;
        if (info->rssi <= dgl_scan_list[weak].rssi)
            return;
        dgl_scan_rec_t *rec = &dgl_scan_list[weak];
        tmos_memcpy(rec->addr, info->addr, B_ADDR_LEN);
        rec->addr_type = info->addrType;
        rec->rssi = info->rssi;
        tmos_memcpy(rec->name, name, sizeof(rec->name));
        AT_Response("+BT_SCAN:%d,%02X%02X%02X%02X%02X%02X,%d,%s%s (evict)",
                weak,
                rec->addr[5], rec->addr[4], rec->addr[3],
                rec->addr[2], rec->addr[1], rec->addr[0],
                rec->rssi, rec->name[0] ? rec->name : "?",
                is_hid ? " [HID]" : "");
        return;
    }

    dgl_scan_rec_t *rec = &dgl_scan_list[dgl_scan_count];
    tmos_memcpy(rec->addr, info->addr, B_ADDR_LEN);
    rec->addr_type = info->addrType;
    rec->rssi = info->rssi;
    tmos_memcpy(rec->name, name, sizeof(rec->name));
    dgl_scan_count++;

    AT_Response("+BT_SCAN:%d,%02X%02X%02X%02X%02X%02X,%d,%s%s",
                dgl_scan_count - 1,
                rec->addr[5], rec->addr[4], rec->addr[3],
                rec->addr[2], rec->addr[1], rec->addr[0],
                rec->rssi, rec->name[0] ? rec->name : "?",
                is_hid ? " [HID]" : "");
}

/* ===== GATT discovery ===== */

/* Forward decls */
static void dgl_discovery_failed(const char *why);

static void dgl_start_svc_discovery(void)
{
    uint8_t uuid[ATT_BT_UUID_SIZE] = { LO_UINT16(HID_SERV_UUID), HI_UINT16(HID_SERV_UUID) };

    dgl_svc_start = dgl_svc_end = 0;
    dgl_boot_hdl = dgl_proto_hdl = 0;
    dgl_disc_state = DISC_SVC;

    bStatus_t st = GATT_DiscPrimaryServiceByUUID(dgl_conn_handle, uuid,
                                                 ATT_BT_UUID_SIZE, dgl_task_id);
    if (st != SUCCESS) {
        /* procedure didn't start (blePending etc.) — no response will
           arrive, count it as a failed attempt right now */
        DGL_DBG("+BT_GATT: disc svc start %d", st);
        dgl_discovery_failed("svc start busy");
    }
}

/* Discovery failed — retry with backoff while budget lasts */
static void dgl_discovery_failed(const char *why)
{
    if (dgl_disc_retries > 0 && dgl_state == DGL_CONNECTED) {
        dgl_disc_retries--;
        AT_Response("+BT_CONN: %s — retry", why);
        tmos_start_task(dgl_task_id, DGL_SVC_DISC_EVT, MS1_TO_SYSTEM_TIME(1000));
    } else {
        AT_Response("+BT_CONN: err %s", why);
    }
    dgl_disc_state = DISC_IDLE;
}

/* Find characteristic by 16-bit UUID within the HID service range */
static void dgl_find_char(uint16_t uuid16, uint8_t next_state)
{
    attReadByTypeReq_t req;
    req.startHandle = dgl_svc_start;
    req.endHandle   = dgl_svc_end;
    req.type.len    = ATT_BT_UUID_SIZE;
    req.type.uuid[0] = LO_UINT16(uuid16);
    req.type.uuid[1] = HI_UINT16(uuid16);
    dgl_disc_state = next_state;
    GATT_ReadUsingCharUUID(dgl_conn_handle, &req, dgl_task_id);
}

/* Write a 1/2-byte value to a handle (protocol mode, CCCD) */
static void dgl_write_handle(uint16_t handle, uint16_t value, uint8_t len)
{
    attWriteReq_t req;
    req.handle = handle;
    req.len = len;
    req.pValue = GATT_bm_alloc(dgl_conn_handle, ATT_WRITE_REQ, len, NULL, 0);
    if (req.pValue == NULL) {
        AT_Response("+BT_CONN: err alloc");
        return;
    }
    req.pValue[0] = LO_UINT16(value);
    if (len > 1) req.pValue[1] = HI_UINT16(value);
    req.sig = 0;
    req.cmd = 0;
    if (GATT_WriteCharValue(dgl_conn_handle, &req, dgl_task_id) != SUCCESS)
        GATT_bm_free((gattMsg_t *)&req, ATT_WRITE_REQ);
}

/* DIAG: dump a raw Read-By-Type response — handle parsing is suspect,
   ground truth needed for stride/offset analysis. tag identifies the
   discovery state: B=boot P=proto R=report C=cccd. */
static void dgl_dump_rbt_rsp(char tag, attReadByTypeRsp_t *r)
{
#if DGL_DBG_ON
    char hex[14 * 2 + 1];
    static const char nib[] = "0123456789ABCDEF";
    uint16_t n = (uint16_t)r->len * r->numPairs;
    if (n > 14) n = 14;
    for (uint16_t i = 0; i < n; i++) {
        hex[i * 2]     = nib[r->pDataList[i] >> 4];
        hex[i * 2 + 1] = nib[r->pDataList[i] & 0xF];
    }
    hex[n * 2] = '\0';
    DGL_DBG("+BT_DISC: %c plen=%d np=%d raw %s", tag, r->len, r->numPairs, hex);
#else
    (void)tag; (void)r;
#endif
}

/* Extract a value handle from item <idx> of a Read-By-Type response.

   WCH's GATT_ReadUsingCharUUID requests by characteristic VALUE uuid,
   so each response item is [valueHandle(2)][currentValue(...)] — the
   handle is ALWAYS at offset 0. (TI docs show the DECLARATION layout —
   handle(2)+props(1)+valueHandle(2)+uuid(2), value handle at offset 3 —
   which only applies when requesting by declaration UUID 0x2803. The
   offset-3 assumption produced garbage handles like 0x23A8.)

   Handles are sanity-checked against the service range by callers. */
static uint16_t dgl_rbt_vhandle(attReadByTypeRsp_t *r, uint8_t idx)
{
    uint8_t *p = &r->pDataList[idx * r->len];
    return BUILD_UINT16(p[0], p[1]);
}

/* Advance the discovery state machine on each GATT response */
static void dgl_discovery_event(gattMsgEvent_t *pMsg)
{
    uint8_t complete = ((pMsg->hdr.status == bleProcedureComplete) ||
                        (pMsg->method == ATT_ERROR_RSP));

    /* DIAG: surface ATT errors (e.g. insufficient authentication when
       the keyboard demands an encrypted link before exposing chars) */
    if (pMsg->method == ATT_ERROR_RSP) {
        DGL_DBG("+BT_GATT: err %02X hdl=%04X state=%d",
                pMsg->msg.errorRsp.errCode,
                pMsg->msg.errorRsp.handle,
                dgl_disc_state);
        if (pMsg->msg.errorRsp.errCode == ATT_ERR_INSUFFICIENT_AUTHEN) {
            /* Pairing in progress — park discovery so the pair-complete
               retry restarts it cleanly (guard requires DISC_IDLE). */
            dgl_disc_state = DISC_IDLE;
            return;
        }
    }

    switch (dgl_disc_state) {
    case DISC_SVC:
        if (pMsg->method == ATT_FIND_BY_TYPE_VALUE_RSP &&
            pMsg->msg.findByTypeValueRsp.numInfo > 0) {
            dgl_svc_start = ATT_ATTR_HANDLE(pMsg->msg.findByTypeValueRsp.pHandlesInfo, 0);
            dgl_svc_end   = ATT_GRP_END_HANDLE(pMsg->msg.findByTypeValueRsp.pHandlesInfo, 0);
        }
        if (complete) {
            DGL_DBG("+BT_GATT: svc %04X-%04X hdrst=%d", dgl_svc_start, dgl_svc_end, pMsg->hdr.status);
            if (dgl_svc_start) {
                dgl_find_char(BOOT_KEY_INPUT_UUID, DISC_BOOT_CHAR);
            } else {
                dgl_discovery_failed("no HID service");
            }
        }
        break;

    case DISC_BOOT_CHAR:
        if (pMsg->method == ATT_READ_BY_TYPE_RSP &&
            pMsg->msg.readByTypeRsp.numPairs > 0) {
            dgl_dump_rbt_rsp('B', &pMsg->msg.readByTypeRsp);
            uint16_t h = dgl_rbt_vhandle(&pMsg->msg.readByTypeRsp, 0);
            if (h > dgl_svc_start && h <= dgl_svc_end) {
                dgl_boot_hdl = h;
            } else {
                DGL_DBG("+BT_DISC: B bad hdl %04X", h);
            }
        }
        if (complete) {
            if (dgl_boot_hdl) {
                dgl_find_char(PROTOCOL_MODE_UUID, DISC_PROTO_CHAR);
            } else {
                /* No Boot protocol — fall back to Report mode:
                   subscribe every Report char and forward 8-byte
                   reports in the standard keyboard layout. */
                dgl_rpt_count = 0;
                dgl_find_char(REPORT_UUID, DISC_RPT_CHAR);
            }
        }
        break;

    case DISC_RPT_CHAR:
        if (pMsg->method == ATT_READ_BY_TYPE_RSP) {
            dgl_dump_rbt_rsp('R', &pMsg->msg.readByTypeRsp);
            /* collect value handles across ALL response packets */
            for (uint16_t i = 0; i < pMsg->msg.readByTypeRsp.numPairs &&
                                   dgl_rpt_count < DGL_MAX_REPORTS; i++) {
                uint16_t h = dgl_rbt_vhandle(&pMsg->msg.readByTypeRsp, i);
                if (h > dgl_svc_start && h <= dgl_svc_end) {
                    dgl_rpt_hdls[dgl_rpt_count++] = h;
                } else {
                    DGL_DBG("+BT_DISC: R bad hdl %04X", h);
                }
            }
        }
        if (complete) {
            if (dgl_rpt_count) {
                dgl_cccd_count = 0;
                dgl_cccd_list[0] = 0;   /* clear stale pick — boot-mode
                                           "keep smallest" compares against it */
                dgl_find_char(GATT_CLIENT_CHAR_CFG_UUID, DISC_CCCD);
            } else {
                dgl_discovery_failed("no HID reports");
            }
        }
        break;

    case DISC_PROTO_CHAR:
        if (pMsg->method == ATT_READ_BY_TYPE_RSP &&
            pMsg->msg.readByTypeRsp.numPairs > 0) {
            dgl_dump_rbt_rsp('P', &pMsg->msg.readByTypeRsp);
            uint16_t h = dgl_rbt_vhandle(&pMsg->msg.readByTypeRsp, 0);
            if (h > dgl_svc_start && h <= dgl_svc_end) {
                dgl_proto_hdl = h;
            } else {
                DGL_DBG("+BT_DISC: P bad hdl %04X", h);
            }
        }
        if (complete) {
            /* Protocol Mode is optional — proceed to CCCD either way */
            dgl_cccd_count = 0;
            dgl_cccd_list[0] = 0;   /* clear stale pick — boot-mode
                                       "keep smallest" compares against it */
            dgl_find_char(GATT_CLIENT_CHAR_CFG_UUID, DISC_CCCD);
        }
        break;

    case DISC_CCCD:
        if (pMsg->method == ATT_READ_BY_TYPE_RSP) {
            dgl_dump_rbt_rsp('C', &pMsg->msg.readByTypeRsp);
            /* collect CCCDs (all in svc range; each report char has one) */
            for (uint16_t i = 0; i < pMsg->msg.readByTypeRsp.numPairs &&
                                   dgl_cccd_count < DGL_MAX_CCCD; i++) {
                uint16_t h = dgl_rbt_vhandle(&pMsg->msg.readByTypeRsp, i);
                if (h <= dgl_svc_start || h > dgl_svc_end) {
                    DGL_DBG("+BT_DISC: C bad hdl %04X", h);
                    continue;
                }
                if (dgl_boot_hdl) {
                    /* boot mode: single CCCD, first after boot char */
                    if (h > dgl_boot_hdl && (dgl_cccd_list[0] == 0 || h < dgl_cccd_list[0])) {
                        dgl_cccd_list[0] = h;
                        dgl_cccd_count = 1;
                    } else {
                        DGL_DBG("+BT_DISC: C skip %04X (boot=%04X)", h, dgl_boot_hdl);
                    }
                } else {
                    dgl_cccd_list[dgl_cccd_count++] = h;
                }
            }
        }
        if (complete) {
            DGL_DBG("+BT_DISC: C done m=%02X cnt=%d boot=%04X proto=%04X",
                    pMsg->method, dgl_cccd_count, dgl_boot_hdl, dgl_proto_hdl);
            if (dgl_cccd_count) {
                dgl_cccd_wr = 0;
                if (dgl_boot_hdl && dgl_proto_hdl) {
                    /* Boot protocol first, CCCDs after its WRITE_RSP */
                    dgl_proto_pending = 1;
                    dgl_write_handle(dgl_proto_hdl, 0, 1);
                } else {
                    /* enable notify one CCCD at a time (ATT serializes) */
                    dgl_write_handle(dgl_cccd_list[0], GATT_CLIENT_CFG_NOTIFY, 2);
                }
            } else {
                /* Transient races (encryption not up yet, error-rsp before
                   data) can zero the CCCD count — retry like other
                   discovery failures instead of giving up. */
                dgl_discovery_failed("no CCCD");
            }
        }
        break;

    default:
        break;
    }
}

/* Called on ATT_WRITE_RSP / ATT_ERROR_RSP while enabling CCCDs */
static void dgl_cccd_write_next(void)
{
    if (dgl_proto_pending) {
        dgl_proto_pending = 0;
        dgl_write_handle(dgl_cccd_list[0], GATT_CLIENT_CFG_NOTIFY, 2);
        return;
    }
    dgl_cccd_wr++;
    if (dgl_cccd_wr < dgl_cccd_count) {
        dgl_write_handle(dgl_cccd_list[dgl_cccd_wr], GATT_CLIENT_CFG_NOTIFY, 2);
    } else {
        dgl_state = DGL_ARMED;
        dgl_disc_state = DISC_IDLE;
        AT_Response("+BT_CONN: armed (%s mode, %d rpt, %d cccd)",
                    dgl_boot_hdl ? "boot" : "report", dgl_rpt_count, dgl_cccd_count);
#if DGL_DBG_ON
        /* DIAG: kick report-char reads to verify accessibility */
        dgl_rd_idx = 0;
        if (dgl_rpt_count) {
            attReadReq_t rr;
            rr.handle = dgl_rpt_hdls[0];
            GATT_ReadCharValue(dgl_conn_handle, &rr, dgl_task_id);
        }
#endif
    }
}

/* ===== GATT message pump ===== */

static void dgl_process_gatt_msg(gattMsgEvent_t *pMsg)
{
    /* Report notification (boot char or report-mode fallback) — forward
       to USB HID */
    if (pMsg->method == ATT_HANDLE_VALUE_NOTI && dgl_state == DGL_ARMED) {
        uint16_t h = pMsg->msg.handleValueNoti.handle;
        uint16_t l = pMsg->msg.handleValueNoti.len;
        uint8_t *r = pMsg->msg.handleValueNoti.pValue;
        /* DIAG: dump EVERY notification — the keyboard input report may
           sit on a handle outside our match list (multi-report maps) */
        DGL_DBG("+BT_NTF:h=%04X l=%d %02X %02X %02X %02X %02X %02X %02X %02X",
                h, l,
                l > 0 ? r[0] : 0, l > 1 ? r[1] : 0, l > 2 ? r[2] : 0,
                l > 3 ? r[3] : 0, l > 4 ? r[4] : 0, l > 5 ? r[5] : 0,
                l > 6 ? r[6] : 0, l > 7 ? r[7] : 0);
        uint8_t match = (h == dgl_boot_hdl);
        for (uint8_t i = 0; !match && i < dgl_rpt_count; i++)
            match = (h == dgl_rpt_hdls[i]);
        if (match && l >= 8)
            kb_usb_send_report(r[0], &r[2], 6);
        GATT_bm_free(&pMsg->msg, pMsg->method);
        return;
    }

    /* Write responses — advance the proto/CCCD write queue */
    if ((pMsg->method == ATT_WRITE_RSP || pMsg->method == ATT_ERROR_RSP) &&
        (dgl_cccd_count > 0 && dgl_disc_state == DISC_CCCD)) {
        /* DIAG: surface write failures (ERROR_RSP would otherwise be
           silently treated as success and falsely report "armed") */
        if (pMsg->method == ATT_ERROR_RSP) {
            DGL_DBG("+BT_GATT: wr err %02X hdl=%04X",
                    pMsg->msg.errorRsp.errCode, pMsg->msg.errorRsp.handle);
        }
        dgl_cccd_write_next();
        GATT_bm_free(&pMsg->msg, pMsg->method);
        return;
    }

#if DGL_DBG_ON
    /* DIAG: report-char read chain after arming */
    if (pMsg->method == ATT_READ_RSP && dgl_state == DGL_ARMED &&
        dgl_rd_idx < dgl_rpt_count) {
        DGL_DBG("+BT_RD:h=%04X l=%d %02X %02X %02X %02X %02X %02X %02X %02X",
                dgl_rpt_hdls[dgl_rd_idx], pMsg->msg.readRsp.len,
                pMsg->msg.readRsp.pValue[0], pMsg->msg.readRsp.pValue[1],
                pMsg->msg.readRsp.pValue[2], pMsg->msg.readRsp.pValue[3],
                pMsg->msg.readRsp.pValue[4], pMsg->msg.readRsp.pValue[5],
                pMsg->msg.readRsp.pValue[6], pMsg->msg.readRsp.pValue[7]);
        dgl_rd_idx++;
        if (dgl_rd_idx < dgl_rpt_count) {
            attReadReq_t rr;
            rr.handle = dgl_rpt_hdls[dgl_rd_idx];
            GATT_ReadCharValue(dgl_conn_handle, &rr, dgl_task_id);
        }
        GATT_bm_free(&pMsg->msg, pMsg->method);
        return;
    }
    if (pMsg->method == ATT_ERROR_RSP && dgl_state == DGL_ARMED &&
        dgl_rd_idx < dgl_rpt_count) {
        DGL_DBG("+BT_RD:h=%04X err %02X",
                dgl_rpt_hdls[dgl_rd_idx], pMsg->msg.errorRsp.errCode);
        dgl_rd_idx++;
        if (dgl_rd_idx < dgl_rpt_count) {
            attReadReq_t rr;
            rr.handle = dgl_rpt_hdls[dgl_rd_idx];
            GATT_ReadCharValue(dgl_conn_handle, &rr, dgl_task_id);
        }
        GATT_bm_free(&pMsg->msg, pMsg->method);
        return;
    }
#endif

    if (dgl_disc_state != DISC_IDLE)
        dgl_discovery_event(pMsg);

    GATT_bm_free(&pMsg->msg, pMsg->method);
}

/* ===== GAP callbacks ===== */

static void dgl_event_cb(gapRoleEvent_t *pEvent)
{
    switch (pEvent->gap.opcode) {
    case GAP_DEVICE_INIT_DONE_EVENT:
        /* Central role ready — if keyboards are bonded, auto-reconnect
           (manual AT+BT_SCAN/CONN remains available when idle). */
        AT_Response("+BT: central ready");
        tmos_start_task(dgl_task_id, DGL_AUTO_EVT, MS1_TO_SYSTEM_TIME(1000));
        break;

    case GAP_DEVICE_INFO_EVENT:
        if (dgl_state == DGL_SCANNING) {
            /* DIAG: log every advertiser (pre-filter) to verify scan works */
            DGL_DBG("+BT_ADV:%02X%02X%02X%02X%02X%02X,%d,len=%d",
                    pEvent->deviceInfo.addr[5], pEvent->deviceInfo.addr[4],
                    pEvent->deviceInfo.addr[3], pEvent->deviceInfo.addr[2],
                    pEvent->deviceInfo.addr[1], pEvent->deviceInfo.addr[0],
                    pEvent->deviceInfo.rssi, pEvent->deviceInfo.dataLen);
            dgl_add_scan_rec(&pEvent->deviceInfo);
        }
        break;

    case GAP_DIRECT_DEVICE_INFO_EVENT:
        /* Directed advertising (ADV_DIRECT_IND): a keyboard that still
           holds a bond for OUR address is trying to reconnect (after a
           reflash wiped our SNV). No payload — address only, but being
           directed at us is signal enough: list it unfiltered. */
        if (dgl_state == DGL_SCANNING && dgl_scan_count < DGL_MAX_SCAN_RES) {
            uint8_t dup = 0;
            for (uint8_t i = 0; i < dgl_scan_count; i++)
                dup |= tmos_memcmp(pEvent->deviceDirectInfo.addr,
                                   dgl_scan_list[i].addr, B_ADDR_LEN);
            if (!dup) {
                dgl_scan_rec_t *rec = &dgl_scan_list[dgl_scan_count];
                tmos_memcpy(rec->addr, pEvent->deviceDirectInfo.addr, B_ADDR_LEN);
                rec->addr_type = pEvent->deviceDirectInfo.addrType;
                rec->rssi = pEvent->deviceDirectInfo.rssi;
                rec->name[0] = '\0';
                AT_Response("+BT_SCAN:%d,%02X%02X%02X%02X%02X%02X,%d,(directed)",
                            dgl_scan_count,
                            rec->addr[5], rec->addr[4], rec->addr[3],
                            rec->addr[2], rec->addr[1], rec->addr[0], rec->rssi);
                dgl_scan_count++;
            }
        }
        break;

    case GAP_DEVICE_DISCOVERY_EVENT:   /* scan window complete */
        if (dgl_state == DGL_SCANNING) {
            AT_Response("+BT_SCAN: done %d", dgl_scan_count);
            dgl_state = DGL_IDLE;
        }
        break;

    case GAP_LINK_ESTABLISHED_EVENT:
        tmos_stop_task(dgl_task_id, DGL_LINK_TIMEOUT_EVT);
        if (pEvent->gap.hdr.status == SUCCESS) {
            uint8_t was_auto = (dgl_state == DGL_AUTOCONN);
            dgl_reset_link_state();   /* no stale handles from a prior link */
            dgl_conn_handle = pEvent->linkCmpl.connectionHandle;
            dgl_state = DGL_CONNECTED;
            dgl_auto_hold = 0;        /* a live link proves user intent */
            dgl_disc_retries = 2;
            AT_Response(was_auto ? "+BT_AUTO: connected" : "+BT_CONN: connected");
            /* kick GATT discovery shortly after link up */
            tmos_start_task(dgl_task_id, DGL_SVC_DISC_EVT, MS1_TO_SYSTEM_TIME(500));
        } else {
            AT_Response("+BT_CONN: failed %X", pEvent->gap.hdr.status);
            dgl_reset_link_state();
            dgl_state = DGL_IDLE;
            /* link attempt failed — re-arm auto-reconnect with backoff */
            tmos_start_task(dgl_task_id, DGL_AUTO_EVT, MS1_TO_SYSTEM_TIME(DGL_AUTO_RETRY_MS));
        }
        break;

    case GAP_LINK_TERMINATED_EVENT:
        dgl_reset_link_state();
        dgl_state = DGL_IDLE;
        AT_Response("+BT_DISC: reason %X", pEvent->linkTerminate.reason);
        /* spontaneous drop — reconnect immediately: a keyboard re-looking
           for its host only directs-advertises for ~1.28 s */
        tmos_set_event(dgl_task_id, DGL_AUTO_EVT);
        break;

    default:
        break;
    }
}

static void dgl_rssi_cb(uint16_t connHandle, int8_t rssi) { (void)connHandle; (void)rssi; }
static void dgl_mtu_cb(uint16_t connHandle, uint16_t maxTxOctets, uint16_t maxRxOctets)
{ (void)connHandle; (void)maxTxOctets; (void)maxRxOctets; }

static void dgl_pair_state_cb(uint16_t connHandle, uint8_t state, uint8_t status)
{
    (void)connHandle;
    if (state == GAPBOND_PAIRING_STATE_COMPLETE) {
        AT_Response("+BT_BOND: pair %s", status == SUCCESS ? "ok" : "fail");
        /* Pairing done — (re)start GATT discovery. Keyboards that gate
           characteristics behind encryption error out the first
           discovery attempt; retry now that the link is encrypted. */
        if (status == SUCCESS && dgl_state == DGL_CONNECTED)
            tmos_set_event(dgl_task_id, DGL_SVC_DISC_EVT);
    }
    else if (state == GAPBOND_PAIRING_STATE_BONDED)
        AT_Response("+BT_BOND: bonded");
}

static void dgl_passcode_cb(uint8_t *deviceAddr, uint16_t connectionHandle,
                            uint8_t uiInputs, uint8_t uiOutputs)
{
    (void)deviceAddr; (void)uiInputs; (void)uiOutputs;
    /* Auto-answer with the preset passkey (123456 default — many
       keyboards accept the common static PIN). If pairing fails and
       the keyboard displays a different code, the user presets it via
       AT+BT_PASSKEY=<code> and reconnects. */
    AT_Response("+BT_BOND: passkey auto %06d (on fail: AT+BT_PASSKEY=<shown>, reconnect)",
                (int)dgl_passkey_preset);
    GAPBondMgr_PasscodeRsp(connectionHandle, SUCCESS, dgl_passkey_preset);
}

/* ===== Passkey entry (AT+BT_PASSKEY) ===== */

int ble_dongle_passkey(uint32_t code)
{
    /* Live request pending — answer it right now */
    if (dgl_passkey_pending) {
        dgl_passkey_pending = 0;
        GAPBondMgr_PasscodeRsp(dgl_passkey_conn, SUCCESS, code);
        dgl_passkey_conn = 0xFFFF;
        return 0;
    }
    /* Otherwise store as preset for the NEXT pairing attempt */
    dgl_passkey_preset = code;
    return 0;
}

/* ===== TMOS task ===== */

static tmosEvents dgl_process_event(tmosTaskID task_id, tmosEvents events)
{
    if (events & SYS_EVENT_MSG) {
        uint8_t *pMsg;
        while ((pMsg = tmos_msg_receive(task_id)) != NULL) {
            tmos_event_hdr_t *hdr = (tmos_event_hdr_t *)pMsg;
            if (hdr->event == GATT_MSG_EVENT)
                dgl_process_gatt_msg((gattMsgEvent_t *)pMsg);
            tmos_msg_deallocate(pMsg);
        }
        return events ^ SYS_EVENT_MSG;
    }

    if (events & DGL_START_DEVICE_EVT) {
        dgl_start_status = GAPRole_CentralStartDevice(dgl_task_id, &dgl_bond_cb, &dgl_role_cb);
        AT_Response("+BT: start device %d", dgl_start_status);
        return events ^ DGL_START_DEVICE_EVT;
    }

    if (events & DGL_SVC_DISC_EVT) {
        /* Guard: don't restart discovery mid-flight (500ms kick and the
           pair-complete retry can both fire while one is running) */
        if (dgl_state == DGL_CONNECTED && dgl_disc_state == DISC_IDLE)
            dgl_start_svc_discovery();
        return events ^ DGL_SVC_DISC_EVT;
    }

    if (events & DGL_AUTO_EVT) {
        if (dgl_state == DGL_AUTOCONN && (!dgl_auto || dgl_auto_hold)) {
            /* aborted via AT+BT_AUTO=0 or AT+BT_DISC — cancel the
               pending white-list establish (TerminateLink with
               INVALID_CONNHANDLE cancels the initiating state) */
            GAPRole_TerminateLink(INVALID_CONNHANDLE);
            dgl_state = DGL_IDLE;
            AT_Response("+BT_AUTO: aborted");
        } else {
            dgl_auto_kick();
        }
        return events ^ DGL_AUTO_EVT;
    }

    if (events & DGL_LINK_TIMEOUT_EVT) {
        if (dgl_state == DGL_CONNECTING) {
            GAPRole_TerminateLink(INVALID_CONNHANDLE);
            dgl_state = DGL_IDLE;
            AT_Response("+BT_CONN: timeout");
        }
        return events ^ DGL_LINK_TIMEOUT_EVT;
    }

    if (events & DGL_SCAN_EVT) {
        /* Deferred from AT task — actual discovery starts HERE (dongle
           task context) so GAP_DEVICE_INFO/DISCOVERY events route back
           to this task's message queue. */
        if (dgl_state == DGL_IDLE && dgl_pend_scan_sec >= 0) {
            GAP_SetParamValue(TGAP_DISC_SCAN, (uint32_t)dgl_pend_scan_sec * 1600);
            dgl_scan_count = 0;
            tmos_memset(dgl_scan_list, 0, sizeof(dgl_scan_list));
            dgl_state = DGL_SCANNING;
            bStatus_t st = GAPRole_CentralStartDiscovery(DEVDISC_MODE_ALL, TRUE, FALSE);
            AT_Response("+BT: disc start %d", st);
        }
        dgl_pend_scan_sec = -1;
        return events ^ DGL_SCAN_EVT;
    }

    if (events & DGL_CONN_EVT) {
        /* Deferred from AT task — see DGL_SCAN_EVT comment. */
        if (dgl_state == DGL_IDLE && dgl_pend_conn_idx >= 0 &&
            dgl_pend_conn_idx < dgl_scan_count) {
            dgl_state = DGL_CONNECTING;
            GAPRole_CentralEstablishLink(FALSE, FALSE,
                                         dgl_scan_list[dgl_pend_conn_idx].addr_type,
                                         dgl_scan_list[dgl_pend_conn_idx].addr);
            tmos_start_task(dgl_task_id, DGL_LINK_TIMEOUT_EVT,
                            MS1_TO_SYSTEM_TIME(DGL_LINK_TIMEOUT_MS));
        }
        dgl_pend_conn_idx = -1;
        return events ^ DGL_CONN_EVT;
    }

    return 0;
}

/* ===== Public API ===== */

void ble_dongle_init(void)
{
    /* GAP Central role init — MUST come before GAPRole_CentralStartDevice
       (see EVT Central/APP/central_main.c). Without it the role's
       internal task mapping doesn't exist and every role API fails. */
    GAPRole_CentralInit();

    dgl_task_id = TMOS_ProcessEventRegister(dgl_process_event);

    /* GAP parameters */
    GAP_SetParamValue(TGAP_DISC_SCAN, DGL_SCAN_SECONDS * 1600);   /* 0.625 ms units */
    GAP_SetParamValue(TGAP_CONN_EST_INT_MIN, 20);                 /* 25 ms */
    GAP_SetParamValue(TGAP_CONN_EST_INT_MAX, 40);                 /* 50 ms */
    GAP_SetParamValue(TGAP_CONN_EST_SUPERV_TIMEOUT, 500);         /* 5 s */

    /* Bond manager (central): INITIATE pairing — keyboards that protect
       HID characteristics with ATT_ERR_INSUFFICIENT_AUTHEN expect the
       central to start pairing; WAIT_FOR_REQ would deadlock.
       IO cap KEYBOARD_ONLY + MITM: the keyboard DISPLAYS a passkey,
       the user reads it and enters it via AT+BT_PASSKEY (many keyboards
       withhold keystream from Just Works bonds — MITM required). */
    {
        uint32_t passkey = 0;
        uint8_t  pairMode = GAPBOND_PAIRING_MODE_INITIATE;
        uint8_t  mitm = TRUE;
        uint8_t  ioCap = GAPBOND_IO_CAP_KEYBOARD_ONLY;
        uint8_t  bonding = TRUE;
        GAPBondMgr_SetParameter(GAPBOND_CENT_DEFAULT_PASSCODE, sizeof(uint32_t), &passkey);
        GAPBondMgr_SetParameter(GAPBOND_CENT_PAIRING_MODE, sizeof(uint8_t), &pairMode);
        GAPBondMgr_SetParameter(GAPBOND_CENT_MITM_PROTECTION, sizeof(uint8_t), &mitm);
        GAPBondMgr_SetParameter(GAPBOND_CENT_IO_CAPABILITIES, sizeof(uint8_t), &ioCap);
        GAPBondMgr_SetParameter(GAPBOND_CENT_BONDING_ENABLED, sizeof(uint8_t), &bonding);
    }

    GATT_InitClient();
    GATT_RegisterForInd(dgl_task_id);

    tmos_set_event(dgl_task_id, DGL_START_DEVICE_EVT);
}

int ble_dongle_scan(uint8_t seconds)
{
    if (dgl_state != DGL_IDLE)
        return -1;
    if (seconds == 0) seconds = DGL_SCAN_SECONDS;

    /* Defer to dongle task — GAP discovery must start in its context */
    dgl_pend_scan_sec = (int8_t)seconds;
    tmos_set_event(dgl_task_id, DGL_SCAN_EVT);
    return 0;
}

int ble_dongle_connect(uint8_t index)
{
    if (dgl_state != DGL_IDLE || index >= dgl_scan_count)
        return -1;

    /* Defer to dongle task — link establish must start in its context */
    dgl_pend_conn_idx = (int8_t)index;
    tmos_set_event(dgl_task_id, DGL_CONN_EVT);
    return 0;
}

int ble_dongle_disconnect(void)
{
    if (dgl_state == DGL_AUTOCONN) {
        /* abort the pending white-list establish and hold auto-reconnect
           (re-arm via AT+BT_AUTO=1 or a successful connection) */
        dgl_auto_hold = 1;
        tmos_set_event(dgl_task_id, DGL_AUTO_EVT);
        return 0;
    }
    if (dgl_conn_handle == GAP_CONNHANDLE_INIT)
        return -1;
    /* manual disconnect must not bounce back via auto-reconnect */
    dgl_auto_hold = 1;
    GAPRole_TerminateLink(dgl_conn_handle);
    return 0;
}

int ble_dongle_auto(int mode)
{
    if (mode < 0)
        return dgl_auto;   /* query */
    dgl_auto = (mode != 0);
    if (dgl_auto)
        dgl_auto_hold = 0;   /* explicit re-enable also re-arms */
    /* task context: cancels a pending establish when turning off,
       kicks a reconnect when turning on */
    tmos_set_event(dgl_task_id, DGL_AUTO_EVT);
    return dgl_auto;
}

void ble_dongle_forget_bonds(void)
{
    /* Single-keyboard model: replacing the keyboard = erase the old
       bond, then scan+conn the new one. Call ble_dongle_disconnect()
       first so auto-reconnect can't re-grab the keyboard being erased. */
    GAPBondMgr_SetParameter(GAPBOND_ERASE_ALLBONDS, 0, NULL);
}

uint8_t ble_dongle_connected(void)
{
    return (dgl_conn_handle != GAP_CONNHANDLE_INIT) ? 1 : 0;
}

/* DIAG — current state for the AT busy-error message */
uint8_t ble_dongle_state_debug(void) { return dgl_state; }

/* DIAG — StartDevice return value (0=SUCCESS, 0xFF=never ran) */
uint8_t ble_dongle_start_status_debug(void) { return dgl_start_status; }

/* ===== Bond list (AT+BT_LIST) ===== */

int ble_dongle_list_bonds(void)
{
    uint8_t count = 0;
    GAPBondMgr_GetParameter(GAPBOND_BOND_COUNT, &count);
    if (count == 0) {
        AT_Response("+BT_LIST: empty");
        return 0;
    }
    for (uint8_t i = 0; i < count; i++) {
        gapBondRec_t rec;
        if (tmos_snv_read(calcNvID(i, GAP_BOND_REC_ID_OFFSET),
                          sizeof(rec), &rec) != SUCCESS) {
            AT_Response("+BT_LIST:%d <snv read err>", i);
            continue;
        }
        AT_Response("+BT_LIST:%d,%02X%02X%02X%02X%02X%02X,flags=%04X",
                    i,
                    rec.publicAddr[5], rec.publicAddr[4], rec.publicAddr[3],
                    rec.publicAddr[2], rec.publicAddr[1], rec.publicAddr[0],
                    rec.stateFlags);
    }
    return (int)count;
}

#endif /* BLE_DONGLE == TRUE */
