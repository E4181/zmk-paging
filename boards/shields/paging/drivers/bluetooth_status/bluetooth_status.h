/*
 * Copyright (c) 2025 Your Name
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 公开的API函数（如果需要） */
int bluetooth_status_get_connection_state(void);
void bluetooth_status_update(void);

#ifdef __cplusplus
}
#endif