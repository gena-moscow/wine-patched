/*
 * Thread pooling
 *
 * Copyright (c) 2006 Robert Shearman
 * Copyright (c) 2014-2015 Sebastian Lackner
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"
#include "wine/port.h"

#include <assert.h>
#include <stdarg.h>
#include <limits.h>

#define NONAMELESSUNION
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "winternl.h"

#include "wine/debug.h"
#include "wine/list.h"

#include "ntdll_misc.h"

WINE_DEFAULT_DEBUG_CHANNEL(threadpool);

/*
 * Old thread pooling API
 */

#define OLD_WORKER_TIMEOUT 30000        /* 30 seconds */
#define EXPIRE_NEVER       (~(ULONGLONG)0)
#define TIMER_QUEUE_MAGIC  0x516d6954   /* TimQ */

static RTL_CRITICAL_SECTION_DEBUG critsect_debug;
static RTL_CRITICAL_SECTION_DEBUG critsect_compl_debug;

static struct
{
    /* threadpool_cs must be held while modifying the following four elements */
    struct list             work_item_list;
    LONG                    num_workers;
    LONG                    num_busy_workers;
    LONG                    num_items_processed;
    RTL_CONDITION_VARIABLE  threadpool_cond;
    RTL_CRITICAL_SECTION    threadpool_cs;
    HANDLE                  compl_port;
    RTL_CRITICAL_SECTION    threadpool_compl_cs;
}
old_threadpool =
{
    LIST_INIT(old_threadpool.work_item_list),   /* work_item_list */
    0,                                          /* num_workers */
    0,                                          /* num_busy_workers */
    0,                                          /* num_items_processed */
    RTL_CONDITION_VARIABLE_INIT,                /* threadpool_cond */
    { &critsect_debug, -1, 0, 0, 0, 0 },        /* threadpool_cs */
    NULL,                                       /* compl_port */
    { &critsect_compl_debug, -1, 0, 0, 0, 0 },  /* threadpool_compl_cs */
};

static RTL_CRITICAL_SECTION_DEBUG critsect_debug =
{
    0, 0, &old_threadpool.threadpool_cs,
    { &critsect_debug.ProcessLocksList, &critsect_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": threadpool_cs") }
};

static RTL_CRITICAL_SECTION_DEBUG critsect_compl_debug =
{
    0, 0, &old_threadpool.threadpool_compl_cs,
    { &critsect_compl_debug.ProcessLocksList, &critsect_compl_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": threadpool_compl_cs") }
};

struct work_item
{
    struct list entry;
    PRTL_WORK_ITEM_ROUTINE function;
    PVOID context;
};

struct wait_work_item
{
    HANDLE Object;
    HANDLE CancelEvent;
    WAITORTIMERCALLBACK Callback;
    PVOID Context;
    ULONG Milliseconds;
    ULONG Flags;
    HANDLE CompletionEvent;
    LONG DeleteCount;
    BOOLEAN CallbackInProgress;
};

struct timer_queue;
struct queue_timer
{
    struct timer_queue *q;
    struct list entry;
    ULONG runcount;             /* number of callbacks pending execution */
    RTL_WAITORTIMERCALLBACKFUNC callback;
    PVOID param;
    DWORD period;
    ULONG flags;
    ULONGLONG expire;
    BOOL destroy;               /* timer should be deleted; once set, never unset */
    HANDLE event;               /* removal event */
};

struct timer_queue
{
    DWORD magic;
    RTL_CRITICAL_SECTION cs;
    struct list timers;         /* sorted by expiration time */
    BOOL quit;                  /* queue should be deleted; once set, never unset */
    HANDLE event;
    HANDLE thread;
};

/*
 * New object-oriented thread pooling API
 */

#define THREADPOOL_WORKER_TIMEOUT 5000

/* internal threadpool representation */
struct threadpool
{
    LONG                    refcount;
    BOOL                    shutdown;
    CRITICAL_SECTION        cs;
    /* pool of work items, locked via .cs */
    struct list             pool;
    RTL_CONDITION_VARIABLE  update_event;
    /* information about worker threads, locked via .cs */
    int                     max_workers;
    int                     min_workers;
    int                     num_workers;
    int                     num_busy_workers;
};

enum threadpool_objtype
{
    TP_OBJECT_TYPE_SIMPLE,
    TP_OBJECT_TYPE_WORK
};

/* internal threadpool object representation */
struct threadpool_object
{
    LONG                    refcount;
    BOOL                    shutdown;
    /* read-only information */
    enum threadpool_objtype type;
    struct threadpool       *pool;
    struct threadpool_group *group;
    PVOID                   userdata;
    PTP_CLEANUP_GROUP_CANCEL_CALLBACK group_cancel_callback;
    PTP_SIMPLE_CALLBACK     finalization_callback;
    BOOL                    may_run_long;
    HMODULE                 race_dll;
    /* information about the group, locked via .group->cs */
    struct list             group_entry;
    BOOL                    is_group_member;
    /* information about the pool, locked via .pool->cs */
    struct list             pool_entry;
    RTL_CONDITION_VARIABLE  finished_event;
    LONG                    num_pending_callbacks;
    LONG                    num_running_callbacks;
    /* arguments for callback */
    union
    {
        struct
        {
            PTP_SIMPLE_CALLBACK callback;
        } simple;
        struct
        {
            PTP_WORK_CALLBACK callback;
        } work;
    } u;
};

/* internal threadpool instance representation */
struct threadpool_instance
{
    struct threadpool_object *object;
    DWORD                   threadid;
    BOOL                    may_run_long;
};

/* internal threadpool group representation */
struct threadpool_group
{
    LONG                    refcount;
    BOOL                    shutdown;
    CRITICAL_SECTION        cs;
    /* list of group members, locked via .cs */
    struct list             members;
};

static inline struct threadpool *impl_from_TP_POOL( TP_POOL *pool )
{
    return (struct threadpool *)pool;
}

static inline struct threadpool_object *impl_from_TP_WORK( TP_WORK *work )
{
    struct threadpool_object *object = (struct threadpool_object *)work;
    assert( !object || object->type == TP_OBJECT_TYPE_WORK );
    return object;
}

static inline struct threadpool_group *impl_from_TP_CLEANUP_GROUP( TP_CLEANUP_GROUP *group )
{
    return (struct threadpool_group *)group;
}

static inline struct threadpool_instance *impl_from_TP_CALLBACK_INSTANCE( TP_CALLBACK_INSTANCE *instance )
{
    return (struct threadpool_instance *)instance;
}

static void CALLBACK threadpool_worker_proc( void *param );
static void tp_object_submit( struct threadpool_object *object );
static void tp_object_shutdown( struct threadpool_object *object );
static BOOL tp_object_release( struct threadpool_object *object );
static struct threadpool *default_threadpool = NULL;

static inline LONG interlocked_inc( PLONG dest )
{
    return interlocked_xchg_add( dest, 1 ) + 1;
}

static inline LONG interlocked_dec( PLONG dest )
{
    return interlocked_xchg_add( dest, -1 ) - 1;
}

