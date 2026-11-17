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

enum Mode { MODE_WAIT, MODE_DROP };

struct Params {
    Mode mode = MODE_WAIT;
    int gen = 2;
    int work = 2;
    int cap = 10;
    int interval = 20;
    int service = 5;
    long ops = kBaseOperations + kStudentId * 5;
    unsigned seed = 1000 + kStudentId * 97;
    int verbose = 0;
    int timeout = 120000;
    std::string csv, label;
};
static Params P;

static LARGE_INTEGER g_freq, g_t0;
static CRITICAL_SECTION g_logCs;

static double nowMs() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - g_t0.QuadPart) * 1000.0 / (double)g_freq.QuadPart;
}

static void logEv(const char* who, const char* ev, long req, const char* extra = "") {
    if (!P.verbose) return;
    EnterCriticalSection(&g_logCs);
    printf("[%06.0f ms] %s | TID=%lu | %s REQ=%ld %s\n", nowMs(), who, GetCurrentThreadId(), ev, req, extra);
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

static void usage() {
    fprintf(stderr,
            "Usage: lab4_queue.exe mode=wait|drop [gen=2] [work=2] [cap=10] [interval=20] [service=5]\n"
            "       [ops=%ld] [seed=%u] [timeout=120000] [verbose=0|1] [csv=file] [label=text]\n",
            P.ops, P.seed);
}

static bool parseArgs(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        size_t eq = a.find('=');
        if (eq == std::string::npos) { fprintf(stderr, "Bad argument: %s\n", argv[i]); return false; }
        std::string k = a.substr(0, eq), v = a.substr(eq + 1);
        if (k == "mode") {
            if (v == "wait") P.mode = MODE_WAIT;
            else if (v == "drop") P.mode = MODE_DROP;
            else { fprintf(stderr, "mode must be wait or drop\n"); return false; }
        }
        else if (k == "gen") P.gen = atoi(v.c_str());
        else if (k == "work") P.work = atoi(v.c_str());
        else if (k == "cap") P.cap = atoi(v.c_str());
        else if (k == "interval") P.interval = atoi(v.c_str());
        else if (k == "service") P.service = atoi(v.c_str());
        else if (k == "ops") P.ops = atol(v.c_str());
        else if (k == "seed") P.seed = (unsigned)strtoul(v.c_str(), nullptr, 10);
        else if (k == "verbose") P.verbose = atoi(v.c_str());
        else if (k == "timeout") P.timeout = atoi(v.c_str());
        else if (k == "csv") P.csv = v;
        else if (k == "label") P.label = v;
        else { fprintf(stderr, "Unknown parameter: %s\n", k.c_str()); return false; }
    }
    if (P.gen < 1 || P.gen > 64 || P.work < 1 || P.work > 64 || P.cap < 1 || P.interval < 0 ||
        P.service < 0 || P.ops < 1 || P.timeout < 1000) {
        fprintf(stderr, "Parameter out of range\n");
        return false;
    }
    return true;
}

struct Request {
    long id = 0;
    double tEnq = 0;
};

struct Queue {
    CRITICAL_SECTION cs;
    HANDLE semEmpty = nullptr;
    HANDLE semFull = nullptr;
    std::vector<Request> buf;
    int head = 0, tail = 0, count = 0;
};
static Queue Q;

static volatile LONG g_issued = 0;
static volatile LONG g_created = 0;
static volatile LONG g_processed = 0;
static volatile LONG g_dropped = 0;
static volatile LONG g_blocked = 0;
static volatile LONG g_violations = 0;
static volatile LONG g_abort = 0;
static volatile LONG64 g_blockedUs = 0;
static volatile LONG64 g_queueWaitUs = 0;
static int g_maxLen = 0;
static std::vector<LONG> g_fate;

static void violation(const char* what, long req) {
    InterlockedIncrement(&g_violations);
    fprintf(stderr, "INVARIANT VIOLATION: %s (REQ=%ld)\n", what, req);
}

static void finishRequest(long id) {
    if (InterlockedIncrement(&g_fate[id]) != 1) violation("request finished twice", id);
}

