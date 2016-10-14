/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <assert.h>
#include <string.h>
#include <errno.h>
#include "bsp/bsp.h"
#include "os/os.h"
#include "nimble/nimble_opt.h"
#include "host/host_hci.h"
#include "ble_hs_priv.h"

/**
 * GAP - Generic Access Profile.
 *
 * Design overview:
 *
 * GAP procedures are initiated by the application via function calls.  Such
 * functions return when either of the following happens:
 *
 * (1) The procedure completes (success or failure).
 * (2) The procedure cannot proceed until a BLE peer responds.
 *
 * For (1), the result of the procedure if fully indicated by the function
 * return code.
 * For (2), the procedure result is indicated by an application-configured
 * callback.  The callback is executed when the procedure completes.
 *
 * Notes on thread-safety:
 * 1. The ble_hs mutex must never be locked when an application callback is
 *    executed.  A callback is free to initiate additional host procedures.
 * 2. Functions called directly by the application never call callbacks.
 *    Generally, these functions lock the ble_hs mutex at the start, and only
 *    unlock it at return.
 * 3. Functions which do call callbacks (receive handlers and timer
 *    expirations) generally only lock the mutex long enough to modify
 *    affected state and make copies of data needed for the callback.  A copy
 *    of various pieces of data is called a "snapshot" (struct
 *    ble_gap_snapshot).  The sole purpose of snapshots is to allow callbacks
 *    to be executed after unlocking the mutex.
 */

/** GAP procedure op codes. */
#define BLE_GAP_OP_NULL                                 0
#define BLE_GAP_OP_M_DISC                               1
#define BLE_GAP_OP_M_CONN                               2
#define BLE_GAP_OP_S_ADV                                1

/**
 * The maximum amount of user data that can be put into the advertising data.
 * The stack may automatically insert some fields on its own, limiting the
 * maximum amount of user data.  The following fields are automatically
 * inserted:
 *     o Flags (3 bytes)
 *     o Tx-power-level (3 bytes) - Only if the application specified a
 *       tx_pwr_llvl_present value of 1 in a call to ble_gap_set_adv_data().
 */
#define BLE_GAP_ADV_DATA_LIMIT_PWR      (BLE_HCI_MAX_ADV_DATA_LEN - 6)
#define BLE_GAP_ADV_DATA_LIMIT_NO_PWR   (BLE_HCI_MAX_ADV_DATA_LEN - 3)

static const struct ble_gap_crt_params ble_gap_params_dflt = {
    .scan_itvl = 0x0010,
    .scan_window = 0x0010,
    .itvl_min = BLE_GAP_INITIAL_CONN_ITVL_MIN,
    .itvl_max = BLE_GAP_INITIAL_CONN_ITVL_MAX,
    .latency = BLE_GAP_INITIAL_CONN_LATENCY,
    .supervision_timeout = BLE_GAP_INITIAL_SUPERVISION_TIMEOUT,
    .min_ce_len = BLE_GAP_INITIAL_CONN_MIN_CE_LEN,
    .max_ce_len = BLE_GAP_INITIAL_CONN_MAX_CE_LEN,
};

static const struct hci_adv_params ble_gap_adv_params_dflt = {
    .adv_itvl_min = 0,
    .adv_itvl_max = 0,
    .adv_type = BLE_HCI_ADV_TYPE_ADV_IND,
    .own_addr_type = BLE_HCI_ADV_OWN_ADDR_PUBLIC,
    .peer_addr_type = BLE_HCI_ADV_PEER_ADDR_PUBLIC,
    .adv_channel_map = BLE_HCI_ADV_CHANMASK_DEF,
    .adv_filter_policy = BLE_HCI_ADV_FILT_DEF,
};

/**
 * The state of the in-progress master connection.  If no master connection is
 * currently in progress, then the op field is set to BLE_GAP_OP_NULL.
 */
static bssnz_t struct {
    uint8_t op;

    unsigned exp_set:1;
    uint32_t exp_os_ticks;

    union {
        struct {
            ble_gap_conn_fn *cb;
            void *cb_arg;

            unsigned using_wl:1;
        } conn;

        struct {
            uint8_t disc_mode;
            ble_gap_disc_fn *cb;
            void *cb_arg;
        } disc;
    };
} ble_gap_master;

/**
 * The state of the in-progress slave connection.  If no slave connection is
 * currently in progress, then the op field is set to BLE_GAP_OP_NULL.
 */
static bssnz_t struct {
    uint8_t op;

    uint8_t conn_mode;
    uint8_t disc_mode;
    ble_gap_conn_fn *cb;
    void *cb_arg;

    uint8_t adv_data[BLE_HCI_MAX_ADV_DATA_LEN];
    uint8_t rsp_data[BLE_HCI_MAX_ADV_DATA_LEN];
    uint8_t adv_data_len;
    uint8_t rsp_data_len;
    int8_t tx_pwr_lvl;

    unsigned adv_pwr_lvl:1;
} ble_gap_slave;

static int ble_gap_disc_tx_disable(void);

struct ble_gap_snapshot {
    struct ble_gap_conn_desc desc;
    ble_gap_conn_fn *cb;
    void *cb_arg;
};

STATS_SECT_DECL(ble_gap_stats) ble_gap_stats;
STATS_NAME_START(ble_gap_stats)
    STATS_NAME(ble_gap_stats, wl_set)
    STATS_NAME(ble_gap_stats, wl_set_fail)
    STATS_NAME(ble_gap_stats, adv_stop)
    STATS_NAME(ble_gap_stats, adv_stop_fail)
    STATS_NAME(ble_gap_stats, adv_start)
    STATS_NAME(ble_gap_stats, adv_start_fail)
    STATS_NAME(ble_gap_stats, adv_set_fields)
    STATS_NAME(ble_gap_stats, adv_set_fields_fail)
    STATS_NAME(ble_gap_stats, adv_rsp_set_fields)
    STATS_NAME(ble_gap_stats, adv_rsp_set_fields_fail)
    STATS_NAME(ble_gap_stats, discover)
    STATS_NAME(ble_gap_stats, discover_fail)
    STATS_NAME(ble_gap_stats, initiate)
    STATS_NAME(ble_gap_stats, initiate_fail)
    STATS_NAME(ble_gap_stats, terminate)
    STATS_NAME(ble_gap_stats, terminate_fail)
    STATS_NAME(ble_gap_stats, cancel)
    STATS_NAME(ble_gap_stats, cancel_fail)
    STATS_NAME(ble_gap_stats, update)
    STATS_NAME(ble_gap_stats, update_fail)
    STATS_NAME(ble_gap_stats, connect_mst)
    STATS_NAME(ble_gap_stats, connect_slv)
    STATS_NAME(ble_gap_stats, disconnect)
    STATS_NAME(ble_gap_stats, rx_disconnect)
    STATS_NAME(ble_gap_stats, rx_update_complete)
    STATS_NAME(ble_gap_stats, rx_adv_report)
    STATS_NAME(ble_gap_stats, rx_conn_complete)
STATS_NAME_END(ble_gap_stats)

/*****************************************************************************
 * $log                                                                      *
 *****************************************************************************/

static void
ble_gap_log_conn(uint8_t addr_type, uint8_t *addr,
                 struct ble_gap_crt_params *params)
{
    BLE_HS_LOG(INFO, "addr_type=%d addr=", addr_type);
    if (addr == NULL) {
        BLE_HS_LOG(INFO, "N/A");
    } else {
        BLE_HS_LOG_ADDR(INFO, addr);
    }

    BLE_HS_LOG(INFO, " scan_itvl=%d scan_window=%d itvl_min=%d itvl_max=%d "
                     "latency=%d supervision_timeout=%d min_ce_len=%d "
                     "max_ce_len=%d",
               params->scan_itvl, params->scan_window, params->itvl_min,
               params->itvl_max, params->latency, params->supervision_timeout,
               params->min_ce_len, params->max_ce_len);
}

static void
ble_gap_log_disc(uint8_t scan_type, uint8_t filter_policy)
{
    BLE_HS_LOG(INFO, "disc_mode=%d filter_policy=%d scan_type=%d",
               ble_gap_master.disc.disc_mode,
               filter_policy, scan_type);
}