static void WINAPI worker_thread_proc(void * param)
{
    struct list *item;
    struct work_item *work_item_ptr, work_item;
    LARGE_INTEGER timeout;
    timeout.QuadPart = -(OLD_WORKER_TIMEOUT * (ULONGLONG)10000);

    RtlEnterCriticalSection( &old_threadpool.threadpool_cs );
    old_threadpool.num_workers++;

    for (;;)
    {
        if ((item = list_head( &old_threadpool.work_item_list )))
        {
            work_item_ptr = LIST_ENTRY( item, struct work_item, entry );
            list_remove( &work_item_ptr->entry );
            old_threadpool.num_busy_workers++;
            old_threadpool.num_items_processed++;
            RtlLeaveCriticalSection( &old_threadpool.threadpool_cs );

            /* copy item to stack and do the work */
            work_item = *work_item_ptr;
            RtlFreeHeap( GetProcessHeap(), 0, work_item_ptr );
            TRACE("executing %p(%p)\n", work_item.function, work_item.context);
            work_item.function( work_item.context );

            RtlEnterCriticalSection( &old_threadpool.threadpool_cs );
            old_threadpool.num_busy_workers--;
        }
        else if (RtlSleepConditionVariableCS( &old_threadpool.threadpool_cond,
                 &old_threadpool.threadpool_cs, &timeout ) != STATUS_SUCCESS)
        {
            break;
        }
    }

    old_threadpool.num_workers--;
    RtlLeaveCriticalSection( &old_threadpool.threadpool_cs );
    RtlExitUserThread( 0 );

    /* never reached */
}

/***********************************************************************
 *              RtlQueueWorkItem   (NTDLL.@)
 *
 * Queues a work item into a thread in the thread pool.
 *
 * PARAMS
 *  Function [I] Work function to execute.
 *  Context  [I] Context to pass to the work function when it is executed.
 *  Flags    [I] Flags. See notes.
 *
 * RETURNS
 *  Success: STATUS_SUCCESS.
 *  Failure: Any NTSTATUS code.
 *
 * NOTES
 *  Flags can be one or more of the following:
 *|WT_EXECUTEDEFAULT - Executes the work item in a non-I/O worker thread.
 *|WT_EXECUTEINIOTHREAD - Executes the work item in an I/O worker thread.
 *|WT_EXECUTEINPERSISTENTTHREAD - Executes the work item in a thread that is persistent.
 *|WT_EXECUTELONGFUNCTION - Hints that the execution can take a long time.
 *|WT_TRANSFER_IMPERSONATION - Executes the function with the current access token.
 */
NTSTATUS WINAPI RtlQueueWorkItem(PRTL_WORK_ITEM_ROUTINE Function, PVOID Context, ULONG Flags)
{
    HANDLE thread;
    NTSTATUS status;
    LONG items_processed;
    struct work_item *work_item = RtlAllocateHeap(GetProcessHeap(), 0, sizeof(struct work_item));

    if (!work_item)
        return STATUS_NO_MEMORY;

    work_item->function = Function;
    work_item->context = Context;

    if (Flags & ~WT_EXECUTELONGFUNCTION)
        FIXME("Flags 0x%x not supported\n", Flags);

    RtlEnterCriticalSection( &old_threadpool.threadpool_cs );
    list_add_tail( &old_threadpool.work_item_list, &work_item->entry );
    status = (old_threadpool.num_workers > old_threadpool.num_busy_workers) ?
             STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
    items_processed = old_threadpool.num_items_processed;
    RtlLeaveCriticalSection( &old_threadpool.threadpool_cs );

    /* FIXME: tune this algorithm to not be as aggressive with creating threads
     * if WT_EXECUTELONGFUNCTION isn't specified */
    if (status == STATUS_SUCCESS)
        RtlWakeConditionVariable( &old_threadpool.threadpool_cond );
    else
    {
        status = RtlCreateUserThread( GetCurrentProcess(), NULL, FALSE, NULL, 0, 0,
                                      worker_thread_proc, NULL, &thread, NULL );

        /* NOTE: we don't care if we couldn't create the thread if there is at
         * least one other available to process the request */
        if (status == STATUS_SUCCESS)
            NtClose( thread );
        else
        {
            RtlEnterCriticalSection( &old_threadpool.threadpool_cs );
            if (old_threadpool.num_workers > 0 ||
                old_threadpool.num_items_processed != items_processed)
            {
                status = STATUS_SUCCESS;
            }
            else
                list_remove( &work_item->entry );
            RtlLeaveCriticalSection( &old_threadpool.threadpool_cs );

            if (status != STATUS_SUCCESS)
                RtlFreeHeap( GetProcessHeap(), 0, work_item );
        }
    }

    return status;
}

/***********************************************************************
 * iocp_poller - get completion events and run callbacks
 */
static DWORD CALLBACK iocp_poller(LPVOID Arg)
{
    HANDLE cport = Arg;

    while( TRUE )
    {
        PRTL_OVERLAPPED_COMPLETION_ROUTINE callback;
        LPVOID overlapped;
        IO_STATUS_BLOCK iosb;
        NTSTATUS res = NtRemoveIoCompletion( cport, (PULONG_PTR)&callback, (PULONG_PTR)&overlapped, &iosb, NULL );
        if (res)
        {
            ERR("NtRemoveIoCompletion failed: 0x%x\n", res);
        }
        else
        {
            DWORD transferred = 0;
            DWORD err = 0;

            if (iosb.u.Status == STATUS_SUCCESS)
                transferred = iosb.Information;
            else
                err = RtlNtStatusToDosError(iosb.u.Status);

            callback( err, transferred, overlapped );
        }
    }
    return 0;
}

/***********************************************************************
 *              RtlSetIoCompletionCallback  (NTDLL.@)
 *
 * Binds a handle to a thread pool's completion port, and possibly
 * starts a non-I/O thread to monitor this port and call functions back.
 *
 * PARAMS
 *  FileHandle [I] Handle to bind to a completion port.
 *  Function   [I] Callback function to call on I/O completions.
 *  Flags      [I] Not used.
 *
 * RETURNS
 *  Success: STATUS_SUCCESS.
 *  Failure: Any NTSTATUS code.
 *
 */
NTSTATUS WINAPI RtlSetIoCompletionCallback(HANDLE FileHandle, PRTL_OVERLAPPED_COMPLETION_ROUTINE Function, ULONG Flags)
{
    IO_STATUS_BLOCK iosb;
    FILE_COMPLETION_INFORMATION info;

    if (Flags) FIXME("Unknown value Flags=0x%x\n", Flags);

    if (!old_threadpool.compl_port)
    {
        NTSTATUS res = STATUS_SUCCESS;

        RtlEnterCriticalSection(&old_threadpool.threadpool_compl_cs);
        if (!old_threadpool.compl_port)
        {
            HANDLE cport;

            res = NtCreateIoCompletion( &cport, IO_COMPLETION_ALL_ACCESS, NULL, 0 );
            if (!res)
            {
                /* FIXME native can start additional threads in case of e.g. hung callback function. */
                res = RtlQueueWorkItem( iocp_poller, cport, WT_EXECUTEDEFAULT );
                if (!res)
                    old_threadpool.compl_port = cport;
                else
                    NtClose( cport );
            }
        }
        RtlLeaveCriticalSection(&old_threadpool.threadpool_compl_cs);
        if (res) return res;
    }

    info.CompletionPort = old_threadpool.compl_port;
    info.CompletionKey = (ULONG_PTR)Function;

    return NtSetInformationFile( FileHandle, &iosb, &info, sizeof(info), FileCompletionInformation );
}

static inline PLARGE_INTEGER get_nt_timeout( PLARGE_INTEGER pTime, ULONG timeout )
{
    if (timeout == INFINITE) return NULL;
    pTime->QuadPart = (ULONGLONG)timeout * -10000;
    return pTime;
}

static void delete_wait_work_item(struct wait_work_item *wait_work_item)
{
    NtClose( wait_work_item->CancelEvent );
    RtlFreeHeap( GetProcessHeap(), 0, wait_work_item );
}

