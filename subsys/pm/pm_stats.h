/*
 * Copyright (c) 2018 Intel Corporation.
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_SUBSYS_PM_PM_STATS_H_
#define ZEPHYR_SUBSYS_PM_PM_STATS_H_

#include <pm/state.h>

#ifdef CONFIG_PM_STATS
void pm_start_timer(void);
void pm_stop_timer(void);
void pm_stats_update(enum pm_state state);
#else
static inline void pm_start_timer(void) {}
static inline void pm_stop_timer(void) {}
static inline void pm_stats_update(enum pm_state state) {}
#endif /* CONFIG_PM_STATS */

#endif /* ZEPHYR_SUBSYS_PM_PM_STATS_H_ */