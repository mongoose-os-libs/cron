/*
 * Copyright (c) 2014-2016 Cesanta Software Limited
 * All rights reserved
 */

#ifndef CS_MOS_LIBS_CRON_SRC_TEST_MGOS_HAL_H_
#define CS_MOS_LIBS_CRON_SRC_TEST_MGOS_HAL_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

void mgos_lock(void);
void mgos_unlock(void);

typedef void (*mgos_cb_t)(void *arg);
static inline bool mgos_invoke_cb(mgos_cb_t cb, void *arg, bool from_isr) {
  return false;
}

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* CS_MOS_LIBS_CRON_SRC_TEST_MGOS_HAL_H_ */
