// The VU1 program table when no translated programs were generated (src/lib/vu/aot/ is local: the generated code is
// derived from the game, like src/runner). Every VU1 run then uses the interpreter. See docs/vu1_aot.md.
#include "vu1aot.h"

namespace vu1aot
{
    const Program kPrograms[1] = {};
    const uint32_t kProgramCount = 0;
}
