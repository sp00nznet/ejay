/* dispatch.c - the lifted functions' table, for Linux and macOS: the
 * engine's rows (0x10000000), then the graphics DLL's (0x10100000), which is
 * address order, as recomp_lookup's binary search needs. tools/lift_ejay2.py
 * writes each module's rows; CMakeLists.txt puts src/recomp/<title> on the
 * include path. */
#include "recomp_types.h"
#include "gen_engine/recomp_funcs.h"
#include "gen_gfx/recomp_funcs.h"

const recomp_dispatch_entry_t recomp_dispatch_table[] = {
#include "gen_engine/recomp_dispatch.inc"
#include "gen_gfx/recomp_dispatch.inc"
};
const uint32_t recomp_dispatch_count = sizeof recomp_dispatch_table / sizeof *recomp_dispatch_table;
