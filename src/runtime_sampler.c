/* BB_SAMPLE_PROFILE=1 (or =file): sampling CPU profiler for finding where the port spends its time.
 * A background thread stops every other thread of the process about once per millisecond, reads
 * its instruction pointer and counts (thread, address) pairs. Every 10 s the counts, the thread
 * names and the loaded modules go to wbp_samples.txt (or the given file), replaced atomically,
 * so a killed game still leaves the last snapshot. tools/symbolize_samples (scratch) maps the
 * addresses to functions through the DWARF information of wbp-game.exe.
 *
 * Between SuspendThread and ResumeThread the sampler calls nothing that can take a lock (no
 * malloc, no stdio): the stopped thread may hold it. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#include <tlhelp32.h>

#define SAMPLE_SLOTS (1u << 20) /* power of two */
#define MAX_THREADS 512

typedef struct {
    uint64_t address;
    uint32_t tid;
    uint32_t count;
} Sample;

typedef struct {
    DWORD tid;
    HANDLE handle;
    int seen;
} SampledThread;

static Sample *samples;
static uint64_t dropped;
static SampledThread threads[MAX_THREADS];
static unsigned thread_count;
static char output[MAX_PATH];

typedef HRESULT(WINAPI *GetThreadDescriptionFn)(HANDLE, PWSTR *);

static void count(uint32_t tid, uint64_t address) {
    uint64_t hash = (address ^ ((uint64_t)tid << 40)) * 0x9E3779B97F4A7C15ull;
    for (uint32_t probe = 0; probe < 64; ++probe) {
        Sample *s = &samples[((uint32_t)(hash >> 44) + probe) & (SAMPLE_SLOTS - 1)];
        if (s->count && (s->address != address || s->tid != tid)) continue;
        s->address = address;
        s->tid = tid;
        ++s->count;
        return;
    }
    ++dropped;
}

/* Adds new threads of the process and closes the handles of finished ones. */
static void refresh_threads(DWORD self) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    for (unsigned i = 0; i < thread_count; ++i) threads[i].seen = 0;
    THREADENTRY32 entry = {.dwSize = sizeof(entry)};
    const DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &entry); ok; ok = Thread32Next(snap, &entry)) {
        if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) continue;
        unsigned i = 0;
        while (i < thread_count && threads[i].tid != entry.th32ThreadID) ++i;
        if (i == thread_count) {
            if (thread_count == MAX_THREADS) continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
                                  FALSE, entry.th32ThreadID);
            if (!h) continue;
            threads[thread_count++] = (SampledThread){entry.th32ThreadID, h, 0};
        }
        threads[i].seen = 1;
    }
    CloseHandle(snap);
    for (unsigned i = 0; i < thread_count;) {
        if (threads[i].seen) {
            ++i;
            continue;
        }
        CloseHandle(threads[i].handle);
        threads[i] = threads[--thread_count];
    }
}

static void dump(uint64_t rounds) {
    char temp[MAX_PATH + 8];
    snprintf(temp, sizeof(temp), "%s.tmp", output);
    FILE *f = fopen(temp, "w");
    if (!f) return;
    fprintf(f, "BBSAMPLES1 rounds=%llu dropped=%llu\n", (unsigned long long)rounds, (unsigned long long)dropped);
    static GetThreadDescriptionFn describe;
    if (!describe)
        describe = (GetThreadDescriptionFn)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                                                   "GetThreadDescription");
    for (unsigned i = 0; i < thread_count; ++i) {
        char name[128] = "";
        PWSTR wide = NULL;
        if (describe && SUCCEEDED(describe(threads[i].handle, &wide)) && wide) {
            WideCharToMultiByte(CP_UTF8, 0, wide, -1, name, sizeof(name), NULL, NULL);
            LocalFree(wide);
        }
        fprintf(f, "T %lu %s\n", (unsigned long)threads[i].tid, name);
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32 module = {.dwSize = sizeof(module)};
        for (BOOL ok = Module32First(snap, &module); ok; ok = Module32Next(snap, &module))
            fprintf(f, "M %llx %lx %s\n", (unsigned long long)(uintptr_t)module.modBaseAddr,
                    (unsigned long)module.modBaseSize, module.szExePath);
        CloseHandle(snap);
    }
    for (uint32_t i = 0; i < SAMPLE_SLOTS; ++i)
        if (samples[i].count)
            fprintf(f, "S %lu %llx %lu\n", (unsigned long)samples[i].tid,
                    (unsigned long long)samples[i].address, (unsigned long)samples[i].count);
    fclose(f);
    MoveFileExA(temp, output, MOVEFILE_REPLACE_EXISTING);
}

static DWORD WINAPI sampler(LPVOID unused) {
    (void)unused;
    const DWORD self = GetCurrentThreadId();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    timeBeginPeriod(1);
    uint64_t rounds = 0;
    ULONGLONG last_refresh = 0, last_dump = GetTickCount64();
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        if (now - last_refresh >= 500) {
            refresh_threads(self);
            last_refresh = now;
        }
        if (now - last_dump >= 10000) {
            dump(rounds);
            last_dump = now;
        }
        for (unsigned i = 0; i < thread_count; ++i) {
            if (SuspendThread(threads[i].handle) == (DWORD)-1) continue;
            CONTEXT context = {.ContextFlags = CONTEXT_CONTROL};
            const BOOL ok = GetThreadContext(threads[i].handle, &context);
            ResumeThread(threads[i].handle);
            if (ok) count(threads[i].tid, context.Rip);
        }
        ++rounds;
        Sleep(1);
    }
    return 0;
}

void runtime_sampler_start(void) {
    const char *env = getenv("BB_SAMPLE_PROFILE");
    if (!env || !env[0] || !strcmp(env, "0")) return;
    const char *path = strcmp(env, "1") ? env : "wbp_samples.txt";
    if (!GetFullPathNameA(path, sizeof(output), output, NULL)) snprintf(output, sizeof(output), "%s", path);
    samples = calloc(SAMPLE_SLOTS, sizeof(Sample));
    if (!samples) return;
    HANDLE thread = CreateThread(NULL, 0, sampler, NULL, 0, NULL);
    if (!thread) return;
    CloseHandle(thread);
    printf("Profiler: sampling every thread about once per ms into %s (every 10 s)\n", output);
}
#else
void runtime_sampler_start(void) {}
#endif
