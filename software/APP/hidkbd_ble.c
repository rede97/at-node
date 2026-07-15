/********************************** (C) COPYRIGHT *******************************
 * File Name          : hidkbd.c
 * Author             : WCH
 * Version            : V1.0
 * Date               : 2018/12/10
 * Description        : 蓝牙键盘应用程序，初始化广播连接参数，然后广播，直至连接主机后，定时上传键值
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for 
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/

/*********************************************************************
 * INCLUDES
 */
#include "config.h"
#include "ble_dev_info.h"
#include "ble_batt.h"
#include "ble_hid_kbd.h"
#include "ble_hid_dev.h"
#include "hidkbd.h"
#include "usb_dev.h"
#include "hidkbd_common.h"

/*********************************************************************
 * MACROS
 */

#define HID_LED_OUT_RPT_LEN                  1
/*********************************************************************
 * CONSTANTS
 */
// Param update delay
#define START_PARAM_UPDATE_EVT_DELAY         12800

// Param update delay
#define START_PHY_UPDATE_DELAY               1600

// HID idle timeout in msec; set to zero to disable timeout
#define DEFAULT_HID_IDLE_TIMEOUT             60000

// Minimum connection interval (units of 1.25ms)
#define DEFAULT_DESIRED_MIN_CONN_INTERVAL    8

// Maximum connection interval (units of 1.25ms)
#define DEFAULT_DESIRED_MAX_CONN_INTERVAL    8

// Slave latency to use if parameter update request
#define DEFAULT_DESIRED_SLAVE_LATENCY        0

// Supervision timeout value (units of 10ms)
#define DEFAULT_DESIRED_CONN_TIMEOUT         500

// Default passcode
#define DEFAULT_PASSCODE                     0

// Default GAP pairing mode
#define DEFAULT_PAIRING_MODE                 GAPBOND_PAIRING_MODE_WAIT_FOR_REQ

// Default MITM mode (TRUE to require passcode or OOB when pairing)
#define DEFAULT_MITM_MODE                    FALSE

// Default bonding mode, TRUE to bond
#define DEFAULT_BONDING_MODE                 TRUE

// Default GAP bonding I/O capabilities
#define DEFAULT_IO_CAPABILITIES              GAPBOND_IO_CAP_NO_INPUT_NO_OUTPUT

// Battery level is critical when it is less than this %
#define BLE_BATT_DEFAULT_CRITICAL_LEVEL          6

/*********************************************************************
 * TYPEDEFS
 */

/*********************************************************************
 * GLOBAL VARIABLES
 */

// Task ID
static uint8_t ble_hid_emu_task_id = INVALID_TASK_ID;

/*********************************************************************
 * EXTERNAL VARIABLES
 */

/*********************************************************************
 * EXTERNAL FUNCTIONS
 */

/*********************************************************************
 * LOCAL VARIABLES
 */

// GAP Profile - Name attribute for SCAN RSP data
static uint8_t scanRspData[] = {
    0x05, // length of this data
    GAP_ADTYPE_SLAVE_CONN_INTERVAL_RANGE,
    LO_UINT16(DEFAULT_DESIRED_MIN_CONN_INTERVAL), // 100ms
    HI_UINT16(DEFAULT_DESIRED_MIN_CONN_INTERVAL),
    LO_UINT16(DEFAULT_DESIRED_MAX_CONN_INTERVAL), // 1s
    HI_UINT16(DEFAULT_DESIRED_MAX_CONN_INTERVAL),

    // service UUIDs
    0x05, // length of this data
    GAP_ADTYPE_16BIT_MORE,
    LO_UINT16(HID_SERV_UUID),
    HI_UINT16(HID_SERV_UUID),
    LO_UINT16(BATT_SERV_UUID),
    HI_UINT16(BATT_SERV_UUID),

    // Tx power level
    0x02, // length of this data
    GAP_ADTYPE_POWER_LEVEL,
    0 // 0dBm
};

// Advertising data
static uint8_t advertData[] = {
    // flags
    0x02, // length of this data
    GAP_ADTYPE_FLAGS,
    GAP_ADTYPE_FLAGS_LIMITED | GAP_ADTYPE_FLAGS_BREDR_NOT_SUPPORTED,

    // appearance
    0x03, // length of this data
    GAP_ADTYPE_APPEARANCE,
    LO_UINT16(GAP_APPEARE_HID_KEYBOARD),
    HI_UINT16(GAP_APPEARE_HID_KEYBOARD),

    0x07,                           // length of this data (7)
    GAP_ADTYPE_LOCAL_NAME_COMPLETE, // AD Type = Complete local name
    'A',
    'T',
    '-',
    'N',
    'o',
    'd',
    'e',
};

