#include <stdarg.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <string.h>
#include <linux/mman.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <pthread.h>

static inline void pause_getchar(void) {
    printf("Press anykey to continue.\n");
    getchar();
}

#define THREADS 65536
#define SIZE_IN_MB 32
#define PAGE_SIZE 4096
#define MB 1024UL * 1024UL
#define atomic_type unsigned long

static void* P_work(void *p) {
    char *buf = p;
    while (true) {
        for (unsigned long i = 0; i < SIZE_IN_MB; i += 1) {
            struct timespec remaining, request = { 0, 1000 };
            madvise(buf + i * MB, MB, MADV_PAGEOUT);
            nanosleep(&request, &remaining);
        }
        madvise(p, SIZE_IN_MB * MB, MADV_PAGEOUT);
    }
    return NULL;
}

static void* C1_work(void *p) {
    for (unsigned long i = 0; i < (SIZE_IN_MB * MB) / sizeof(atomic_type); i += PAGE_SIZE / sizeof(atomic_type)) {
        struct timespec remaining, request = { 0, 1 };
        /* A slow delay so the worker won't finish in a blink and have the change to race with others */
        nanosleep(&request, &remaining);
        atomic_type *buf = p;
        __atomic_fetch_add(&buf[i], 1, __ATOMIC_SEQ_CST);
    }
    return NULL;
}

static void* C2_work(void *p) {
    for (unsigned long i = 0; i < (SIZE_IN_MB * MB) / sizeof(atomic_type); i += PAGE_SIZE / sizeof(atomic_type)) {
        struct timespec remaining, request = { 0, 1 };
        /* A slow delay so the worker won't finish in a blink and have the change to race with others */
        nanosleep(&request, &remaining);
        atomic_type *buf = p;
        __atomic_fetch_add(&buf[(((SIZE_IN_MB * MB) - PAGE_SIZE) / sizeof(atomic_type)) - i], 1, __ATOMIC_SEQ_CST);
    }
    return NULL;
}

int main(int argc, char **argv) {
    int round = 0;
    pthread_t ptid;
    struct timeval start;

    // pause_getchar();

    printf("Polulating %dMB of memory region... \n", SIZE_IN_MB);
    void *p = mmap(NULL, SIZE_IN_MB * MB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memset(p, 0, SIZE_IN_MB * MB);

    if (!pthread_create(&ptid, NULL, P_work, p))
        printf("Keep swapping out... \n");
    else
        exit(1);

    while (true) {
        int worker_num = 0;
        pthread_t tid[THREADS] = { 0 };

        printf("Starting round %d...\n", round);
        memset(p, 0, SIZE_IN_MB * MB);

        for (unsigned long i = 0; i < (SIZE_IN_MB * MB) / sizeof(atomic_type); i += PAGE_SIZE / sizeof(atomic_type)) {
            atomic_type *buf = p;
            buf[i + 1] = i;
        }

        __sync_synchronize();

        printf("Spawning %d workers...\n", THREADS);
        for (int i = 0; i < THREADS / 2; i++) {
            if (!pthread_create(&tid[i], NULL, C1_work, p))
                worker_num += 1;
            else
                tid[i] = 0;
            if (!pthread_create(&tid[THREADS - i], NULL, C2_work, p))
                worker_num += 1;
            else
                tid[i] = 0;
        }

        printf("%d workers spawned, wait for done...\n", worker_num);
        for (int i = 0; i < THREADS; i++) {
            if (tid[i] != 0)
                pthread_join(tid[i], NULL);
        }

        int total_loss = 0, total_misload = 0;
        for (unsigned long i = 0; i < SIZE_IN_MB * MB / sizeof(atomic_type); i += PAGE_SIZE / sizeof(atomic_type)) {
            atomic_type *buf = p, val;
            val = __atomic_load_n(&buf[i], __ATOMIC_SEQ_CST);
            if (val != worker_num) {
                printf("Round %d: Error on 0x%lx, expected %ld, got %ld, %ld data loss!\n", round, i, worker_num, val, worker_num - val);
                total_loss += worker_num - val;
            }

            if (buf[i + 1] != i) {
                printf("Round %d: Misload on 0x%lx, expected %ld, got %ld!\n", round, i, i, buf[i + 1]);
                total_misload += 1;
            }
        }

        if (total_loss) {
            printf("Round %d Failed, %d data loss, %d misload!\n", round++, total_loss, total_misload);
            // pthread_exit(NULL);
            exit(1);
        } else {
            printf("Round %d Good!\n", round++);
        }
    }

    return 0;
}
