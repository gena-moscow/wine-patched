/*
 * Unit test suite for thread pool functions
 *
 * Copyright 2015 Sebastian Lackner
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

#include "ntdll_test.h"

static HMODULE hntdll = 0;
static NTSTATUS (WINAPI *pTpAllocCleanupGroup)(TP_CLEANUP_GROUP **);
static NTSTATUS (WINAPI *pTpAllocIoCompletion)(TP_IO **,HANDLE,PTP_WIN32_IO_CALLBACK,PVOID,TP_CALLBACK_ENVIRON *);
static NTSTATUS (WINAPI *pTpAllocPool)(TP_POOL **,PVOID);
static NTSTATUS (WINAPI *pTpAllocTimer)(TP_TIMER **,PTP_TIMER_CALLBACK,PVOID,TP_CALLBACK_ENVIRON *);
static NTSTATUS (WINAPI *pTpAllocWait)(TP_WAIT **,PTP_WAIT_CALLBACK,PVOID,TP_CALLBACK_ENVIRON *);
static NTSTATUS (WINAPI *pTpAllocWork)(TP_WORK **,PTP_WORK_CALLBACK,PVOID,TP_CALLBACK_ENVIRON *);
static VOID     (WINAPI *pTpCallbackLeaveCriticalSectionOnCompletion)(TP_CALLBACK_INSTANCE *,CRITICAL_SECTION *);
static NTSTATUS (WINAPI *pTpCallbackMayRunLong)(TP_CALLBACK_INSTANCE *);
static VOID     (WINAPI *pTpCallbackReleaseMutexOnCompletion)(TP_CALLBACK_INSTANCE *,HANDLE);
static VOID     (WINAPI *pTpCallbackReleaseSemaphoreOnCompletion)(TP_CALLBACK_INSTANCE *,HANDLE,DWORD);
static VOID     (WINAPI *pTpCallbackSetEventOnCompletion)(TP_CALLBACK_INSTANCE *,HANDLE);
static VOID     (WINAPI *pTpCallbackUnloadDllOnCompletion)(TP_CALLBACK_INSTANCE *,HMODULE);
static VOID     (WINAPI *pTpCancelAsyncIoOperation)(TP_IO *);
static VOID     (WINAPI *pTpDisassociateCallback)(TP_CALLBACK_INSTANCE *);
static BOOL     (WINAPI *pTpIsTimerSet)(TP_TIMER *);
static VOID     (WINAPI *pTpPostWork)(TP_WORK *);
static VOID     (WINAPI *pTpReleaseCleanupGroup)(TP_CLEANUP_GROUP *);
static VOID     (WINAPI *pTpReleaseCleanupGroupMembers)(TP_CLEANUP_GROUP *,BOOL,PVOID);
static VOID     (WINAPI *pTpReleaseIoCompletion)(TP_IO *);
static VOID     (WINAPI *pTpReleasePool)(TP_POOL *);
static VOID     (WINAPI *pTpReleaseTimer)(TP_TIMER *);
static VOID     (WINAPI *pTpReleaseWait)(TP_WAIT *);
static VOID     (WINAPI *pTpReleaseWork)(TP_WORK *);
static VOID     (WINAPI *pTpSetPoolMaxThreads)(TP_POOL *,DWORD);
static BOOL     (WINAPI *pTpSetPoolMinThreads)(TP_POOL *,DWORD);
static VOID     (WINAPI *pTpSetTimer)(TP_TIMER *,LARGE_INTEGER *,LONG,LONG);
static VOID     (WINAPI *pTpSetWait)(TP_WAIT *,HANDLE,LARGE_INTEGER *);
static NTSTATUS (WINAPI *pTpSimpleTryPost)(PTP_SIMPLE_CALLBACK,PVOID,TP_CALLBACK_ENVIRON *);
static VOID     (WINAPI *pTpStartAsyncIoOperation)(TP_IO *);
static VOID     (WINAPI *pTpWaitForIoCompletion)(TP_IO *,BOOL);
static VOID     (WINAPI *pTpWaitForTimer)(TP_TIMER *,BOOL);
static VOID     (WINAPI *pTpWaitForWait)(TP_WAIT *,BOOL);
static VOID     (WINAPI *pTpWaitForWork)(TP_WORK *,BOOL);

#define NTDLL_GET_PROC(func) \
    do \
    { \
        p ## func = (void *)GetProcAddress(hntdll, #func); \
        if (!p ## func) trace("Failed to get address for %s\n", #func); \
    } \
    while (0)

static BOOL init_threadpool(void)
{
    hntdll = GetModuleHandleA("ntdll");
    if (!hntdll)
    {
        win_skip("Could not load ntdll\n");
        return FALSE;
    }

    NTDLL_GET_PROC(TpAllocCleanupGroup);
    NTDLL_GET_PROC(TpAllocIoCompletion);
    NTDLL_GET_PROC(TpAllocPool);
    NTDLL_GET_PROC(TpAllocTimer);
    NTDLL_GET_PROC(TpAllocWait);
    NTDLL_GET_PROC(TpAllocWork);
    NTDLL_GET_PROC(TpCallbackLeaveCriticalSectionOnCompletion);
    NTDLL_GET_PROC(TpCallbackMayRunLong);
    NTDLL_GET_PROC(TpCallbackReleaseMutexOnCompletion);
    NTDLL_GET_PROC(TpCallbackReleaseSemaphoreOnCompletion);
    NTDLL_GET_PROC(TpCallbackSetEventOnCompletion);
    NTDLL_GET_PROC(TpCallbackUnloadDllOnCompletion);
    NTDLL_GET_PROC(TpCancelAsyncIoOperation);
    NTDLL_GET_PROC(TpDisassociateCallback);
    NTDLL_GET_PROC(TpIsTimerSet);
    NTDLL_GET_PROC(TpPostWork);
    NTDLL_GET_PROC(TpReleaseCleanupGroup);
    NTDLL_GET_PROC(TpReleaseCleanupGroupMembers);
    NTDLL_GET_PROC(TpReleaseIoCompletion);
    NTDLL_GET_PROC(TpReleasePool);
    NTDLL_GET_PROC(TpReleaseTimer);
    NTDLL_GET_PROC(TpReleaseWait);
    NTDLL_GET_PROC(TpReleaseWork);
    NTDLL_GET_PROC(TpSetPoolMaxThreads);
    NTDLL_GET_PROC(TpSetPoolMinThreads);
    NTDLL_GET_PROC(TpSetTimer);
    NTDLL_GET_PROC(TpSetWait);
    NTDLL_GET_PROC(TpSimpleTryPost);
    NTDLL_GET_PROC(TpStartAsyncIoOperation);
    NTDLL_GET_PROC(TpWaitForIoCompletion);
    NTDLL_GET_PROC(TpWaitForTimer);
    NTDLL_GET_PROC(TpWaitForWait);
    NTDLL_GET_PROC(TpWaitForWork);

    if (!pTpAllocPool)
    {
        win_skip("Threadpool functions not supported, skipping tests\n");
        return FALSE;
    }

    return TRUE;
}

#undef NTDLL_GET_PROC


static void CALLBACK simple_cb(TP_CALLBACK_INSTANCE *instance, void *userdata)
{
    HANDLE semaphore = userdata;
    trace("Running simple callback\n");
    ReleaseSemaphore(semaphore, 1, NULL);
}

static void CALLBACK simple2_cb(TP_CALLBACK_INSTANCE *instance, void *userdata)
{
    trace("Running simple2 callback\n");
    Sleep(100);
    InterlockedIncrement((LONG *)userdata);
}

static void test_tp_simple(void)
{
    TP_CALLBACK_ENVIRON environment;
    TP_CLEANUP_GROUP *group;
    HANDLE semaphore;
    NTSTATUS status;
    TP_POOL *pool;
    LONG userdata;
    DWORD result;
    int i;

    semaphore = CreateSemaphoreA(NULL, 0, 1, NULL);
    ok(semaphore != NULL, "CreateSemaphoreA failed %u\n", GetLastError());

    /* post the callback using the default threadpool */
    memset(&environment, 0, sizeof(environment));
    environment.Version = 1;
    environment.Pool = NULL;
    status = pTpSimpleTryPost(simple_cb, semaphore, &environment);
    ok(!status, "TpSimpleTryPost failed with status %x\n", status);
    result = WaitForSingleObject(semaphore, 1000);
    ok(result == WAIT_OBJECT_0, "WaitForSingleObject returned %u\n", result);

    /* allocate new threadpool */
    pool = NULL;
    status = pTpAllocPool(&pool, NULL);
    ok(!status, "TpAllocPool failed with status %x\n", status);
    ok(pool != NULL, "expected pool != NULL\n");

    /* post the callback using the new threadpool */
    memset(&environment, 0, sizeof(environment));
    environment.Version = 1;
    environment.Pool = pool;
    status = pTpSimpleTryPost(simple_cb, semaphore, &environment);
    ok(!status, "TpSimpleTryPost failed with status %x\n", status);
    result = WaitForSingleObject(semaphore, 1000);
    ok(result == WAIT_OBJECT_0, "WaitForSingleObject returned %u\n", result);

    /* test with invalid version number */
    memset(&environment, 0, sizeof(environment));
    environment.Version = 9999;
    environment.Pool = pool;
    status = pTpSimpleTryPost(simple_cb, semaphore, &environment);
    todo_wine
    ok(status == STATUS_INVALID_PARAMETER || broken(!status) /* Vista/2008 */,
       "TpSimpleTryPost unexpectedly returned status %x\n", status);
    if (!status)
    {
        result = WaitForSingleObject(semaphore, 1000);
        ok(result == WAIT_OBJECT_0, "WaitForSingleObject returned %u\n", result);
    }

    /* allocate a cleanup group for synchronization */
    group = NULL;
    status = pTpAllocCleanupGroup(&group);
    ok(!status, "TpAllocCleanupGroup failed with status %x\n", status);
    ok(group != NULL, "expected pool != NULL\n");

    /* use cleanup group to wait for a simple callback */
    userdata = 0;
    memset(&environment, 0, sizeof(environment));
    environment.Version = 1;
    environment.Pool = pool;
    environment.CleanupGroup = group;
    status = pTpSimpleTryPost(simple2_cb, &userdata, &environment);
    ok(!status, "TpSimpleTryPost failed with status %x\n", status);
    pTpReleaseCleanupGroupMembers(group, FALSE, NULL);
    ok(userdata == 1, "expected userdata = 1, got %u\n", userdata);

    /* test cancellation of pending simple callbacks */
    userdata = 0;
    memset(&environment, 0, sizeof(environment));
    environment.Version = 1;
    environment.Pool = pool;
    environment.CleanupGroup = group;
    for (i = 0; i < 100; i++)
    {
        status = pTpSimpleTryPost(simple2_cb, &userdata, &environment);
        ok(!status, "TpSimpleTryPost failed with status %x\n", status);
    }
    pTpReleaseCleanupGroupMembers(group, TRUE, NULL);
    ok(userdata < 100, "expected userdata < 100, got %u\n", userdata);

    /* cleanup */
    pTpReleaseCleanupGroup(group);
    pTpReleasePool(pool);
    CloseHandle(semaphore);
}

