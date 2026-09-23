/*
 * ee-transition-stress: reproduce Rosetta 2 missing the 32->64-bit mode switch
 * in Wine's wow64cpu.dll, without the game.
 *
 * Every syscall and unix call a 32-bit Windows program makes under Wine's new
 * WoW64 goes through a `jmp far [ptr]` from 32-bit code into a 64-bit landing
 * pad (syscall_32to64 / unix_call_32to64).  Occasionally Rosetta executes that
 * landing pad in 32-bit mode, and the first RIP-relative load in it faults at
 * its own displacement (00004ECD / 00004DC9).  This program generates those
 * transitions from many threads, optionally alongside the things suspected of
 * provoking the failure, so a fix can be measured in seconds instead of by
 * relaunching the game dozens of times.
 *
 * Usage: ee-transition-stress.exe <seconds> <threads> <flags>
 *   s  syscall threads (NtWaitForSingleObject / NtQueryVirtualMemory / NtYieldExecution)
 *   u  unix-call threads (ws2_32 gethostname -> __wine_unix_call)
 *   l  one thread loading and unloading DLLs (code mapped, run, unmapped)
 *   x  one thread writing, running and freeing fresh executable code
 *   g  one thread suspending workers and reading their context (signals)
 *   X  one thread writing fresh code into never-used pages and running it (new code, nothing freed)
 *   p  one thread flipping a code page RW <-> RX between runs (protection changes)
 *   m  one thread rewriting and re-running code in the same RWX page (self-modifying code)
 *   U  one thread running code once, then unmapping it and mapping a fresh page (unmap only)
 * Prints a single summary line; a Wine page fault means the bug reproduced.
 */
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_THREADS 64

static volatile LONG stop_flag;
static volatile LONG counts[MAX_THREADS];
static HANDLE workers[MAX_THREADS];
static int nworkers;

static DWORD WINAPI syscall_thread(void *arg)
{
    int idx = (int)(INT_PTR)arg;
    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    MEMORY_BASIC_INFORMATION mbi;
    LONG n = 0;

    while (!stop_flag)
    {
        WaitForSingleObject(ev, 0);
        VirtualQuery(&mbi, &mbi, sizeof(mbi));
        SwitchToThread();
        n += 3;
        if (!(n & 0x3ff)) counts[idx] = n;
    }
    counts[idx] = n;
    return 0;
}

static DWORD WINAPI unixcall_thread(void *arg)
{
    int idx = (int)(INT_PTR)arg;
    char name[256];
    LONG n = 0;

    while (!stop_flag)
    {
        gethostname(name, sizeof(name));
        n++;
        if (!(n & 0x3ff)) counts[idx] = n;
    }
    counts[idx] = n;
    return 0;
}

static DWORD WINAPI loadlib_thread(void *arg)
{
    static const char *dlls[] = { "d3d9.dll", "dsound.dll", "ddraw.dll", "dinput.dll",
                                  "winmm.dll", "msacm32.dll", "d3dim.dll", "dplayx.dll" };
    int idx = (int)(INT_PTR)arg;
    LONG n = 0;

    while (!stop_flag)
    {
        HMODULE h = LoadLibraryA(dlls[n % (sizeof(dlls) / sizeof(dlls[0]))]);
        if (h) FreeLibrary(h);
        n++;
        counts[idx] = n;
    }
    return 0;
}

static DWORD WINAPI jit_thread(void *arg)
{
    int idx = (int)(INT_PTR)arg;
    LONG n = 0;

    while (!stop_flag)
    {
        unsigned char *code = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!code) break;
        /* mov eax, n ; ret */
        code[0] = 0xb8;
        memcpy(code + 1, &n, 4);
        code[5] = 0xc3;
        FlushInstructionCache(GetCurrentProcess(), code, 6);
        if (((LONG (*)(void))code)() != n) { printf("jit mismatch\n"); fflush(stdout); }
        VirtualFree(code, 0, MEM_RELEASE);
        n++;
        if (!(n & 0xff)) counts[idx] = n;
    }
    counts[idx] = n;
    return 0;
}

static DWORD WINAPI getctx_thread(void *arg)
{
    int idx = (int)(INT_PTR)arg;
    LONG n = 0;
    CONTEXT ctx;

    while (!stop_flag)
    {
        HANDLE t = workers[n % nworkers];
        if (SuspendThread(t) != (DWORD)-1)
        {
            ctx.ContextFlags = CONTEXT_FULL;
            GetThreadContext(t, &ctx);
            ResumeThread(t);
        }
        n++;
        if (!(n & 0xff)) counts[idx] = n;
        Sleep(0);
    }
    counts[idx] = n;
    return 0;
}

