# pwdbg — PokéWalker debug emulator CLI

`pwdbg` runs the **PokéWalker firmware on your PC** for testing, debugging and
two-walker peer-play. It reuses the H8/38606 CPU core from
pokeStride — vendored under [`vendor/`](vendor/README.md) — so the
repo builds **standalone** with plain POSIX `gcc` and no cross-repo path dependencies.

Three subcommands: a batch runner, an interactive REPL debugger, and a `duo` mode that
links two emulated walkers over a virtual IR channel.

```
$ pwdbg version
pwdbg 0.3 (run + repl + duo IR bridge)
```

---

## Build

```bash
make # -> ./pwdbg
make clean
```

- **Compiler:** any POSIX `gcc` (override with `CC=...`). Flags: `-O2 -g -Wall`, plus
 `-D__3DS__` (this activates the SCI3↔IR bridge surface inside `walker.c`).
- **Dependencies:** none beyond libc + libm (fully self-contained).
- **Build against a live pokeStride checkout** instead of the vendored copy:

 ```bash
 make POKESTRIDER=an external pokeStride checkout
 ```

> `walker.c` is **unity-included** by `src/walker_ext.c` (`#include "walker.c"`), not
> compiled separately — that keeps the emulator's `static` state (`pc`, `ER`, `memory`,
> `eeprom`, timers, SCI3…) in scope so the debug accessors live in the same translation
> unit. Only `queue.c` is compiled as its own object.

---

## Modes

All modes take `--rom-dir DIR` (source ROMs, default `/workspace/05-roms/pokewalker` →
in this workspace, `05-roms/pokewalker/`) and use a temp working directory unless you
pass `--workdir`. Events are emitted as **JSON Lines** to `--events <target>` where
`<target>` is `-` (stdout), `stderr`, or a file path.

### `pwdbg run` — batch runner

Runs until a cycle limit, breakpoint, stuck loop, decode error or SIGINT, then prints a
summary. Exit code 0 on success, 1 on emulator error.

```bash
pwdbg run --rom-dir ../../05-roms/pokewalker \
 --cycles 30000000 \
 --events run.jsonl \
 --lcd-pgm final.pgm \
 --save-eeprom final.rom
```

| Option | Meaning |
|--------|---------|
| `--cycles N` | stop after N H8 cycles (0 = unlimited; default 10,000,000) |
| `--break PC` | break at PC (hex, repeatable; up to 16) |
| `--trace-pc` | emit one JSON event per instruction (very noisy) |
| `--lcd` | dump final LCD as ASCII to stderr |
| `--lcd-pgm FILE` | write final LCD as binary PGM (P5) |
| `--save-eeprom FILE` | copy `pweep.rom` out after the run |
| `--no-rtc` | do not fire quarter-second RTC interrupts |

### `pwdbg repl` — interactive debugger

```bash
pwdbg repl --rom-dir ../../05-roms/pokewalker --workdir ./session
```

