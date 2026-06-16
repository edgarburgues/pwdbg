# vendor/ — pinned emulator core

These files are **vendored copies** of the PokeStride emulator core. pwdbg
reuses the H8/38606 CPU emulator instead of reimplementing it: `walker.c`
is unity-included by `../src/walker_ext.c` (`#include "walker.c"`), which
keeps the emulator's `static` state (`pc`, `ER`, `memory`, `eeprom`, ...)
in scope so pwdbg can add debug accessors in the same translation unit.

Keeping pinned copies here (instead of a `an external pokeStride checkout` path
reference) makes pwdbg a self-contained, independently-buildable repo.

## Files

| File | Role |
|------|------|
| `walker.c` | H8 CPU + all peripheral emulation (unity-included) |
| `walker.h` | emulator public API |
| `queue.c` / `queue.h` | input event ring buffer (compiled separately) |
| `definitions.h` | hardware register addresses, peripheral structs |
| `regRef.h` | `static inline` register access helpers |
| `utils.c` | utilities, unity-included by `walker.c` |

`ir.h` and `irtrace.h` are NOT vendored — pwdbg ships its own shim
versions in `../src/` that satisfy the `#include "ir.h"` /
`#include "irtrace.h"` that `walker.c` emits under `-D__3DS__`, exposing
only the IR surface pwdbg's bridge needs.

## Provenance / refreshing the snapshot

Source: the `pokestride` repo, `src/`. To refresh after the emulator core
changes upstream:

```sh
for f in walker.c walker.h queue.c queue.h definitions.h regRef.h utils.c; do
 cp an external pokeStride checkout/$f vendor/$f
done
```

Then rebuild and run the tests in `../tests/`. If `walker.c` changed its IR
surface (renamed/added an SCI3 function, changed `SCI3_t`, etc.), the build
will fail until the shims in `../src/ir.h` / `../src/irtrace.h` are updated
to match — that compile error is the signal to look, not something to work
around.

The build can also be pointed at a live checkout without touching these
copies: `make POKESTRIDER=an external pokeStride checkout`.
