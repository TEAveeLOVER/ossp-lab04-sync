#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <iostream>
#include <vector>
#include <queue>
#include <string>
#include <cstdio>
#include <ctime>
#include <cmath>

// ===================== ИНДИВИДУАЛЬНЫЕ ПАРАМЕТРЫ =====================
const int VARIANT_ID = 21;
const int STUDENT_ID = 90;                    // последние 3 цифры (090 → 90)

const int BASE_OPERATIONS = 200;
const int OPERATIONS = BASE_OPERATIONS + STUDENT_ID * 5;  // 200 + 450 = 650
const unsigned SEED = 1000 + STUDENT_ID * 97;            // 1000 + 8730 = 9730

const int NUM_WORKERS = 3;
const int QUEUE_CAP = 20;          // ёмкость очереди
const int PROCESS_MS_MIN = 5;           // длительность обработки (мин)
const int PROCESS_MS_MAX = 25;          // длительность обработки (макс)
const int PRODUCE_DELAY = 2;           // пауза между генерацией заявок (мс)
// ====================================================================

struct Request {
    int    id;
    DWORD  arrivalMs;    // время появления (от старта)
    DWORD  deadlineMs;   // arrival + maxWait
};

struct Stats {
    LONG completed;
    LONG expired;
    LONG produced;
    LONG maxQueueLen;
    // сумма времён ожидания успешных (для среднего)
    LONGLONG sumWaitMs;
};

// ----- глобальное состояние -----
CRITICAL_SECTION g_cs;
HANDLE           g_semFree;      // сколько свободных мест в очереди
HANDLE           g_semItems;     // сколько заявок в очереди
HANDLE           g_stopEvent;

std::queue<Request> g_queue;
Stats               g_stats = {};
DWORD               g_startTick = 0;
int                 g_maxWaitMs = 50;   // задаётся перед запуском эксперимента
volatile LONG       g_runningWorkers = 0;

// ----- утилиты -----
DWORD Elapsed() {
    return GetTickCount() - g_startTick;
}

void Log(const char* who, const char* event, int id = -1, const char* extra = "") {
    EnterCriticalSection(&g_cs);
    printf("[%06lu ms] %-10s %-12s", Elapsed(), who, event);
    if (id >= 0) printf(" id=%d", id);
    if (extra && extra[0]) printf(" %s", extra);
    printf("\n");
    LeaveCriticalSection(&g_cs);
}

int RandRange(int a, int b) {
    return a + (rand() % (b - a + 1));
}

// ----- Producer -----
DWORD WINAPI ProducerThread(LPVOID) {
    for (int i = 0; i < OPERATIONS; ++i) {
        if (WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0)
            break;

        Request r;
        r.id = i + 1;
        r.arrivalMs = Elapsed();
        r.deadlineMs = r.arrivalMs + (DWORD)g_maxWaitMs;

        // ждём свободное место в очереди
        DWORD w = WaitForSingleObject(g_semFree, 3000);
        if (w != WAIT_OBJECT_0) {
            Log("PRODUCER", "TIMEOUT_ENQUEUE", r.id);
            continue;
        }

        EnterCriticalSection(&g_cs);
        g_queue.push(r);
        size_t qlen = g_queue.size();
        if ((LONG)qlen > g_stats.maxQueueLen)
            g_stats.maxQueueLen = (LONG)qlen;
        InterlockedIncrement(&g_stats.produced);
        LeaveCriticalSection(&g_cs);

        ReleaseSemaphore(g_semItems, 1, nullptr);
        Log("PRODUCER", "ENQUEUE", r.id);

        Sleep(PRODUCE_DELAY);
    }
    Log("PRODUCER", "FINISHED");
    return 0;
}

// ----- Worker -----
DWORD WINAPI WorkerThread(LPVOID param) {
    int wid = (int)(INT_PTR)param;
    char name[32];
    sprintf(name, "WORKER_%d", wid);
    InterlockedIncrement(&g_runningWorkers);

    Log(name, "STARTED");

    for (;;) {
        // ждём заявку или сигнал остановки
        HANDLE waits[2] = { g_semItems, g_stopEvent };
        DWORD w = WaitForMultipleObjects(2, waits, FALSE, 2000);

        if (w == WAIT_OBJECT_0 + 1)  // stop
            break;
        if (w == WAIT_TIMEOUT) {
            // если producer уже всё положил и очередь пуста — можно выходить
            EnterCriticalSection(&g_cs);
            bool empty = g_queue.empty();
            bool done = (g_stats.produced >= OPERATIONS);
            LeaveCriticalSection(&g_cs);
            if (empty && done) break;
            continue;
        }
        if (w != WAIT_OBJECT_0)
            break;

        // взять заявку
        Request r;
        bool got = false;
        EnterCriticalSection(&g_cs);
        if (!g_queue.empty()) {
            r = g_queue.front();
            g_queue.pop();
            got = true;
        }
        LeaveCriticalSection(&g_cs);

        if (!got) {
            ReleaseSemaphore(g_semFree, 1, nullptr);
            continue;
        }

        ReleaseSemaphore(g_semFree, 1, nullptr);  // освободили слот очереди

        DWORD now = Elapsed();
        Log(name, "DEQUEUE", r.id);

        // ===== проверка срока =====
        if (now > r.deadlineMs) {
            InterlockedIncrement(&g_stats.expired);
            char buf[64];
            sprintf(buf, "deadline=%lu now=%lu", r.deadlineMs, now);
            Log(name, "EXPIRED", r.id, buf);
            continue;
        }

        // обработка
        Log(name, "PROCESS", r.id);
        int work = RandRange(PROCESS_MS_MIN, PROCESS_MS_MAX);
        Sleep(work);  // имитация работы (разрешено методичкой)

        // повторная проверка: могла просрочиться во время работы
        now = Elapsed();
        if (now > r.deadlineMs) {
            InterlockedIncrement(&g_stats.expired);
            Log(name, "EXPIRED_LATE", r.id);
        }
        else {
            InterlockedIncrement(&g_stats.completed);
            DWORD waitTime = r.arrivalMs < now ? (now - r.arrivalMs) : 0;
            EnterCriticalSection(&g_cs);
            g_stats.sumWaitMs += waitTime;
            LeaveCriticalSection(&g_cs);
            Log(name, "COMPLETED", r.id);
        }
    }

    Log(name, "FINISHED");
    InterlockedDecrement(&g_runningWorkers);
    return 0;
}