static void
ble_gap_log_update(uint16_t conn_handle, struct ble_gap_upd_params *params)
{
    BLE_HS_LOG(INFO, "connection parameter update; "
                     "conn_handle=%d itvl_min=%d itvl_max=%d latency=%d "
                     "supervision_timeout=%d min_ce_len=%d max_ce_len=%d",
               conn_handle, params->itvl_min, params->itvl_max,
               params->latency, params->supervision_timeout,
               params->min_ce_len, params->max_ce_len);
}

static void
ble_gap_log_wl(struct ble_gap_white_entry *white_list,
               uint8_t white_list_count)
{
    struct ble_gap_white_entry *entry;
    int i;

    BLE_HS_LOG(INFO, "count=%d ", white_list_count);

    for (i = 0; i < white_list_count; i++) {
        entry = white_list + i;

        BLE_HS_LOG(INFO, "entry-%d={addr_type=%d addr=", i, entry->addr_type);
        BLE_HS_LOG_ADDR(INFO, entry->addr);
        BLE_HS_LOG(INFO, "} ");
    }
}

static void
ble_gap_log_adv(struct hci_adv_params *adv_params)
{
    BLE_HS_LOG(INFO, "disc_mode=%d addr_type=%d addr=",
               ble_gap_slave.disc_mode, adv_params->peer_addr_type);
    BLE_HS_LOG_ADDR(INFO, adv_params->peer_addr);
    BLE_HS_LOG(INFO, " adv_type=%d adv_channel_map=%d own_addr_type=%d "
                     "adv_filter_policy=%d adv_itvl_min=%d adv_itvl_max=%d "
                     "adv_data_len=%d",
               adv_params->adv_type,
               adv_params->adv_channel_map,
               adv_params->own_addr_type,
               adv_params->adv_filter_policy,
               adv_params->adv_itvl_min,
               adv_params->adv_itvl_max,
               ble_gap_slave.adv_data_len);
}

/*****************************************************************************
 * $snapshot                                                                 *
 *****************************************************************************/

static void
ble_gap_fill_conn_desc(struct ble_hs_conn *conn,
                       struct ble_gap_conn_desc *desc)
{
    desc->conn_handle = conn->bhc_handle;
    desc->peer_addr_type = conn->bhc_addr_type;
    memcpy(desc->peer_addr, conn->bhc_addr, sizeof desc->peer_addr);
    desc->conn_itvl = conn->bhc_itvl;
    desc->conn_latency = conn->bhc_latency;
    desc->supervision_timeout = conn->bhc_supervision_timeout;
    desc->sec_state = conn->bhc_sec_state;
}

static void
ble_gap_conn_to_snapshot(struct ble_hs_conn *conn,
                         struct ble_gap_snapshot *snap)
{
    ble_gap_fill_conn_desc(conn, &snap->desc);
    snap->cb = conn->bhc_cb;
    snap->cb_arg = conn->bhc_cb_arg;
}

static int
ble_gap_find_snapshot(uint16_t handle, struct ble_gap_snapshot *snap)
{
    struct ble_hs_conn *conn;

    ble_hs_lock();

    conn = ble_hs_conn_find(handle);
    if (conn != NULL) {
        ble_gap_conn_to_snapshot(conn, snap);
    }

    ble_hs_unlock();

    if (conn == NULL) {
        return BLE_HS_ENOTCONN;
    } else {
        return 0;
    }
}

/*****************************************************************************
 * $misc                                                                     *
 *****************************************************************************/

static int
ble_gap_call_conn_cb(int event, int status, struct ble_gap_conn_ctxt *ctxt,
                     ble_gap_conn_fn *cb, void *cb_arg)
{
    int rc;

    BLE_HS_DBG_ASSERT(!ble_hs_locked_by_cur_task());

    if (cb != NULL) {
        rc = cb(event, status, ctxt, cb_arg);
    } else {
        if (event == BLE_GAP_EVENT_CONN_UPDATE_REQ) {
            /* Just copy peer parameters back into reply. */
            *ctxt->update.self_params = *ctxt->update.peer_params;
        }
        rc = 0;
    }

    return rc;
}

static void
ble_gap_call_slave_cb(int event, int status, int reset_state)
{
    struct ble_gap_conn_ctxt ctxt;
    struct ble_gap_conn_desc desc;
    ble_gap_conn_fn *cb;
    void *cb_arg;

    ble_hs_lock();

    desc.conn_handle = BLE_HS_CONN_HANDLE_NONE;

    cb = ble_gap_slave.cb;
    cb_arg = ble_gap_slave.cb_arg;

    if (reset_state) {
        ble_gap_slave.op = BLE_GAP_OP_NULL;
    }

    ble_hs_unlock();

    if (cb != NULL) {
        memset(&ctxt, 0, sizeof ctxt);
        ctxt.desc = &desc;

        cb(event, status, &ctxt, cb_arg);
    }
}

static int
ble_gap_call_master_conn_cb(int event, int status, int reset_state)
{
    struct ble_gap_conn_ctxt ctxt;
    struct ble_gap_conn_desc desc;
    ble_gap_conn_fn *cb;
    void *cb_arg;
    int rc;

    ble_hs_lock();

    memset(&desc, 0, sizeof ctxt);

    desc.conn_handle = BLE_HS_CONN_HANDLE_NONE;

    cb = ble_gap_master.conn.cb;
    cb_arg = ble_gap_master.conn.cb_arg;

    if (reset_state) {
        ble_gap_master.op = BLE_GAP_OP_NULL;
    }

    ble_hs_unlock();

    if (cb != NULL) {
        memset(&ctxt, 0, sizeof ctxt);
        ctxt.desc = &desc;

        rc = cb(event, status, &ctxt, cb_arg);
    } else {
        rc = 0;
    }

    return rc;
}

static void
ble_gap_call_master_disc_cb(int event, int status, struct ble_hs_adv *adv,
                            struct ble_hs_adv_fields *fields, int reset_state)
{
    struct ble_gap_disc_desc desc;
    ble_gap_disc_fn *cb;
    void *cb_arg;

    ble_hs_lock();

    if (adv != NULL) {
        desc.event_type = adv->event_type;
        desc.addr_type = adv->addr_type;
        desc.length_data = adv->length_data;
        desc.rssi = adv->rssi;
        memcpy(desc.addr, adv->addr, sizeof adv->addr);
        desc.data = adv->data;
        desc.fields = fields;
    } else {
        memset(&desc, 0, sizeof desc);
    }

    cb = ble_gap_master.disc.cb;
    cb_arg = ble_gap_master.disc.cb_arg;

    if (reset_state) {
        ble_gap_master.op = BLE_GAP_OP_NULL;
    }

    ble_hs_unlock();

    if (cb != NULL) {
        cb(event, status, &desc, cb_arg);
    }
}

static void
ble_gap_update_notify(uint16_t conn_handle, int status)
{
    struct ble_gap_conn_ctxt ctxt;
    struct ble_gap_snapshot snap;
    int rc;

    rc = ble_gap_find_snapshot(conn_handle, &snap);
    if (rc != 0) {
        return;
    }

    memset(&ctxt, 0, sizeof ctxt);
    ctxt.desc = &snap.desc;
    ble_gap_call_conn_cb(BLE_GAP_EVENT_CONN_UPDATED, status, &ctxt,
                         snap.cb, snap.cb_arg);
}

static void
ble_gap_master_set_timer(uint32_t ms_from_now)
{
    ble_gap_master.exp_os_ticks =
        os_time_get() + ms_from_now * OS_TICKS_PER_SEC / 1000;
    ble_gap_master.exp_set = 1;
}

/**
 * Called when an error is encountered while the master-connection-fsm is
 * active.  Resets the state machine, clears the HCI ack callback, and notifies
 * the host task that the next hci_batch item can be processed.
 */
static void
ble_gap_master_failed(int status)
{
    switch (ble_gap_master.op) {
    case BLE_GAP_OP_M_DISC:
        STATS_INC(ble_gap_stats, discover_fail);
        ble_gap_call_master_disc_cb(BLE_GAP_EVENT_DISC_FINISHED, status,
                                    NULL, NULL, 1);
        break;

    case BLE_GAP_OP_M_CONN:
        STATS_INC(ble_gap_stats, initiate_fail);
        ble_gap_call_master_conn_cb(BLE_GAP_EVENT_CONN, status, 1);
        break;

    default:
        break;
    }
}

static void
ble_gap_update_failed(uint16_t conn_handle, int status)
{
    STATS_INC(ble_gap_stats, update_fail);
    ble_hs_atomic_conn_set_flags(conn_handle, BLE_HS_CONN_F_UPDATE, 0);
    ble_gap_update_notify(conn_handle, status);
}

