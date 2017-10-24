/*
 * Copyright (c) 2014-2017 Cesanta Software Limited
 * All rights reserved
 */

/*
 * View this file on GitHub:
 * [mgos_cron.h](https://github.com/mongoose-os-libs/dht/blob/master/src/mgos_cron.h)
 */

#ifndef CS_MOS_LIBS_CRON_SRC_MGOS_CRON_H_
#define CS_MOS_LIBS_CRON_SRC_MGOS_CRON_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define MGOS_INVALID_CRON_ID (0)

/* Cron ID */
typedef uintptr_t mgos_cron_id_t;

/* Cron callback */
typedef void (*mgos_cron_callback_t)(void *user_data, mgos_cron_id_t id);

/*
 * Adds cron entry with `expr`null-terminated string (should be no longer
 * that 256 bytes) and `cb` as a callback.
 * `user_data` is a parameter to pass to `cb`.
 * Returns cron ID.
 */
mgos_cron_id_t mgos_cron_add(const char *expr, mgos_cron_callback_t cb,
                             void *user_data);

/*
 * Returns whether the given string is a valid cron expression or not. In case
 * of an error, if `perr` is not NULL, `*perr` is set to an error message; it
 * should NOT be freed by the caller.
 */
bool mgos_cron_is_expr_valid(const char *expr, const char **perr);

/*
 * Removes cron entry with a given cron ID.
 */
void mgos_cron_remove(mgos_cron_id_t id);

bool mgos_cron_init(void);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* CS_MOS_LIBS_CRON_SRC_MGOS_CRON_H_ */
