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

#ifndef H_BLE_GAP_
#define H_BLE_GAP_

#include <inttypes.h>
#include "host/ble_hs.h"
struct hci_le_conn_complete;
struct hci_conn_update;
struct hci_adv_params;

/** 30 ms. */
#define BLE_GAP_ADV_FAST_INTERVAL1_MIN      (30 * 1000 / BLE_HCI_ADV_ITVL)

/** 60 ms. */
#define BLE_GAP_ADV_FAST_INTERVAL1_MAX      (60 * 1000 / BLE_HCI_ADV_ITVL)

/** 100 ms. */
#define BLE_GAP_ADV_FAST_INTERVAL2_MIN      (100 * 1000 / BLE_HCI_ADV_ITVL)

/** 150 ms. */
#define BLE_GAP_ADV_FAST_INTERVAL2_MAX      (150 * 1000 / BLE_HCI_ADV_ITVL)

/** 30 ms; active scanning. */
#define BLE_GAP_SCAN_FAST_INTERVAL_MIN      (30 * 1000 / BLE_HCI_ADV_ITVL)

/** 60 ms; active scanning. */
#define BLE_GAP_SCAN_FAST_INTERVAL_MAX      (60 * 1000 / BLE_HCI_ADV_ITVL)

/** 30 ms; active scanning. */
#define BLE_GAP_SCAN_FAST_WINDOW            (30 * 1000 / BLE_HCI_SCAN_ITVL)

/* 30.72 seconds; active scanning. */
#define BLE_GAP_SCAN_FAST_PERIOD            (30.72 * 1000)

/** 1.28 seconds; background scanning. */
#define BLE_GAP_SCAN_SLOW_INTERVAL1         (1280 * 1000 / BLE_HCI_SCAN_ITVL)

/** 11.25 ms; background scanning. */
#define BLE_GAP_SCAN_SLOW_WINDOW1           (11.25 * 1000 / BLE_HCI_SCAN_ITVL)

/** 10.24 seconds. */
#define BLE_GAP_GEN_DISC_SCAN_MIN           (10.24 * 1000)

/** 1 second. */
#define BLE_GAP_CONN_PAUSE_CENTRAL          (1 * 1000)

/** 5 seconds. */
#define BLE_GAP_CONN_PAUSE_PERIPHERAL       (5 * 1000)

/* 30 ms. */
#define BLE_GAP_INITIAL_CONN_ITVL_MIN       (30 * 1000 / BLE_HCI_CONN_ITVL)

/* 50 ms. */
#define BLE_GAP_INITIAL_CONN_ITVL_MAX       (50 * 1000 / BLE_HCI_CONN_ITVL)

#define BLE_GAP_INITIAL_CONN_LATENCY        0
#define BLE_GAP_INITIAL_SUPERVISION_TIMEOUT 0x0100
#define BLE_GAP_INITIAL_CONN_MIN_CE_LEN     0x0010
#define BLE_GAP_INITIAL_CONN_MAX_CE_LEN     0x0300

#define BLE_GAP_SVC_UUID16                              0x1800
#define BLE_GAP_CHR_UUID16_DEVICE_NAME                  0x2a00
#define BLE_GAP_CHR_UUID16_APPEARANCE                   0x2a01
#define BLE_GAP_CHR_UUID16_PERIPH_PRIV_FLAG             0x2a02
#define BLE_GAP_CHR_UUID16_RECONNECT_ADDR               0x2a03
#define BLE_GAP_CHR_UUID16_PERIPH_PREF_CONN_PARAMS      0x2a04

#define BLE_GAP_APPEARANCE_GEN_COMPUTER                 128

#define BLE_GAP_ADDR_TYPE_WL                0xff

#define BLE_GAP_EVENT_CONN                  0
#define BLE_GAP_EVENT_CONN_UPDATED          1
#define BLE_GAP_EVENT_CONN_UPDATE_REQ       2
#define BLE_GAP_EVENT_L2CAP_UPDATE_REQ      3
#define BLE_GAP_EVENT_CANCEL                4
#define BLE_GAP_EVENT_TERM_FAILURE          5
#define BLE_GAP_EVENT_DISC_SUCCESS          6
#define BLE_GAP_EVENT_DISC_FINISHED         7
#define BLE_GAP_EVENT_ADV_FINISHED          8
#define BLE_GAP_EVENT_SECURITY              9
#define BLE_GAP_EVENT_PASSKEY_ACTION        10
#define BLE_GAP_EVENT_LTK_REQUEST           11
#define BLE_GAP_EVENT_KEY_EXCHANGE          12

struct ble_gap_sec_state {
    uint8_t pair_alg;
    unsigned enc_enabled:1;
    unsigned authenticated:1;
    unsigned bonded:1;
};