/* Fresh code in pages that are never written again or freed: new translations only. */
static DWORD WINAPI freshcode_thread(void *arg)
{
    int idx = (int)(INT_PTR)arg;
    const SIZE_T pages = 16384;
    unsigned char *base = VirtualAlloc(NULL, pages * 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    LONG n = 0;

    while (base && !stop_flag)
    {
        unsigned char *code = base + (SIZE_T)(n % pages) * 4096;
        if ((SIZE_T)n >= pages) { Sleep(1); continue; }   /* out of fresh pages: idle */
        code[0] = 0xb8; memcpy(code + 1, &n, 4); code[5] = 0xc3;
        FlushInstructionCache(GetCurrentProcess(), code, 6);
        ((LONG (*)(void))code)();
        n++;
        if (!(n & 0xff)) counts[idx] = n;
    }
    counts[idx] = n;
    return 0;
}

/* Protection flips on a page whose code has been run. */
static DWORD WINAPI protect_thread(void *arg)
{
    int idx = (int)(INT_PTR)arg;
    unsigned char *code = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    DWORD old;
    LONG n = 0;

    code[0] = 0xb8; memcpy(code + 1, &n, 4); code[5] = 0xc3;
    while (!stop_flag)
    {
        ((LONG (*)(void))code)();
        VirtualProtect(code, 4096, PAGE_READWRITE, &old);
        VirtualProtect(code, 4096, PAGE_EXECUTE_READ, &old);
        n++;
        if (!(n & 0xff)) counts[idx] = n;
    }
    counts[idx] = n;
    return 0;
}

/* Self-modifying code: rewrite the immediate in place and run it again. */
static DWORD WINAPI smc_thread(void *arg)
{
    int idx = (int)(INT_PTR)arg;
    unsigned char *code = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    LONG n = 0;

    code[0] = 0xb8; code[5] = 0xc3;
    while (!stop_flag)
    {
        memcpy(code + 1, &n, 4);
        FlushInstructionCache(GetCurrentProcess(), code, 6);
        ((LONG (*)(void))code)();
        n++;
        if (!(n & 0xff)) counts[idx] = n;
    }
    counts[idx] = n;
    return 0;
}

/* Run code once, then unmap it (the only change is the unmap). */
static DWORD WINAPI unmap_thread(void *arg)
{
    int idx = (int)(INT_PTR)arg;
    LONG n = 0;

    while (!stop_flag)
    {
        unsigned char *code = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!code) break;
        code[0] = 0xb8; memcpy(code + 1, &n, 4); code[5] = 0xc3;
        ((LONG (*)(void))code)();
        VirtualFree(code, 0, MEM_RELEASE);
        n++;
        if (!(n & 0xff)) counts[idx] = n;
        Sleep(1);
    }
    counts[idx] = n;
    return 0;
}

int main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 20;
    int nthreads = argc > 2 ? atoi(argv[2]) : 8;
    const char *flags = argc > 3 ? argv[3] : "su";
    HANDLE extra[8];
    int nextra = 0, slot = 0, i;
    WSADATA wsa;
    LONG total = 0;

    if (nthreads < 1) nthreads = 1;
    if (nthreads > MAX_THREADS - 8) nthreads = MAX_THREADS - 8;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    for (i = 0; i < nthreads; i++)
    {
        int want_unix = strchr(flags, 'u') && (!strchr(flags, 's') || (i & 1));
        LPTHREAD_START_ROUTINE fn = want_unix ? unixcall_thread : syscall_thread;
        if (!strchr(flags, 's') && !strchr(flags, 'u')) break;
        workers[nworkers++] = CreateThread(NULL, 0, fn, (void *)(INT_PTR)slot++, 0, NULL);
    }
    if (strchr(flags, 'l')) extra[nextra++] = CreateThread(NULL, 0, loadlib_thread, (void *)(INT_PTR)slot++, 0, NULL);
    if (strchr(flags, 'x')) extra[nextra++] = CreateThread(NULL, 0, jit_thread, (void *)(INT_PTR)slot++, 0, NULL);
    if (strchr(flags, 'X')) extra[nextra++] = CreateThread(NULL, 0, freshcode_thread, (void *)(INT_PTR)slot++, 0, NULL);
    if (strchr(flags, 'p')) extra[nextra++] = CreateThread(NULL, 0, protect_thread, (void *)(INT_PTR)slot++, 0, NULL);
    if (strchr(flags, 'm')) extra[nextra++] = CreateThread(NULL, 0, smc_thread, (void *)(INT_PTR)slot++, 0, NULL);
    if (strchr(flags, 'U')) extra[nextra++] = CreateThread(NULL, 0, unmap_thread, (void *)(INT_PTR)slot++, 0, NULL);
    if (strchr(flags, 'g') && nworkers)
        extra[nextra++] = CreateThread(NULL, 0, getctx_thread, (void *)(INT_PTR)slot++, 0, NULL);

    Sleep(seconds * 1000);
    InterlockedExchange(&stop_flag, 1);
    if (nworkers) WaitForMultipleObjects(nworkers, workers, TRUE, 10000);
    if (nextra) WaitForMultipleObjects(nextra, extra, TRUE, 10000);

    for (i = 0; i < slot; i++) total += counts[i];
    {
        /* Recovery counters kept by the patched wow64cpu.dll (.data RVA 0x2ff0),
         * which Wine maps at 0x7bf20000.  Absent/unpatched -> "n/a". */
        DWORD rec[3];
        SIZE_T got = 0;
        if (ReadProcessMemory(GetCurrentProcess(), (void *)0x7bf22ff0, rec, sizeof(rec), &got) && got == sizeof(rec))
            printf("ee-transition-stress: wow64cpu recoveries syscall=%lu unixcall=%lu return=%lu\n",
                   rec[0], rec[1], rec[2]);
        else
            printf("ee-transition-stress: wow64cpu recoveries n/a\n");
    }
    printf("ee-transition-stress: survived %ds threads=%d flags=%s ops=%ld\n",
           seconds, nworkers, flags, (long)total);
    fflush(stdout);
    return 0;
}