// Device name attribute value
static const uint8_t attDeviceName[GAP_DEVICE_NAME_LEN] = "AT-Node";

// HID Dev configuration
static ble_hid_dev_cfg_t ble_hid_emu_cfg = {
    DEFAULT_HID_IDLE_TIMEOUT, // Idle timeout
    BLE_HID_KBD_FEATURE_FLAGS         // HID feature flags
};

uint16_t ble_hid_emu_conn_handle = GAP_CONNHANDLE_INIT;

uint8_t kb_ble_connected(void)  { return (ble_hid_emu_conn_handle != GAP_CONNHANDLE_INIT) ? 1 : 0; }

void kb_ble_send_report(uint8_t mods, uint8_t *keys, int count)
{
    uint8_t buf[8];
    buf[0] = mods;
    buf[1] = 0;
    for (int j = 0; j < 6; j++) buf[2+j] = (j < count) ? keys[j] : 0;
    ble_hid_dev_report(BLE_HID_RPT_ID_KEY_IN, BLE_HID_REPORT_TYPE_INPUT, 8, buf);
}

/*********************************************************************
 * LOCAL FUNCTIONS
 */

static void    ble_hid_emu_process_tmos_msg(tmos_event_hdr_t *pMsg);
static uint8_t ble_hid_emu_rcv_report(uint8_t len, uint8_t *pData);
static uint8_t ble_hid_emu_rpt_cb(uint8_t id, uint8_t type, uint16_t uuid,
                           uint8_t oper, uint16_t *pLen, uint8_t *pData);
static void    ble_hid_emu_evt_cb(uint8_t evt);
static void    ble_hid_emu_state_cb(gapRole_States_t newState, gapRoleEvent_t *pEvent);

/*********************************************************************
 * PROFILE CALLBACKS
 */

static ble_hid_dev_cb_t ble_hid_emu_cbs = {
    ble_hid_emu_rpt_cb,
    ble_hid_emu_evt_cb,
    NULL,
    ble_hid_emu_state_cb};

/*********************************************************************
 * PUBLIC FUNCTIONS
 */

/*********************************************************************
 * @fn      ble_hid_emu_init
 *
 * @brief   Initialization function for the HidEmuKbd App Task.
 *          This is called during initialization and should contain
 *          any application specific initialization (ie. hardware
 *          initialization/setup, table initialization, power up
 *          notificaiton ... ).
 *
 * @param   task_id - the ID assigned by TMOS.  This ID should be
 *                    used to send messages and set timers.
 *
 * @return  none
 */
void ble_hid_emu_init(void)
{
    ble_hid_emu_task_id = TMOS_ProcessEventRegister(ble_hid_emu_process_event);

    // Setup the GAP Peripheral Role Profile
    {
        uint8_t initial_advertising_enable = TRUE;

        // Set the GAP Role Parameters
        GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &initial_advertising_enable);

        GAPRole_SetParameter(GAPROLE_ADVERT_DATA, sizeof(advertData), advertData);
        GAPRole_SetParameter(GAPROLE_SCAN_RSP_DATA, sizeof(scanRspData), scanRspData);
    }

    // Set the GAP Characteristics
    GGS_SetParameter(GGS_DEVICE_NAME_ATT, sizeof(attDeviceName), (void *)attDeviceName);

    // Setup the GAP Bond Manager
    {
        uint32_t passkey = DEFAULT_PASSCODE;
        uint8_t  pairMode = DEFAULT_PAIRING_MODE;
        uint8_t  mitm = DEFAULT_MITM_MODE;
        uint8_t  ioCap = DEFAULT_IO_CAPABILITIES;
        uint8_t  bonding = DEFAULT_BONDING_MODE;
        GAPBondMgr_SetParameter(GAPBOND_PERI_DEFAULT_PASSCODE, sizeof(uint32_t), &passkey);
        GAPBondMgr_SetParameter(GAPBOND_PERI_PAIRING_MODE, sizeof(uint8_t), &pairMode);
        GAPBondMgr_SetParameter(GAPBOND_PERI_MITM_PROTECTION, sizeof(uint8_t), &mitm);
        GAPBondMgr_SetParameter(GAPBOND_PERI_IO_CAPABILITIES, sizeof(uint8_t), &ioCap);
        GAPBondMgr_SetParameter(GAPBOND_PERI_BONDING_ENABLED, sizeof(uint8_t), &bonding);
    }

    // Setup Battery Characteristic Values
    {
        uint8_t critical = BLE_BATT_DEFAULT_CRITICAL_LEVEL;
        ble_batt_set_param(BLE_BATT_PARAM_CRITICAL_LEVEL, sizeof(uint8_t), &critical);
    }

    // Set up HID keyboard service
    ble_hid_kbd_add_service();

    // Register for HID Dev callback
    ble_hid_dev_register(&ble_hid_emu_cfg, &ble_hid_emu_cbs);

    // Setup a delayed profile startup
    tmos_set_event(ble_hid_emu_task_id, START_DEVICE_EVT);
}

