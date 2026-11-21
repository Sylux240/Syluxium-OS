// SPDX-License-Identifier: MIT
/*
 * Copyright 2021 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: AMD
 *
 */

#include "dc_trace.h"

#include <linux/fpu.h>

/**
 * DOC: DC FPU manipulation overview
 *
 * DC core uses FPU operations in multiple parts of the code, which requires a
 * more specialized way to manage these areas' entrance. To fulfill this
 * requirement, we created some wrapper functions that encapsulate
 * kernel_fpu_begin/end to better fit our need in the display component. In
 * summary, in this file, you can find functions related to FPU operation
 * management.
 */

/*
 * Syluxium: per-CPU variables are plain globals in LinuxKPI, so the
 * recursion depth lives with the kernel FPU section itself, which nests per
 * CPU (arch fpu::kernel_begin/end).
 */
bool lkpi_kernel_fpu_active(void);

inline void dc_assert_fp_enabled(void)
{
	ASSERT(lkpi_kernel_fpu_active());
}

void dc_fpu_begin(const char *function_name, const int line)
{
	WARN_ON_ONCE(!in_task());
	kernel_fpu_begin();
	TRACE_DCN_FPU(true, function_name, line, 1);
}

void dc_fpu_end(const char *function_name, const int line)
{
	kernel_fpu_end();
	TRACE_DCN_FPU(false, function_name, line, 0);
}
