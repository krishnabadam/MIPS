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

#include <string.h>
#include "ble_hs_priv.h"

#define BLE_IBEACON_MFG_DATA_SIZE       25

int
ble_ibeacon_set_adv_data(void *uuid128, uint16_t major, uint16_t minor)
{
    struct ble_hs_adv_fields fields;
    uint8_t buf[BLE_IBEACON_MFG_DATA_SIZE];
    int8_t tx_pwr;
    int rc;

    /** Company identifier (Apple). */
    buf[0] = 0x4c;
    buf[1] = 0x00;

    /** iBeacon indicator. */
    buf[2] = 0x02;
    buf[3] = 0x15;

    /** UUID. */
    memcpy(buf + 4, uuid128, 16);

    /** Version number. */
    htobe16(buf + 20, major);
    htobe16(buf + 22, minor);

    /** Last byte (tx power level) filled in after HCI exchange. */

    rc = ble_hci_util_read_adv_tx_pwr(&tx_pwr);
    if (rc != 0) {
        return rc;
    }
    buf[24] = tx_pwr;

    memset(&fields, 0, sizeof fields);
    fields.mfg_data = buf;
    fields.mfg_data_len = sizeof buf;

    rc = ble_gap_adv_set_fields(&fields);
    return rc;
}