static void
ble_gap_conn_broken(struct ble_gap_snapshot *snap, int status)
{
    struct ble_gap_conn_ctxt ctxt;

    ble_l2cap_sm_connection_broken(snap->desc.conn_handle);
    ble_gattc_connection_broken(snap->desc.conn_handle);

    ble_hs_atomic_conn_delete(snap->desc.conn_handle);

    memset(&ctxt, 0, sizeof ctxt);
    ctxt.desc = &snap->desc;
    ble_gap_call_conn_cb(BLE_GAP_EVENT_CONN, status, &ctxt,
                         snap->cb, snap->cb_arg);

    STATS_INC(ble_gap_stats, disconnect);
}

void
ble_gap_rx_disconn_complete(struct hci_disconn_complete *evt)
{
#if !NIMBLE_OPT(CONNECT)
    return;
#endif

    struct ble_gap_conn_ctxt ctxt;
    struct ble_gap_snapshot snap;
    int status;
    int rc;

    STATS_INC(ble_gap_stats, rx_disconnect);

    rc = ble_gap_find_snapshot(evt->connection_handle, &snap);
    if (rc != 0) {
        /* No longer connected. */
        return;
    }

    if (evt->status == 0) {
        if (evt->reason == BLE_ERR_CONN_TERM_LOCAL) {
            /* Don't confuse the application with an HCI error code in the
             * success case.
             */
            status = BLE_HS_ENOTCONN;
        } else {
            status = BLE_HS_HCI_ERR(evt->reason);
        }
        ble_gap_conn_broken(&snap, status);
    } else {
        memset(&ctxt, 0, sizeof ctxt);
        ctxt.desc = &snap.desc;
        ble_gap_call_conn_cb(BLE_GAP_EVENT_TERM_FAILURE,
                             BLE_HS_HCI_ERR(evt->status), &ctxt,
                             snap.cb, snap.cb_arg);
    }
}

void
ble_gap_rx_update_complete(struct hci_le_conn_upd_complete *evt)
{
#if !NIMBLE_OPT(CONNECT)
    return;
#endif

    struct ble_gap_conn_ctxt ctxt;
    struct ble_gap_snapshot snap;
    struct ble_hs_conn *conn;

    STATS_INC(ble_gap_stats, rx_update_complete);

    ble_hs_lock();

    conn = ble_hs_conn_find(evt->connection_handle);
    if (conn != NULL) {
        if (evt->status == 0) {
            conn->bhc_itvl = evt->conn_itvl;
            conn->bhc_latency = evt->conn_latency;
            conn->bhc_supervision_timeout = evt->supervision_timeout;
        }

        ble_gap_conn_to_snapshot(conn, &snap);
    }

    conn->bhc_flags &= ~BLE_HS_CONN_F_UPDATE;

    ble_hs_unlock();

    if (conn != NULL) {
        memset(&ctxt, 0, sizeof ctxt);
        ctxt.desc = &snap.desc;
        ble_gap_call_conn_cb(BLE_GAP_EVENT_CONN_UPDATED,
                             BLE_HS_HCI_ERR(evt->status), &ctxt,
                             snap.cb, snap.cb_arg);
    }
}

/**
 * Tells you if the BLE host is in the process of creating a master connection.
 */
int
ble_gap_master_in_progress(void)
{
    return ble_gap_master.op != BLE_GAP_OP_NULL;
}

/**
 * Tells you if the BLE host is in the process of creating a slave connection.
 */
int
ble_gap_slave_in_progress(void)
{
    return ble_gap_slave.op != BLE_GAP_OP_NULL;
}

static int
ble_gap_currently_advertising(void)
{
    return ble_gap_slave.op == BLE_GAP_OP_S_ADV;
}

/**
 * Attempts to complete the master connection process in response to a
 * "connection complete" event from the controller.  If the master connection
 * FSM is in a state that can accept this event, and the peer device address is
 * valid, the master FSM is reset and success is returned.
 *
 * @param addr_type             The address type of the peer; one of the
 *                                  following values:
 *                                  o    BLE_ADDR_TYPE_PUBLIC
 *                                  o    BLE_ADDR_TYPE_RANDOM
 * @param addr                  The six-byte address of the connection peer.
 *
 * @return                      0 if the connection complete event was
 *                                  accepted;
 *                              BLE_HS_ENOENT if the event does not apply.
 */
static int
ble_gap_accept_master_conn(uint8_t addr_type, uint8_t *addr)
{
    int rc;

    switch (ble_gap_master.op) {
    case BLE_GAP_OP_NULL:
    case BLE_GAP_OP_M_DISC:
        rc = BLE_HS_ENOENT;
        break;

    case BLE_GAP_OP_M_CONN:
        rc = 0;
        break;

    default:
        BLE_HS_DBG_ASSERT(0);
        rc = BLE_HS_ENOENT;
        break;
    }

    if (rc == 0) {
        STATS_INC(ble_gap_stats, connect_mst);
    }

    return rc;
}

/**
 * Attempts to complete the slave connection process in response to a
 * "connection complete" event from the controller.  If the slave connection
 * FSM is in a state that can accept this event, and the peer device address is
 * valid, the master FSM is reset and success is returned.
 *
 * @param addr_type             The address type of the peer; one of the
 *                                  following values:
 *                                  o    BLE_ADDR_TYPE_PUBLIC
 *                                  o    BLE_ADDR_TYPE_RANDOM
 * @param addr                  The six-byte address of the connection peer.
 *
 * @return                      0 if the connection complete event was
 *                                  accepted;
 *                              BLE_HS_ENOENT if the event does not apply.
 */
static int
ble_gap_accept_slave_conn(uint8_t addr_type, uint8_t *addr)
{
    int rc;

    if (!ble_gap_currently_advertising()) {
        rc = BLE_HS_ENOENT;
    } else {
        switch (ble_gap_slave.conn_mode) {
        case BLE_GAP_CONN_MODE_NON:
            rc = BLE_HS_ENOENT;
            break;

        case BLE_GAP_CONN_MODE_UND:
            rc = 0;
            break;

        case BLE_GAP_CONN_MODE_DIR:
            rc = 0;
            break;

        default:
            BLE_HS_DBG_ASSERT(0);
            rc = BLE_HS_ENOENT;
            break;
        }
    }

    if (rc == 0) {
        STATS_INC(ble_gap_stats, connect_slv);
    }

    return rc;
}

void
ble_gap_rx_adv_report(struct ble_hs_adv *adv)
{
#if !NIMBLE_OPT(ROLE_OBSERVER)
    return;
#endif

    struct ble_hs_adv_fields fields;
    int rc;

    STATS_INC(ble_gap_stats, rx_adv_report);

    if (ble_gap_master.op != BLE_GAP_OP_M_DISC) {
        return;
    }

    rc = ble_hs_adv_parse_fields(&fields, adv->data, adv->length_data);
    if (rc != 0) {
        /* XXX: Increment stat. */
        return;
    }

    if (ble_gap_master.disc.disc_mode == BLE_GAP_DISC_MODE_LTD &&
        !(fields.flags & BLE_HS_ADV_F_DISC_LTD)) {

        return;
    }

    ble_gap_call_master_disc_cb(BLE_GAP_EVENT_DISC_SUCCESS, 0, adv,
                                &fields, 0);
}

/**
 * Processes an incoming connection-complete HCI event.
 */