// ----- один эксперимент -----
void RunExperiment(int maxWaitMs) {
    g_maxWaitMs = maxWaitMs;
    g_stats = {};
    while (!g_queue.empty()) g_queue.pop();

    printf("\n========== EXPERIMENT: maxWait = %d ms ==========\n", maxWaitMs);
    printf("VARIANT=%d  STUDENT_ID=%d  OPERATIONS=%d  WORKERS=%d  QUEUE_CAP=%d  SEED=%u\n",
        VARIANT_ID, STUDENT_ID, OPERATIONS, NUM_WORKERS, QUEUE_CAP, SEED);

    srand(SEED);
    g_startTick = GetTickCount();

    g_semFree = CreateSemaphoreA(nullptr, QUEUE_CAP, QUEUE_CAP, nullptr);
    g_semItems = CreateSemaphoreA(nullptr, 0, QUEUE_CAP, nullptr);
    g_stopEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);  // manual-reset

    if (!g_semFree || !g_semItems || !g_stopEvent) {
        printf("ERROR: cannot create sync objects\n");
        return;
    }

    HANDLE hProd = CreateThread(nullptr, 0, ProducerThread, nullptr, 0, nullptr);
    HANDLE hWorkers[NUM_WORKERS];
    for (int i = 0; i < NUM_WORKERS; ++i)
        hWorkers[i] = CreateThread(nullptr, 0, WorkerThread, (LPVOID)(INT_PTR)i, 0, nullptr);

    // ждём producer
    WaitForSingleObject(hProd, INFINITE);
    CloseHandle(hProd);

    // даём worker'ам добрать очередь
    DWORD deadline = GetTickCount() + 15000;
    while (g_runningWorkers > 0 && GetTickCount() < deadline) {
        EnterCriticalSection(&g_cs);
        bool empty = g_queue.empty();
        LeaveCriticalSection(&g_cs);
        if (empty && g_stats.produced >= OPERATIONS) {
            // ещё чуть-чуть
            Sleep(100);
            if (g_runningWorkers > 0)
                SetEvent(g_stopEvent);
            break;
        }
        Sleep(50);
    }
    SetEvent(g_stopEvent);
    WaitForMultipleObjects(NUM_WORKERS, hWorkers, TRUE, 5000);
    for (int i = 0; i < NUM_WORKERS; ++i)
        CloseHandle(hWorkers[i]);

    CloseHandle(g_semFree);
    CloseHandle(g_semItems);
    CloseHandle(g_stopEvent);

    DWORD totalMs = Elapsed();
    double avgWait = (g_stats.completed > 0)
        ? (double)g_stats.sumWaitMs / g_stats.completed
        : 0.0;

    printf("\n----- RESULTS maxWait=%d ms -----\n", maxWaitMs);
    printf("Produced:   %ld\n", g_stats.produced);
    printf("Completed:  %ld\n", g_stats.completed);
    printf("Expired:    %ld\n", g_stats.expired);
    printf("Max queue:  %ld\n", g_stats.maxQueueLen);
    printf("Total time: %lu ms\n", totalMs);
    printf("Avg wait (completed): %.2f ms\n", avgWait);
    printf("Invariant check: completed+expired <= produced : %s\n",
        (g_stats.completed + g_stats.expired <= g_stats.produced) ? "OK" : "FAIL");
    printf("========================================\n");
}

int main() {
    SetConsoleOutputCP(1251);
    SetConsoleCP(1251);

    InitializeCriticalSection(&g_cs);

    printf("LAB4  VARIANT %d  |  STUDENT_ID=%d  |  Ограничение времени ожидания\n",
        VARIANT_ID, STUDENT_ID);
    printf("Mechanisms: Semaphore + Critical Section\n");
    printf("OPERATIONS=%d  SEED=%u\n\n", OPERATIONS, SEED);

    // Эксперимент 1: короткий срок
    RunExperiment(20);

    Sleep(500);

    // Эксперимент 2: длинный срок
    RunExperiment(100);

    DeleteCriticalSection(&g_cs);

    printf("\nDone. Press Enter...\n");
    getchar();
    return 0;
}