static int enqueue(long id, const char* who) {
    DWORD w = WaitForSingleObject(Q.semEmpty, 0);
    if (w == WAIT_FAILED) die("WaitForSingleObject(empty)");
    if (w == WAIT_TIMEOUT) {
        if (P.mode == MODE_DROP) return 0;
        InterlockedIncrement(&g_blocked);
        logEv(who, "WAITING", id);
        double t0 = nowMs();
        for (;;) {
            if (g_abort) return -1;
            w = WaitForSingleObject(Q.semEmpty, 100);
            if (w == WAIT_OBJECT_0) break;
            if (w != WAIT_TIMEOUT) die("WaitForSingleObject(empty)");
        }
        InterlockedExchangeAdd64(&g_blockedUs, (LONG64)((nowMs() - t0) * 1000.0));
    }
    EnterCriticalSection(&Q.cs);
    Request r;
    r.id = id;
    r.tEnq = nowMs();
    Q.buf[Q.head] = r;
    Q.head = (Q.head + 1) % P.cap;
    ++Q.count;
    if (Q.count > P.cap) violation("queue length exceeds capacity", id);
    if (Q.count > g_maxLen) g_maxLen = Q.count;
    int len = Q.count;
    LeaveCriticalSection(&Q.cs);
    if (!ReleaseSemaphore(Q.semFull, 1, nullptr)) die("ReleaseSemaphore(full)");
    char extra[32];
    snprintf(extra, sizeof extra, "LEN=%d", len);
    logEv(who, "ENQUEUED", id, extra);
    return 1;
}

static bool dequeue(Request& out) {
    EnterCriticalSection(&Q.cs);
    bool ok = Q.count > 0;
    if (ok) {
        out = Q.buf[Q.tail];
        Q.tail = (Q.tail + 1) % P.cap;
        --Q.count;
    }
    LeaveCriticalSection(&Q.cs);
    return ok;
}

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
        int r = enqueue(req, who);
        if (r <= 0) {
            InterlockedIncrement(&g_dropped);
            finishRequest(req);
            logEv(who, "DROPPED", req);
            if (r < 0) break;
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
    for (;;) {
        if (WaitForSingleObject(Q.semFull, INFINITE) != WAIT_OBJECT_0) die("WaitForSingleObject(full)");
        Request r;
        if (!dequeue(r)) break;
        if (!ReleaseSemaphore(Q.semEmpty, 1, nullptr)) die("ReleaseSemaphore(empty)");
        double tStart = nowMs();
        InterlockedExchangeAdd64(&g_queueWaitUs, (LONG64)((tStart - r.tEnq) * 1000.0));
        logEv(who, "PROCESSING", r.id);
        if (P.service > 0) Sleep(P.service);
        finishRequest(r.id);
        InterlockedIncrement(&g_processed);
        logEv(who, "DONE", r.id);
    }
    logEv(who, "FINISHED", 0);
    return 0;
}

