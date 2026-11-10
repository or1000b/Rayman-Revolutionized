// The VU0 program table when no translated VU0 programs were generated (tools/vu1aot/vu1gen.py --unit 0 writes
// src/lib/vu/aot/aot_programs0.cpp; the folder is local). Every VU0 run then uses the interpreter. See docs/vu1_aot.md.
#include "vu1aot.h"

namespace vu1aot
{
    const Program kPrograms0[1] = {};
    const uint32_t kProgramCount0 = 0;
}