/*********************************************************************
 * @fn      ble_hid_emu_process_event
 *
 * @brief   HidEmuKbd Application Task event processor.  This function
 *          is called to process all events for the task.  Events
 *          include timers, messages and any other user defined events.
 *
 * @param   task_id  - The TMOS assigned task ID.
 * @param   events - events to process.  This is a bit map and can
 *                   contain more than one event.
 *
 * @return  events not processed
 */
uint16_t ble_hid_emu_process_event(uint8_t task_id, uint16_t events)
{

    if(events & SYS_EVENT_MSG)
    {
        uint8_t *pMsg;

        if((pMsg = tmos_msg_receive(ble_hid_emu_task_id)) != NULL)
        {
            ble_hid_emu_process_tmos_msg((tmos_event_hdr_t *)pMsg);

            // Release the TMOS message
            tmos_msg_deallocate(pMsg);
        }

        // return unprocessed events
        return (events ^ SYS_EVENT_MSG);
    }

    if(events & START_DEVICE_EVT)
    {
        return (events ^ START_DEVICE_EVT);
    }

    if(events & START_PARAM_UPDATE_EVT)
    {
        // Send connect param update request
        GAPRole_PeripheralConnParamUpdateReq(ble_hid_emu_conn_handle,
                                             DEFAULT_DESIRED_MIN_CONN_INTERVAL,
                                             DEFAULT_DESIRED_MAX_CONN_INTERVAL,
                                             DEFAULT_DESIRED_SLAVE_LATENCY,
                                             DEFAULT_DESIRED_CONN_TIMEOUT,
                                             ble_hid_emu_task_id);

        return (events ^ START_PARAM_UPDATE_EVT);
    }

    if(events & START_PHY_UPDATE_EVT)
    {
        // start phy update
        PRINT("Send Phy Update %x...\n", GAPRole_UpdatePHY(ble_hid_emu_conn_handle, 0, 
                    GAP_PHY_BIT_LE_2M, GAP_PHY_BIT_LE_2M, GAP_PHY_OPTIONS_NOPRE));

        return (events ^ START_PHY_UPDATE_EVT);
    }

    return 0;
}

/*********************************************************************
 * @fn      ble_hid_emu_process_tmos_msg
 *
 * @brief   Process an incoming task message.
 *
 * @param   pMsg - message to process
 *
 * @return  none
 */
static void ble_hid_emu_process_tmos_msg(tmos_event_hdr_t *pMsg)
{
    switch(pMsg->event)
    {
        default:
            break;
    }
}

/*********************************************************************
 * @fn      ble_hid_emu_state_cb
 *
 * @brief   GAP state change callback.
 *
 * @param   newState - new state
 *
 * @return  none
 */
