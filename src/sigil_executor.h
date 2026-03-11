#ifndef SIGIL_EXECUTOR_H
#define SIGIL_EXECUTOR_H

#include "sigil_thunk.h"
#include "sigil_hardware.h"

/* Unified entry: expand → classify → dispatch */
SigilVal execute_thunk(SigilThunk *root, ThunkArena *arena, HardwareProfile *hw);

/* Force a SigilVal: if it's a thunk, run execute_thunk; otherwise pass through.
   This is the primary forcing function at evaluation boundaries. */
SigilVal sigil_execute_val(SigilVal v, ThunkArena *arena, HardwareProfile *hw);

#endif /* SIGIL_EXECUTOR_H */