static DWORD CALLBACK wait_thread_proc(LPVOID Arg)
{
    struct wait_work_item *wait_work_item = Arg;
    NTSTATUS status;
    BOOLEAN alertable = (wait_work_item->Flags & WT_EXECUTEINIOTHREAD) != 0;
    HANDLE handles[2] = { wait_work_item->Object, wait_work_item->CancelEvent };
    LARGE_INTEGER timeout;
    HANDLE completion_event;

    TRACE("\n");

    while (TRUE)
    {
        status = NtWaitForMultipleObjects( 2, handles, TRUE, alertable,
                                           get_nt_timeout( &timeout, wait_work_item->Milliseconds ) );
        if (status == STATUS_WAIT_0 || status == STATUS_TIMEOUT)
        {
            BOOLEAN TimerOrWaitFired;

            if (status == STATUS_WAIT_0)
            {
                TRACE( "object %p signaled, calling callback %p with context %p\n",
                    wait_work_item->Object, wait_work_item->Callback,
                    wait_work_item->Context );
                TimerOrWaitFired = FALSE;
            }
            else
            {
                TRACE( "wait for object %p timed out, calling callback %p with context %p\n",
                    wait_work_item->Object, wait_work_item->Callback,
                    wait_work_item->Context );
                TimerOrWaitFired = TRUE;
            }
            wait_work_item->CallbackInProgress = TRUE;
            wait_work_item->Callback( wait_work_item->Context, TimerOrWaitFired );
            wait_work_item->CallbackInProgress = FALSE;

            if (wait_work_item->Flags & WT_EXECUTEONLYONCE)
                break;
        }
        else
            break;
    }

    completion_event = wait_work_item->CompletionEvent;
    if (completion_event) NtSetEvent( completion_event, NULL );

    if (interlocked_inc( &wait_work_item->DeleteCount ) == 2 )
        delete_wait_work_item( wait_work_item );

    return 0;
}

/***********************************************************************
 *              RtlRegisterWait   (NTDLL.@)
 *
 * Registers a wait for a handle to become signaled.
 *
 * PARAMS
 *  NewWaitObject [I] Handle to the new wait object. Use RtlDeregisterWait() to free it.
 *  Object   [I] Object to wait to become signaled.
 *  Callback [I] Callback function to execute when the wait times out or the handle is signaled.
 *  Context  [I] Context to pass to the callback function when it is executed.
 *  Milliseconds [I] Number of milliseconds to wait before timing out.
 *  Flags    [I] Flags. See notes.
 *
 * RETURNS
 *  Success: STATUS_SUCCESS.
 *  Failure: Any NTSTATUS code.
 *
 * NOTES
 *  Flags can be one or more of the following:
 *|WT_EXECUTEDEFAULT - Executes the work item in a non-I/O worker thread.
 *|WT_EXECUTEINIOTHREAD - Executes the work item in an I/O worker thread.
 *|WT_EXECUTEINPERSISTENTTHREAD - Executes the work item in a thread that is persistent.
 *|WT_EXECUTELONGFUNCTION - Hints that the execution can take a long time.
 *|WT_TRANSFER_IMPERSONATION - Executes the function with the current access token.
 */
NTSTATUS WINAPI RtlRegisterWait(PHANDLE NewWaitObject, HANDLE Object,
                                RTL_WAITORTIMERCALLBACKFUNC Callback,
                                PVOID Context, ULONG Milliseconds, ULONG Flags)
{
    struct wait_work_item *wait_work_item;
    NTSTATUS status;

    TRACE( "(%p, %p, %p, %p, %d, 0x%x)\n", NewWaitObject, Object, Callback, Context, Milliseconds, Flags );

    wait_work_item = RtlAllocateHeap( GetProcessHeap(), 0, sizeof(*wait_work_item) );
    if (!wait_work_item)
        return STATUS_NO_MEMORY;

    wait_work_item->Object = Object;
    wait_work_item->Callback = Callback;
    wait_work_item->Context = Context;
    wait_work_item->Milliseconds = Milliseconds;
    wait_work_item->Flags = Flags;
    wait_work_item->CallbackInProgress = FALSE;
    wait_work_item->DeleteCount = 0;
    wait_work_item->CompletionEvent = NULL;

    status = NtCreateEvent( &wait_work_item->CancelEvent, EVENT_ALL_ACCESS, NULL, NotificationEvent, FALSE );
    if (status != STATUS_SUCCESS)
    {
        RtlFreeHeap( GetProcessHeap(), 0, wait_work_item );
        return status;
    }

    Flags = Flags & (WT_EXECUTEINIOTHREAD | WT_EXECUTEINPERSISTENTTHREAD |
                     WT_EXECUTELONGFUNCTION | WT_TRANSFER_IMPERSONATION);
    status = RtlQueueWorkItem( wait_thread_proc, wait_work_item, Flags );
    if (status != STATUS_SUCCESS)
    {
        delete_wait_work_item( wait_work_item );
        return status;
    }

    *NewWaitObject = wait_work_item;
    return status;
}

/***********************************************************************
 *              RtlDeregisterWaitEx   (NTDLL.@)
 *
 * Cancels a wait operation and frees the resources associated with calling
 * RtlRegisterWait().
 *
 * PARAMS
 *  WaitObject [I] Handle to the wait object to free.
 *
 * RETURNS
 *  Success: STATUS_SUCCESS.
 *  Failure: Any NTSTATUS code.
 */
NTSTATUS WINAPI RtlDeregisterWaitEx(HANDLE WaitHandle, HANDLE CompletionEvent)
{
    struct wait_work_item *wait_work_item = WaitHandle;
    NTSTATUS status = STATUS_SUCCESS;

    TRACE( "(%p)\n", WaitHandle );

    NtSetEvent( wait_work_item->CancelEvent, NULL );
    if (wait_work_item->CallbackInProgress)
    {
        if (CompletionEvent != NULL)
        {
            if (CompletionEvent == INVALID_HANDLE_VALUE)
            {
                status = NtCreateEvent( &CompletionEvent, EVENT_ALL_ACCESS, NULL, NotificationEvent, FALSE );
                if (status != STATUS_SUCCESS)
                    return status;
                interlocked_xchg_ptr( &wait_work_item->CompletionEvent, CompletionEvent );
                if (wait_work_item->CallbackInProgress)
                    NtWaitForSingleObject( CompletionEvent, FALSE, NULL );
                NtClose( CompletionEvent );
            }
            else
            {
                interlocked_xchg_ptr( &wait_work_item->CompletionEvent, CompletionEvent );
                if (wait_work_item->CallbackInProgress)
                    status = STATUS_PENDING;
            }
        }
        else
            status = STATUS_PENDING;
    }

    if (interlocked_inc( &wait_work_item->DeleteCount ) == 2 )
    {
        status = STATUS_SUCCESS;
        delete_wait_work_item( wait_work_item );
    }

    return status;
}

/***********************************************************************
 *              RtlDeregisterWait   (NTDLL.@)
 *
 * Cancels a wait operation and frees the resources associated with calling
 * RtlRegisterWait().
 *
 * PARAMS
 *  WaitObject [I] Handle to the wait object to free.
 *
 * RETURNS
 *  Success: STATUS_SUCCESS.
 *  Failure: Any NTSTATUS code.
 */
NTSTATUS WINAPI RtlDeregisterWait(HANDLE WaitHandle)
{
    return RtlDeregisterWaitEx(WaitHandle, NULL);
}


/************************** Timer Queue Impl **************************/

static void queue_remove_timer(struct queue_timer *t)
{
    /* We MUST hold the queue cs while calling this function.  This ensures
       that we cannot queue another callback for this timer.  The runcount
       being zero makes sure we don't have any already queued.  */
    struct timer_queue *q = t->q;

    assert(t->runcount == 0);
    assert(t->destroy);

    list_remove(&t->entry);
    if (t->event)
        NtSetEvent(t->event, NULL);
    RtlFreeHeap(GetProcessHeap(), 0, t);

    if (q->quit && list_empty(&q->timers))
        NtSetEvent(q->event, NULL);
}