Reads commands from stdin (`pwdbg>` prompt). Extra options: `--script FILE` (run commands
non-interactively from a file), `--ir-sniff` (start with IR events on), `--instance N`
(JSON `i` label), and the `--ir-listen` / `--ir-connect` peer sockets (see
[peer-play](#peer-play)).

<details>
<summary><b>REPL command reference</b> (type <code>help</code> at the prompt)</summary>

```
Execution
 step [N] run N instructions (default 1)
 run [CYCLES] run CYCLES cycles (or until break/err/sig)
 c | cont | continue run with no limit
 info show cycles / instrs / pc / steps / watts / sleep

Registers & memory
 reg print all registers
 reg set R VALUE R = 0..7 (ER), 8 (PC), 9 (CCR)
 mem ADDR [LEN] hex dump (default 64 bytes)
 memw ADDR BYTE [BYTE...] write bytes to RAM/MMIO
 eeprom ADDR [LEN] dump EEPROM region
 eepromw ADDR BYTE [BYTE...] write raw EEPROM (bypasses SPI emulation)

Breakpoints & watchpoints
 break add|rm PC set / clear breakpoint
 break list
 watch add ADDR watch a byte, stop on change
 watch trace ADDR watch a byte, emit event but keep running
 watch rm IDX | watch list

Display
 lcd print LCD as ASCII (96x64 -> " .:#")
 lcd pgm FILE save LCD as binary PGM

IR
 ir-inject HEX... feed bytes into LOCAL RX queue (no peer needed)
 ir-send HEX... send bytes to peer over the IR bridge
 ir-recv [N] read up to N bytes from bridge (non-blocking)
 ir-wait N [TIMEOUT_MS] block until N bytes arrive (default 5000ms)
 ir-sniff on|off toggle IR event emission

Hardware events
 key ENTER|LEFT|RIGHT queue a walker button press
 rtc fire one quarter-second RTC tick
 rtc auto on|off enable/disable automatic RTC ticks
 sleep MS sleep MS milliseconds (script synchronization)

Peer-play testing
 peer-patch on|off patch identity checks for peer-play
 force-slave on|off force this peer to be SLAVE on any SYN

State
 snapshot save|load PATH full state: memory, EEPROM, registers, timers
 stuck on|off toggle repeating-PC 'stuck' loop detector
 help | quit
```
</details>

### `pwdbg duo` — two walkers, one virtual IR link

Forks into two independent walker processes connected by an `AF_UNIX` socketpair (peer
A's TX → peer B's RX and vice versa), modeling the IR link. Events carry `"i":1` (A) or
`"i":2` (B). Waits for both to exit; non-zero exit if either peer failed.

```bash
pwdbg duo --rom-dir ../../05-roms/pokewalker \
 --events-a peer_a.jsonl --events-b peer_b.jsonl
```

Per-peer options: `--workdir-a/-b`, `--break-a/-b PC` (repeatable), `--save-eeprom-a/-b`.
Shared: `--cycles N`, `--trace-pc`, `--no-rtc`.

---

## Peer-play

Two ways to link walkers over the emulated IR channel:

**Automatic (one command):** `pwdbg duo` — see above.

**Manual (two REPLs over a Unix socket):**

```bash
# terminal 1 — server
pwdbg repl --rom-dir ../../05-roms/pokewalker --instance 1 --ir-listen /tmp/pw_ir

# terminal 2 — client
pwdbg repl --rom-dir ../../05-roms/pokewalker --instance 2 --ir-connect /tmp/pw_ir
```

For testing communication against an empty EEPROM (no prior pairing), enable the test
patches: `peer-patch on` (makes the ROM's identity check pass) and, on one side,
`force-slave on` (forces SLAVE on any SYN).

---

## Scripts

`--script FILE` runs REPL commands non-interactively, one per line; `#` and blank lines
are ignored. Sample scripts live in `tests/scripts/`:

```bash
pwdbg repl --rom-dir ../../05-roms/pokewalker \
 --script tests/scripts/diag_single.script --events ev.jsonl
```

- `diag_single.script` — single-walker diagnostic (RTC auto, run, dump memory, press key,
 run more).
- `peerplay_a.script` / `peerplay_b.script` — symmetric peer-play scripts with `peer-patch`
 and `watch trace` on key locations to observe the exchange.

---

## Event stream

Every significant emulator event is one JSON object per line:

```json
{"t":12345,"ev":"pc","pc":"0x1234"}
{"i":1,"t":67890,"ev":"ir_tx","byte":"0xFC","pc":"0x5678"}
{"t":100000,"ev":"break","pc":"0xABCD"}
```

- `t` — total H8 cycles elapsed · `ev` — event kind · `i` — peer label (duo mode only).
- Kinds: `start`, `pc`, `ir_tx_start`/`ir_tx`/`ir_tx_end`/`ir_tx_flush`,
 `ir_rx_start`/`ir_rx`/`ir_rx_stop`, `ir_dispatch`, `break`, `watch`, `rtc`, `stuck`,
 `error`, `signal`, `done` — plus kind-specific fields.

---

## Architecture

```
walker.c (vendored H8 core, -D__3DS__)
 │ emits SCI3↔IR calls expected from 3DS hardware
 ▼
src/ir.h + src/ir_stubs.c SCI3 surface shims: buffer TX into 144-byte
 │ packets, poll RX, feed an injection buffer,
 ▼ emit JSON events
src/ir_bridge.c single non-blocking socket fd (send/recv)
 │
 ▼
AF_UNIX socketpair ←→ the other walker (duo / --ir-listen / --ir-connect)
```

| Area | Files |
|------|-------|
| Entry / subcommand routing | `src/pwdbg.c` |
| Batch runner | `src/run.c` |
| Interactive REPL | `src/repl.c` |
| Duo (fork + socketpair) | `src/duo.c` |
| H8 core accessors + snapshots + peer-patch | `src/walker_ext.c` (unity-includes `walker.c`) |
| IR shims / bridge | `src/ir_stubs.c`, `src/ir.h`, `src/ir_bridge.c`, `src/irtrace.h` |
| LCD → ASCII / PGM | `src/lcd.c` |
| JSON event stream | `src/events.c` |
| Workdir / ROM / EEPROM setup | `src/common.c` |
| Vendored emulator core | `vendor/walker.c`, `walker.h`, `queue.c/h`, `definitions.h`, `regRef.h`, `utils.c` |

`ir.h` and `irtrace.h` are **not** vendored — pwdbg ships its own `src/` shims that
satisfy the includes `walker.c` emits under `-D__3DS__`, exposing only the IR surface the
bridge needs. To refresh the vendored core after pokeStride changes, see
[`vendor/README.md`](vendor/README.md).

---

## Tests

`tests/test_bridge.c` is a smoke test for the IR bridge: it makes a socketpair, attaches
one end to the bridge, sends bytes via `ir_tx_byte()` and receives them on the other end
(and vice versa via `ir_recv_poll()`), and confirms an empty poll returns 0. There is no
`make test` target; compile it manually, e.g.:

```bash
gcc -I src -I vendor src/ir_stubs.c src/ir_bridge.c tests/test_bridge.c -o test_bridge && ./test_bridge
```

---

## License

pwdbg reuses the pokeStride emulator core and is distributed under the **GNU GPLv3** —
see [`LICENSE`](LICENSE). The PokéWalker ROM/EEPROM dumps it loads are Nintendo
property and are **not** included in this repository.
