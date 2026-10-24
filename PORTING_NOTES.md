# Porting notes

Hard-won knowledge for working on this codebase (Stunts disassembly/port).
Read this before changing anything under `src/`.

## Build

```sh
export WATCOM=$HOME/watcom-2
export PATH=$WATCOM/armo64:$PATH
export INCLUDE=$WATCOM/h

make -C src c     # compile the C sources into build/c/dos16/librestunts.a
make -C src       # build asm + link -> build/platform/dos16/restunts.exe
```

Outputs:

- `build/platform/dos16/restunts.exe` — the **port** (`RESTUNTS_ORIGINAL`
  *not* defined). Handy aliases / ported C functions are used.
- `build/platform/dos16/restunto.exe` — the **original**: asm only, no port
  (assembled with `-dRESTUNTS_ORIGINAL`). Keep it byte-faithful to the
  original; use it as the reference for regressions.

### Rebuild gotchas (important)

- `src/asm/Makefile` only lists `structs.inc` in `INCLUDES`. Changes to any
  other `segNNN.inc` / `segNNN.def` do **not** trigger recompilation of the
  `.o` files that include them. Force a rebuild by touching the matching
  `.asm`, or by deleting the build tree:
  `rm -rf build/asm/port build/asm/orig build/c/dos16 && make -C src`.
- `make -C src c` is a separate step; `make -C src` alone will not recompile
  changed C files if `librestunts.a` already exists.

## Segment size invariant

Every translated segment must assemble to **exactly** the size of the original
binary. `src/asm/Makefile` (`SEG_SIZES`) enforces this and fails the build
otherwise (e.g. `seg012 size 77C6 does not match target size 7793`).

Consequences:

- You cannot freely add code or data. If you must add something, remove equal
  bytes elsewhere, or **pad with `nop`/`db` so the size is unchanged**.
- Enlarging a segment silently shifts the memory layout; even if you disable
  the check, the game will not boot (black screen). Do **not** disable the
  check for anything other than a throwaway diagnostic.
- Do port-specific edits with `ifdef`/`ifndef RESTUNTS_ORIGINAL` so the
  original build stays byte-faithful.

## Function aliasing and porting a function to C

Functions that *can* be ported are defined in asm as `<name>_asm_` and the
plain `<name>` is provided by an **alias**. There are two copies of that alias
table, and **both** matter:

- `src/asm/segNNN.inc` — used by the segment's own `.asm`.
- `src/asm/segNNN.def` — included by *other* segments (e.g. `seg000.inc`
  pulls in `seg001.def … segNNN.def`).

Each table has an `ifdef RESTUNTS_ORIGINAL` / `else` split:

- the **original** branch does `name = name_asm_` (alias to asm), and
- the **port** branch does `extrn name:proc` for ported functions.

To port a function (replace its asm with a C implementation):

1. Add it to `PORTED_FUNCS_BY_FILE` in `ghidra/restunts-export.py` and
   re-run the Ghidra export so the asm proc is renamed to `<name>_asm_`.
2. In the **port** branch of `segNNN.inc` **and** `segNNN.def`, change
   `name = name_asm_` to `extrn name:proc`.
3. Remove the `ALIAS name = name_asm_` entry from
   `src/platform/dos16/aliases-port.lnk` (the wlink alias list).

Miss any of these and callers silently keep binding to the asm version — this
exact trap is why an early memory-manager hook had no effect: the `.def` alias
included by every other segment was left in place.

**Warning:** if a symbol is defined in C (`extrn name:proc`) *and* an
`ALIAS name = name_asm_` is still present in `aliases-port.lnk`, `wlink`
**aborts with no diagnostic** (`Trace/BPT trap` / `Abort trap`). If a link
starts crashing out of nowhere, check for a duplicate alias first.

Only rename/edit the aliases referenced by the **port** build; leave the
original branch alone.

## Far pointer / calling conventions

- Ported functions use `__cdecl16far` (far call, cdecl arg order). Declare C
  wrappers with the matching `cdecl` and pointer types.
- A far pointer in memory is `offset` (low word) followed by `segment` (high
  word); as a 32-bit value it is `(segment << 16) | offset`. So `p >> 16` is
  the segment.