static void timer_cleanup_callback(struct queue_timer *t)
{
    struct timer_queue *q = t->q;
    RtlEnterCriticalSection(&q->cs);

    assert(0 < t->runcount);
    --t->runcount;

    if (t->destroy && t->runcount == 0)
        queue_remove_timer(t);

    RtlLeaveCriticalSection(&q->cs);
}

static DWORD WINAPI timer_callback_wrapper(LPVOID p)
{
    struct queue_timer *t = p;
    t->callback(t->param, TRUE);
    timer_cleanup_callback(t);
    return 0;
}

static inline ULONGLONG queue_current_time(void)
{
    LARGE_INTEGER now, freq;
    NtQueryPerformanceCounter(&now, &freq);
    return now.QuadPart * 1000 / freq.QuadPart;
}

static void queue_add_timer(struct queue_timer *t, ULONGLONG time,
                            BOOL set_event)
{
    /* We MUST hold the queue cs while calling this function.  */
    struct timer_queue *q = t->q;
    struct list *ptr = &q->timers;

    assert(!q->quit || (t->destroy && time == EXPIRE_NEVER));

    if (time != EXPIRE_NEVER)
        LIST_FOR_EACH(ptr, &q->timers)
        {
            struct queue_timer *cur = LIST_ENTRY(ptr, struct queue_timer, entry);
            if (time < cur->expire)
                break;
        }
    list_add_before(ptr, &t->entry);

    t->expire = time;

    /* If we insert at the head of the list, we need to expire sooner
       than expected.  */
    if (set_event && &t->entry == list_head(&q->timers))
        NtSetEvent(q->event, NULL);
}

static inline void queue_move_timer(struct queue_timer *t, ULONGLONG time,
                                    BOOL set_event)
{
    /* We MUST hold the queue cs while calling this function.  */
    list_remove(&t->entry);
    queue_add_timer(t, time, set_event);
}

static void queue_timer_expire(struct timer_queue *q)
{
    struct queue_timer *t = NULL;

    RtlEnterCriticalSection(&q->cs);
    if (list_head(&q->timers))
    {
        ULONGLONG now, next;
        t = LIST_ENTRY(list_head(&q->timers), struct queue_timer, entry);
        if (!t->destroy && t->expire <= ((now = queue_current_time())))
        {
            ++t->runcount;
            if (t->period)
            {
                next = t->expire + t->period;
                /* avoid trigger cascade if overloaded / hibernated */
                if (next < now)
                    next = now + t->period;
            }
            else
                next = EXPIRE_NEVER;
            queue_move_timer(t, next, FALSE);
        }
        else
            t = NULL;
    }
    RtlLeaveCriticalSection(&q->cs);

    if (t)
    {
        if (t->flags & WT_EXECUTEINTIMERTHREAD)
            timer_callback_wrapper(t);
        else
        {
            ULONG flags
                = (t->flags
                   & (WT_EXECUTEINIOTHREAD | WT_EXECUTEINPERSISTENTTHREAD
                      | WT_EXECUTELONGFUNCTION | WT_TRANSFER_IMPERSONATION));
            NTSTATUS status = RtlQueueWorkItem(timer_callback_wrapper, t, flags);
            if (status != STATUS_SUCCESS)
                timer_cleanup_callback(t);
        }
    }
}

static ULONG queue_get_timeout(struct timer_queue *q)
{
    struct queue_timer *t;
    ULONG timeout = INFINITE;

    RtlEnterCriticalSection(&q->cs);
    if (list_head(&q->timers))
    {
        t = LIST_ENTRY(list_head(&q->timers), struct queue_timer, entry);
        assert(!t->destroy || t->expire == EXPIRE_NEVER);

        if (t->expire != EXPIRE_NEVER)
        {
            ULONGLONG time = queue_current_time();
            timeout = t->expire < time ? 0 : t->expire - time;
        }
    }
    RtlLeaveCriticalSection(&q->cs);

    return timeout;
}

static void WINAPI timer_queue_thread_proc(LPVOID p)
{
    struct timer_queue *q = p;
    ULONG timeout_ms;

    timeout_ms = INFINITE;
    for (;;)
    {
        LARGE_INTEGER timeout;
        NTSTATUS status;
        BOOL done = FALSE;

        status = NtWaitForSingleObject(
            q->event, FALSE, get_nt_timeout(&timeout, timeout_ms));

        if (status == STATUS_WAIT_0)
        {
            /* There are two possible ways to trigger the event.  Either
               we are quitting and the last timer got removed, or a new
               timer got put at the head of the list so we need to adjust
               our timeout.  */
            RtlEnterCriticalSection(&q->cs);
            if (q->quit && list_empty(&q->timers))
                done = TRUE;
            RtlLeaveCriticalSection(&q->cs);
        }
        else if (status == STATUS_TIMEOUT)
            queue_timer_expire(q);

        if (done)
            break;

        timeout_ms = queue_get_timeout(q);
    }

    NtClose(q->event);
    RtlDeleteCriticalSection(&q->cs);
    q->magic = 0;
    RtlFreeHeap(GetProcessHeap(), 0, q);
}

static void queue_destroy_timer(struct queue_timer *t)
{
    /* We MUST hold the queue cs while calling this function.  */
    t->destroy = TRUE;
    if (t->runcount == 0)
        /* Ensure a timer is promptly removed.  If callbacks are pending,
           it will be removed after the last one finishes by the callback
           cleanup wrapper.  */
        queue_remove_timer(t);
    else
        /* Make sure no destroyed timer masks an active timer at the head
           of the sorted list.  */
        queue_move_timer(t, EXPIRE_NEVER, FALSE);
}

/***********************************************************************
 *              RtlCreateTimerQueue   (NTDLL.@)
 *
 * Creates a timer queue object and returns a handle to it.
 *
 * PARAMS
 *  NewTimerQueue [O] The newly created queue.
 *
 * RETURNS
 *  Success: STATUS_SUCCESS.
 *  Failure: Any NTSTATUS code.
 */
NTSTATUS WINAPI RtlCreateTimerQueue(PHANDLE NewTimerQueue)
{
    NTSTATUS status;
    struct timer_queue *q = RtlAllocateHeap(GetProcessHeap(), 0, sizeof *q);
    if (!q)
        return STATUS_NO_MEMORY;

    RtlInitializeCriticalSection(&q->cs);
    list_init(&q->timers);
    q->quit = FALSE;
    q->magic = TIMER_QUEUE_MAGIC;
    status = NtCreateEvent(&q->event, EVENT_ALL_ACCESS, NULL, SynchronizationEvent, FALSE);
    if (status != STATUS_SUCCESS)
    {
        RtlFreeHeap(GetProcessHeap(), 0, q);
        return status;
    }
    status = RtlCreateUserThread(GetCurrentProcess(), NULL, FALSE, NULL, 0, 0,
                                 timer_queue_thread_proc, q, &q->thread, NULL);
    if (status != STATUS_SUCCESS)
    {
        NtClose(q->event);
        RtlFreeHeap(GetProcessHeap(), 0, q);
        return status;
    }

    *NewTimerQueue = q;
    return STATUS_SUCCESS;
}

/***********************************************************************
 *              RtlDeleteTimerQueueEx   (NTDLL.@)
 *
 * Deletes a timer queue object.
 *
 * PARAMS
 *  TimerQueue      [I] The timer queue to destroy.
 *  CompletionEvent [I] If NULL, return immediately.  If INVALID_HANDLE_VALUE,
 *                      wait until all timers are finished firing before
 *                      returning.  Otherwise, return immediately and set the
 *                      event when all timers are done.
 *
 * RETURNS
 *  Success: STATUS_SUCCESS if synchronous, STATUS_PENDING if not.
 *  Failure: Any NTSTATUS code.
 */
