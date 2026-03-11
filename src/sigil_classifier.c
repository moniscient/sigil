#include "sigil_classifier.h"

ExecutionBin classify(ExpansionStats *stats, HardwareProfile *hw) {
    if (!stats || !hw) return BIN_SEQ_OPTIMAL;

    /* Terminated within probe bounds → small enough for sequential */
    if (stats->terminated)
        return BIN_SEQ_OPTIMAL;

    /* Linear chain: max width == 1 → obligate sequential */
    if (stats->max_width <= 1)
        return BIN_SEQ_OBLIGATE;

    /* Narrow: width below parallelism threshold */
    if (stats->max_width < hw->parallelism_threshold)
        return BIN_SEQ_OPTIMAL;

    /* Uniform ops with very wide graph → GPU candidate */
    if (stats->uniform_ops && stats->max_width >= hw->gpu_width_threshold && hw->gpu_available)
        return BIN_GPU;

    /* Deep + wide → thread parallel */
    if (stats->depth >= hw->thread_depth_threshold)
        return BIN_THREAD;

    /* Default: coroutine / work-stealing */
    return BIN_CORO;
}

const char *execution_bin_name(ExecutionBin bin) {
    switch (bin) {
        case BIN_SEQ_OBLIGATE: return "SEQ_OBLIGATE";
        case BIN_SEQ_OPTIMAL:  return "SEQ_OPTIMAL";
        case BIN_CORO:         return "CORO";
        case BIN_THREAD:       return "THREAD";
        case BIN_GPU:          return "GPU";
    }
    return "UNKNOWN";
}