- `externs.h` may declare asm globals with `#pragma aux sym "*"` (keep the
  symbol name) and `extern`.
- Functions resolved via `aliases-port.lnk` are the *unported* asm helpers.

## Do NOT grow `dseg` — it silently kills audio

The game is extremely sensitive to the size/layout of the `dseg` segment.
Adding even a handful of bytes to `dseg` **shifts the memory layout and
silently kills sound**: the game still boots and plays, there is no error,
just no audio. This cost a long debugging session — treat `dseg` as sacred.

Rules:

- Never declare new C data with `#pragma data_seg/bss_seg(dseg, "STUNTSD")`.
  Let it land in the compiler's own data segment (`_DATA`), which is also part
  of DGROUP (this is what `math.c` does). `dseg` must stay exactly `55d0`.
- To hand strings/paths to asm helpers that take **near** pointers, reuse
  existing `dseg` symbols (`TPath`, `a_trk`, `g_path_buf`, …) exactly like the
  original code does, instead of introducing new buffers.
- After building, verify: `grep '^dseg ' build/platform/dos16/restunts.map`
  must show size `000055d0`.

## The memory-manager "BLOCK NOT FOUND" crash

`src/asm/seg012.asm` is the memory manager. `mmgr_free_asm_`,
`mmgr_release_asm_`, `mmgr_get_chunk_size_asm_`, `mmgr_resize_memory_asm_` and
`mmgr_op_unk_asm_` all `jmp near ptr loc_31498` when a block is not found;
`loc_31498` calls `fatal_error("memory manager - BLOCK NOT FOUND at SEG= %x")`.

Returning to the main menu after a race (finish **or** ESC-quit) frees a
bogus **segment-0** pointer and aborts the game here. Notes on the fix:

- Growing `seg012` to add instrument/logging breaks the size invariant and the
  game will not boot.
- Routing the five functions through C wrappers fixes the crash, but it also
  killed sound — because those wrappers' data was placed in `dseg` (see the
  `dseg` section above), not because of the extra call layer.
- The working fix is a **size-neutral patch of `loc_31498` itself**, port-only,
  that returns cleanly (`mov sp,bp / pop bp / retf` + `nop` padding to the
  original 12 bytes) instead of fataling. All six callers share a
  `push bp / mov bp,sp` frame, so this unwinds correctly for each.

## CLI / boot features (port-only)

`src/c/replayboot.c` parses the DOS command line (see the README table):

- `/tTRACK` — boot straight into a race: `run_intro_looped` returns 0 (skip the
  intro); `run_menu` loads the track
  (`file_build_path(TPath, gameconfig.game_trackname, a_trk, g_path_buf)` — all
  *existing* `dseg` symbols) then calls the original menu with `idle_expired`
  forced, which makes it auto-select "Let's Drive".
- `/cCAR` — set `gameconfig.game_playercarid`; without it the car defaults to
  `PMIN`.
- `FILE` — boot into a replay: `run_menu` returns 4 → option menu → the
  `run_option_menu` override loads the `.rpl` via `file_load_replay` (dir/name
  in `TPath`/`RPath`) and returns nonzero to enter replay mode.
