/*
 * Coroutine internals
 *
 * Copyright (c) 2011 Kevin Wolf <kwolf@redhat.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef QEMU_COROUTINE_INT_H
#define QEMU_COROUTINE_INT_H

#include "qemu/queue.h"
#include "qemu/coroutine.h"

#define COROUTINE_STACK_SIZE (1 << 20)

typedef enum {
    COROUTINE_YIELD = 1,
    COROUTINE_TERMINATE = 2,
    COROUTINE_ENTER = 3,
} CoroutineAction;

struct Coroutine {
	//任务函数指针
    CoroutineEntry *entry;
	//任务参数
    void *entry_arg;
	//协程调用者，当协程让出时，主动跳转到调用者发起调用时保存的上下文
    Coroutine *caller;
	//协程通过这个成员将自己加入到pending等待队列中
    QSLIST_ENTRY(Coroutine) pool_next;
    size_t locks_held;

    /*
     * Coroutines that should be woken up when we yield or terminate 
     *
     * 当前协程正在运行时，其余协程也想要执行，通过co_queue_next将自身加入到co_queue_wakeup等待队列中
     * 当前协程执行完或者主动让出cpu之后，如果co_queue_wakeup等待队列中有成员，就将其取出加到pending队列中，继续运行协程 
	 */
    QSIMPLEQ_HEAD(, Coroutine) co_queue_wakeup;
    QSIMPLEQ_ENTRY(Coroutine) co_queue_next;
};

Coroutine *qemu_coroutine_new(void);
void qemu_coroutine_delete(Coroutine *co);
CoroutineAction qemu_coroutine_switch(Coroutine *from, Coroutine *to,
                                      CoroutineAction action);
void coroutine_fn qemu_co_queue_run_restart(Coroutine *co);

#endif
