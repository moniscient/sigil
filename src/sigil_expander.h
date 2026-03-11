#ifndef SIGIL_EXPANDER_H
#define SIGIL_EXPANDER_H

#include "sigil_thunk.h"

#define EXPAND_MAX_DEPTH 64
#define EXPAND_MAX_THUNKS 1000

typedef struct {
    int depth;
    int total_thunks;
    int width_at_depth[EXPAND_MAX_DEPTH];
    int max_width;
    bool terminated;     /* all leaves COMPLETED within bounds */
    bool uniform_ops;    /* same func_id at each level */
} ExpansionStats;

ExpansionStats expand_thunk_graph(SigilThunk *root, ThunkArena *arena,
                                   int max_thunks, int max_depth);

#endif /* SIGIL_EXPANDER_H */