- `/nNICK` — write the nickname into `byte_459E0`, the buffer `enter_hiscore`
  passes to `call_read_line`; `read_line` keeps the buffer's initial text
  (flags `0x2` = don't clear), so the high-score name field comes up prefilled.
- `/pos` — publish the car's current map cell to `POS.TXT`
  (`col row wx wy vx vz`, 0-29) while a race runs. `update_gamestate` is wrapped
  in C (the asm→C aliasing path): the wrapper calls `update_gamestate_asm_()`
  then writes `POS.TXT` (raw DOS `INT 21h`, no memory manager, so it is safe
  mid-race) at most every `BOOT_POS_INTERVAL` frames (the first write is
  immediate). The asm stays byte-faithful and no caller reads its return value. The cell is the game's own `detect_penalty`
  square (`col = lx>>16`, `row = 29 - (lz>>16)`) — the same coordinates the
  track tiles use. `wx`/`wy` are continuous screen coords (256 per cell; `wy`
  uses `30 - z` so `wy>>8 == row`), for a smooth marker. `vx`/`vz` are the
  smoothed per-frame world velocity (an EMA of the position delta), so the host
  derives an exact heading via `atan2(-vz, vx)` (finer than any fixed set of
  sectors); before the car first moves they carry the start heading
  `track_angle` (0=N, 0x100=E, 0x200=S, 0x300=W).
- `/extmus` — scene tags are **not** written at scene entry (that corrupts game
  state and crashes). Set a pending tag in the C wrappers (`run_intro_looped`
  → TITL, `run_menu` → SLCT, `run_game` → NONE, and VICT/OVER after the race)
  and flush it from a ported `kb_get_char` (alias in `seg012.inc`/`.def`, remove
  from `aliases-port.lnk`). `input_checking` polls `kb_get_char` every intro,
  menu and result frame, so the write happens once the scene is stable. On the
  first poll, also call `audio_disable_flag2` followed by `sub_372F4`, exactly
  like the Options → Music Off item does: silence the in-game music, then reset
  the voices so SFX keep working. Do not call the disable right after
  `file_load_audiores`: the driver is only half-initialised there and the first
  SFX crashes. Do not port `file_load_audiores` itself.

`run_game` is also routed through C to auto-save every finished race
(`lastrep.rpl` + `lastrep.rp2` — see [RP2_FILE_FORMAT.md](RP2_FILE_FORMAT.md)).

Wiring for each overridden function: declare it `extrn` in the **port** block of
`segNNN.def` **and** `segNNN.inc`, then remove its `ALIAS` from
`aliases-port.lnk` (see the aliasing section above). A `push cs / call near`
becomes a `call far`, offset by deleting filler `nop`s to keep the segment size.

None of this state belongs in `dseg`: an early `/t` build put it there and broke
audio (see the `dseg` section).

## Synthwave mode (port-only)

`/synthwave` swaps three assets without touching `dseg` or any segment size.
`SYNT.PVS` (palette + horizon), `SYNTM.PVS` (main menu) and `SYNTO.PVS`
(opponent select) sit next to `REST2.EXE` and are generated by
`src/tools/synthwave/` (workflow in the README).

- `init_main` is a **C wrapper** (unaliased in the port branch of
  `seg031.inc`/`seg031.def` and `aliases-port.lnk`). The original
  `init_main_asm_()` clears the C runtime's BSS (the memory manager takes that
  memory), so parse the CLI **after** it. Parsing first silently loses
  `boot_track`/`boot_car` (they live in `_BSS`).
- The original also treats any `/s…` argument as an audio-driver selector.
  `boot_strip_argv()` blanks `/synthwave`, `/dumppal` and `/vid` in `argv`
  before calling it.
- `video_set_palette()` reads the DAC table **relative to DS**. Use a DGROUP
  buffer; the wrapper uses a 768-byte stack buffer, exactly like the original
  `load_palandcursor`. Free the temporary `SYNT.PVS` container afterwards:
  keeping it resident starves the DOS heap, and the large custom-car
  `STDA*.VSH` resources then fail to load (e.g. OXIA/STRA with `/synthwave`).
- Filenames are patched **in place** so dseg does not grow: `g_sceneries`
  (5×9 bytes) → `"synt.pvs"` (≤8 chars), `aSdmsel` → `"syntm"`, `aSdosel` →
  `"synto"`, and `aOpp1`'s prefix `"opp"` → `"syn"`.
- Opponent files are tried as `<name>.RES` (raw binary) first and then
  `<name>.PRE` (decompressed). Shipping raw `SYN<n>.RES` avoids needing a
  compressor. The game overwrites `aOpp1[3]` with the opponent type before each
  load, so only `aOpp1[0..2]` changes.
- The same `.RES`-over-`.PRE` rule applies to `MISC`: a raw `MISC.RES` carries
  the rebranded `gstu` ("SynthStunts") and `gver` strings and is loaded in
  every mode. `/dumpmisc` writes the decompressed container for editing.
- An opponent container's menu description is the `des` text resource
  (`]` separates lines); the two-letter initials are the first NUL-terminated
  string in `nam` (`g_opponent_initials` is a word). In OPP3..OPP6 `des`
  points 3 bytes into `nam`; the bake script rebuilds the container so they are
  independent.
