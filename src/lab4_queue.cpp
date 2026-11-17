#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

static const int kStudentId = 70;
static const long kBaseOperations = 1000;

struct Params {
    int gen = 2;
    int work = 2;
    int cap = 10;
    int interval = 20;
    int service = 5;
    long ops = kBaseOperations + kStudentId * 5;
    unsigned seed = 1000 + kStudentId * 97;
    int verbose = 0;
    int timeout = 120000;
};
static Params P;

static LARGE_INTEGER g_freq, g_t0;
static CRITICAL_SECTION g_logCs;

static double nowMs() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - g_t0.QuadPart) * 1000.0 / (double)g_freq.QuadPart;
}

static void logEv(const char* who, const char* ev, long req) {
    if (!P.verbose) return;
    EnterCriticalSection(&g_logCs);
    printf("[%06.0f ms] %s | TID=%lu | %s REQ=%ld\n", nowMs(), who, GetCurrentThreadId(), ev, req);
    LeaveCriticalSection(&g_logCs);
}

[[noreturn]] static void die(const char* op, DWORD code = GetLastError()) {
    wchar_t* wmsg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, (LPWSTR)&wmsg, 0, nullptr);
    char text[512] = "(no description)";
    if (wmsg) {
        WideCharToMultiByte(CP_UTF8, 0, wmsg, -1, text, sizeof text, nullptr, nullptr);
        LocalFree(wmsg);
    }
    fprintf(stderr, "ERROR: %s failed. GetLastError=%lu: %s\n", op, code, text);
    ExitProcess(1);
}

static bool parseArgs(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        size_t eq = a.find('=');
        if (eq == std::string::npos) { fprintf(stderr, "Bad argument: %s\n", argv[i]); return false; }
        std::string k = a.substr(0, eq), v = a.substr(eq + 1);
        if (k == "gen") P.gen = atoi(v.c_str());
        else if (k == "work") P.work = atoi(v.c_str());
        else if (k == "cap") P.cap = atoi(v.c_str());
        else if (k == "interval") P.interval = atoi(v.c_str());
        else if (k == "service") P.service = atoi(v.c_str());
        else if (k == "ops") P.ops = atol(v.c_str());
        else if (k == "seed") P.seed = (unsigned)strtoul(v.c_str(), nullptr, 10);
        else if (k == "verbose") P.verbose = atoi(v.c_str());
        else if (k == "timeout") P.timeout = atoi(v.c_str());
        else { fprintf(stderr, "Unknown parameter: %s\n", k.c_str()); return false; }
    }
    if (P.gen < 1 || P.gen > 64 || P.work < 1 || P.work > 64 || P.cap < 1 || P.interval < 0 ||
        P.service < 0 || P.ops < 1 || P.timeout < 1000) {
        fprintf(stderr, "Parameter out of range\n");
        return false;
    }
    return true;
}

static std::vector<long> g_buf;
static volatile int g_head = 0, g_tail = 0, g_count = 0;

static volatile LONG g_issued = 0, g_created = 0, g_processed = 0, g_dropped = 0;
static volatile LONG g_genDone = 0, g_abort = 0;

static DWORD WINAPI generator(LPVOID p) {
    int id = (int)(INT_PTR)p;
    char who[24];
    snprintf(who, sizeof who, "GEN%d", id);
    std::mt19937 rng(P.seed + (unsigned)id);
    std::exponential_distribution<double> expo(P.interval > 0 ? 1.0 / P.interval : 1.0);
    for (;;) {
        if (g_abort) break;
        long req = InterlockedIncrement(&g_issued);
        if (req > P.ops) break;
        if (P.interval > 0) {
            DWORD d = (DWORD)(expo(rng) + 0.5);
            if (d > 0) Sleep(d);
        }
        InterlockedIncrement(&g_created);
        logEv(who, "CREATED", req);
        if (g_count < P.cap) {
            g_buf[g_head] = req;
            g_head = (g_head + 1) % P.cap;
            g_count = g_count + 1;
            logEv(who, "ENQUEUED", req);
        } else {
            InterlockedIncrement(&g_dropped);
            logEv(who, "DROPPED", req);
        }
    }
    logEv(who, "FINISHED", 0);
    return 0;
}

static DWORD WINAPI worker(LPVOID p) {
    int id = (int)(INT_PTR)p;
    char who[24];
    snprintf(who, sizeof who, "WORKER%d", id);
    logEv(who, "STARTED", 0);
    while (!g_abort) {
        if (g_count > 0) {
            long req = g_buf[g_tail];
            g_tail = (g_tail + 1) % P.cap;
            g_count = g_count - 1;
            logEv(who, "PROCESSING", req);
            if (P.service > 0) Sleep(P.service);
            InterlockedIncrement(&g_processed);
            logEv(who, "DONE", req);
        } else if (g_genDone) {
            break;
        } else {
            Sleep(1);
        }
    }
    logEv(who, "FINISHED", 0);
    return 0;
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (!parseArgs(argc, argv)) return 2;
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
    timeBeginPeriod(1);
    InitializeCriticalSection(&g_logCs);
    g_buf.assign(P.cap, 0);

    std::vector<HANDLE> gens(P.gen), works(P.work);
    for (int i = 0; i < P.work; ++i) {
        works[i] = CreateThread(nullptr, 0, worker, (LPVOID)(INT_PTR)(i + 1), 0, nullptr);
        if (!works[i]) die("CreateThread(worker)");
    }
    for (int i = 0; i < P.gen; ++i) {
        gens[i] = CreateThread(nullptr, 0, generator, (LPVOID)(INT_PTR)(i + 1), 0, nullptr);
        if (!gens[i]) die("CreateThread(generator)");
    }

    DWORD w = WaitForMultipleObjects(P.gen, gens.data(), TRUE, P.timeout);
    if (w == WAIT_TIMEOUT) { g_abort = 1; WaitForMultipleObjects(P.gen, gens.data(), TRUE, 10000); }
    else if (w == WAIT_FAILED) die("WaitForMultipleObjects(generators)");
    g_genDone = 1;
    w = WaitForMultipleObjects(P.work, works.data(), TRUE, P.timeout);
    if (w == WAIT_TIMEOUT) { g_abort = 1; WaitForMultipleObjects(P.work, works.data(), TRUE, 10000); }
    else if (w == WAIT_FAILED) die("WaitForMultipleObjects(workers)");
    double total = nowMs();

    printf("created=%ld processed=%ld dropped=%ld left_in_queue=%d total=%.1f ms\n",
           (long)g_created, (long)g_processed, (long)g_dropped, (int)g_count, total);
    if (g_created != g_processed + g_dropped || g_count != 0)
        printf("RACE DETECTED: created != processed + dropped (синхронизации пока нет)\n");

    for (HANDLE h : gens) CloseHandle(h);
    for (HANDLE h : works) CloseHandle(h);
    DeleteCriticalSection(&g_logCs);
    timeEndPeriod(1);
    return 0;
}
