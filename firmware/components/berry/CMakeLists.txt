# Berry-VM ESP-IDF component — a vendored copy of berry-lang/berry 1.1.0 (MIT,
# see LICENSE) with the same source tree (src/ + generate/ + include/), plus
# an ESP-IDF port (src/be_port_idf.c) and an allocation counter (src/be_mem.c).
#
# Layout convention (kept identical to upstream berry so the precompiled-
# object includes like `#include "../generate/be_fixed_*.h"` resolve from
# the .c file's own location):
#
#   components/berry/
#     ├── src/        ← all .c implementation + private .h
#     ├── generate/   ← precompiled tables (be_fixed_*.h, be_const_*.h)
#     └── include/    ← berry_conf.h (public, plus berry.h via src/)
#
# We add src/ to the include path so consumers can `#include "berry.h"`.

idf_component_register(
    SRC_DIRS  "src"
    INCLUDE_DIRS "include" "src"
)

# Berry-VM upstream tolerates some warnings that ESP-IDF's stricter default
# flags promote to errors.  The suppressions below let this component build
# cleanly without forcing project-wide -Wno-* flags.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-parameter
    -Wno-implicit-fallthrough
    -Wno-discarded-qualifiers
    -Wno-array-bounds
    -Wno-format
    -Wno-unused-but-set-variable
    -Wno-stringop-overread
    # 32-bit RISC-V: be_solidifylib casts (void*) to uint64_t for hex
    # serialisation of constants — benign upcast, but -Werror promotes it.
    -Wno-pointer-to-int-cast
    # Some upstream Berry helpers store function pointers via void* and
    # the compiler complains in places like map iteration.
    -Wno-int-to-pointer-cast
    # be_solidifylib toidentifier() does isalnum(p[0]) where p[0] is a plain
    # char; gcc-14 (the IDF 5.5 toolchain) promotes the
    # signed-char-as-ctype-subscript to -Werror=char-subscripts.  Benign
    # (identifiers are ASCII), and a no-op where the warning does not occur.
    -Wno-char-subscripts
    # be_module.c raises `#warning module: unsupported OS` — accurate and
    # harmless, since Berry has no OS-specific module loader under ESP-IDF.
    # IDF 6.1-dev promotes -Wcpp to an error where 6.0.1 did not, which stops
    # the build.  -Wno-ERROR (not -Wno-cpp) on purpose: the warning stays
    # visible, only the escalation goes.  Additive — a no-op on 6.0.1.
    -Wno-error=cpp
)