static int run() {
    InitializeCriticalSection(&g_logCs);
    InitializeCriticalSection(&Q.cs);
    Q.buf.assign(P.cap, Request());
    Q.semEmpty = CreateSemaphoreA(nullptr, P.cap, P.cap, nullptr);
    if (!Q.semEmpty) die("CreateSemaphore(empty)");
    Q.semFull = CreateSemaphoreA(nullptr, 0, P.cap + P.work, nullptr);
    if (!Q.semFull) die("CreateSemaphore(full)");
    g_fate.assign((size_t)P.ops + 1, 0);

    QueryPerformanceCounter(&g_t0);
    std::vector<HANDLE> gens(P.gen), works(P.work);
    for (int i = 0; i < P.work; ++i) {
        works[i] = CreateThread(nullptr, 0, worker, (LPVOID)(INT_PTR)(i + 1), 0, nullptr);
        if (!works[i]) die("CreateThread(worker)");
    }
    for (int i = 0; i < P.gen; ++i) {
        gens[i] = CreateThread(nullptr, 0, generator, (LPVOID)(INT_PTR)(i + 1), 0, nullptr);
        if (!gens[i]) die("CreateThread(generator)");
    }

    bool aborted = false;
    DWORD w = WaitForMultipleObjects(P.gen, gens.data(), TRUE, P.timeout);
    if (w == WAIT_TIMEOUT) {
        aborted = true;
        g_abort = 1;
        if (WaitForMultipleObjects(P.gen, gens.data(), TRUE, 10000) != WAIT_OBJECT_0) die("generators did not stop", WAIT_TIMEOUT);
    } else if (w == WAIT_FAILED) die("WaitForMultipleObjects(generators)");
    double genMs = nowMs();

    if (!ReleaseSemaphore(Q.semFull, P.work, nullptr)) die("ReleaseSemaphore(full, shutdown)");
    w = WaitForMultipleObjects(P.work, works.data(), TRUE, P.timeout + 10000);
    if (w == WAIT_TIMEOUT) die("workers did not stop", WAIT_TIMEOUT);
    if (w == WAIT_FAILED) die("WaitForMultipleObjects(workers)");
    double totalMs = nowMs();

    if (Q.count != 0) violation("queue is not empty at the end", 0);
    if (g_created != g_processed + g_dropped) violation("created != processed + dropped", 0);
    if (!aborted) {
        for (long id = 1; id <= P.ops; ++id)
            if (g_fate[id] != 1) violation("request has no single outcome", id);
    }

    long created = g_created, processed = g_processed, dropped = g_dropped;
    double lossPct = created ? 100.0 * dropped / created : 0.0;
    double avgQueueWait = processed ? (double)g_queueWaitUs / 1000.0 / processed : 0.0;
    double avgBlocked = g_blocked ? (double)g_blockedUs / 1000.0 / g_blocked : 0.0;
    double throughput = totalMs > 0 ? processed * 1000.0 / totalMs : 0.0;
    double arrivalRate = genMs > 0 ? created * 1000.0 / genMs : 0.0;

    printf("=== RESULT ===\n");
    printf("mode=%s gen=%d work=%d cap=%d interval=%d service=%d ops=%ld seed=%u\n",
           P.mode == MODE_WAIT ? "wait" : "drop", P.gen, P.work, P.cap, P.interval, P.service, P.ops, P.seed);
    printf("created=%ld processed=%ld dropped=%ld loss=%.1f%%\n", created, processed, dropped, lossPct);
    printf("avg time in queue: %.2f ms, max queue length: %d\n", avgQueueWait, g_maxLen);
    printf("generator blocked: %ld times, avg %.2f ms\n", (long)g_blocked, avgBlocked);
    printf("generation time: %.1f ms, total time: %.1f ms\n", genMs, totalMs);
    printf("throughput: %.1f req/s, actual arrival rate: %.1f req/s\n", throughput, arrivalRate);
    printf("%s\n", aborted ? "ABORTED: time limit exceeded" : "completed");
    printf("INVARIANT VIOLATIONS: %ld\n", (long)g_violations);

    if (!P.csv.empty()) {
        FILE* f = fopen(P.csv.c_str(), "a");
        if (!f) { fprintf(stderr, "Cannot open %s\n", P.csv.c_str()); }
        else {
            fseek(f, 0, SEEK_END);
            if (ftell(f) == 0)
                fprintf(f, "label,mode,gen,work,cap,interval,service,ops,seed,created,processed,dropped,"
                           "loss_pct,avg_queue_wait_ms,max_queue_len,blocked,avg_blocked_ms,gen_ms,total_ms,"
                           "throughput,arrival_rate,violations,aborted\n");
            fprintf(f, "%s,%s,%d,%d,%d,%d,%d,%ld,%u,%ld,%ld,%ld,%.3f,%.3f,%d,%ld,%.3f,%.1f,%.1f,%.1f,%.1f,%ld,%d\n",
                    P.label.c_str(), P.mode == MODE_WAIT ? "wait" : "drop", P.gen, P.work, P.cap, P.interval,
                    P.service, P.ops, P.seed, created, processed, dropped, lossPct, avgQueueWait, g_maxLen,
                    (long)g_blocked, avgBlocked, genMs, totalMs, throughput, arrivalRate, (long)g_violations,
                    aborted ? 1 : 0);
            fclose(f);
        }
    }

    for (HANDLE h : gens) CloseHandle(h);
    for (HANDLE h : works) CloseHandle(h);
    CloseHandle(Q.semEmpty);
    CloseHandle(Q.semFull);
    DeleteCriticalSection(&Q.cs);
    DeleteCriticalSection(&g_logCs);
    return (g_violations == 0 && !aborted) ? 0 : 3;
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (!parseArgs(argc, argv)) { usage(); return 2; }
    QueryPerformanceFrequency(&g_freq);
    timeBeginPeriod(1);                                    // точность Sleep около 1 мс
    int rc = run();
    timeEndPeriod(1);
    return rc;
}