- The `ed*`/`ev*` resources are the individual win/lose dialogue lines
  (`ed1a`..`ed4a`, `ev1a`..`ev3c`); each is a single NUL-terminated string and
  the dialog code wraps it. `opponents.txt` has a `[dialogue]` block per
  opponent (`key: text`).
- The game writes the DAC exactly once (`load_palandcursor`), so one
  `video_set_palette(0, 256, pal)` at init persists for the whole session
  (menus, previews, races).

### Shape2D / `.PVS` gotchas

- `.PVS` = `file_decomp` + `file_unflip_shape2d`; an unknown extension is the
  raw fallback (`file_load_binary`, no decompression/unflip).
- `file_unflip_shape2d` reads the flip type from **byte +14 (`unk5`)** and the
  "is flipped" nibble from **+15 (`unk6`)**. Fliptype 0 is skipped and stays
  row-major. `s2d_unk4` (+13) is *not* the flip type.
- A container dumped by the game (`/dumpmenu`, `/dumpdosel`) is **already
  unflipped**. Before re-encoding it as `.PVS`, clear +14 and +15 on every
  shape; otherwise the loader unflips already-unflipped pixels a second time.
  The opponent portraits use fliptype 1, so this bit hard.
- `file_get_chunk_size_bytes(chunk)` returns the mmgr chunk size (rounded to
  paragraphs, includes padding); use it to dump a whole container.
- RLE is single-pass: `[01][declen:3][enclen:4][0x83][esc1..3][tokens]`.
  `0x83` sets bit7 (skip the sequential pass) and says 3 escape codes are
  present. Tokens: literal byte, `esc1 count value`, `esc2 value`,
  `esc3 count_lo count_hi value`. `make_synt.rle_encode`/`rle_decode`
  implement it; `file_decomp_paras` sizes the chunk from `declen`.
- When replacing a dumped shape, keep its original 16-byte `SHAPE2D` header
  (width/height, `pos_x`/`pos_y`, `unk1`/`unk2`) and only change the pixels.
  The menu/opponent headers use `pos=(0,0)`/`(112,6)` and must stay put.

### Asset tooling

- `make_synt.py build|menu|opponents` generates `SYNT.PVS`, `SYNTM.PVS` and
  `SYNTO.PVS`.
- `bake_horizon.py` rebuilds `SYNT.PVS` from the committed
  `horizon.png` (1024×48; 320/192/320/192 slices bottom-aligned).
- `bake_opponent_text.py` rebuilds `SYN1.RES`..`SYN6.RES` from
  `opponents.txt` and the `OPP*.BIN` templates (`/dumpopp`).
- `bake_menu.py` / `bake_opponents.py` rebuild the resource from the committed
  PNGs. They carry PEP 723 metadata, so run them with `uv run` (Pillow is
  installed automatically).
- `/dumppal`, `/dumpmenu`, `/dumpdosel` and `/vid` are dev helpers; `/vid`
  also dumps the composed main menu to `MENUVID.BIN`.
- The generated `.PVS`/`.RES` files are game data and are not committed to
  restunts2; the editable PNGs (`horizon.png`, `menu.png`,
  `opp0..opp6.png`), `opponents.txt` and `synthwave.pal` are.

## Web demo (js-dos)

`web/` builds browser bundles (`make-bundles.py`: one `.jsdos` per track, per
track x car and per replay, plus `tracks.json` / `cars.json` / `replays.json`)
and serves them with COOP/COEP headers (`serve.py`) so `SharedArrayBuffer`
works.

- js-dos caches bundles in IndexedDB **keyed by the full URL**, so a changed
  bundle or server behaviour is only picked up after bumping the `?v=N`
  cache-bust in `index.html`; a URL first served before a change stays stale.
- The launcher has a car selector (defaults to `PMIN`), a high-score *Name*
  field and an opt-in *Auto-download replay files* checkbox.
- When a `.jsdos` request carries `?nick=NAME`, `serve.py` rewrites the
  bundle's `.jsdos/dosbox.conf` on the fly to append `/nNAME` to the `REST2.EXE`
  line — no per-nickname bundles. The page adds `&nick=` only when the Name box
  is non-empty.
- The page watches the DOS filesystem and, when enabled, downloads new/changed
  replays; it parses the `.rp2` trailer and shows the race time.
