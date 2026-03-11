#ifndef SIGIL_CLASSIFIER_H
#define SIGIL_CLASSIFIER_H

#include "sigil_expander.h"
#include "sigil_hardware.h"

typedef enum {
    BIN_SEQ_OBLIGATE = 0,  /* linear chain, serial deps */
    BIN_SEQ_OPTIMAL  = 1,  /* small/narrow graph */
    BIN_CORO         = 2,  /* wide, work-stealing */
    BIN_THREAD       = 3,  /* deep+wide, OS threads */
    BIN_GPU          = 4   /* uniform matrix ops */
} ExecutionBin;

ExecutionBin classify(ExpansionStats *stats, HardwareProfile *hw);

const char *execution_bin_name(ExecutionBin bin);

#endif /* SIGIL_CLASSIFIER_H */