NTSTATUS WINAPI RtlDeleteTimerQueueEx(HANDLE TimerQueue, HANDLE CompletionEvent)
{
    struct timer_queue *q = TimerQueue;
    struct queue_timer *t, *temp;
    HANDLE thread;
    NTSTATUS status;

    if (!q || q->magic != TIMER_QUEUE_MAGIC)
        return STATUS_INVALID_HANDLE;

    thread = q->thread;

    RtlEnterCriticalSection(&q->cs);
    q->quit = TRUE;
    if (list_head(&q->timers))
        /* When the last timer is removed, it will signal the timer thread to
           exit...  */
        LIST_FOR_EACH_ENTRY_SAFE(t, temp, &q->timers, struct queue_timer, entry)
            queue_destroy_timer(t);
    else
        /* However if we have none, we must do it ourselves.  */
        NtSetEvent(q->event, NULL);
    RtlLeaveCriticalSection(&q->cs);

    if (CompletionEvent == INVALID_HANDLE_VALUE)
    {
        NtWaitForSingleObject(thread, FALSE, NULL);
        status = STATUS_SUCCESS;
    }
    else
    {
        if (CompletionEvent)
        {
            FIXME("asynchronous return on completion event unimplemented\n");
            NtWaitForSingleObject(thread, FALSE, NULL);
            NtSetEvent(CompletionEvent, NULL);
        }
        status = STATUS_PENDING;
    }

    NtClose(thread);
    return status;
}

static struct timer_queue *get_timer_queue(HANDLE TimerQueue)
{
    static struct timer_queue *default_timer_queue;

    if (TimerQueue)
        return TimerQueue;
    else
    {
        if (!default_timer_queue)
        {
            HANDLE q;
            NTSTATUS status = RtlCreateTimerQueue(&q);
            if (status == STATUS_SUCCESS)
            {
                PVOID p = interlocked_cmpxchg_ptr(
                    (void **) &default_timer_queue, q, NULL);
                if (p)
                    /* Got beat to the punch.  */
                    RtlDeleteTimerQueueEx(q, NULL);
            }
        }
        return default_timer_queue;
    }
}

/***********************************************************************
 *              RtlCreateTimer   (NTDLL.@)
 *
 * Creates a new timer associated with the given queue.
 *
 * PARAMS
 *  NewTimer   [O] The newly created timer.
 *  TimerQueue [I] The queue to hold the timer.
 *  Callback   [I] The callback to fire.
 *  Parameter  [I] The argument for the callback.
 *  DueTime    [I] The delay, in milliseconds, before first firing the
 *                 timer.
 *  Period     [I] The period, in milliseconds, at which to fire the timer
 *                 after the first callback.  If zero, the timer will only
 *                 fire once.  It still needs to be deleted with
 *                 RtlDeleteTimer.
 * Flags       [I] Flags controlling the execution of the callback.  In
 *                 addition to the WT_* thread pool flags (see
 *                 RtlQueueWorkItem), WT_EXECUTEINTIMERTHREAD and
 *                 WT_EXECUTEONLYONCE are supported.
 *
 * RETURNS
 *  Success: STATUS_SUCCESS.
 *  Failure: Any NTSTATUS code.
 */
NTSTATUS WINAPI RtlCreateTimer(PHANDLE NewTimer, HANDLE TimerQueue,
                               RTL_WAITORTIMERCALLBACKFUNC Callback,
                               PVOID Parameter, DWORD DueTime, DWORD Period,
                               ULONG Flags)
{
    NTSTATUS status;
    struct queue_timer *t;
    struct timer_queue *q = get_timer_queue(TimerQueue);

    if (!q) return STATUS_NO_MEMORY;
    if (q->magic != TIMER_QUEUE_MAGIC) return STATUS_INVALID_HANDLE;

    t = RtlAllocateHeap(GetProcessHeap(), 0, sizeof *t);
    if (!t)
        return STATUS_NO_MEMORY;

    t->q = q;
    t->runcount = 0;
    t->callback = Callback;
    t->param = Parameter;
    t->period = Period;
    t->flags = Flags;
    t->destroy = FALSE;
    t->event = NULL;

    status = STATUS_SUCCESS;
    RtlEnterCriticalSection(&q->cs);
    if (q->quit)
        status = STATUS_INVALID_HANDLE;
    else
        queue_add_timer(t, queue_current_time() + DueTime, TRUE);
    RtlLeaveCriticalSection(&q->cs);

    if (status == STATUS_SUCCESS)
        *NewTimer = t;
    else
        RtlFreeHeap(GetProcessHeap(), 0, t);

    return status;
}

/***********************************************************************
 *              RtlUpdateTimer   (NTDLL.@)
 *
 * Changes the time at which a timer expires.
 *
 * PARAMS
 *  TimerQueue [I] The queue that holds the timer.
 *  Timer      [I] The timer to update.
 *  DueTime    [I] The delay, in milliseconds, before next firing the timer.
 *  Period     [I] The period, in milliseconds, at which to fire the timer
 *                 after the first callback.  If zero, the timer will not
 *                 refire once.  It still needs to be deleted with
 *                 RtlDeleteTimer.
 *
 * RETURNS
 *  Success: STATUS_SUCCESS.
 *  Failure: Any NTSTATUS code.
 */
NTSTATUS WINAPI RtlUpdateTimer(HANDLE TimerQueue, HANDLE Timer,
                               DWORD DueTime, DWORD Period)
{
    struct queue_timer *t = Timer;
    struct timer_queue *q = t->q;

    RtlEnterCriticalSection(&q->cs);
    /* Can't change a timer if it was once-only or destroyed.  */
    if (t->expire != EXPIRE_NEVER)
    {
        t->period = Period;
        queue_move_timer(t, queue_current_time() + DueTime, TRUE);
    }
    RtlLeaveCriticalSection(&q->cs);

    return STATUS_SUCCESS;
}

/***********************************************************************
 *              RtlDeleteTimer   (NTDLL.@)
 *
 * Cancels a timer-queue timer.
 *
 * PARAMS
 *  TimerQueue      [I] The queue that holds the timer.
 *  Timer           [I] The timer to update.
 *  CompletionEvent [I] If NULL, return immediately.  If INVALID_HANDLE_VALUE,
 *                      wait until the timer is finished firing all pending
 *                      callbacks before returning.  Otherwise, return
 *                      immediately and set the timer is done.
 *
 * RETURNS
 *  Success: STATUS_SUCCESS if the timer is done, STATUS_PENDING if not,
             or if the completion event is NULL.
 *  Failure: Any NTSTATUS code.
 */
NTSTATUS WINAPI RtlDeleteTimer(HANDLE TimerQueue, HANDLE Timer,
                               HANDLE CompletionEvent)
{
    struct queue_timer *t = Timer;
    struct timer_queue *q;
    NTSTATUS status = STATUS_PENDING;
    HANDLE event = NULL;

    if (!Timer)
        return STATUS_INVALID_PARAMETER_1;
    q = t->q;
    if (CompletionEvent == INVALID_HANDLE_VALUE)
    {
        status = NtCreateEvent(&event, EVENT_ALL_ACCESS, NULL, SynchronizationEvent, FALSE);
        if (status == STATUS_SUCCESS)
            status = STATUS_PENDING;
    }
    else if (CompletionEvent)
        event = CompletionEvent;

    RtlEnterCriticalSection(&q->cs);
    t->event = event;
    if (t->runcount == 0 && event)
        status = STATUS_SUCCESS;
    queue_destroy_timer(t);
    RtlLeaveCriticalSection(&q->cs);

    if (CompletionEvent == INVALID_HANDLE_VALUE && event)
    {
        if (status == STATUS_PENDING)
        {
            NtWaitForSingleObject(event, FALSE, NULL);
            status = STATUS_SUCCESS;
        }
        NtClose(event);
    }

    return status;
}

