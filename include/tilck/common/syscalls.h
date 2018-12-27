/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once

#define TILCK_TESTCMD_SYSCALL    499

enum tilck_testcmd_type {
   TILCK_TESTCMD_RUN_SELFTEST = 0,
   TILCK_TESTCMD_DUMP_COVERAGE = 1,
   TILCK_TESTCMD_GCOV_GET_NUM_FILES = 2,
   TILCK_TESTCMD_GCOV_FILE_INFO = 3
};