static void CALLBACK work_cb(TP_CALLBACK_INSTANCE *instance, void *userdata, TP_WORK *work)
{
    trace("Running work callback\n");
    Sleep(10);
    InterlockedIncrement((LONG *)userdata);
}

static void CALLBACK work2_cb(TP_CALLBACK_INSTANCE *instance, void *userdata, TP_WORK *work)
{
    trace("Running work2 callback\n");
    Sleep(10);
    InterlockedExchangeAdd((LONG *)userdata, 0x10000);
}

static void test_tp_work(void)
{
    TP_CALLBACK_ENVIRON environment;
    TP_WORK *work;
    TP_POOL *pool;
    NTSTATUS status;
    LONG userdata;
    int i;

    /* allocate new threadpool */
    pool = NULL;
    status = pTpAllocPool(&pool, NULL);
    ok(!status, "TpAllocPool failed with status %x\n", status);
    ok(pool != NULL, "expected pool != NULL\n");

    /* allocate new work item */
    work = NULL;
    memset(&environment, 0, sizeof(environment));
    environment.Version = 1;
    environment.Pool = pool;
    status = pTpAllocWork(&work, work_cb, &userdata, &environment);
    ok(!status, "TpAllocWork failed with status %x\n", status);
    ok(work != NULL, "expected work != NULL\n");

    /* post 10 identical work items at once */
    userdata = 0;
    for (i = 0; i < 10; i++)
        pTpPostWork(work);
    pTpWaitForWork(work, FALSE);
    ok(userdata == 10, "expected userdata = 10, got %u\n", userdata);

    /* add more tasks and cancel them immediately */
    userdata = 0;
    for (i = 0; i < 10; i++)
        pTpPostWork(work);
    pTpWaitForWork(work, TRUE);
    ok(userdata < 10, "expected userdata < 10, got %u\n", userdata);

    /* cleanup */
    pTpReleaseWork(work);
    pTpReleasePool(pool);
}

