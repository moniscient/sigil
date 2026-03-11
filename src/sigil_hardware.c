#include "sigil_hardware.h"
#include <unistd.h>
#include <time.h>
#include <pthread.h>

static void *noop_thread(void *arg) {
    (void)arg;
    return NULL;
}

void calibrate_hardware(HardwareProfile *hw) {
    /* Core count */
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    hw->core_count = (ncpu > 0) ? (int)ncpu : 1;

    /* Measure thread spawn cost */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pthread_t th;
    pthread_create(&th, NULL, noop_thread, NULL);
    pthread_join(th, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    hw->thread_spawn_cost_ns = (t1.tv_sec - t0.tv_sec) * 1000000000LL +
                                (t1.tv_nsec - t0.tv_nsec);

    /* Thresholds */
    hw->parallelism_threshold = hw->core_count * 2;
    if (hw->parallelism_threshold < 4) hw->parallelism_threshold = 4;
    hw->gpu_width_threshold = 256;
    hw->thread_depth_threshold = 8;

    /* GPU detection: stub — no GPU support yet */
    hw->gpu_available = false;
}