/* allocate a new threadpool (with at least one worker thread) */
static NTSTATUS tp_threadpool_alloc( struct threadpool **out )
{
    struct threadpool *pool;
    NTSTATUS status;
    HANDLE thread;

    pool = RtlAllocateHeap( GetProcessHeap(), 0, sizeof(*pool) );
    if (!pool)
        return STATUS_NO_MEMORY;

    pool->refcount              = 2; /* this thread + worker proc */
    pool->shutdown              = FALSE;

    RtlInitializeCriticalSection( &pool->cs );
    pool->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": threadpool.cs");

    list_init( &pool->pool );
    RtlInitializeConditionVariable( &pool->update_event );

    pool->max_workers           = 500;
    pool->min_workers           = 1;
    pool->num_workers           = 1;
    pool->num_busy_workers      = 0;

    status = RtlCreateUserThread( GetCurrentProcess(), NULL, FALSE, NULL, 0, 0,
                                  threadpool_worker_proc, pool, &thread, NULL );
    if (status != STATUS_SUCCESS)
    {
        pool->cs.DebugInfo->Spare[0] = 0;
        RtlDeleteCriticalSection( &pool->cs );
        RtlFreeHeap( GetProcessHeap(), 0, pool );
        return status;
    }
    NtClose( thread );

    TRACE("allocated threadpool %p\n", pool);

    *out = pool;
    return STATUS_SUCCESS;
}

/* shutdown all threads of the threadpool */
static void tp_threadpool_shutdown( struct threadpool *pool )
{
    assert( pool != default_threadpool );

    pool->shutdown = TRUE;
    RtlWakeAllConditionVariable( &pool->update_event );
}

/* release a reference to a threadpool */
static BOOL tp_threadpool_release( struct threadpool *pool )
{
    if (interlocked_dec( &pool->refcount ))
        return FALSE;

    TRACE("destroying threadpool %p\n", pool);

    assert( pool->shutdown );
    assert( list_empty( &pool->pool ) );

    pool->cs.DebugInfo->Spare[0] = 0;
    RtlDeleteCriticalSection( &pool->cs );

    RtlFreeHeap( GetProcessHeap(), 0, pool );
    return TRUE;
}

/* returns the threadpool based on the environment structure */
static struct threadpool *get_threadpool( TP_CALLBACK_ENVIRON *environment )
{
    struct threadpool *pool;

    if (environment)
    {
        pool = (struct threadpool *)environment->Pool;
        if (pool) return pool;
    }

    if (!default_threadpool)
    {
        if (tp_threadpool_alloc( &pool ) != STATUS_SUCCESS)
            return NULL;

        if (interlocked_cmpxchg_ptr( (void *)&default_threadpool, pool, NULL ) != NULL)
        {
            tp_threadpool_shutdown( pool );
            tp_threadpool_release( pool );
        }
    }

    return default_threadpool;
}

/* allocates a new cleanup group */
static NTSTATUS tp_group_alloc( struct threadpool_group **out )
{
    struct threadpool_group *group;

    group = RtlAllocateHeap( GetProcessHeap(), 0, sizeof(*group) );
    if (!group)
        return STATUS_NO_MEMORY;

    group->refcount     = 1;
    group->shutdown     = FALSE;

    RtlInitializeCriticalSection( &group->cs );
    group->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": threadpool_group.cs");

    list_init( &group->members );

    TRACE("allocated group %p\n", group);

    *out = group;
    return STATUS_SUCCESS;
}

/* marks a cleanup group for shutdown */
static void tp_group_shutdown( struct threadpool_group *group )
{
    group->shutdown = TRUE;
}

/* releases a reference to a cleanup group */
static BOOL tp_group_release( struct threadpool_group *group )
{
    if (interlocked_dec( &group->refcount ))
        return FALSE;

    TRACE("destroying group %p\n", group);

    assert( group->shutdown );
    assert( list_empty( &group->members ) );

    group->cs.DebugInfo->Spare[0] = 0;
    RtlDeleteCriticalSection( &group->cs );

    RtlFreeHeap( GetProcessHeap(), 0, group );
    return TRUE;
}

/* initializes a new threadpool object */
static void tp_object_initialize( struct threadpool_object *object, struct threadpool *pool,
                                  PVOID userdata, TP_CALLBACK_ENVIRON *environment )
{
    BOOL simple_cb = (object->type == TP_OBJECT_TYPE_SIMPLE);

    object->refcount                = 1;
    object->shutdown                = FALSE;

    object->pool                    = pool;
    object->group                   = NULL;
    object->userdata                = userdata;
    object->group_cancel_callback   = NULL;
    object->finalization_callback   = NULL;
    object->may_run_long            = 0;
    object->race_dll                = NULL;

    memset( &object->group_entry, 0, sizeof(object->group_entry) );
    object->is_group_member         = FALSE;

    memset( &object->pool_entry, 0, sizeof(object->pool_entry) );
    RtlInitializeConditionVariable( &object->finished_event );
    object->num_pending_callbacks   = 0;
    object->num_running_callbacks   = 0;

    if (environment)
    {
        if (environment->Version != 1)
            FIXME("unsupported environment version %u\n", environment->Version);

        object->group = impl_from_TP_CLEANUP_GROUP( environment->CleanupGroup );
        object->group_cancel_callback   = environment->CleanupGroupCancelCallback;
        object->finalization_callback   = environment->FinalizationCallback;
        object->may_run_long            = environment->u.s.LongFunction != 0;
        object->race_dll                = environment->RaceDll;

        if (environment->ActivationContext)
            FIXME("activation context not supported yet\n");

        if (environment->u.s.Persistent)
            FIXME("persistent thread support not supported yet\n");
    }

    /* Increase dll refcount */
    if (object->race_dll)
        LdrAddRefDll( 0, object->race_dll );

    /* Increase reference-count on the pool */
    interlocked_inc( &pool->refcount );

    TRACE("allocated object %p of type %u\n", object, object->type);

    /* For simple callbacks we have to run tp_object_submit before adding this object
     * to the cleanup group. As soon as the cleanup group members are released ->shutdown
     * will be set, and tp_object_submit would fail with an assertion. */
    if (simple_cb)
        tp_object_submit( object );

    if (object->group)
    {
        struct threadpool_group *group = object->group;
        interlocked_inc( &group->refcount );

        RtlEnterCriticalSection( &group->cs );
        list_add_tail( &group->members, &object->group_entry );
        object->is_group_member = TRUE;
        RtlLeaveCriticalSection( &group->cs );
    }

    if (simple_cb)
    {
        tp_object_shutdown( object );
        tp_object_release( object );
    }
}

/* submits an object to a threadpool */
static void tp_object_submit( struct threadpool_object *object )
{
    struct threadpool *pool = object->pool;

    assert( !object->shutdown );
    assert( !pool->shutdown );

    RtlEnterCriticalSection( &pool->cs );

    /* Start new worker threads if required (and allowed) */
    if (pool->num_busy_workers >= pool->num_workers && pool->num_workers < pool->max_workers)
    {
        NTSTATUS status;
        HANDLE thread;

        status = RtlCreateUserThread( GetCurrentProcess(), NULL, FALSE, NULL, 0, 0,
                                      threadpool_worker_proc, pool, &thread, NULL );
        if (status == STATUS_SUCCESS)
        {
            interlocked_inc( &pool->refcount );
            pool->num_workers++;
            NtClose( thread );
            goto out;
        }
    }

    assert( pool->num_workers > 0 );
    RtlWakeConditionVariable( &pool->update_event );

out:
    /* Queue work item into pool and increment refcount */
    interlocked_inc( &object->refcount );
    if (!object->num_pending_callbacks++)
        list_add_tail( &pool->pool, &object->pool_entry );

    RtlLeaveCriticalSection( &pool->cs );
}

