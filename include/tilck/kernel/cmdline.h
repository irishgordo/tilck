/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once
#include <tilck/common/basic_defs.h>

#define MAX_CMD_ARGS 16

extern const char *cmd_args[MAX_CMD_ARGS];
extern void (*self_test_to_run)(void);

void parse_kernel_cmdline(const char *cmdline);
