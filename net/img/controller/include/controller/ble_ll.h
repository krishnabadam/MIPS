#ifndef H_BLELL_
#define H_BLELL_

#include "log/log.h"
extern struct log ble_ll_log;
#define BLELL_LOG_MODULE (LOG_MODULE_PERUSER + 9)
#define BLELL_LOG(lvl, ...) LOG_ ## lvl(&ble_ll_log, BLELL_LOG_MODULE, __VA_ARGS__)
extern int ble_ll_init(uint8_t ll_task_prio, uint8_t num_acl_pkts, uint16_t acl_pkt_size);

#endif