static void tp_object_cancel( struct threadpool_object *object, BOOL group_cancel, PVOID userdata )
{
    struct threadpool *pool = object->pool;
    LONG pending_callbacks = 0;

    RtlEnterCriticalSection( &pool->cs );

    /* Remove the pending callbacks from the pool */
    if (object->num_pending_callbacks)
    {
        pending_callbacks = object->num_pending_callbacks;
        object->num_pending_callbacks = 0;
        list_remove( &object->pool_entry );
    }

    RtlLeaveCriticalSection( &pool->cs );

    /* Execute group cancellation callback if defined, and if this was actually a group cancel. */
    if (pending_callbacks && group_cancel && object->group_cancel_callback)
    {
        TRACE( "executing group cancel callback %p(%p, %p)\n", object->group_cancel_callback, object, userdata );
        object->group_cancel_callback( object, userdata );
        TRACE( "callback %p returned\n", object->group_cancel_callback );
    }

    /* Release references */
    while (pending_callbacks--)
        tp_object_release( object );
}

static void tp_object_wait( struct threadpool_object *object )
{
    struct threadpool *pool = object->pool;

    RtlEnterCriticalSection( &pool->cs );

    /* Wait until there are no longer pending or running callbacks */
    while (object->num_pending_callbacks || object->num_running_callbacks)
        RtlSleepConditionVariableCS( &object->finished_event, &pool->cs, NULL );

    RtlLeaveCriticalSection( &pool->cs );
}

/* mark an object as 'shutdown', submitting is no longer possible */
static void tp_object_shutdown( struct threadpool_object *object )
{
    object->shutdown = TRUE;
}

/* release a reference to a threadpool object */
static BOOL tp_object_release( struct threadpool_object *object )
{
    if (interlocked_dec( &object->refcount ))
        return FALSE;

    TRACE("destroying object %p of type %u\n", object, object->type);

    assert( object->shutdown );
    assert( !object->num_pending_callbacks );
    assert( !object->num_running_callbacks );

    /* release reference to the group */
    if (object->group)
    {
        struct threadpool_group *group = object->group;

        RtlEnterCriticalSection( &group->cs );
        if (object->is_group_member)
        {
            list_remove( &object->group_entry );
            object->is_group_member = FALSE;
        }
        RtlLeaveCriticalSection( &group->cs );

        tp_group_release( group );
    }

    /* release reference to threadpool */
    tp_threadpool_release( object->pool );

    /* release reference to library */
    if (object->race_dll)
        LdrUnloadDll( object->race_dll );

    RtlFreeHeap( GetProcessHeap(), 0, object );
    return TRUE;
}

/* initializes a threadpool instance structure */
static void tp_instance_initialize( struct threadpool_instance *instance, struct threadpool_object *object )
{
    instance->object                    = object;
    instance->threadid                  = GetCurrentThreadId();
    instance->may_run_long              = object->may_run_long;
}

/* hint for the threadpool that the execution might take long, spawn additional workers */
static BOOL tp_instance_may_run_long( struct threadpool_instance *instance )
{
    struct threadpool_object *object;
    struct threadpool *pool;
    NTSTATUS status = STATUS_SUCCESS;

    if (instance->threadid != GetCurrentThreadId())
    {
        ERR("called from wrong thread, ignoring\n");
        return FALSE;
    }

    if (instance->may_run_long)
        return TRUE;

    object = instance->object;
    pool   = object->pool;
    RtlEnterCriticalSection( &pool->cs );

    if (pool->num_busy_workers >= pool->num_workers && pool->num_workers < pool->max_workers)
    {
        HANDLE thread;
        status = RtlCreateUserThread( GetCurrentProcess(), NULL, FALSE, NULL, 0, 0,
                                      threadpool_worker_proc, pool, &thread, NULL );
        if (status == STATUS_SUCCESS)
        {
            interlocked_inc( &pool->refcount );
            pool->num_workers++;
            NtClose( thread );
        }
    }

    RtlLeaveCriticalSection( &pool->cs );
    instance->may_run_long = TRUE;
    return !status;
}

/* threadpool worker function */
static void CALLBACK threadpool_worker_proc( void *param )
{
    struct threadpool_instance instance;
    TP_CALLBACK_INSTANCE *cb_instance = (TP_CALLBACK_INSTANCE *)&instance;
    struct threadpool *pool = param;
    LARGE_INTEGER timeout;
    struct list *ptr;

    RtlEnterCriticalSection( &pool->cs );
    for (;;)
    {
        while ((ptr = list_head( &pool->pool )))
        {
            struct threadpool_object *object = LIST_ENTRY( ptr, struct threadpool_object, pool_entry );
            assert( object->num_pending_callbacks > 0 );

            /* If further pending callbacks are queued, move the work item to
             * the end of the pool list. Otherwise remove it from the pool. */
            list_remove( &object->pool_entry );
            if (--object->num_pending_callbacks)
                list_add_tail( &pool->pool, &object->pool_entry );

            /* Leave critical section and do the actual callback. */
            object->num_running_callbacks++;
            pool->num_busy_workers++;
            RtlLeaveCriticalSection( &pool->cs );
            tp_instance_initialize( &instance, object );

            switch (object->type)
            {
                case TP_OBJECT_TYPE_SIMPLE:
                {
                    TRACE( "executing simple callback %p(%p, %p)\n",
                           object->u.simple.callback, cb_instance, object->userdata );
                    object->u.simple.callback( cb_instance, object->userdata );
                    TRACE( "callback %p returned\n", object->u.simple.callback );
                    break;
                }

                case TP_OBJECT_TYPE_WORK:
                {
                    TRACE( "executing work callback %p(%p, %p, %p)\n",
                           object->u.work.callback, cb_instance, object->userdata, object );
                    object->u.work.callback( cb_instance, object->userdata, (TP_WORK *)object );
                    TRACE( "callback %p returned\n", object->u.work.callback );
                    break;
                }

                default:
                    assert(0);
                    break;
            }

            /* Execute finalization callback */
            if (object->finalization_callback)
            {
                TRACE( "executing finalization callback %p(%p, %p)\n",
                       object->finalization_callback, cb_instance, object->userdata );
                object->finalization_callback( cb_instance, object->userdata );
                TRACE( "callback %p returned\n", object->finalization_callback );
            }

            RtlEnterCriticalSection( &pool->cs );
            pool->num_busy_workers--;
            object->num_running_callbacks--;
            if (!object->num_pending_callbacks && !object->num_running_callbacks)
                RtlWakeAllConditionVariable( &object->finished_event );
            tp_object_release( object );
        }

        /* Shutdown worker thread if requested. */
        if (pool->shutdown)
            break;

        /* Wait for new tasks or until timeout expires. Never terminate the last worker. */
        timeout.QuadPart = (ULONGLONG)THREADPOOL_WORKER_TIMEOUT * -10000;
        if (RtlSleepConditionVariableCS( &pool->update_event, &pool->cs, &timeout ) == STATUS_TIMEOUT &&
            !list_head( &pool->pool ) && pool->num_workers > 1)
        {
            break;
        }
    }
    pool->num_workers--;
    RtlLeaveCriticalSection( &pool->cs );
    tp_threadpool_release( pool );
}


/***********************************************************************
 *           TpAllocCleanupGroup    (NTDLL.@)
 */
NTSTATUS WINAPI TpAllocCleanupGroup( TP_CLEANUP_GROUP **out )
{
    TRACE("%p\n", out);

    if (!out)
        return STATUS_ACCESS_VIOLATION;

    return tp_group_alloc( (struct threadpool_group **)out );
}

/***********************************************************************
 *           TpAllocPool    (NTDLL.@)
 */