static void ble_hid_emu_state_cb(gapRole_States_t newState, gapRoleEvent_t *pEvent)
{
    switch(newState & GAPROLE_STATE_ADV_MASK)
    {
        case GAPROLE_STARTED:
        {
            uint8_t ownAddr[6];
            GAPRole_GetParameter(GAPROLE_BD_ADDR, ownAddr);
            GAP_ConfigDeviceAddr(ADDRTYPE_STATIC, ownAddr);
            PRINT("Initialized..\n");
        }
        break;

        case GAPROLE_ADVERTISING:
            if(pEvent->gap.opcode == GAP_MAKE_DISCOVERABLE_DONE_EVENT)
            {
                PRINT("Advertising..\n");
            }
            break;

        case GAPROLE_CONNECTED:
            if(pEvent->gap.opcode == GAP_LINK_ESTABLISHED_EVENT)
            {
                gapEstLinkReqEvent_t *event = (gapEstLinkReqEvent_t *)pEvent;

                // get connection handle
                ble_hid_emu_conn_handle = event->connectionHandle;
                tmos_start_task(ble_hid_emu_task_id, START_PARAM_UPDATE_EVT, START_PARAM_UPDATE_EVT_DELAY);
                PRINT("Connected..\n");
            }
            break;

        case GAPROLE_CONNECTED_ADV:
            if(pEvent->gap.opcode == GAP_MAKE_DISCOVERABLE_DONE_EVENT)
            {
                PRINT("Connected Advertising..\n");
            }
            break;

        case GAPROLE_WAITING:
            if(pEvent->gap.opcode == GAP_END_DISCOVERABLE_DONE_EVENT)
            {
                PRINT("Waiting for advertising..\n");
            }
            else if(pEvent->gap.opcode == GAP_LINK_TERMINATED_EVENT)
            {
                PRINT("Disconnected.. Reason:%x\n", pEvent->linkTerminate.reason);
            }
            else if(pEvent->gap.opcode == GAP_LINK_ESTABLISHED_EVENT)
            {
                PRINT("Advertising timeout..\n");
            }
            // Enable advertising
            {
                uint8_t initial_advertising_enable = TRUE;
                // Set the GAP Role Parameters
                GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &initial_advertising_enable);
            }
            break;

        case GAPROLE_ERROR:
            PRINT("Error %x ..\n", pEvent->gap.opcode);
            break;

        default:
            break;
    }
}

/*********************************************************************
 * @fn      ble_hid_emu_rcv_report
 *
 * @brief   Process an incoming HID keyboard report.
 *
 * @param   len - Length of report.
 * @param   pData - Report data.
 *
 * @return  status
 */
static uint8_t ble_hid_emu_rcv_report(uint8_t len, uint8_t *pData)
{
    // verify data length
    if(len == HID_LED_OUT_RPT_LEN)
    {
        // set LEDs
        return SUCCESS;
    }
    else
    {
        return ATT_ERR_INVALID_VALUE_SIZE;
    }
}

/*********************************************************************
 * @fn      ble_hid_emu_rpt_cb
 *
 * @brief   HID Dev report callback.
 *
 * @param   id - HID report ID.
 * @param   type - HID report type.
 * @param   uuid - attribute uuid.
 * @param   oper - operation:  read, write, etc.
 * @param   len - Length of report.
 * @param   pData - Report data.
 *
 * @return  GATT status code.
 */
static uint8_t ble_hid_emu_rpt_cb(uint8_t id, uint8_t type, uint16_t uuid,
                           uint8_t oper, uint16_t *pLen, uint8_t *pData)
{
    uint8_t status = SUCCESS;

    // write
    if(oper == BLE_HID_DEV_OPER_WRITE)
    {
        if(uuid == REPORT_UUID)
        {
            // process write to LED output report; ignore others
            if(type == BLE_HID_REPORT_TYPE_OUTPUT)
            {
                status = ble_hid_emu_rcv_report(*pLen, pData);
            }
        }

        if(status == SUCCESS)
        {
            status = ble_hid_kbd_set_param(id, type, uuid, *pLen, pData);
        }
    }
    // read
    else if(oper == BLE_HID_DEV_OPER_READ)
    {
        status = ble_hid_kbd_get_param(id, type, uuid, pLen, pData);
    }
    // notifications enabled
    else if(oper == BLE_HID_DEV_OPER_ENABLE)
    {
        // Key scanning already activated in main() — works with USB even without BLE
        // tmos_start_task(ble_hid_emu_task_id, START_REPORT_EVT, 500);
    }
    return status;
}

/*********************************************************************
 * @fn      ble_hid_emu_evt_cb
 *
 * @brief   HID Dev event callback.
 *
 * @param   evt - event ID.
 *
 * @return  HID response code.
 */
static void ble_hid_emu_evt_cb(uint8_t evt)
{
    // process enter/exit suspend or enter/exit boot mode
    return;
}