static void test_tp_work_scheduler(void)
{
    TP_CALLBACK_ENVIRON environment;
    TP_CLEANUP_GROUP *group;
    TP_WORK *work, *work2;
    TP_POOL *pool;
    NTSTATUS status;
    LONG userdata;
    int i;

    /* allocate new threadpool */
    pool = NULL;
    status = pTpAllocPool(&pool, NULL);
    ok(!status, "TpAllocPool failed with status %x\n", status);
    ok(pool != NULL, "expected pool != NULL\n");

    /* we limit the pool to a single thread */
    pTpSetPoolMaxThreads(pool, 1);

    /* create a cleanup group */
    group = NULL;
    status = pTpAllocCleanupGroup(&group);
    ok(!status, "TpAllocCleanupGroup failed with status %x\n", status);
    ok(group != NULL, "expected pool != NULL\n");

    /* the first work item has no cleanup group associated */
    work = NULL;
    memset(&environment, 0, sizeof(environment));
    environment.Version = 1;
    environment.Pool = pool;
    status = pTpAllocWork(&work, work_cb, &userdata, &environment);
    ok(!status, "TpAllocWork failed with status %x\n", status);
    ok(work != NULL, "expected work != NULL\n");

    /* allocate a second work item with a cleanup group */
    work2 = NULL;
    memset(&environment, 0, sizeof(environment));
    environment.Version = 1;
    environment.Pool = pool;
    environment.CleanupGroup = group;
    status = pTpAllocWork(&work2, work2_cb, &userdata, &environment);
    ok(!status, "TpAllocWork failed with status %x\n", status);
    ok(work2 != NULL, "expected work2 != NULL\n");

    /* the 'work' callbacks are not blocking execution of 'work2' callbacks */
    userdata = 0;
    for (i = 0; i < 10; i++)
        pTpPostWork(work);
    for (i = 0; i < 10; i++)
        pTpPostWork(work2);
    Sleep(30);
    pTpWaitForWork(work, TRUE);
    pTpWaitForWork(work2, TRUE);
    ok(userdata & 0xffff, "expected userdata & 0xffff != 0, got %u\n", userdata & 0xffff);
    ok(userdata >> 16, "expected userdata >> 16 != 0, got %u\n", userdata >> 16);

    /* test ReleaseCleanupGroupMembers on a work item */
    userdata = 0;
    for (i = 0; i < 100; i++)
        pTpPostWork(work);
    for (i = 0; i < 10; i++)
        pTpPostWork(work2);
    pTpReleaseCleanupGroupMembers(group, FALSE, NULL);
    pTpWaitForWork(work, TRUE);
    ok((userdata & 0xffff) < 100, "expected userdata & 0xffff < 100, got %u\n", userdata & 0xffff);
    ok((userdata >> 16) == 10, "expected userdata >> 16 == 10, got %u\n", userdata >> 16);

    /* cleanup */
    pTpReleaseWork(work);
    pTpReleaseCleanupGroup(group);
    pTpReleasePool(pool);
}

static void CALLBACK instance_cb(TP_CALLBACK_INSTANCE *instance, void *userdata)
{
    trace("Running instance callback\n");
    pTpCallbackMayRunLong(instance);
    Sleep(100);
    InterlockedIncrement( (LONG *)userdata );
}

static void CALLBACK instance2_cb(TP_CALLBACK_INSTANCE *instance, void *userdata)
{
    trace("Running instance2 callback\n");
    ok(*(LONG *)userdata == 1, "expected *userdata = 1, got %u\n", *(LONG *)userdata);
    InterlockedIncrement( (LONG *)userdata );
}

static void CALLBACK instance3_cb(TP_CALLBACK_INSTANCE *instance, void *userdata)
{
    trace("Running instance3 callback\n");
    pTpDisassociateCallback(instance);
    Sleep(100);
    InterlockedIncrement( (LONG *)userdata );
}

static void CALLBACK instance4_cb(TP_CALLBACK_INSTANCE *instance, void *userdata)
{
    trace("Running instance4 callback\n");
    pTpCallbackReleaseSemaphoreOnCompletion(instance, userdata, 1);
}

static HANDLE instance_finalization_semaphore;