NTSTATUS WINAPI TpAllocPool( TP_POOL **out, PVOID reserved )
{
    TRACE("%p %p\n", out, reserved);

    if (reserved)
        FIXME("reserved argument is nonzero (%p)", reserved);

    if (!out)
        return STATUS_ACCESS_VIOLATION;

    return tp_threadpool_alloc( (struct threadpool **)out );
}

/***********************************************************************
 *           TpAllocWork    (NTDLL.@)
 */
NTSTATUS WINAPI TpAllocWork( TP_WORK **out, PTP_WORK_CALLBACK callback, PVOID userdata,
                             TP_CALLBACK_ENVIRON *environment )
{
    struct threadpool_object *object;
    struct threadpool *pool;

    TRACE("%p %p %p %p\n", out, callback, userdata, environment);

    if (!(pool = get_threadpool( environment )))
        return STATUS_NO_MEMORY;

    object = RtlAllocateHeap( GetProcessHeap(), 0, sizeof(*object) );
    if (!object)
        return STATUS_NO_MEMORY;

    object->type = TP_OBJECT_TYPE_WORK;
    object->u.work.callback = callback;
    tp_object_initialize( object, pool, userdata, environment );

    *out = (TP_WORK *)object;
    return STATUS_SUCCESS;
}

/***********************************************************************
 *           TpCallbackMayRunLong    (NTDLL.@)
 */
NTSTATUS WINAPI TpCallbackMayRunLong( TP_CALLBACK_INSTANCE *instance )
{
    struct threadpool_instance *this = impl_from_TP_CALLBACK_INSTANCE( instance );
    TRACE("%p\n", instance);

    if (!this)
        return STATUS_ACCESS_VIOLATION;

    return tp_instance_may_run_long( this );
}

/***********************************************************************
 *           TpPostWork    (NTDLL.@)
 */
VOID WINAPI TpPostWork( TP_WORK *work )
{
    struct threadpool_object *this = impl_from_TP_WORK( work );
    TRACE("%p\n", work);

    if (this)
    {
        tp_object_submit( this );
    }
}

/***********************************************************************
 *           TpReleaseCleanupGroup    (NTDLL.@)
 */
VOID WINAPI TpReleaseCleanupGroup( TP_CLEANUP_GROUP *group )
{
    struct threadpool_group *this = impl_from_TP_CLEANUP_GROUP( group );
    TRACE("%p\n", group);

    if (this)
    {
        tp_group_shutdown( this );
        tp_group_release( this );
    }
}

/***********************************************************************
 *           TpReleaseCleanupGroupMembers    (NTDLL.@)
 */
VOID WINAPI TpReleaseCleanupGroupMembers( TP_CLEANUP_GROUP *group, BOOL cancel_pending, PVOID userdata )
{
    struct threadpool_group *this = impl_from_TP_CLEANUP_GROUP( group );
    struct threadpool_object *object, *next;
    struct list members;

    TRACE("%p %d %p\n", group, cancel_pending, userdata);

    if (!this)
        return;

    RtlEnterCriticalSection( &this->cs );

    /* Unset group, increase references, and mark objects for shutdown */
    LIST_FOR_EACH_ENTRY_SAFE( object, next, &this->members, struct threadpool_object, group_entry )
    {
        assert( object->group == this );
        assert( object->is_group_member );

        /* Simple callbacks are very special. The user doesn't hold any reference, so
         * they would be released too early. Add one additional temporary reference. */
        if (object->type == TP_OBJECT_TYPE_SIMPLE)
        {
            if (interlocked_inc( &object->refcount ) == 1)
            {
                /* Object is basically already destroyed, but group reference
                 * was not deleted yet. We can safely ignore this object. */
                interlocked_dec( &object->refcount );
                list_remove( &object->group_entry );
                object->is_group_member = FALSE;
                continue;
            }
        }

        object->is_group_member = FALSE;
        tp_object_shutdown( object );
    }

    /* Move members to a local list */
    list_init( &members );
    list_move_tail( &members, &this->members );

    RtlLeaveCriticalSection( &this->cs );

    /* Cancel pending callbacks if requested */
    if (cancel_pending)
    {
        LIST_FOR_EACH_ENTRY( object, &members, struct threadpool_object, group_entry )
        {
            tp_object_cancel( object, TRUE, userdata );
        }
    }

    /* Wait for remaining callbacks to finish */
    LIST_FOR_EACH_ENTRY_SAFE( object, next, &members, struct threadpool_object, group_entry )
    {
        tp_object_wait( object );
        tp_object_release( object );
    }
}

/***********************************************************************
 *           TpReleasePool    (NTDLL.@)
 */
VOID WINAPI TpReleasePool( TP_POOL *pool )
{
    struct threadpool *this = impl_from_TP_POOL( pool );
    TRACE("%p\n", pool);

    if (this)
    {
        tp_threadpool_shutdown( this );
        tp_threadpool_release( this );
    }
}

/***********************************************************************
 *           TpReleaseWork    (NTDLL.@)
 */
VOID WINAPI TpReleaseWork( TP_WORK *work )
{
    struct threadpool_object *this = impl_from_TP_WORK( work );
    TRACE("%p\n", work);

    if (this)
    {
        tp_object_shutdown( this );
        tp_object_release( this );
    }
}

/***********************************************************************
 *           TpSetPoolMaxThreads    (NTDLL.@)
 */
VOID WINAPI TpSetPoolMaxThreads( TP_POOL *pool, DWORD maximum )
{
    struct threadpool *this = impl_from_TP_POOL( pool );
    TRACE("%p %d\n", pool, maximum);

    if (this)
    {
        RtlEnterCriticalSection( &this->cs );
        this->max_workers = max( maximum, 1 );
        this->min_workers = min( this->min_workers, this->max_workers );
        RtlLeaveCriticalSection( &this->cs );
    }
}

/***********************************************************************
 *           TpSetPoolMinThreads    (NTDLL.@)
 */
BOOL WINAPI TpSetPoolMinThreads( TP_POOL *pool, DWORD minimum )
{
    struct threadpool *this = impl_from_TP_POOL( pool );
    FIXME("%p %d: semi-stub\n", pool, minimum);

    if (this)
    {
        RtlEnterCriticalSection( &this->cs );
        this->min_workers = max( minimum, 1 );
        this->max_workers = max( this->min_workers, this->max_workers );
        RtlLeaveCriticalSection( &this->cs );
    }
    return TRUE;
}

/***********************************************************************
 *           TpSimpleTryPost    (NTDLL.@)
 */
NTSTATUS WINAPI TpSimpleTryPost( PTP_SIMPLE_CALLBACK callback, PVOID userdata,
                                 TP_CALLBACK_ENVIRON *environment )
{
    struct threadpool_object *object;
    struct threadpool *pool;

    TRACE("%p %p %p\n", callback, userdata, environment);

    if (!(pool = get_threadpool( environment )))
        return STATUS_NO_MEMORY;

    object = RtlAllocateHeap( GetProcessHeap(), 0, sizeof(*object) );
    if (!object)
        return STATUS_NO_MEMORY;

    object->type = TP_OBJECT_TYPE_SIMPLE;
    object->u.simple.callback = callback;
    tp_object_initialize( object, pool, userdata, environment );

    return STATUS_SUCCESS;
}

/***********************************************************************
 *           TpWaitForWork    (NTDLL.@)
 */
VOID WINAPI TpWaitForWork( TP_WORK *work, BOOL cancel_pending )
{
    struct threadpool_object *this = impl_from_TP_WORK( work );
    TRACE("%p %d\n", work, cancel_pending);

    if (this)
    {
        if (cancel_pending)
            tp_object_cancel( this, FALSE, NULL );
        tp_object_wait( this );
    }
}
