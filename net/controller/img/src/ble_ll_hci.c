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
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include "os/os.h"
#include "nimble/ble.h"
#include "nimble/nimble_opt.h"
#include "nimble/hci_common.h"
#include "nimble/hci_transport.h"
/* XXX: For now, put this here */
#define FILE_NAME "/home/krishnab/bt_nimble_p4/ensigma/sw/connectivity/whisper/DEV/BASE/bt/support/HCI_TRANSPORT/source/bash/topDevice"

/* Need to move these to a header file */
#define H4_CMD                   1
#define H4_ACL                   2
#define H4_EVENT                 4

int
ble_hci_transport_host_cmd_send(uint8_t *cmd)
{
    int rc; 
    uint16_t opcode;

    int fd;
    int noBytes, sizeBytes;
    uint8_t tcmd[256];

    opcode = le16toh(cmd);
    printf("opcode:%02x\n", opcode);
    printf("ocf:%02x\n", BLE_HCI_OCF(opcode));
    printf("ogf%02x\n", BLE_HCI_OGF(opcode));
    
    sizeBytes = 4 + cmd[2];
    tcmd[0] = H4_CMD;
    memcpy(tcmd + 1, cmd, sizeBytes);
 
    printf("DATA Received FROM HOST : \n");
    {
       int i;
       printf("COMMAND DATA Received from HOST for Tx: \n");
       for(i=0; i < sizeBytes; i++)
          printf("%x ", cmd[i]);
       printf("\n\n");
    }

    fd = open(FILE_NAME, O_WRONLY, S_IWUSR | S_IRUSR);
    noBytes = write(fd,(void *)tcmd, sizeBytes);
    printf("NO OF BYTES written to Controller %d\n", noBytes);
    close(fd);

    rc = BLE_ERR_SUCCESS;
    return rc;
}

/* Send ACL data from host to contoller */
int
ble_hci_transport_host_acl_data_send(struct os_mbuf *om)
{

    int fd;
    int noBytes, sizeBytes;
    uint8_t tcmd[256];

    // Total Data Length + 4 Bytes Header + 1 Byte for HCI Data Type
    sizeBytes = *(om->om_data + 2) + 5;
    printf("TX ACL DATA LENGTH %x LENGTH %x \n", sizeBytes, *(om->om_data + 2));

    tcmd[0] = H4_ACL;
    memcpy(tcmd + 1, om->om_data, sizeBytes - 1);

    {
       int i;
       printf("ACL DATA Received from HOST for Tx: \n");
       for(i=0; i < sizeBytes; i++)
          printf("%x ", om->om_data[i]);
       printf("\n\n");
    }
    fd = open(FILE_NAME, O_WRONLY, S_IWUSR | S_IRUSR);
    noBytes = write(fd,(void *)tcmd, sizeBytes);
    printf("Tx :  NO OF BYTES written %d\n", noBytes);
    close(fd);

    return 0;
}

