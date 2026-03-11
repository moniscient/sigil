#ifndef SIGIL_HARDWARE_H
#define SIGIL_HARDWARE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int core_count;
    int parallelism_threshold;
    int gpu_width_threshold;
    int thread_depth_threshold;
    int64_t thread_spawn_cost_ns;
    bool gpu_available;
} HardwareProfile;

void calibrate_hardware(HardwareProfile *hw);

#endif /* SIGIL_HARDWARE_H */
