#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "os/os.h"
#include "bsp/bsp.h"

/* BLE */
#include "nimble/hci_common.h"
#include "nimble/hci_transport.h"
#include "controller/ble_ll.h"

#define H4_CMD                   1
#define H4_ACL                   2
#define H4_EVENT                 4

#define BLE_IMG_LL_TASK_PRIO              1
#define BLE_IMG_LL_STACK_SIZE             (OS_STACK_ALIGN(288))


struct os_task ble_img_ll_task;
bssnz_t os_stack_t ble_img_ll_stack[BLE_IMG_LL_STACK_SIZE];
extern int ble_hs_rx_data(struct os_mbuf *om);

#define FILE_NAME "/dev/ttyHS0"

int gFd;

static struct log_handler ble_ll_log_console_handler;
struct log ble_ll_log;

/**
 * BLE_ll test task
 *
 * @param arg
 */
static void
ble_img_ll_task_handler(void *arg)
{

    int noBytes = 0;
    int fd;
    uint8_t *evbuf;
    struct os_mbuf *om;
    uint8_t opCode[256] = {0};
    int flags = 0;
    int rc;

    fd = open(FILE_NAME, O_RDONLY);

    while (1) {

       flags = fcntl(fd, F_GETFL, 0);
       fcntl(fd, F_SETFL, flags & (~O_NONBLOCK));
       noBytes = read(fd, &opCode, sizeof(opCode));

       BLELL_LOG(DEBUG,"No. of Bytes read from Device %d \n",noBytes);
       BLELL_LOG(DEBUG,"RECVD EVENT VALUE %x\n", opCode[0]);

       if(opCode[0] == H4_ACL)
       {
          BLELL_LOG(DEBUG,"acl Data RECEIVED\n");
          om = os_msys_get_pkthdr(0, 0);
          if(om != NULL)
          {
             rc = os_mbuf_copyinto(om, 0, opCode + 1, noBytes - 1);
             if(!rc)
                ble_hs_rx_data(om);
             else
                BLELL_LOG(DEBUG,"ACL DATA copy to MBUF FAILED \n");
          }
          else
          {
             BLELL_LOG(DEBUG,"ACL BUFFER ALLOCATION FAILED \n");
             assert(om == 0);
          }
       }
       else // EVENT Received
       {
          evbuf = os_memblock_get(&g_hci_cmd_pool);
          if(evbuf)
          {
             memcpy(evbuf, opCode + 1, noBytes - 1);
             ble_hci_transport_ctlr_event_send(evbuf);
          }
          else
             assert(evbuf == 0);
       }
    }
    close(fd);
}

int ble_ll_init(uint8_t ll_task_prio, uint8_t num_acl_pkts, uint16_t acl_pkt_size)
{
   log_init();
   log_console_handler_init(&ble_ll_log_console_handler);
   log_register("ble_ll", &ble_ll_log, &ble_ll_log_console_handler);

   os_task_init(&ble_img_ll_task, "ble_img_ll", ble_img_ll_task_handler,
                 NULL, 2, OS_WAIT_FOREVER,
                 ble_img_ll_stack, BLE_IMG_LL_STACK_SIZE);
   return 0;
}