static void CALLBACK instance_finalization_cb(TP_CALLBACK_INSTANCE *instance, void *userdata)
{
    DWORD ret;
    trace("Running instance finalization callback\n");
    ok(*(LONG *)userdata == 1, "expected *userdata = 1, got %u\n", *(LONG *)userdata);

    /* Make sure that this callback is called before the regular instance cleanup tasks */
    ret = WaitForSingleObject(instance_finalization_semaphore, 100);
    ok(ret == WAIT_TIMEOUT, "expected ret = WAIT_TIMEOUT, got %u\n", ret);

    InterlockedIncrement( (LONG *)userdata );
}

static void CALLBACK instance5_cb(TP_CALLBACK_INSTANCE *instance, void *userdata)
{
    trace("Running instance5 callback\n");
    pTpCallbackReleaseSemaphoreOnCompletion(instance, instance_finalization_semaphore, 1);
    InterlockedIncrement( (LONG *)userdata );
}


static void test_tp_instance(void)
{
    TP_CALLBACK_ENVIRON environment;
    TP_CLEANUP_GROUP *group;
    TP_POOL *pool;
    NTSTATUS status;
    LONG userdata;
    HANDLE semaphore;
    DWORD ret;

    /* Allocate new threadpool */
    pool = NULL;
    status = pTpAllocPool(&pool, NULL);
    ok(!status, "TpAllocPool failed with status %x\n", status);
    ok(pool != NULL, "expected pool != NULL\n");

    /* We limit the pool to a single thread */
    pTpSetPoolMaxThreads(pool, 1);

    /* Test behaviour of TpCallbackMayRunLong when the max number of threads is reached */
    userdata = 0;
    memset(&environment, 0, sizeof(environment));
    environment.Version = 1;
    environment.Pool = pool;
    status = pTpSimpleTryPost(instance_cb, &userdata, &environment);
    ok(!status, "TpSimpleTryPost failed with status %x\n", status);
    status = pTpSimpleTryPost(instance2_cb, &userdata, &environment);
    ok(!status, "TpSimpleTryPost failed with status %x\n", status);
    while (userdata != 2) Sleep(10);

    /* Test behaviour of TpDisassociateCallback on wait functions */
    group = NULL;
    status = pTpAllocCleanupGroup(&group);
    ok(!status, "TpAllocCleanupGroup failed with status %x\n", status);
    ok(group != NULL, "expected pool != NULL\n");

    userdata = 0;
    memset(&environment, 0, sizeof(environment));
    environment.Version = 1;
    environment.Pool = pool;
    environment.CleanupGroup = group;
    status = pTpSimpleTryPost(instance3_cb, &userdata, &environment);
    ok(!status, "TpSimpleTryPost failed with status %x\n", status);

    pTpReleaseCleanupGroupMembers(group, FALSE, NULL);
    todo_wine /* behaviour contradicts the MSDN description? */
    ok(userdata == 1, "expected userdata = 1, got %u\n", userdata);
    while (userdata == 0) Sleep(10);

    pTpReleaseCleanupGroup(group);

    /* Test for TpCallbackReleaseSemaphoreOnCompletion */
    semaphore = CreateSemaphoreW(NULL, 0, 1, NULL);
    ok(semaphore != NULL, "failed to create semaphore\n");
    memset(&environment, 0, sizeof(environment));
    environment.Version = 1;
    environment.Pool = pool;
    status = pTpSimpleTryPost(instance4_cb, semaphore, &environment);
    ok(!status, "TpSimpleTryPost failed with status %x\n", status);
    ret = WaitForSingleObject(semaphore, 1000);
    ok(ret == WAIT_OBJECT_0, "expected ret = WAIT_OBJECT_0, got %u\n", ret);

    /* Test for finalization callback */
    userdata = 0;
    instance_finalization_semaphore = semaphore;
    memset(&environment, 0, sizeof(environment));
    environment.Version = 1;
    environment.Pool = pool;
    environment.FinalizationCallback = instance_finalization_cb;
    status = pTpSimpleTryPost(instance5_cb, &userdata, &environment);
    ok(!status, "TpSimpleTryPost failed with status %x\n", status);
    ret = WaitForSingleObject(semaphore, 1000);
    ok(ret == WAIT_OBJECT_0, "expected ret = WAIT_OBJECT_0, got %u\n", ret);
    while (userdata != 2) Sleep(10);

    CloseHandle(semaphore);

    /* Cleanup */
    pTpReleasePool(pool);
}

static DWORD group_cancel_tid;

static void CALLBACK group_cancel_cleanup_cb(void *object, void *userdata)
{
    trace("Running group cancel cleanup callback\n");
    InterlockedIncrement( (LONG *)userdata );
    group_cancel_tid = GetCurrentThreadId();
}

static void CALLBACK group_cancel_cb(TP_CALLBACK_INSTANCE *instance, void *userdata)
{
    trace("Running group cancel callback\n");
    pTpCallbackMayRunLong(instance);
    Sleep(100);
    ok(*(LONG *)userdata == 1, "expected *userdata = 1, got %u\n", *(LONG *)userdata);
    InterlockedIncrement( (LONG *)userdata );
}

static void CALLBACK dummy_cb(TP_CALLBACK_INSTANCE *instance, void *userdata)
{
    ok(0, "Unexpected call to dummy_cb function\n");
}

