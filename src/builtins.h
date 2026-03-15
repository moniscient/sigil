#ifndef SIGIL_BUILTINS_H
#define SIGIL_BUILTINS_H

#include "traits.h"

/* Register built-in trait definitions:
 *   Functor, Applicative, Monad  (type-level, with method requirements)
 *   Bind, Unit, Map              (function-level marker traits)
 *
 * These traits have source_algebra == "__builtin__" so any algebra
 * may implement them without triggering the orphan rule. */
void register_builtin_traits(TraitRegistry *tr, Arena *arena, InternTable *intern_tab);

#endif /* SIGIL_BUILTINS_H */
