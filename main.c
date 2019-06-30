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
    ELCRQ *target; // target queue for further message passing
    int id; // 0 - NUM_THREAD
} params;

static void *sender(void *par);

static void *intermediate(void *params);

static void *receiver(void *par);

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
    if (pthread_barrier_init(barrier_t, &barattr, NUM_THREAD*3)) {
        printf("Could not create the barrier\n");
        return -1;
    }
    pthread_barrierattr_destroy(&barattr);

    ELCRQ *global1 = shm_malloc(sizeof(ELCRQ));
    init_queue(global1);
    ELCRQ *global2 = shm_malloc(sizeof(ELCRQ));
    init_queue(global2);
    ELCRQ *global3 = shm_malloc(sizeof(ELCRQ));
    init_queue(global3);

    if (fork() == 0) { // Process 1
        shm_child();
        params p[NUM_THREAD]; // Number of threads in the simulation
        pthread_t sthreads[NUM_THREAD];

        for (int i = 0; i < NUM_THREAD; i++) {
            p[i].global = global1;
            p[i].target = global2;
            p[i].id = i;
        }

        // feed in global
        for (int i = 0; i < NUM_ITERS * NUM_THREAD; i++) {
            enqueueOnce(i, 0, global1);
        }

        // initialize per core queues
        ELCRQ *thread_q[NUM_THREAD];
        for (int i = 0; i < NUM_THREAD; i++) {
            thread_q[i] = shm_malloc(sizeof(ELCRQ));
            init_queue(thread_q[i]);
            enqueueOnce(100, 0, thread_q[i]);
        }

        // start threads
        for (int i = 0; i < NUM_THREAD; i++) {
            // pass per core queue to each thread as parameter
            for (int j = 0; j < NUM_THREAD; ++j) {
                p[i].thread_q[j] = thread_q[j];
            }
            pthread_create(&sthreads[i], NULL, sender, &p[i]);
        }
        for (int i = 0; i < NUM_THREAD; i++) {
            pthread_join(sthreads[i], NULL);
        }
        printf("Process 1 Done\n");
        exit(0);
    } // Process 1 ends

    if (fork() == 0) { // Process 2
        shm_child();
        params p[NUM_THREAD]; // Number of threads in the simulation
        pthread_t ithreads[NUM_THREAD];

        for (int i = 0; i < NUM_THREAD; i++) {
            p[i].global = global2;
            p[i].target = global3;
            p[i].id = i + NUM_THREAD;
        }

        // feed once to initialize global
        enqueueOnce(100, 0, global2);

        // initialize per core queues
        ELCRQ *thread_q[NUM_THREAD];
        for (int i = 0; i < NUM_THREAD; i++) {
            thread_q[i] = shm_malloc(sizeof(ELCRQ));
            init_queue(thread_q[i]);
            enqueueOnce(100, 0, thread_q[i]);
        }

        // start threads
        for (int i = 0; i < NUM_THREAD; i++) {
            // pass per core queue to each thread as parameter
            for (int j = 0; j < NUM_THREAD; ++j) {
                p[i].thread_q[j] = thread_q[j];
            }
            pthread_create(&ithreads[i], NULL, intermediate, &p[i]);
        }
        for (int i = 0; i < NUM_THREAD; i++) {
            pthread_join(ithreads[i], NULL);
        }
        printf("Process 2 Done\n");
        exit(0);
    } // Process 2 ends

    if (fork() == 0) { // Process 3
        shm_child();
        params p[NUM_THREAD]; // Number of threads in the simulation
        pthread_t rthreads[NUM_THREAD];

        for (int i = 0; i < NUM_THREAD; i++) {
            p[i].global = global3;
            p[i].target = NULL;
            p[i].id = i + NUM_THREAD*2;
        }

        // feed once to initialize global
        enqueueOnce(100, 0, global2);

        // initialize per core queues
        ELCRQ *thread_q[NUM_THREAD];
        for (int i = 0; i < NUM_THREAD; i++) {
            thread_q[i] = shm_malloc(sizeof(ELCRQ));
            init_queue(thread_q[i]);
            enqueueOnce(100, 0, thread_q[i]);
        }

        // start threads
        for (int i = 0; i < NUM_THREAD; i++) {
            // pass per core queue to each thread as parameter
            for (int j = 0; j < NUM_THREAD; ++j) {
                p[i].thread_q[j] = thread_q[j];
            }
            pthread_create(&rthreads[i], NULL, receiver, &p[i]);
        }
        for (int i = 0; i < NUM_THREAD; i++) {
            pthread_join(rthreads[i], NULL);
        }
        printf("Process 3 Done\n");
        exit(0);
    } // Process 3 ends


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
    ELCRQ *target = p->target;
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
            enqueue(e, id, target);
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

static void *intermediate(void *par) {
    params *p = (params *) par;

    int id = p->id;
    ELCRQ *global = p->global;
    ELCRQ *local = p->thread_q[id - NUM_THREAD];
    ELCRQ *target = p->target;
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
            enqueue(e, id, target);
//            printf("Thread id: %d %lu\n", id, e);
        } else {
            num = 0;
            switch (counter++ % 61) {
                case 0:
                    while (num++ < BATCH_SIZE && (e = dequeue(id, global)) != EMPTY) {
                        enqueueOnce(e, id, local);
                    }
                    if (counter == 61) counter = 0;
                    break;
                default:
                    // select randomly and enqueue half of the stuff
                    while ((rand_choice = rand() % NUM_THREAD) == (id - NUM_THREAD));
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

static void *receiver(void *par) {
    params *p = (params *) par;

    int id = p->id;
    ELCRQ *global = p->global;
    ELCRQ *local = p->thread_q[id - NUM_THREAD*2];
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
//            printf("Thread id: %d %lu\n", id, e);
        } else {
            num = 0;
            switch (counter++ % 61) {
                case 0:
                    while (num++ < BATCH_SIZE && (e = dequeue(id, global)) != EMPTY) {
                        enqueueOnce(e, id, local);
                    }
                    if (counter == 61) counter = 0;
                    break;
                default:
                    // select randomly and enqueue half of the stuff
                    while ((rand_choice = rand() % NUM_THREAD) == (id - NUM_THREAD*2));
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