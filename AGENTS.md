# AGENTS.md

This repo is a disassembly/port of the DOS game **Stunts**. Before touching
anything under `src/`, read **[PORTING_NOTES.md](./PORTING_NOTES.md)** — it
documents the segment-size invariant, the asm↔C alias mechanism
(`segNNN.inc` + `segNNN.def` + `aliases-port.lnk`), rebuild gotchas, calling
conventions, and the memory-manager crash. Most build/link failures in this
repo come from ignoring one of those rules.

## Build

```sh
export WATCOM=$HOME/watcom-2
export PATH=$WATCOM/armo64:$PATH
export INCLUDE=$WATCOM/h

make -C src c     # compile C -> build/c/dos16/librestunts.a
make -C src       # assemble + link -> build/platform/dos16/restunts.exe
```

- `restunts.exe` = the port. `restunto.exe` = the original asm (reference).
- `.inc`/`.def` edits are not tracked as dependencies: after changing them,
  `rm -rf build/asm/port build/c/dos16 && make -C src` (see PORTING_NOTES).
- Do not enlarge any `segNNN` segment — the build enforces exact sizes and an
  enlarged segment (or a disabled check) yields a non-booting black screen.

## Port-only features

- `src/c/replayboot.c` owns the CLI (`/t`, `/c`, `/n`, `/info`, `/pos`, replay)
  and the `init_main` wrapper.
- `/synthwave` swaps the palette plus the horizon, main-menu and opponent-select
  assets from `SYNT.PVS`, `SYNTM.PVS`, `SYNTO.PVS` and `SYN1.RES`..`SYN6.RES`.
  Those files are game data (not committed); their generators, editable
  PNGs/text and PEP 723 bake scripts live in `src/tools/synthwave/` (see
  PORTING_NOTES).
- The `init_main` wrapper must parse the CLI **after** `init_main_asm_()` (the
  original clears the C BSS) and strip `/s…` flags from `argv` (the original
  treats them as audio drivers). Dev flags: `/dumppal`, `/dumpmenu`,
  `/dumpdosel`, `/dumpopp`, `/vid`.

## Conventions

- Keep `restunto.exe` byte-faithful; gate port-only changes with
  `ifdef`/`ifndef RESTUNTS_ORIGINAL`.
- No new comments in generated asm unless required; keep changes size-neutral.
- Do not commit generated game assets (`.PVS`); commit the generators and
  editable PNGs instead.
- Web demo lives in `web/` (js-dos); cache-bust with `?v=N` in `index.html`.

## Git

- **Never** run `git add -A` or `git add .`. Stage explicit paths only.
- Commit/push only when explicitly asked.