int
ble_gap_rx_conn_complete(struct hci_le_conn_complete *evt)
{
#if !NIMBLE_OPT(CONNECT)
    return BLE_HS_ENOTSUP;
#endif

    struct ble_gap_conn_ctxt ctxt;
    struct ble_gap_snapshot snap;
    struct ble_hs_conn *conn;
    int rc;

    STATS_INC(ble_gap_stats, rx_conn_complete);

    /* Determine if this event refers to a completed connection or a connection
     * in progress.
     */
    rc = ble_gap_find_snapshot(evt->connection_handle, &snap);

    /* Apply the event to the existing connection if it exists. */
    if (rc == 0) {
        /* XXX: Does this ever happen? */

        if (evt->status != 0) {
            ble_gap_conn_broken(&snap, BLE_HS_HCI_ERR(evt->status));
        }
        return 0;
    }

    /* This event refers to a new connection. */

    if (evt->status != BLE_ERR_SUCCESS) {
        /* Determine the role from the status code. */
        switch (evt->status) {
        case BLE_ERR_DIR_ADV_TMO:
            if (ble_gap_slave_in_progress()) {
                ble_gap_call_slave_cb(BLE_GAP_EVENT_ADV_FINISHED, 0, 1);
            }
            break;

        default:
            if (ble_gap_master_in_progress()) {
                if (evt->status == BLE_ERR_UNK_CONN_ID) {
                    /* Connect procedure successfully cancelled. */
                    ble_gap_call_master_conn_cb(BLE_GAP_EVENT_CANCEL, 0, 1);
                } else {
                    ble_gap_master_failed(BLE_HS_HCI_ERR(evt->status));
                }
            }
            break;
        }

        return 0;
    }

    switch (evt->role) {
    case BLE_HCI_LE_CONN_COMPLETE_ROLE_MASTER:
        rc = ble_gap_accept_master_conn(evt->peer_addr_type, evt->peer_addr);
        if (rc != 0) {
            return rc;
        }
        break;

    case BLE_HCI_LE_CONN_COMPLETE_ROLE_SLAVE:
        rc = ble_gap_accept_slave_conn(evt->peer_addr_type, evt->peer_addr);
        if (rc != 0) {
            return rc;
        }
        break;

    default:
        BLE_HS_DBG_ASSERT(0);
        break;
    }

    /* We verified that there is a free connection when the procedure began. */
    conn = ble_hs_conn_alloc();
    BLE_HS_DBG_ASSERT(conn != NULL);

    conn->bhc_handle = evt->connection_handle;
    memcpy(conn->bhc_addr, evt->peer_addr, sizeof conn->bhc_addr);
    conn->bhc_addr_type = evt->peer_addr_type;
    conn->bhc_itvl = evt->conn_itvl;
    conn->bhc_latency = evt->conn_latency;
    conn->bhc_supervision_timeout = evt->supervision_timeout;
    if (evt->role == BLE_HCI_LE_CONN_COMPLETE_ROLE_MASTER) {
        conn->bhc_flags |= BLE_HS_CONN_F_MASTER;
        conn->bhc_cb = ble_gap_master.conn.cb;
        conn->bhc_cb_arg = ble_gap_master.conn.cb_arg;
        ble_gap_master.op = BLE_GAP_OP_NULL;
    } else {
        conn->bhc_cb = ble_gap_slave.cb;
        conn->bhc_cb_arg = ble_gap_slave.cb_arg;
        ble_gap_slave.op = BLE_GAP_OP_NULL;
    }

    ble_gap_conn_to_snapshot(conn, &snap);

    ble_hs_atomic_conn_insert(conn);

    memset(&ctxt, 0, sizeof ctxt);
    ctxt.desc = &snap.desc;
    ble_gap_call_conn_cb(BLE_GAP_EVENT_CONN, 0, &ctxt, snap.cb, snap.cb_arg);

    return 0;
}

int
ble_gap_rx_l2cap_update_req(uint16_t conn_handle,
                            struct ble_gap_upd_params *params)
{
    struct ble_gap_conn_ctxt ctxt;
    struct ble_gap_snapshot snap;
    int rc;

    rc = ble_gap_find_snapshot(conn_handle, &snap);
    if (rc != 0) {
        return rc;
    }

    if (snap.cb != NULL) {
        memset(&ctxt, 0, sizeof ctxt);
        ctxt.desc = &snap.desc;
        ctxt.update.peer_params = params;
        rc = snap.cb(BLE_GAP_EVENT_L2CAP_UPDATE_REQ, 0, &ctxt, snap.cb_arg);
    } else {
        rc = 0;
    }

    return rc;
}

/**
 * Called by the ble_hs heartbeat timer.  Handles timed out master procedures.
 */
void
ble_gap_heartbeat(void)
{
    int timer_expired;
    int rc;

    if (ble_gap_master.op != BLE_GAP_OP_NULL &&
        ble_gap_master.exp_set &&
        (int32_t)(os_time_get() - ble_gap_master.exp_os_ticks) >= 0) {

        timer_expired = 1;

        /* Clear the timer. */
        ble_gap_master.exp_set = 0;
    } else {
        timer_expired = 0;
    }

    if (timer_expired) {
        switch (ble_gap_master.op) {
        case BLE_GAP_OP_M_DISC:
            /* When a discovery procedure times out, it is not a failure. */
            rc = ble_gap_disc_tx_disable();
            ble_gap_call_master_disc_cb(BLE_GAP_EVENT_DISC_FINISHED, rc,
                                        NULL, NULL, 1);
            break;

        default:
            ble_gap_master_failed(BLE_HS_ETIMEOUT);
            break;
        }
    }
}

/*****************************************************************************
 * $white list                                                               *
 *****************************************************************************/

static int
ble_gap_wl_busy(void)
{
#if !NIMBLE_OPT(WHITELIST)
    return BLE_HS_ENOTSUP;
#endif

    /* Check if an auto or selective connection establishment procedure is in
     * progress.
     */
    return ble_gap_master.op == BLE_GAP_OP_M_CONN &&
           ble_gap_master.conn.using_wl;
}

