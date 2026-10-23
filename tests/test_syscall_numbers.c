/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The syscall-number check is tools/check-syscall-numbers.py. tools/host-tests.sh
 * runs that script's --self-test and does not compile this file. Compiling it
 * alone is not the check. */
#include <stdio.h>

int main(void)
{
    fputs("run: python3 tools/check-syscall-numbers.py --self-test\n", stderr);
    return 2;
}