static void test_tp_group_cancel(void)
{
    TP_CALLBACK_ENVIRON environment;
    TP_CLEANUP_GROUP *group;
    TP_WORK *work;
    TP_POOL *pool;
    NTSTATUS status;
    LONG userdata, userdata2;
    int i;

    /* Allocate new threadpool */
    pool = NULL;
    status = pTpAllocPool(&pool, NULL);
    ok(!status, "TpAllocPool failed with status %x\n", status);
    ok(pool != NULL, "expected pool != NULL\n");

    /* We limit the pool to a single thread */
    pTpSetPoolMaxThreads(pool, 1);

    /* Allocate a cleanup group */
    group = NULL;
    status = pTpAllocCleanupGroup(&group);
    ok(!status, "TpAllocCleanupGroup failed with status %x\n", status);
    ok(group != NULL, "expected pool != NULL\n");

    /* Test execution of cancellation callback */
    userdata = 0;
    memset(&environment, 0, sizeof(environment));
    environment.Version = 1;
    environment.Pool = pool;
    status = pTpSimpleTryPost(group_cancel_cb, &userdata, &environment);
    ok(!status, "TpSimpleTryPost failed with status %x\n", status);

    memset(&environment, 0, sizeof(environment));
    environment.Version = 1;
    environment.Pool = pool;
    environment.CleanupGroup = group;
    environment.CleanupGroupCancelCallback = group_cancel_cleanup_cb;
    status = pTpSimpleTryPost(dummy_cb, NULL, &environment);
    ok(!status, "TpSimpleTryPost failed with status %x\n", status);

    group_cancel_tid = 0;
    pTpReleaseCleanupGroupMembers(group, TRUE, &userdata);
    ok(userdata == 1, "expected userdata = 1, got %u\n", userdata);
    ok(group_cancel_tid == GetCurrentThreadId(), "expected tid %x, got %x\n",
       GetCurrentThreadId(), group_cancel_tid);
    while (userdata != 2) Sleep(10);

    /* Test cancellation callback for elements with multiple instances */
    /* Allocate new work item */
    work = NULL;
    memset(&environment, 0, sizeof(environment));
    environment.Version = 1;
    environment.Pool = pool;
    environment.CleanupGroup = group;
    environment.CleanupGroupCancelCallback = group_cancel_cleanup_cb;
    status = pTpAllocWork(&work, work_cb, &userdata, &environment);
    ok(!status, "TpAllocWork failed with status %x\n", status);
    ok(work != NULL, "expected work != NULL\n");

    /* Post 10 identical work items at once */
    userdata = userdata2 = 0;
    for (i = 0; i < 10; i++)
        pTpPostWork(work);

    /* Check if we get multiple cancellation callbacks */
    group_cancel_tid = 0;
    pTpReleaseCleanupGroupMembers(group, TRUE, &userdata2);
    ok(userdata < 10, "expected userdata < 10, got %u\n", userdata);
    ok(userdata2 == 1, "expected only one cancellation callback, got %u\n", userdata2);
    ok(group_cancel_tid == GetCurrentThreadId(), "expected tid %x, got %x\n",
       GetCurrentThreadId(), group_cancel_tid);

    /* Cleanup */
    pTpReleaseCleanupGroup(group);
    pTpReleasePool(pool);
}

static void CALLBACK timer_cb(TP_CALLBACK_INSTANCE *instance, void *userdata, TP_TIMER *timer)
{
    trace("Running timer callback\n");
    InterlockedIncrement( (LONG *)userdata );
}

static void test_tp_timer(void)
{
    TP_CALLBACK_ENVIRON environment;
    TP_TIMER *timer;
    TP_POOL *pool;
    NTSTATUS status;
    LONG userdata;
    BOOL success;
    LARGE_INTEGER when;
    DWORD ticks;

    /* Allocate new threadpool */
    pool = NULL;
    status = pTpAllocPool(&pool, NULL);
    ok(!status, "TpAllocPool failed with status %x\n", status);
    ok(pool != NULL, "expected pool != NULL\n");

    /* Allocate new timer item */
    timer = NULL;
    memset(&environment, 0, sizeof(environment));
    environment.Version = 1;
    environment.Pool = pool;
    status = pTpAllocTimer(&timer, timer_cb, &userdata, &environment);
    ok(!status, "TpAllocTimer failed with status %x\n", status);
    ok(timer != NULL, "expected timer != NULL\n");

    success = pTpIsTimerSet(timer);
    ok(!success, "expected TpIsTimerSet(...) = FALSE\n");

    /* Set a relative timeout */
    userdata = 0;
    when.QuadPart = (ULONGLONG)500 * -10000;
    pTpSetTimer(timer, &when, 0, 0);
    success = pTpIsTimerSet(timer);
    ok(success, "expected TpIsTimerSet(...) = TRUE\n");

    /* Wait until timer has triggered */
    pTpWaitForTimer(timer, FALSE);
    Sleep(250);
    ok(userdata == 0, "expected userdata = 0, got %u\n", userdata);
    while (userdata == 0) Sleep(10);
    ok(userdata == 1, "expected userdata = 1, got %u\n", userdata);
    success = pTpIsTimerSet(timer);
    ok(success, "expected TpIsTimerSet(...) = TRUE\n");

    /* Set an absolute timeout */
    userdata = 0;
    NtQuerySystemTime( &when );
    when.QuadPart += (ULONGLONG)500 * 10000;
    pTpSetTimer(timer, &when, 0, 0);
    success = pTpIsTimerSet(timer);
    ok(success, "expected TpIsTimerSet(...) = TRUE\n");

    /* Wait until timer has triggered */
    pTpWaitForTimer(timer, FALSE);
    Sleep(250);
    ok(userdata == 0, "expected userdata = 0, got %u\n", userdata);
    while (userdata == 0) Sleep(10);
    ok(userdata == 1, "expected userdata = 1, got %u\n", userdata);
    success = pTpIsTimerSet(timer);
    ok(success, "expected TpIsTimerSet(...) = TRUE\n");

    /* Test a relative timeout repeated periodically */
    userdata = 0;
    when.QuadPart = (ULONGLONG)50 * -10000;
    pTpSetTimer(timer, &when, 50, 0);
    success = pTpIsTimerSet(timer);
    ok(success, "expected TpIsTimerSet(...) = TRUE\n");

    /* Wait until the timer was triggered a couple of times */
    ticks = GetTickCount();
    while (userdata < 100) Sleep(10);
    ticks = GetTickCount() - ticks;
    pTpSetTimer(timer, NULL, 0, 0);
    pTpWaitForTimer(timer, TRUE);
    ok(ticks >= 4500 && ticks <= 5500, "expected approximately 5000 ticks, got %u\n", ticks);
    success = pTpIsTimerSet(timer);
    ok(!success, "expected TpIsTimerSet(...) = FALSE\n");

    /* Test with zero timeout, we expect a call immediately */
    userdata = 0;
    when.QuadPart = 0;
    pTpSetTimer(timer, &when, 0, 0);
    success = pTpIsTimerSet(timer);
    ok(success, "expected TpIsTimerSet(...) = TRUE\n");

    /* Wait until timer has triggered */
    pTpWaitForTimer(timer, FALSE);
    ok(userdata == 1 || broken(userdata == 0) /* Win 8 */,
       "expected userdata = 1, got %u\n", userdata);
    while (userdata == 0) Sleep(10);
    success = pTpIsTimerSet(timer);
    ok(success, "expected TpIsTimerSet(...) = TRUE\n");

    /* Unset the timer again */
    pTpSetTimer(timer, NULL, 0, 0);
    success = pTpIsTimerSet(timer);
    ok(!success, "expected TpIsTimerSet(...) = FALSE\n");

    /* Cleanup */
    pTpReleaseTimer(timer);
    pTpReleasePool(pool);
}