static int
ble_gap_wl_tx_add(struct ble_gap_white_entry *entry)
{
    uint8_t buf[BLE_HCI_CMD_HDR_LEN + BLE_HCI_CHG_WHITE_LIST_LEN];
    int rc;

    rc = host_hci_cmd_build_le_add_to_whitelist(entry->addr, entry->addr_type,
                                                buf, sizeof buf);
    if (rc != 0) {
        return rc;
    }

    rc = ble_hci_cmd_tx_empty_ack(buf);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

static int
ble_gap_wl_tx_clear(void)
{
    uint8_t buf[BLE_HCI_CMD_HDR_LEN];
    int rc;

    host_hci_cmd_build_le_clear_whitelist(buf, sizeof buf);
    rc = ble_hci_cmd_tx_empty_ack(buf);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

int
ble_gap_wl_set(struct ble_gap_white_entry *white_list,
               uint8_t white_list_count)
{
#if !NIMBLE_OPT(WHITELIST)
    return BLE_HS_ENOTSUP;
#endif

    int rc;
    int i;

    STATS_INC(ble_gap_stats, wl_set);

    if (white_list_count == 0) {
        rc = BLE_HS_EINVAL;
        goto err;
    }

    for (i = 0; i < white_list_count; i++) {
        if (white_list[i].addr_type != BLE_ADDR_TYPE_PUBLIC &&
            white_list[i].addr_type != BLE_ADDR_TYPE_RANDOM) {

            rc = BLE_HS_EINVAL;
            goto err;
        }
    }

    if (ble_gap_wl_busy()) {
        rc = BLE_HS_EBUSY;
        goto err;
    }

    BLE_HS_LOG(INFO, "GAP procedure initiated: set whitelist; ");
    ble_gap_log_wl(white_list, white_list_count);
    BLE_HS_LOG(INFO, "\n");

    rc = ble_gap_wl_tx_clear();
    if (rc != 0) {
        goto err;
    }

    for (i = 0; i < white_list_count; i++) {
        rc = ble_gap_wl_tx_add(white_list + i);
        if (rc != 0) {
            goto err;
        }
    }

    return 0;

err:
    STATS_INC(ble_gap_stats, wl_set_fail);
    return rc;
}

/*****************************************************************************
 * $stop advertise                                                           *
 *****************************************************************************/

static int
ble_gap_adv_disable_tx(void)
{
    uint8_t buf[BLE_HCI_CMD_HDR_LEN + BLE_HCI_SET_ADV_ENABLE_LEN];
    int rc;

    host_hci_cmd_build_le_set_adv_enable(0, buf, sizeof buf);
    rc = ble_hci_cmd_tx_empty_ack(buf);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

int
ble_gap_adv_stop(void)
{
#if !NIMBLE_OPT(ADVERTISE)
    return BLE_HS_ENOTSUP;
#endif

    int rc;

    STATS_INC(ble_gap_stats, adv_stop);

    /* Do nothing if advertising is already disabled. */
    if (!ble_gap_currently_advertising()) {
        rc = BLE_HS_EALREADY;
        goto err;
    }

    BLE_HS_LOG(INFO, "GAP procedure initiated: stop advertising.\n");

    rc = ble_gap_adv_disable_tx();
    if (rc != 0) {
        goto err;
    }

    ble_gap_slave.op = BLE_GAP_OP_NULL;

    return 0;

err:
    STATS_INC(ble_gap_stats, adv_set_fields_fail);
    return rc;
}

/*****************************************************************************
 * $advertise                                                                *
 *****************************************************************************/

static void
ble_gap_adv_itvls(uint8_t disc_mode, uint8_t conn_mode,
                  uint16_t *out_itvl_min, uint16_t *out_itvl_max)
{
    switch (conn_mode) {
    case BLE_GAP_CONN_MODE_NON:
        *out_itvl_min = BLE_GAP_ADV_FAST_INTERVAL2_MIN;
        *out_itvl_max = BLE_GAP_ADV_FAST_INTERVAL2_MAX;
        break;

    case BLE_GAP_CONN_MODE_UND:
        *out_itvl_min = BLE_GAP_ADV_FAST_INTERVAL1_MIN;
        *out_itvl_max = BLE_GAP_ADV_FAST_INTERVAL1_MAX;
        break;

    case BLE_GAP_CONN_MODE_DIR:
        *out_itvl_min = BLE_GAP_ADV_FAST_INTERVAL1_MIN;
        *out_itvl_max = BLE_GAP_ADV_FAST_INTERVAL1_MAX;
        break;

    default:
        BLE_HS_DBG_ASSERT(0);
        break;
    }
}

static int
ble_gap_adv_enable_tx(void)
{
    uint8_t buf[BLE_HCI_CMD_HDR_LEN + BLE_HCI_SET_ADV_PARAM_LEN];
    int rc;

    host_hci_cmd_build_le_set_adv_enable(1, buf, sizeof buf);

    rc = ble_hci_cmd_tx_empty_ack(buf);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

static int
ble_gap_adv_rsp_data_tx(void)
{
    uint8_t buf[BLE_HCI_CMD_HDR_LEN + BLE_HCI_SET_SCAN_RSP_DATA_LEN];
    int rc;

    rc = host_hci_cmd_build_le_set_scan_rsp_data(ble_gap_slave.rsp_data,
                                                 ble_gap_slave.rsp_data_len,
                                                 buf, sizeof buf);
    if (rc != 0) {
        return rc;
    }

    rc = ble_hci_cmd_tx_empty_ack(buf);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

static int
ble_gap_adv_data_tx(void)
{
    uint8_t buf[BLE_HCI_CMD_HDR_LEN + BLE_HCI_SET_ADV_DATA_LEN];
    uint8_t adv_data_len;
    uint8_t flags;
    int rc;

    /* Calculate the value of the flags field from the discoverable mode. */
    flags = 0;
    switch (ble_gap_slave.disc_mode) {
    case BLE_GAP_DISC_MODE_NON:
        break;

    case BLE_GAP_DISC_MODE_LTD:
        flags |= BLE_HS_ADV_F_DISC_LTD;
        break;

    case BLE_GAP_DISC_MODE_GEN:
        flags |= BLE_HS_ADV_F_DISC_GEN;
        break;

    default:
        BLE_HS_DBG_ASSERT(0);
        break;
    }

    flags |= BLE_HS_ADV_F_BREDR_UNSUP;

    /* Encode the flags AD field if it is nonzero. */
    adv_data_len = ble_gap_slave.adv_data_len;
    if (flags != 0) {
        rc = ble_hs_adv_set_flat(BLE_HS_ADV_TYPE_FLAGS, 1, &flags,
                                 ble_gap_slave.adv_data, &adv_data_len,
                                 BLE_HCI_MAX_ADV_DATA_LEN);
        BLE_HS_DBG_ASSERT(rc == 0);
    }

    /* Encode the transmit power AD field. */
    if (ble_gap_slave.adv_pwr_lvl) {
        rc = ble_hs_adv_set_flat(BLE_HS_ADV_TYPE_TX_PWR_LVL, 1,
                                 &ble_gap_slave.tx_pwr_lvl,
                                 ble_gap_slave.adv_data,
                                 &adv_data_len, BLE_HCI_MAX_ADV_DATA_LEN);
        BLE_HS_DBG_ASSERT(rc == 0);
    }

    rc = host_hci_cmd_build_le_set_adv_data(ble_gap_slave.adv_data,
                                            adv_data_len, buf, sizeof buf);
    if (rc != 0) {
        return rc;
    }

    rc = ble_hci_cmd_tx_empty_ack(buf);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

static int
ble_gap_adv_params_tx(struct hci_adv_params *adv_params)
{
    uint8_t buf[BLE_HCI_CMD_HDR_LEN + BLE_HCI_SET_ADV_PARAM_LEN];
    int rc;

    switch (ble_gap_slave.conn_mode) {
    case BLE_GAP_CONN_MODE_NON:
        adv_params->adv_type = BLE_HCI_ADV_TYPE_ADV_NONCONN_IND;
        break;

    case BLE_GAP_CONN_MODE_DIR:
        adv_params->adv_type = BLE_HCI_ADV_TYPE_ADV_DIRECT_IND_HD;
        break;

    case BLE_GAP_CONN_MODE_UND:
        adv_params->adv_type = BLE_HCI_ADV_TYPE_ADV_IND;
        break;

    default:
        BLE_HS_DBG_ASSERT(0);
        break;
    }

    rc = host_hci_cmd_build_le_set_adv_params(adv_params, buf, sizeof buf);
    if (rc != 0) {
        return rc;
    }

    rc = ble_hci_cmd_tx_empty_ack(buf);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

/**
 * Enables the specified discoverable mode and connectable mode, and initiates
 * the advertising process.
 *
 * @param discoverable_mode     One of the following constants:
 *                                  o BLE_GAP_DISC_MODE_NON
 *                                      (non-discoverable; 3.C.9.2.2).
 *                                  o BLE_GAP_DISC_MODE_LTD
 *                                      (limited-discoverable; 3.C.9.2.3).
 *                                  o BLE_GAP_DISC_MODE_GEN
 *                                      (general-discoverable; 3.C.9.2.4).
 * @param connectable_mode      One of the following constants:
 *                                  o BLE_GAP_CONN_MODE_NON
 *                                      (non-connectable; 3.C.9.3.2).
 *                                  o BLE_GAP_CONN_MODE_DIR
 *                                      (directed-connectable; 3.C.9.3.3).
 *                                  o BLE_GAP_CONN_MODE_UND
 *                                      (undirected-connectable; 3.C.9.3.4).
 * @param peer_addr             The address of the peer who is allowed to
 *                                  connect; only meaningful for directed
 *                                  connectable mode.  For other modes, specify
 *                                  NULL.
 * @param peer_addr_type        The type of address specified for the
 *                                  "peer_addr" parameter; only meaningful for
 *                                  directed connectable mode.  For other
 *                                  modes, specify 0.  For directed connectable
 *                                  mode, this should be one of the following
 *                                  constants:
 *                                      o BLE_ADDR_TYPE_PUBLIC
 *                                      o BLE_ADDR_TYPE_RANDOM
 *
 * @return                      0 on success; nonzero on failure.
 */
int
ble_gap_adv_start(uint8_t discoverable_mode, uint8_t connectable_mode,
                  uint8_t *peer_addr, uint8_t peer_addr_type,
                  struct hci_adv_params *adv_params,
                  ble_gap_conn_fn *cb, void *cb_arg)
{
#if !NIMBLE_OPT(ADVERTISE)
    return BLE_HS_ENOTSUP;
#endif

    struct hci_adv_params adv_params_copy;
    int rc;

    ble_hs_lock();

    STATS_INC(ble_gap_stats, adv_start);

    if (ble_gap_slave.op != BLE_GAP_OP_NULL) {
        rc = BLE_HS_EALREADY;
        goto done;
    }

    if (discoverable_mode >= BLE_GAP_DISC_MODE_MAX) {
        rc = BLE_HS_EINVAL;
        goto done;
    }

    /* Don't initiate a connection procedure if we won't be able to allocate a
     * connection object on completion.
     */
    if (connectable_mode != BLE_GAP_CONN_MODE_NON &&
        !ble_hs_conn_can_alloc()) {

        rc = BLE_HS_ENOMEM;
        goto done;
    }

    switch (connectable_mode) {
    case BLE_GAP_CONN_MODE_NON:
    case BLE_GAP_CONN_MODE_UND:
        break;

    case BLE_GAP_CONN_MODE_DIR:
        if (peer_addr_type != BLE_ADDR_TYPE_PUBLIC &&
            peer_addr_type != BLE_ADDR_TYPE_RANDOM) {

            rc = BLE_HS_EINVAL;
            goto done;
        }
        break;

    default:
        rc = BLE_HS_EINVAL;
        goto done;
    }

    if (adv_params != NULL) {
        adv_params_copy = *adv_params;
    } else {
        adv_params_copy = ble_gap_adv_params_dflt;
    }

    BLE_HS_LOG(INFO, "GAP procedure initiated: advertise; ");
    ble_gap_log_adv(&adv_params_copy);
    BLE_HS_LOG(INFO, "\n");

    if (connectable_mode == BLE_GAP_CONN_MODE_DIR) {
        adv_params_copy.peer_addr_type = peer_addr_type;
        memcpy(adv_params_copy.peer_addr, peer_addr,
               sizeof adv_params_copy.peer_addr);
    }

    ble_gap_slave.cb = cb;
    ble_gap_slave.cb_arg = cb_arg;
    ble_gap_slave.conn_mode = connectable_mode;
    ble_gap_slave.disc_mode = discoverable_mode;

    ble_gap_adv_itvls(discoverable_mode, connectable_mode,
                      &adv_params_copy.adv_itvl_min,
                      &adv_params_copy.adv_itvl_max);

    rc = ble_gap_adv_params_tx(&adv_params_copy);
    if (rc != 0) {
        goto done;
    }

    if (ble_gap_slave.adv_pwr_lvl) {
        rc = ble_hci_util_read_adv_tx_pwr(&ble_gap_slave.tx_pwr_lvl);
        if (rc != 0) {
            goto done;
        }
    }

    if (ble_gap_slave.conn_mode != BLE_GAP_CONN_MODE_DIR) {
        rc = ble_gap_adv_data_tx();
        if (rc != 0) {
            goto done;
        }

        rc = ble_gap_adv_rsp_data_tx();
        if (rc != 0) {
            goto done;
        }
    }

    rc = ble_gap_adv_enable_tx();
    if (rc != 0) {
        goto done;
    }

    ble_gap_slave.op = BLE_GAP_OP_S_ADV;

    rc = 0;

done:
    if (rc != 0) {
        STATS_INC(ble_gap_stats, adv_start_fail);
    }

    ble_hs_unlock();

    return rc;
}

int
ble_gap_adv_set_fields(struct ble_hs_adv_fields *adv_fields)
{
#if !NIMBLE_OPT(ADVERTISE)
    return BLE_HS_ENOTSUP;
#endif

    int max_sz;
    int rc;

    ble_hs_lock();

    STATS_INC(ble_gap_stats, adv_set_fields);

    if (adv_fields->tx_pwr_lvl_is_present) {
        max_sz = BLE_GAP_ADV_DATA_LIMIT_PWR;
    } else {
        max_sz = BLE_GAP_ADV_DATA_LIMIT_NO_PWR;
    }

    rc = ble_hs_adv_set_fields(adv_fields, ble_gap_slave.adv_data,
                               &ble_gap_slave.adv_data_len, max_sz);
    if (rc == 0) {
        ble_gap_slave.adv_pwr_lvl = adv_fields->tx_pwr_lvl_is_present;
    } else {
        STATS_INC(ble_gap_stats, adv_set_fields_fail);
    }

    ble_hs_unlock();

    return rc;
}

int
ble_gap_adv_rsp_set_fields(struct ble_hs_adv_fields *rsp_fields)
{
#if !NIMBLE_OPT(ADVERTISE)
    return BLE_HS_ENOTSUP;
#endif

    int rc;

    ble_hs_lock();

    STATS_INC(ble_gap_stats, adv_rsp_set_fields);

    rc = ble_hs_adv_set_fields(rsp_fields, ble_gap_slave.rsp_data,
                               &ble_gap_slave.rsp_data_len,
                               BLE_HCI_MAX_ADV_DATA_LEN);
    if (rc != 0) {
        STATS_INC(ble_gap_stats, adv_rsp_set_fields_fail);
    }

    ble_hs_unlock();

    return rc;
}

/*****************************************************************************
 * $discovery procedures                                                     *
 *****************************************************************************/

static int
ble_gap_disc_tx_disable(void)
{
    uint8_t buf[BLE_HCI_CMD_HDR_LEN + BLE_HCI_SET_SCAN_ENABLE_LEN];
    int rc;

    host_hci_cmd_build_le_set_scan_enable(0, 0, buf, sizeof buf);
    rc = ble_hci_cmd_tx_empty_ack(buf);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

static int
ble_gap_disc_tx_enable(void)
{
    uint8_t buf[BLE_HCI_CMD_HDR_LEN + BLE_HCI_SET_SCAN_ENABLE_LEN];
    int rc;

    host_hci_cmd_build_le_set_scan_enable(1, 0, buf, sizeof buf);
    rc = ble_hci_cmd_tx_empty_ack(buf);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

static int
ble_gap_disc_tx_params(uint8_t scan_type, uint8_t filter_policy)
{
    uint8_t buf[BLE_HCI_CMD_HDR_LEN + BLE_HCI_SET_SCAN_PARAM_LEN];
    int rc;

    rc = host_hci_cmd_build_le_set_scan_params(
        scan_type,
        BLE_GAP_SCAN_FAST_INTERVAL_MIN,
        BLE_GAP_SCAN_FAST_WINDOW,
        BLE_HCI_ADV_OWN_ADDR_PUBLIC,
        filter_policy,
        buf, sizeof buf);
    BLE_HS_DBG_ASSERT_EVAL(rc == 0);

    rc = ble_hci_cmd_tx_empty_ack(buf);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

/**
 * Performs the Limited or General Discovery Procedures, as described in
 * vol. 3, part C, section 9.2.5 / 9.2.6.
 *
 * @return                      0 on success; nonzero on failure.
 */
int
ble_gap_disc(uint32_t duration_ms, uint8_t discovery_mode,
             uint8_t scan_type, uint8_t filter_policy,
             ble_gap_disc_fn *cb, void *cb_arg)
{
#if !NIMBLE_OPT(ROLE_OBSERVER)
    return BLE_HS_ENOTSUP;
#endif

    int rc;

    ble_hs_lock();

    if (ble_gap_master.op != BLE_GAP_OP_NULL) {
        rc = BLE_HS_EALREADY;
        goto done;
    }

    STATS_INC(ble_gap_stats, discover);

    if (discovery_mode != BLE_GAP_DISC_MODE_LTD &&
        discovery_mode != BLE_GAP_DISC_MODE_GEN) {

        rc = BLE_HS_EINVAL;
        goto done;
    }

    if (scan_type != BLE_HCI_SCAN_TYPE_PASSIVE &&
        scan_type != BLE_HCI_SCAN_TYPE_ACTIVE) {

        rc = BLE_HS_EINVAL;
        goto done;
    }

    if (filter_policy > BLE_HCI_SCAN_FILT_MAX) {
        rc = BLE_HS_EINVAL;
        goto done;
    }

    if (duration_ms == 0) {
        duration_ms = BLE_GAP_GEN_DISC_SCAN_MIN;
    }

    ble_gap_master.disc.disc_mode = discovery_mode;
    ble_gap_master.disc.cb = cb;
    ble_gap_master.disc.cb_arg = cb_arg;

    BLE_HS_LOG(INFO, "GAP procedure initiated: discovery; ");
    ble_gap_log_disc(scan_type, filter_policy);
    BLE_HS_LOG(INFO, "\n");

    rc = ble_gap_disc_tx_params(scan_type, filter_policy);
    if (rc != 0) {
        goto done;
    }

    rc = ble_gap_disc_tx_enable();
    if (rc != 0) {
        goto done;
    }

    ble_gap_master_set_timer(duration_ms);
    ble_gap_master.op = BLE_GAP_OP_M_DISC;

    rc = 0;

done:
    if (rc != 0) {
        STATS_INC(ble_gap_stats, discover_fail);
    }

    ble_hs_unlock();

    return rc;
}

/*****************************************************************************
 * $connection establishment procedures                                      *
 *****************************************************************************/

static int
ble_gap_conn_create_tx(int addr_type, uint8_t *addr,
                       struct ble_gap_crt_params *params)
{
    uint8_t buf[BLE_HCI_CMD_HDR_LEN + BLE_HCI_CREATE_CONN_LEN];
    struct hci_create_conn hcc;
    int rc;

    hcc.scan_itvl = params->scan_itvl;
    hcc.scan_window = params->scan_window;

    if (addr_type == BLE_GAP_ADDR_TYPE_WL) {
        hcc.filter_policy = BLE_HCI_CONN_FILT_USE_WL;
        hcc.peer_addr_type = BLE_HCI_ADV_PEER_ADDR_PUBLIC;
        memset(hcc.peer_addr, 0, sizeof hcc.peer_addr);
    } else {
        hcc.filter_policy = BLE_HCI_CONN_FILT_NO_WL;
        hcc.peer_addr_type = addr_type;
        memcpy(hcc.peer_addr, addr, sizeof hcc.peer_addr);
    }
    hcc.own_addr_type = BLE_HCI_ADV_OWN_ADDR_PUBLIC;
    hcc.conn_itvl_min = params->itvl_min;
    hcc.conn_itvl_max = params->itvl_max;
    hcc.conn_latency = params->latency;
    hcc.supervision_timeout = params->supervision_timeout;
    hcc.min_ce_len = params->min_ce_len;
    hcc.max_ce_len = params->max_ce_len;

    rc = host_hci_cmd_build_le_create_connection(&hcc, buf, sizeof buf);
    if (rc != 0) {
        return BLE_HS_EUNKNOWN;
    }

    rc = ble_hci_cmd_tx_empty_ack(buf);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

/**
 * Performs the Direct Connection Establishment Procedure, as described in
 * vol. 3, part C, section 9.3.8.
 *
 * @param addr_type             The peer's address type; one of:
 *                                  o BLE_HCI_CONN_PEER_ADDR_PUBLIC
 *                                  o BLE_HCI_CONN_PEER_ADDR_RANDOM
 *                                  o BLE_HCI_CONN_PEER_ADDR_PUBLIC_IDENT
 *                                  o BLE_HCI_CONN_PEER_ADDR_RANDOM_IDENT
 *                                  o BLE_GAP_ADDR_TYPE_WL
 * @param addr                  The address of the peer to connect to.
 *
 * @return                      0 on success; nonzero on failure.
 */
int
ble_gap_conn_initiate(int addr_type, uint8_t *addr,
                      struct ble_gap_crt_params *params,
                      ble_gap_conn_fn *cb, void *cb_arg)
{
#if !NIMBLE_OPT(ROLE_CENTRAL)
    return BLE_HS_ENOTSUP;
#endif

    int rc;

    ble_hs_lock();

    if (ble_gap_master.op != BLE_GAP_OP_NULL) {
        rc = BLE_HS_EALREADY;
        goto done;
    }

    STATS_INC(ble_gap_stats, initiate);

    if (addr_type != BLE_HCI_CONN_PEER_ADDR_PUBLIC &&
        addr_type != BLE_HCI_CONN_PEER_ADDR_RANDOM &&
        addr_type != BLE_GAP_ADDR_TYPE_WL) {

        rc = BLE_HS_EINVAL;
        goto done;
    }

    if (params == NULL) {
        params = (void *)&ble_gap_params_dflt;
    }

    /* XXX: Verify params. */

    BLE_HS_LOG(INFO, "GAP procedure initiated: connect; ");
    ble_gap_log_conn(addr_type, addr, params);
    BLE_HS_LOG(INFO, "\n");

    ble_gap_master.conn.cb = cb;
    ble_gap_master.conn.cb_arg = cb_arg;
    ble_gap_master.conn.using_wl = addr_type == BLE_GAP_ADDR_TYPE_WL;

    rc = ble_gap_conn_create_tx(addr_type, addr, params);
    if (rc != 0) {
        goto done;
    }

    ble_gap_master.op = BLE_GAP_OP_M_CONN;

    rc = 0;

done:
    if (rc != 0) {
        STATS_INC(ble_gap_stats, initiate_fail);
    }

    ble_hs_unlock();

    return rc;
}

/*****************************************************************************
 * $terminate connection procedure                                           *
 *****************************************************************************/

int
ble_gap_terminate(uint16_t conn_handle)
{
    uint8_t buf[BLE_HCI_CMD_HDR_LEN + BLE_HCI_DISCONNECT_CMD_LEN];
    int rc;

    ble_hs_lock();

    STATS_INC(ble_gap_stats, terminate);

    if (!ble_hs_conn_exists(conn_handle)) {
        rc = BLE_HS_ENOTCONN;
        goto done;
    }

    BLE_HS_LOG(INFO, "GAP procedure initiated: terminate connection; "
                     "conn_handle=%d\n", conn_handle);

    host_hci_cmd_build_disconnect(conn_handle, BLE_ERR_REM_USER_CONN_TERM,
                                  buf, sizeof buf);
    rc = ble_hci_cmd_tx_empty_ack(buf);
    if (rc != 0) {
        goto done;
    }

    rc = 0;

done:
    if (rc != 0) {
        STATS_INC(ble_gap_stats, terminate_fail);
    }

    ble_hs_unlock();

    return rc;
}

/*****************************************************************************
 * $cancel                                                                   *
 *****************************************************************************/

int
ble_gap_cancel(void)
{
    uint8_t buf[BLE_HCI_CMD_HDR_LEN];
    int rc;

    ble_hs_lock();

    STATS_INC(ble_gap_stats, cancel);

    if (!ble_gap_master_in_progress()) {
        rc = BLE_HS_ENOENT;
        goto done;
    }

    BLE_HS_LOG(INFO, "GAP procedure initiated: cancel connection\n");

    host_hci_cmd_build_le_create_conn_cancel(buf, sizeof buf);
    rc = ble_hci_cmd_tx_empty_ack(buf);
    if (rc != 0) {
        goto done;
    }

    rc = 0;

done:
    if (rc != 0) {
        STATS_INC(ble_gap_stats, cancel_fail);
    }

    ble_hs_unlock();

    return rc;
}

/*****************************************************************************
 * $update connection parameters                                             *
 *****************************************************************************/

static int
ble_gap_tx_param_pos_reply(uint16_t conn_handle,
                           struct ble_gap_upd_params *params)
{
    uint8_t buf[BLE_HCI_CMD_HDR_LEN + BLE_HCI_CONN_PARAM_REPLY_LEN];
    struct hci_conn_param_reply pos_reply;
    int rc;

    pos_reply.handle = conn_handle;
    pos_reply.conn_itvl_min = params->itvl_min;
    pos_reply.conn_itvl_max = params->itvl_max;
    pos_reply.conn_latency = params->latency;
    pos_reply.supervision_timeout = params->supervision_timeout;
    pos_reply.min_ce_len = params->min_ce_len;
    pos_reply.max_ce_len = params->max_ce_len;

    host_hci_cmd_build_le_conn_param_reply(&pos_reply, buf, sizeof buf);
    rc = ble_hci_cmd_tx_empty_ack(buf);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

static int
ble_gap_tx_param_neg_reply(uint16_t conn_handle, uint8_t reject_reason)
{
    uint8_t buf[BLE_HCI_CMD_HDR_LEN + BLE_HCI_CONN_PARAM_NEG_REPLY_LEN];
    struct hci_conn_param_neg_reply neg_reply;
    int rc;

    neg_reply.handle = conn_handle;
    neg_reply.reason = reject_reason;

    host_hci_cmd_build_le_conn_param_neg_reply(&neg_reply, buf, sizeof buf);
    rc = ble_hci_cmd_tx_empty_ack(buf);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

void
ble_gap_rx_param_req(struct hci_le_conn_param_req *evt)
{
#if !NIMBLE_OPT(CONNECT)
    return;
#endif

    struct ble_gap_upd_params peer_params;
    struct ble_gap_upd_params self_params;
    struct ble_gap_conn_ctxt ctxt;
    struct ble_gap_snapshot snap;
    uint8_t reject_reason;
    int rc;

    reject_reason = 0; /* Silence warning. */

    rc = ble_gap_find_snapshot(evt->connection_handle, &snap);
    if (rc != 0) {
        /* We are not connected to the sender. */
        return;
    }

    peer_params.itvl_min = evt->itvl_min;
    peer_params.itvl_max = evt->itvl_max;
    peer_params.latency = evt->latency;
    peer_params.supervision_timeout = evt->timeout;
    peer_params.min_ce_len = 0;
    peer_params.max_ce_len = 0;

    /* Copy the peer params into the self params to make it easy on the
     * application.  The application callback will change only the fields which
     * it finds unsuitable.
     */
    self_params = peer_params;

    memset(&ctxt, 0, sizeof ctxt);
    ctxt.desc = &snap.desc;
    ctxt.update.self_params = &self_params;
    ctxt.update.peer_params = &peer_params;
    rc = ble_gap_call_conn_cb(BLE_GAP_EVENT_CONN_UPDATE_REQ, 0, &ctxt,
                              snap.cb, snap.cb_arg);
    if (rc != 0) {
        reject_reason = rc;
    }

    if (rc == 0) {
        rc = ble_gap_tx_param_pos_reply(evt->connection_handle, &self_params);
        if (rc != 0) {
            ble_gap_update_failed(evt->connection_handle, rc);
        } else {
            ble_hs_atomic_conn_set_flags(evt->connection_handle,
                                         BLE_HS_CONN_F_UPDATE, 1);
        }
    } else {
        ble_gap_tx_param_neg_reply(evt->connection_handle, reject_reason);
    }
}

static int
ble_gap_update_tx(uint16_t conn_handle, struct ble_gap_upd_params *params)
{
    uint8_t buf[BLE_HCI_CMD_HDR_LEN + BLE_HCI_CONN_UPDATE_LEN];
    struct hci_conn_update cmd;
    int rc;

    cmd.handle = conn_handle;
    cmd.conn_itvl_min = params->itvl_min;
    cmd.conn_itvl_max = params->itvl_max;
    cmd.conn_latency = params->latency;
    cmd.supervision_timeout = params->supervision_timeout;
    cmd.min_ce_len = params->min_ce_len;
    cmd.max_ce_len = params->max_ce_len;

    rc = host_hci_cmd_build_le_conn_update(&cmd, buf, sizeof buf);
    if (rc != 0) {
        return rc;
    }

    rc = ble_hci_cmd_tx_empty_ack(buf);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

int
ble_gap_update_params(uint16_t conn_handle, struct ble_gap_upd_params *params)
{
#if !NIMBLE_OPT(CONNECT)
    return BLE_HS_ENOTSUP;
#endif

    struct ble_hs_conn *conn;
    int rc;

    ble_hs_lock();

    STATS_INC(ble_gap_stats, update);

    conn = ble_hs_conn_find(conn_handle);
    if (conn == NULL) {
        rc = BLE_HS_ENOTCONN;
        goto done;
    }

    if (conn->bhc_flags & BLE_HS_CONN_F_UPDATE) {
        rc = BLE_HS_EALREADY;
        goto done;
    }

    BLE_HS_LOG(INFO, "GAP procedure initiated: ");
    ble_gap_log_update(conn_handle, params);
    BLE_HS_LOG(INFO, "\n");

    rc = ble_gap_update_tx(conn_handle, params);
    if (rc != 0) {
        goto done;
    }

    conn->bhc_flags |= BLE_HS_CONN_F_UPDATE;

done:
    if (rc != 0) {
        STATS_INC(ble_gap_stats, update_fail);
    }

    ble_hs_unlock();

    return rc;
}

/*****************************************************************************
 * $security                                                                 *
 *****************************************************************************/

#if NIMBLE_OPT(SM)
int
ble_gap_security_initiate(uint16_t conn_handle)
{
    ble_hs_conn_flags_t conn_flags;
    int rc;

    rc = ble_hs_atomic_conn_flags(conn_handle, &conn_flags);
    if (rc != 0) {
        return rc;
    }

    if (conn_flags & BLE_HS_CONN_F_MASTER) {
        /* XXX: Search the security database for an LTK for this peer.  If one
         * is found, perform the encryption procedure rather than the pairing
         * procedure.
         */
        rc = ble_l2cap_sm_pair_initiate(conn_handle);
    } else {
        rc = ble_l2cap_sm_slave_initiate(conn_handle);
    }

    return rc;
}

int
ble_gap_encryption_initiate(uint16_t conn_handle,
                            uint8_t *ltk,
                            uint16_t ediv,
                            uint64_t rand_val,
                            int auth)
{
    ble_hs_conn_flags_t conn_flags;
    int rc;

    rc = ble_hs_atomic_conn_flags(conn_handle, &conn_flags);
    if (rc != 0) {
        return rc;
    }

    if (!(conn_flags & BLE_HS_CONN_F_MASTER)) {
        return BLE_HS_EROLE;
    }

    rc = ble_l2cap_sm_enc_initiate(conn_handle, ltk, ediv, rand_val, auth);
    return rc;
}
#endif

void
ble_gap_passkey_event(uint16_t conn_handle, int status,
                      uint8_t passkey_action)
{
    struct ble_gap_conn_ctxt ctxt;
    struct ble_gap_snapshot snap;
    struct ble_hs_conn *conn;
    struct ble_gap_passkey_action act;

    ble_hs_lock();

    conn = ble_hs_conn_find(conn_handle);
    if (conn != NULL) {
        ble_gap_conn_to_snapshot(conn, &snap);
    }

    ble_hs_unlock();

    if (conn == NULL) {
        /* No longer connected. */
        return;
    }

    BLE_HS_LOG(DEBUG, "send passkey action request %d\n", passkey_action);

    memset(&ctxt, 0, sizeof ctxt);
    act.action = passkey_action;
    ctxt.desc = &snap.desc;
    ctxt.passkey_action = &act;
    ble_gap_call_conn_cb(BLE_GAP_EVENT_PASSKEY_ACTION, status, &ctxt,
                         snap.cb, snap.cb_arg);
}

void
ble_gap_key_exchange_event(uint16_t conn_handle,
                           struct ble_gap_key_parms *key_params)
{
    struct ble_gap_conn_ctxt ctxt;
    struct ble_gap_snapshot snap;
    int rc;

    rc = ble_gap_find_snapshot(conn_handle, &snap);
    if (rc != 0) {
        /* No longer connected. */
        return;
    }

    memset(&ctxt, 0, sizeof ctxt);
    ctxt.desc = &snap.desc;
    ctxt.key_params = key_params;
    ble_gap_call_conn_cb(BLE_GAP_EVENT_KEY_EXCHANGE, 0, &ctxt,
                         snap.cb, snap.cb_arg);
}

void
ble_gap_security_event(uint16_t conn_handle, int status,
                       struct ble_gap_sec_state *sec_state)
{
    struct ble_gap_conn_ctxt ctxt;
    struct ble_gap_snapshot snap;
    struct ble_hs_conn *conn;

    ble_hs_lock();

    conn = ble_hs_conn_find(conn_handle);
    if (conn != NULL) {
        conn->bhc_sec_state = *sec_state;
        ble_gap_conn_to_snapshot(conn, &snap);
    }

    ble_hs_unlock();

    if (conn == NULL) {
        /* No longer connected. */
        return;
    }

    memset(&ctxt, 0, sizeof ctxt);
    ctxt.desc = &snap.desc;
    ble_gap_call_conn_cb(BLE_GAP_EVENT_SECURITY, status, &ctxt,
                         snap.cb, snap.cb_arg);
}

int
ble_gap_ltk_event(uint16_t conn_handle, struct ble_gap_ltk_params *ltk_params)
{
    struct ble_gap_conn_ctxt ctxt;
    struct ble_gap_snapshot snap;
    int rc;

    rc = ble_gap_find_snapshot(conn_handle, &snap);
    if (rc != 0) {
        /* No longer connected. */
        return BLE_HS_ENOTCONN;
    }

    memset(&ctxt, 0, sizeof ctxt);
    ctxt.desc = &snap.desc;
    ctxt.ltk_params = ltk_params;
    rc = ble_gap_call_conn_cb(BLE_GAP_EVENT_LTK_REQUEST, 0, &ctxt,
                              snap.cb, snap.cb_arg);
    if (rc != 0) {
        /* No long-term key that matches the specified ediv and rand. */
        return BLE_HS_EREJECT;
    }

    return 0;
}

/*****************************************************************************
 * $init                                                                     *
 *****************************************************************************/

int
ble_gap_init(void)
{
    int rc;

    memset(&ble_gap_master, 0, sizeof ble_gap_master);
    memset(&ble_gap_slave, 0, sizeof ble_gap_slave);

    rc = stats_init_and_reg(
        STATS_HDR(ble_gap_stats), STATS_SIZE_INIT_PARMS(ble_gap_stats,
        STATS_SIZE_32), STATS_NAME_INIT_PARMS(ble_gap_stats), "ble_gap");
    if (rc != 0) {
        return BLE_HS_EOS;
    }

    return 0;
}