struct ble_gap_conn_desc {
    struct ble_gap_sec_state sec_state;
    uint8_t peer_addr[6];
    uint16_t conn_handle;
    uint16_t conn_itvl;
    uint16_t conn_latency;
    uint16_t supervision_timeout;
    uint8_t peer_addr_type;
};

struct ble_gap_crt_params {
    uint16_t scan_itvl;
    uint16_t scan_window;
    uint16_t itvl_min;
    uint16_t itvl_max;
    uint16_t latency;
    uint16_t supervision_timeout;
    uint16_t min_ce_len;
    uint16_t max_ce_len;
};

struct ble_gap_upd_params {
    uint16_t itvl_min;
    uint16_t itvl_max;
    uint16_t latency;
    uint16_t supervision_timeout;
    uint16_t min_ce_len;
    uint16_t max_ce_len;
};

struct ble_gap_key_parms {
    unsigned is_ours:1;
    unsigned ltk_valid:1;
    unsigned ediv_rand_valid:1;
    unsigned irk_valid:1;
    unsigned csrk_valid:1;
    unsigned addr_valid:1;
    uint16_t ediv;
    uint64_t rand_val;
    uint8_t addr_type;
    uint8_t ltk[16];
    uint8_t irk[16];
    uint8_t csrk[16];
    uint8_t addr[6];
};

struct ble_gap_ltk_params {
    /* host --> app. */
    uint16_t ediv;
    uint64_t rand_num;

    /* app --> host. */
    uint8_t ltk[16];
    unsigned authenticated:1;
};

/*
 * when the l2cap calls to the application, it will ask the application
 * to perform one of they key generation roles. In all cases, the
 * application must pass the passkey back to the l2cap via
 * ble_l2cap_sm_set_tk
 */
#define BLE_GAP_PKACT_NONE          0
#define BLE_GAP_PKACT_OOB           1
#define BLE_GAP_PKACT_INPUT         2
#define BLE_GAP_PKACT_DISP          3

struct ble_gap_passkey_action {
    uint8_t action;
};

struct ble_gap_conn_ctxt {
    struct ble_gap_conn_desc *desc;

    union {
        struct {
            struct ble_gap_upd_params *self_params;
            struct ble_gap_upd_params *peer_params;
        } update;

        struct ble_gap_passkey_action *passkey_action;
        struct ble_gap_ltk_params *ltk_params;
        struct ble_gap_key_parms *key_params;
    };
};

typedef int ble_gap_conn_fn(int event, int status,
                            struct ble_gap_conn_ctxt *ctxt, void *arg);

struct ble_gap_disc_desc {
    uint8_t event_type;
    uint8_t addr_type;
    uint8_t length_data;
    int8_t rssi;
    uint8_t addr[6];
    uint8_t *data;
    struct ble_hs_adv_fields *fields;
};

typedef void ble_gap_disc_fn(int event, int status,
                             struct ble_gap_disc_desc *desc, void *arg);

typedef void ble_gap_wl_fn(int status, void *arg);

#define BLE_GAP_CONN_MODE_NON               0
#define BLE_GAP_CONN_MODE_DIR               1
#define BLE_GAP_CONN_MODE_UND               2

#define BLE_GAP_DISC_MODE_NON               0
#define BLE_GAP_DISC_MODE_LTD               1
#define BLE_GAP_DISC_MODE_GEN               2

struct ble_gap_white_entry {
    uint8_t addr_type;
    uint8_t addr[6];
};

int ble_gap_adv_start(uint8_t discoverable_mode, uint8_t connectable_mode,
                      uint8_t *peer_addr, uint8_t peer_addr_type,
                      struct hci_adv_params *adv_params,
                      ble_gap_conn_fn *cb, void *cb_arg);
int ble_gap_adv_stop(void);
int ble_gap_adv_set_fields(struct ble_hs_adv_fields *adv_fields);
int ble_gap_adv_rsp_set_fields(struct ble_hs_adv_fields *rsp_fields);
int ble_gap_disc(uint32_t duration_ms, uint8_t discovery_mode,
                      uint8_t scan_type, uint8_t filter_policy,
                      ble_gap_disc_fn *cb, void *cb_arg);
int ble_gap_conn_initiate(int addr_type, uint8_t *addr,
                          struct ble_gap_crt_params *params,
                          ble_gap_conn_fn *cb, void *cb_arg);
int ble_gap_terminate(uint16_t handle);
int ble_gap_cancel(void);
int ble_gap_wl_set(struct ble_gap_white_entry *white_list,
                   uint8_t white_list_count);
int ble_gap_update_params(uint16_t conn_handle,
                          struct ble_gap_upd_params *params);
int ble_gap_security_initiate(uint16_t conn_handle);
int
ble_gap_encryption_initiate(uint16_t conn_handle, uint8_t *ltk,
                            uint16_t ediv, uint64_t rand_val, int auth);
#endif