static void CALLBACK window_length_cb(TP_CALLBACK_INSTANCE *instance, void *userdata, TP_TIMER *timer)
{
    trace("Running window length callback\n");
    (*(DWORD *)userdata) = GetTickCount();
}

static void test_tp_window_length(void)
{
    TP_CALLBACK_ENVIRON environment;
    TP_TIMER *timer, *timer2;
    DWORD ticks, ticks2;
    TP_POOL *pool;
    NTSTATUS status;
    LARGE_INTEGER when;

    /* Allocate new threadpool */
    pool = NULL;
    status = pTpAllocPool(&pool, NULL);
    ok(!status, "TpAllocPool failed with status %x\n", status);
    ok(pool != NULL, "expected pool != NULL\n");

    /* Allocate two identical timers */
    timer = NULL;
    memset(&environment, 0, sizeof(environment));
    environment.Version = 1;
    environment.Pool = pool;
    status = pTpAllocTimer(&timer, window_length_cb, &ticks, &environment);
    ok(!status, "TpAllocTimer failed with status %x\n", status);
    ok(timer != NULL, "expected timer != NULL\n");

    timer2 = NULL;
    status = pTpAllocTimer(&timer2, window_length_cb, &ticks2, &environment);
    ok(!status, "TpAllocTimer failed with status %x\n", status);
    ok(timer2 != NULL, "expected timer2 != NULL\n");

    /* Choose parameters so that timers are not merged */
    ticks = ticks2 = 0;
    NtQuerySystemTime( &when );
    when.QuadPart += (ULONGLONG)500 * 10000;
    pTpSetTimer(timer2, &when, 0, 0);
    Sleep(50);
    when.QuadPart -= (ULONGLONG)400 * 10000;
    pTpSetTimer(timer, &when, 0, 200);
    while (!ticks || !ticks2) Sleep(10);
    ok(ticks2 >= ticks + 150, "expected that timers are not merged\n");

    /* On Windows the timers also get merged in this case */
    ticks = ticks2 = 0;
    NtQuerySystemTime( &when );
    when.QuadPart += (ULONGLONG)100 * 10000;
    pTpSetTimer(timer, &when, 0, 450);
    Sleep(50);
    when.QuadPart += (ULONGLONG)400 * 10000;
    pTpSetTimer(timer2, &when, 0, 0);
    while (!ticks || !ticks2) Sleep(10);
    todo_wine
    ok(ticks2 >= ticks - 50 && ticks2 <= ticks + 50, "expected that timers are merged\n");

    /* Timers will be merged */
    ticks = ticks2 = 0;
    NtQuerySystemTime( &when );
    when.QuadPart += (ULONGLONG)500 * 10000;
    pTpSetTimer(timer2, &when, 0, 0);
    Sleep(50);
    when.QuadPart -= (ULONGLONG)400 * 10000;
    pTpSetTimer(timer, &when, 0, 450);
    while (!ticks || !ticks2) Sleep(10);
    ok(ticks2 >= ticks - 50 && ticks2 <= ticks + 50, "expected that timers are merged\n");

    /* Cleanup */
    pTpReleaseTimer(timer);
    pTpReleaseTimer(timer2);
    pTpReleasePool(pool);
}

static void CALLBACK wait_cb(TP_CALLBACK_INSTANCE *instance, void *userdata, TP_WAIT *wait, TP_WAIT_RESULT result)
{
    trace("Running wait callback\n");

    if (result == WAIT_OBJECT_0)
        InterlockedIncrement((LONG *)userdata);
    else if (result == WAIT_TIMEOUT)
        InterlockedExchangeAdd((LONG *)userdata, 0x10000);
    else
        ok(0, "unexpected result %u\n", result);
}

