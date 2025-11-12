// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * iocl_utils.h:
 *   utilities for IOCL metadata handling
 *
 * Copyright 2025 Anja Kalaba  <akalaba@princeton.edu>
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 **********************************************************************/

#ifndef _COMMON_IOCL_UTILS_H_
#define _COMMON_IOCL_UTILS_H_

#include "lib/message.h"

uint64_t TagToPid(uint64_t tag)
{
    uint64_t pid = (tag >> 32) & 0xFFFFFFFF;
    return pid;
}

uint64_t TagToSeqno(uint64_t tag)
{
    uint64_t seqno = (tag & 0xFFFFFFFF);
    return seqno;
}

uint64_t CreateTag(uint64_t pid, uint64_t seqno)
{
    return (pid << 32) | (seqno & 0xFFFFFFFF);
}

#endif /* _COMMON_IOCL_UTILS_H_ */
