#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <math.h>
#include <sys/wait.h>
#include <asm/errno.h>
#include "ELCRQ.h"
#include "malloc.h"

#ifndef NUM_ITERS
#define NUM_ITERS 100
#endif

#ifndef NUM_RUNS
#define NUM_RUNS 1
#endif

#define NUM_THREAD 6

#define SHM_FILE "tshm_file"

#define size_lt unsigned long long

#define BATCH_SIZE 10

pthread_barrier_t *barrier_t;

typedef struct params {
    ELCRQ *global; // global queues in each process
    ELCRQ *thread_q[NUM_THREAD];
    ELCRQ *ack; // ack queues in each process
    int id; // 0 - NUM_THREAD
} params;

static void *sender(void *par);

//static void *intermediate(void *params);

//static void *receiver(void *par);

static inline size_lt elapsed_time_ns(size_lt ns) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000L + t.tv_nsec - ns;
}

static inline int thread_pin(int core_id) {
    int num_cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (core_id < 0 || core_id >= num_cores)
        return EINVAL;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);

    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}

int main() {

    srand(100);

    shm_init(SHM_FILE, NULL);

    barrier_t = shm_malloc(sizeof(pthread_barrier_t));

    pthread_barrierattr_t barattr;
    pthread_barrierattr_setpshared(&barattr, PTHREAD_PROCESS_SHARED);
    if (pthread_barrier_init(barrier_t, &barattr, NUM_THREAD)) {
        printf("Could not create the barrier\n");
        return -1;
    }
    pthread_barrierattr_destroy(&barattr);

    ELCRQ *global = shm_malloc(sizeof(ELCRQ));
    init_queue(global);

    ELCRQ *ack = shm_malloc(sizeof(ELCRQ));
    init_queue(ack);
    params p[NUM_THREAD]; // Number of threads in the simulation

    ELCRQ *thread_q[NUM_THREAD];
    for (int i = 0; i < NUM_THREAD; i++) {
        thread_q[i] = shm_malloc(sizeof(ELCRQ));
        init_queue(thread_q[i]);
        enqueueOnce(100, 0, thread_q[i]);
    }

    for (int i = 0; i < NUM_THREAD; i++) {
        p[i].global = global;
        p[i].ack = ack;
//        p[i].thread_q = thread_q;
        for (int j = 0; j < NUM_THREAD; ++j) {
            p[i].thread_q[j] = thread_q[j];
        }
        p[i].id = i;
    }


    if (fork() == 0) { // Process 1
        shm_child();
        pthread_t sthreads[NUM_THREAD];

        // feed in global
        for (int i = 0; i < NUM_ITERS * NUM_THREAD; i++) {
            enqueueOnce(i, 0, p[0].global);
        }

        // start threads
        for (int i = 0; i < NUM_THREAD; i++) {
            pthread_create(&sthreads[i], NULL, sender, &p[i]);
        }
        for (int i = 0; i < NUM_THREAD; i++) {
            pthread_join(sthreads[i], NULL);
        }
        printf("Process 1 Done\n");
        exit(0);
    } // Process 1 ends

    int status;
    while (wait(&status) > 0); // wait for all processes to finish

    // cleanup
    if ((status = pthread_barrier_destroy(barrier_t))) {
        printf("Could not destroy the barrier: %d\n", status);
        return -1;
    }

    shm_fini();
    shm_destroy();
    return 0;
}

static void *sender(void *par) {
    params *p = (params *) par;

    int id = p->id;
    ELCRQ *global = p->global;
    ELCRQ *local = p->thread_q[id];
    thread_pin(id);
    printf("Ready S: %d\n", id);
    int counter = 0;
    int num = 0;
    int rand_choice;
    int deq_count = 0;
    Object e;
    pthread_barrier_wait(barrier_t);
    while (deq_count < NUM_ITERS) {
//        printf("Counter: %d\n", counter % 61);
        if ((e = dequeueOnce(id, local)) != EMPTY) {
            deq_count++;
//            printf("Thread id: %d %lu \n", id, e);
        } else {
            num = 0;
            switch (counter++ % 61) {
                case 0:
                    while (num++ < BATCH_SIZE && (e = dequeueOnce(id, global)) != EMPTY) {
                        enqueueOnce(e, id, local);
                    }
                    if (counter == 61) counter = 0;
                    break;
                default:
                    // select randomly and enqueue half of the stuff
                    while ((rand_choice = rand() % NUM_THREAD) == id);
                    while (num++ < BATCH_SIZE / 2 && (e = dequeueOnce(id, p->thread_q[rand_choice])) != EMPTY) {
                        printf("%d Stole %lu from %d\n", id, e, rand_choice);
                        enqueueOnce(e, id, local);
                    }
                    break;
            }
        }
    }
    return 0;
}