static void test_tp_wait(void)
{
    TP_CALLBACK_ENVIRON environment;
    HANDLE semaphore;
    TP_WAIT *wait, *wait2;
    TP_POOL *pool;
    NTSTATUS status;
    LONG userdata;
    LARGE_INTEGER when;
    DWORD ret;

    /* Allocate new threadpool */
    pool = NULL;
    status = pTpAllocPool(&pool, NULL);
    ok(!status, "TpAllocPool failed with status %x\n", status);
    ok(pool != NULL, "expected pool != NULL\n");

    /* Allocate new wait items */
    wait = NULL;
    memset(&environment, 0, sizeof(environment));
    environment.Version = 1;
    environment.Pool = pool;
    status = pTpAllocWait(&wait, wait_cb, &userdata, &environment);
    ok(!status, "TpAllocWait failed with status %x\n", status);
    ok(wait != NULL, "expected wait != NULL\n");

    wait2 = NULL;
    status = pTpAllocWait(&wait2, wait_cb, &userdata, &environment);
    ok(!status, "TpAllocWait failed with status %x\n", status);
    ok(wait != NULL, "expected wait != NULL\n");

    semaphore = CreateSemaphoreW(NULL, 0, 1, NULL);
    ok(semaphore != NULL, "failed to create semaphore\n");

    /* Infinite timeout, signal the semaphore immediately */
    userdata = 0;
    pTpSetWait(wait, semaphore, NULL);
    ReleaseSemaphore(semaphore, 1, NULL);
    Sleep(50);
    ok(userdata == 1, "expected userdata = 1, got %u\n", userdata);

    /* Relative timeout, no event */
    userdata = 0;
    when.QuadPart = (ULONGLONG)50 * -10000;
    pTpSetWait(wait, semaphore, &when);
    Sleep(100);
    pTpWaitForWait(wait, FALSE);
    ok(userdata == 0x10000, "expected userdata = 0x10000, got %u\n", userdata);
    ret = WaitForSingleObject(semaphore, 50);
    ok(ret == WAIT_TIMEOUT, "expected ret = WAIT_TIMEOUT, got %u\n", ret);

    /* Relative timeout, with event */
    userdata = 0;
    when.QuadPart = (ULONGLONG)500 * -10000;
    pTpSetWait(wait, semaphore, &when);
    pTpWaitForWait(wait, TRUE);
    Sleep(250);
    ReleaseSemaphore(semaphore, 1, NULL);
    Sleep(50);
    pTpWaitForWait(wait, FALSE);
    ok(userdata == 1, "expected userdata = 1, got %u\n", userdata);
    ret = WaitForSingleObject(semaphore, 50);
    ok(ret == WAIT_TIMEOUT, "expected ret = WAIT_TIMEOUT, got %u\n", ret);

    /* Absolute timeout, no event */
    userdata = 0;
    NtQuerySystemTime( &when );
    when.QuadPart += (ULONGLONG)50 * 10000;
    pTpSetWait(wait, semaphore, &when);
    Sleep(100);
    pTpWaitForWait(wait, FALSE);
    ok(userdata == 0x10000, "expected userdata = 0x10000, got %u\n", userdata);
    ret = WaitForSingleObject(semaphore, 50);
    ok(ret == WAIT_TIMEOUT, "expected ret = WAIT_TIMEOUT, got %u\n", ret);

    /* Absolute timeout, with event */
    userdata = 0;
    NtQuerySystemTime( &when );
    when.QuadPart += (ULONGLONG)500 * 10000;
    pTpSetWait(wait, semaphore, &when);
    pTpWaitForWait(wait, TRUE);
    Sleep(250);
    ReleaseSemaphore(semaphore, 1, NULL);
    Sleep(50);
    pTpWaitForWait(wait, FALSE);
    ok(userdata == 1, "expected userdata = 1, got %u\n", userdata);
    ret = WaitForSingleObject(semaphore, 50);
    ok(ret == WAIT_TIMEOUT, "expected ret = WAIT_TIMEOUT, got %u\n", ret);

    /* Trigger event immediately */
    userdata = 0;
    when.QuadPart = 0;
    pTpSetWait(wait, semaphore, &when);
    pTpWaitForWait(wait, FALSE);
    ok(userdata == 0x10000, "expected userdata = 0x10000, got %u\n", userdata);
    ret = WaitForSingleObject(semaphore, 50);
    ok(ret == WAIT_TIMEOUT, "expected ret = WAIT_TIMEOUT, got %u\n", ret);

    /* Cancel a pending wait */
    userdata = 0;
    when.QuadPart = (ULONGLONG)500 * -10000;
    pTpSetWait(wait, semaphore, &when);
    pTpWaitForWait(wait, TRUE);
    Sleep(250);
    pTpSetWait(wait, NULL, (void *)0xdeadbeef);
    Sleep(50);
    ReleaseSemaphore(semaphore, 1, NULL);
    Sleep(50);
    ok(userdata == 0, "expected userdata = 0, got %u\n", userdata);
    ret = WaitForSingleObject(semaphore, 1000);
    ok(ret == WAIT_OBJECT_0, "expected ret = WAIT_OBJECT_0, got %u\n", ret);

    /* Test with INVALID_HANDLE_VALUE */
    userdata = 0;
    when.QuadPart = 0;
    pTpSetWait(wait, INVALID_HANDLE_VALUE, &when);
    Sleep(50);
    ok(userdata == 0x10000, "expected userdata = 0x10000, got %u\n", userdata);

    /* Cancel a pending wait with INVALID_HANDLE_VALUE */
    userdata = 0;
    when.QuadPart = (ULONGLONG)500 * -10000;
    pTpSetWait(wait, semaphore, &when);
    pTpWaitForWait(wait, TRUE);
    Sleep(250);
    when.QuadPart = (ULONGLONG)100 * -10000;
    pTpSetWait(wait, INVALID_HANDLE_VALUE, &when);
    Sleep(250);
    ok(userdata == 0x10000, "expected userdata = 0x10000, got %u\n", userdata);

    /* Add two objects with the same semaphore */
    userdata = 0;
    pTpSetWait(wait, semaphore, NULL);
    pTpSetWait(wait2, semaphore, NULL);
    ok(userdata == 0, "expected userdata = 0, got %u\n", userdata);
    ReleaseSemaphore(semaphore, 1, NULL);
    Sleep(10);
    pTpWaitForWait(wait, FALSE);
    pTpWaitForWait(wait2, FALSE);
    ok(userdata == 1, "expected userdata = 1, got %u\n", userdata);
    ret = WaitForSingleObject(semaphore, 50);
    ok(ret == WAIT_TIMEOUT, "expected ret = WAIT_TIMEOUT, got %u\n", ret);

    CloseHandle(semaphore);
    semaphore = CreateSemaphoreW(NULL, 0, 2, NULL);
    ok(semaphore != NULL, "failed to create semaphore\n");

    /* Repeat test above, but with a semaphore of count 2 */
    userdata = 0;
    pTpSetWait(wait, semaphore, NULL);
    pTpSetWait(wait2, semaphore, NULL);
    ok(userdata == 0, "expected userdata = 0, got %u\n", userdata);
    ReleaseSemaphore(semaphore, 2, NULL);
    Sleep(10);
    pTpWaitForWait(wait, FALSE);
    pTpWaitForWait(wait2, FALSE);
    ok(userdata == 2, "expected userdata = 2, got %u\n", userdata);
    ret = WaitForSingleObject(semaphore, 50);
    ok(ret == WAIT_TIMEOUT, "expected ret = WAIT_TIMEOUT, got %u\n", ret);

    CloseHandle(semaphore);

    /* Cleanup */
    pTpReleaseWait(wait2);
    pTpReleaseWait(wait);
    pTpReleasePool(pool);
}

static LONG multi_wait_callbacks;
static DWORD multi_wait_result;

static void CALLBACK multi_wait_cb(TP_CALLBACK_INSTANCE *instance, void *userdata, TP_WAIT *wait, TP_WAIT_RESULT result)
{
    DWORD index = (DWORD)(DWORD_PTR)userdata;
    InterlockedIncrement(&multi_wait_callbacks);

    if (result == WAIT_OBJECT_0)
        multi_wait_result = index;
    else if (result == WAIT_TIMEOUT)
        multi_wait_result = 0x10000 | index;
    else
        ok(0, "unexpected result %u\n", result);
}

static void test_tp_multi_wait(void)
{
    TP_CALLBACK_ENVIRON environment;
    HANDLE semaphores[512];
    TP_WAIT *waits[512];
    TP_POOL *pool;
    NTSTATUS status;
    LARGE_INTEGER when;
    int i;

    /* Allocate new threadpool */
    pool = NULL;
    status = pTpAllocPool(&pool, NULL);
    ok(!status, "TpAllocPool failed with status %x\n", status);
    ok(pool != NULL, "expected pool != NULL\n");

    memset(&environment, 0, sizeof(environment));
    environment.Version = 1;
    environment.Pool = pool;

    /* Create semaphores, wait objects and enable them */
    for (i = 0; i < sizeof(semaphores)/sizeof(semaphores[0]); i++)
    {
        semaphores[i] = CreateSemaphoreW(NULL, 0, 1, NULL);
        ok(semaphores[i] != NULL, "failed to create semaphores[%d]\n", i);

        waits[i] = NULL;
        status = pTpAllocWait(&waits[i], multi_wait_cb, (void *)i, &environment);
        ok(!status, "TpAllocWait failed with status %x\n", status);
        ok(waits[i] != NULL, "expected waits[%d] != NULL\n", i);

        pTpSetWait(waits[i], semaphores[i], NULL);
    }

    /* Now test releasing the semaphores */
    for (i = 0; i < sizeof(semaphores)/sizeof(semaphores[0]); i++)
    {
        multi_wait_callbacks = 0;
        multi_wait_result = 0;

        ReleaseSemaphore(semaphores[i], 1, NULL);
        while (multi_wait_callbacks == 0) Sleep(10);
        ok(multi_wait_callbacks == 1, "expected multi_wait_callbacks = 1, got %u\n", multi_wait_callbacks);
        ok(multi_wait_result == i, "expected multi_wait_result = %u, got %u\n", multi_wait_result, i);

        pTpSetWait(waits[i], semaphores[i], NULL);
    }

    /* Now again in the reversed order */
    for (i = sizeof(semaphores)/sizeof(semaphores[0]) - 1; i >= 0; i--)
    {
        multi_wait_callbacks = 0;
        multi_wait_result = 0;

        ReleaseSemaphore(semaphores[i], 1, NULL);
        while (multi_wait_callbacks == 0) Sleep(10);
        ok(multi_wait_callbacks == 1, "expected multi_wait_callbacks = 1, got %u\n", multi_wait_callbacks);
        ok(multi_wait_result == i, "expected multi_wait_result = %u, got %u\n", multi_wait_result, i);

        pTpSetWait(waits[i], semaphores[i], NULL);
    }

    /* Now test with a timeout */
    multi_wait_callbacks = 0;
    multi_wait_result = 0;
    for (i = 0; i < sizeof(semaphores)/sizeof(semaphores[0]); i++)
    {
        when.QuadPart = 0;
        pTpSetWait(waits[i], semaphores[i], &when);
    }
    Sleep(50);
    ok(multi_wait_callbacks == sizeof(semaphores)/sizeof(semaphores[0]),
       "got wrong multi_wait_callbacks %u\n", multi_wait_callbacks);
    ok(multi_wait_result >> 16, "expected multi_wait_result >> 16 != 0\n");

    /* Add them all again, we want that the wait is pending while destroying it */
    for (i = 0; i < sizeof(semaphores)/sizeof(semaphores[0]); i++)
        pTpSetWait(waits[i], semaphores[i], NULL);

    /* Destroy the objects and semaphores */
    for (i = 0; i < sizeof(semaphores)/sizeof(semaphores[0]); i++)
    {
        pTpReleaseWait(waits[i]);
        NtClose(semaphores[i]);
    }

    pTpReleasePool(pool);
}

START_TEST(threadpool)
{
    if (!init_threadpool())
        return;

    test_tp_simple();
    test_tp_work();
    test_tp_work_scheduler();
    test_tp_instance();
    test_tp_group_cancel();
    test_tp_timer();
    test_tp_window_length();
    test_tp_wait();
    test_tp_multi_wait();

    /* FIXME: Make sure worker threads have terminated before. */
    Sleep(100);
}
