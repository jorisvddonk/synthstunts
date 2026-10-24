// Boot a race on a track given on the DOS command line, e.g.
//   restunts.exe /tZCT5
// This skips the intro and menu and loads the named track, then starts the
// race. Nothing else (no car selection, no replay loading).

#include <dos.h>
#include <stdint.h>

#include "externs.h"
#include "restunts.h"

// Original implementations in seg000.asm (renamed with _asm_ suffix).
extern uint16_t cdecl run_intro_looped_asm_(void);
extern int cdecl run_menu_asm_(void);
extern int cdecl run_option_menu_asm_(void);
extern void cdecl run_game_asm_(void);
// Wrapped below so we can publish the car's map position while a race runs.
extern void cdecl update_gamestate_asm_(void);
// Original asm replay writer/loader (seg005.asm) and raw file writer (seg012).
extern int cdecl file_write_replay_asm_(const char* filename);
extern int16_t cdecl file_load_replay(const char* dir, const char* name);
extern void cdecl file_write_fatal_asm_(const char* filename, void far* src, unsigned long len);
// Memory manager (seg012): mmgr_alloc_resbytes is declared in externs.h.
extern void far* cdecl mmgr_free_asm_(void far* data);
// Shape loading + palette (resolved via aliases-port.lnk).
extern void far* cdecl file_load_shape2d_nofatal_thunk(const char* filename);
extern void cdecl video_set_palette(uint16_t start, uint16_t count, void near* table);
extern unsigned long cdecl mmgr_get_chunk_size_bytes(void far* data);
extern void far* cdecl file_load_resfile(const char* filename);
// Original init_main (seg031), renamed with the _asm_ suffix.
extern void cdecl init_main_asm_(int argc, char** argv);
// Scenery file names (dseg, 5 entries x 9 bytes); patched in place so dseg
// does not grow (growing it breaks audio).
#pragma aux g_sceneries "*"
extern uint8_t g_sceneries[];
// Main-menu background file (dseg, "sdmsel"); rewritten in place to "syntm".
#pragma aux aSdmsel "*"
extern char aSdmsel[];
// Opponent-select file (dseg, "sdosel"); rewritten in place to "synto".
#pragma aux aSdosel "*"
extern char aSdosel[];
// Per-opponent resource file base name (dseg, "opp1"); prefix -> "syn".
#pragma aux aOpp1 "*"
extern char aOpp1[];

// Unported asm helpers, resolved via aliases-port.lnk.
extern void cdecl file_build_path(const char* dir, const char* name, const char* ext, char* dst);
// dseg-resident scratch path (asm file I/O takes near pointers).
#pragma aux g_path_buf "*"
extern char g_path_buf[];
// Existing dseg symbols used to build the track path exactly like the game
// does; this avoids putting new strings in dseg (which breaks audio).
#pragma aux TPath "*"
extern char TPath[];
#pragma aux RPath "*"
extern char RPath[];
#pragma aux a_trk "*"
extern char a_trk[];
// Main-loop idle flag. Setting it makes the original menu auto-select
// "Let's Drive" (see run_menu_asm_), so we can fast-forward through the menu
// while still running all of its initialization.
#pragma aux idle_expired "*"
extern uint8_t idle_expired;
// Current simulation rate (10 or 20 fps); stored in the enhanced trailer.
#pragma aux framespersec "*"
extern uint16_t framespersec;
// High-score name field, prefilled from /nNICKNAME (see enter_hiscore).
#pragma aux byte_459E0 "*"
extern uint8_t byte_459E0[];
// Initial track heading (set from the start tile at track load); the car faces
// this until it first moves.
#pragma aux track_angle "*"
extern uint16_t track_angle;

// NOTE: do NOT force this data into the game's `dseg` segment. Growing `dseg`
// shifts the game's memory layout and silently breaks audio. Like math.c, let
// the compiler place it in its own data segment; that segment is part of the
// same DGROUP as dseg, so the asm helpers (which access DS-relative near
// pointers) still read these strings correctly at runtime.

static int boot_parsed = 0;
static int boot_track_set = 0;
static int boot_car_set = 0;
static int boot_pending = 0;
static int boot_info = 0;
// /synthwave: swap in SYNT.PVS (palette + skyline). /dumppal: one-time dump of
// the game's own !pal to PALETTE.BIN (used to author the synthwave palette).
static int boot_synthwave = 0;
static int boot_dumppal = 0;
static int boot_dumpvid = 0;
static int boot_dumpmenu = 0;
static int boot_dumpdosel = 0;
static int boot_dumpopp = 0;
static int boot_dumpmisc = 0;
static char boot_track[9];
static char boot_car[4];
static char boot_replay_name[9];
static char boot_timing_txt[96];
// /pos: publish the car's current map square to POS.TXT so a page can show it.
static int boot_pos = 0;
static int boot_pos_have = 0;
static int boot_pos_x = -1;
static int boot_pos_y = -1;
// Smoothed per-frame world velocity (fixed-point), for a precise direction.
static int32_t boot_pos_vx = 0;
static int32_t boot_pos_vz = 0;
static int32_t boot_pos_lastlx = 0;
static int32_t boot_pos_lastlz = 0;
static int boot_pos_velinit = 0;
static uint16_t boot_pos_frames = 0;
static char boot_pos_txt[40];

// PSP segment via INT 21h/62h (explicit registers, no lib call).
static unsigned boot_psp_seg(void);
#pragma aux boot_psp_seg = \
    "mov ah, 062h" \
    "int 021h" \
    "mov ax, bx" \
    value [ax] modify [ax bx cx dx es];

static void boot_upper(char* buf, uint16_t n)
{
    uint16_t k;
    for (k = 0; k < n; k++) {
        if (buf[k] >= 'a' && buf[k] <= 'z') {
            buf[k] -= 'a' - 'A';
        }
    }
}

// Parse the DOS command line for /cXXXX (car id, exactly 4 chars) and
// /tXXXXXXXX (track name, 1-8 chars).
static void boot_parse_cmdline(void)
{
    uint8_t far* tail;
    uint16_t len, i, n, k;
    char tok[18];
    int ok;

    boot_parsed = 1;

    tail = MK_FP(boot_psp_seg(), 0x80);
    len = tail[0];
    if (len > 127) {
        len = 127;
    }

    i = 1;
    for (;;) {
        while (i <= len && (tail[i] == ' ' || tail[i] == '\t' || tail[i] == '\r')) {
            i++;
        }
        if (i > len) {
            return;
        }
        if (tail[i] == '/') {
            n = 0;
            i++;
            while (i <= len && n < 17 && tail[i] != ' ' && tail[i] != '\t' && tail[i] != '\r' && tail[i] != '/') {
                tok[n++] = tail[i++];
            }
            if (n < 2) {
                continue;
            }
            if ((n == 5) && (tok[0] == 'i' || tok[0] == 'I') &&
                (tok[1] == 'n' || tok[1] == 'N') &&
                (tok[2] == 'f' || tok[2] == 'F') &&
                (tok[3] == 'o' || tok[3] == 'O')) {
                boot_info = 1; // /info: report the timing after a race/replay
                continue;
            }
            if ((n == 3) && (tok[0] == 'p' || tok[0] == 'P') &&
                (tok[1] == 'o' || tok[1] == 'O') &&
                (tok[2] == 's' || tok[2] == 'S')) {
                boot_pos = 1; // /pos: publish the car's map square to POS.TXT
                continue;
            }
            if ((n == 9) && (tok[0] == 's' || tok[0] == 'S') &&
                (tok[1] == 'y' || tok[1] == 'Y') &&
                (tok[2] == 'n' || tok[2] == 'N') &&
                (tok[3] == 't' || tok[3] == 'T') &&
                (tok[4] == 'h' || tok[4] == 'H') &&
                (tok[5] == 'w' || tok[5] == 'W') &&
                (tok[6] == 'a' || tok[6] == 'A') &&
                (tok[7] == 'v' || tok[7] == 'V') &&
                (tok[8] == 'e' || tok[8] == 'E')) {
                boot_synthwave = 1; // /synthwave: load SYNT.PVS
                continue;
            }
            if ((n == 3) && (tok[0] == 'v' || tok[0] == 'V') &&
                (tok[1] == 'i' || tok[1] == 'I') &&
                (tok[2] == 'd' || tok[2] == 'D')) {
                boot_dumpvid = 1; // /vid: one-shot framebuffer dump (dev)
                continue;
            }
            if ((n == 8) && (tok[0] == 'd' || tok[0] == 'D') &&
                (tok[1] == 'u' || tok[1] == 'U') &&
                (tok[2] == 'm' || tok[2] == 'M') &&
                (tok[3] == 'p' || tok[3] == 'P') &&
                (tok[4] == 'm' || tok[4] == 'M') &&
                (tok[5] == 'e' || tok[5] == 'E') &&
                (tok[6] == 'n' || tok[6] == 'N') &&
                (tok[7] == 'u' || tok[7] == 'U')) {
                boot_dumpmenu = 1; // dev: dump SDMSEL.PVS "scrn"
                continue;
            }
            if ((n == 9) && (tok[0] == 'd' || tok[0] == 'D') &&
                (tok[1] == 'u' || tok[1] == 'U') &&
                (tok[2] == 'm' || tok[2] == 'M') &&
                (tok[3] == 'p' || tok[3] == 'P') &&
                (tok[4] == 'd' || tok[4] == 'D') &&
                (tok[5] == 'o' || tok[5] == 'O') &&
                (tok[6] == 's' || tok[6] == 'S') &&
                (tok[7] == 'e' || tok[7] == 'E') &&
                (tok[8] == 'l' || tok[8] == 'L')) {
                boot_dumpdosel = 1; // dev: dump SDOSEL.PVS
                continue;
            }
            if ((n == 8) && (tok[0] == 'd' || tok[0] == 'D') &&
                (tok[1] == 'u' || tok[1] == 'U') &&
                (tok[2] == 'm' || tok[2] == 'M') &&
                (tok[3] == 'p' || tok[3] == 'P') &&
                (tok[4] == 'm' || tok[4] == 'M') &&
                (tok[5] == 'i' || tok[5] == 'I') &&
                (tok[6] == 's' || tok[6] == 'S') &&
                (tok[7] == 'c' || tok[7] == 'C')) {
                boot_dumpmisc = 1; // dev: dump the MISC resfile
                continue;
            }
            if ((n == 7) && (tok[0] == 'd' || tok[0] == 'D') &&
                (tok[1] == 'u' || tok[1] == 'U') &&
                (tok[2] == 'm' || tok[2] == 'M') &&
                (tok[3] == 'p' || tok[3] == 'P') &&
                (tok[4] == 'o' || tok[4] == 'O') &&
                (tok[5] == 'p' || tok[5] == 'P') &&
                (tok[6] == 'p' || tok[6] == 'P')) {
                boot_dumpopp = 1; // dev: dump opponent .PRE containers
                continue;
            }
            if ((n == 7) && (tok[0] == 'd' || tok[0] == 'D') &&
                (tok[1] == 'u' || tok[1] == 'U') &&
                (tok[2] == 'm' || tok[2] == 'M') &&
                (tok[3] == 'p' || tok[3] == 'P') &&
                (tok[4] == 'p' || tok[4] == 'P') &&
                (tok[5] == 'a' || tok[5] == 'A') &&
                (tok[6] == 'l' || tok[6] == 'L')) {
                boot_dumppal = 1; // /dumppal: write PALETTE.BIN
                continue;
            }
            if (tok[0] == 'c' || tok[0] == 'C') {
                if (n != 5) {
                    continue; // car id is exactly 4 chars
                }
                ok = 1;
                for (k = 1; k < n; k++) {
                    boot_car[k - 1] = tok[k];
                    if (!((boot_car[k - 1] >= 'a' && boot_car[k - 1] <= 'z')
                        || (boot_car[k - 1] >= 'A' && boot_car[k - 1] <= 'Z')
                        || (boot_car[k - 1] >= '0' && boot_car[k - 1] <= '9'))) {
                        ok = 0;
                    }
                }
                if (ok) {
                    boot_upper(boot_car, 4);
                    boot_car_set = 1;
                }
            } else if (tok[0] == 't' || tok[0] == 'T') {
                if (n > 9) {
                    continue; // track name is at most 8 chars
                }
                ok = 1;
                for (k = 1; k < n; k++) {
                    boot_track[k - 1] = tok[k];
                    if (!((boot_track[k - 1] >= 'a' && boot_track[k - 1] <= 'z')
                        || (boot_track[k - 1] >= 'A' && boot_track[k - 1] <= 'Z')
                        || (boot_track[k - 1] >= '0' && boot_track[k - 1] <= '9'))) {
                        ok = 0;
                    }
                }
                if (ok) {
                    boot_upper(boot_track, n - 1);
                    boot_track[n - 1] = 0;
                    boot_track_set = 1;
                }
            } else if (tok[0] == 'n' || tok[0] == 'N') {
                // /nNICKNAME: prefill the high-score name field (case kept).
                for (k = 1; k < n; k++) {
                    byte_459E0[k - 1] = (uint8_t)tok[k];
                }
                byte_459E0[n - 1] = 0;
            }
        } else {
            // A bare token (not /c, /t) names a replay file (first one wins).
            if (!boot_pending) {
                n = 0;
                while (i <= len && n < 8 && tail[i] != ' ' && tail[i] != '\t' && tail[i] != '\r' && tail[i] != '/') {
                    boot_replay_name[n++] = tail[i++];
                }
                if (n > 4
                    && boot_replay_name[n - 4] == '.'
                    && (boot_replay_name[n - 3] == 'r' || boot_replay_name[n - 3] == 'R')
                    && (boot_replay_name[n - 2] == 'p' || boot_replay_name[n - 2] == 'P')
                    && (boot_replay_name[n - 1] == 'l' || boot_replay_name[n - 1] == 'L')) {
                    n -= 4;
                }
                boot_upper(boot_replay_name, n);
                boot_replay_name[n] = 0;
                if (n != 0) {
                    boot_pending = 1;
                }
            } else {
                while (i <= len && tail[i] != ' ' && tail[i] != '\t' && tail[i] != '\r' && tail[i] != '/') {
                    i++;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Synthwave mode (/synthwave)
// ---------------------------------------------------------------------------
// SYNT.PVS is a normal shape2d file holding the replacement palette (!pal)
// and the four skyline slices used by load_skybox (scen/sce2/sce3/sce4).
// Copy the 768-byte !pal resource (after the 16-byte SHAPE2D header) into a
// caller-supplied buffer; video_set_palette() reads the table relative to DS,
// so the buffer must be in DGROUP (init_main uses a stack buffer, like the
// original load_palandcursor).
static int boot_find_pal(void far* container, uint8_t near* dst)
{
    char far* shape;
    char far* src;
    uint8_t near* d;
    uint16_t k;

    if (container == 0) {
        return 0;
    }
    shape = locate_shape_alt((const char far*)container, "!pal");
    if (shape == 0) {
        return 0;
    }
    src = shape + 0x10;
    d = dst;
    for (k = 0; k < 768; k++) {
        *d++ = *src++;
    }
    return 1;
}

// Terminate the program (used only by the one-shot /dumppal dev tool).
static void boot_quit(void);
#pragma aux boot_quit = \
    "mov ax, 04c00h" \
    "int 021h" \
    modify [ax];

// Overwrite the five scenery names in place. Each entry is exactly 9 bytes
// ("name\0" plus padding), so "synt.pvs" + NUL fits without growing dseg.
static void boot_set_scenery_names(void)
{
    static const char name[9] = "synt.pvs";
    uint16_t k, j;

    for (k = 0; k < 5; k++) {
        for (j = 0; j < 9; j++) {
            g_sceneries[k * 9 + j] = (uint8_t)name[j];
        }
    }
}

// Point the menu background loader at "syntm" (SYNTM.PVS) instead of
// "sdmsel" (SDMSEL.PVS). Both are shape2d files with a "scrn" resource.
static void boot_set_menu_name(void)
{
    static const char name[6] = "syntm";
    uint16_t k;

    for (k = 0; k < 6; k++) {
        aSdmsel[k] = name[k];
    }
}

// Point the opponent-select loader at "synto" (SYNTO.PVS) instead of
// "sdosel" (SDOSEL.PVS). It carries the same resources (scrn, clip, the
// !cg/!eg maps) plus the seven opponent portraits.
static void boot_set_opponent_name(void)
{
    static const char name[6] = "synto";
    uint16_t k;

    for (k = 0; k < 6; k++) {
        aSdosel[k] = name[k];
    }
}

// Per-opponent resource files: "opp1".."opp6" -> "syn1".."syn6". The last
// character is overwritten by the game with the opponent type, so only the
// three-character prefix changes.
static void boot_set_opponent_res_prefix(void)
{
    aOpp1[0] = 's';
    aOpp1[1] = 'y';
    aOpp1[2] = 'n';
}

// The original init_main treats any /s... argument as an audio driver, so
// blank our flags in argv before it runs. (boot_parse_cmdline() reads the PSP
// directly and is unaffected.)
static void boot_strip_argv(int argc, char** argv)
{
    static char empty[1] = "";
    uint16_t k;

    for (k = 1; k < (uint16_t)argc; k++) {
        char* a = argv[k];
        if (a == 0 || a[0] != '/') {
            continue;
        }
        if (_stricmp(a, "/synthwave") == 0 || _stricmp(a, "/dumppal") == 0
            || _stricmp(a, "/vid") == 0) {
            argv[k] = empty;
        }
    }
}

// Wrapper for init_main (seg031), called once at startup before the intro.
// Doing the swap here keeps menus, previews and races consistent.
void cdecl init_main(int argc, char** argv)
{
    uint8_t pal[768];

    // The original init_main initialises the memory manager and clears the C
    // runtime's BSS, so our flags must be parsed afterwards.
    boot_strip_argv(argc, argv);
    init_main_asm_(argc, argv);

    boot_parse_cmdline();

    if (boot_synthwave) {
        void far* c;
        boot_set_scenery_names();
        boot_set_menu_name();
        boot_set_opponent_name();
        boot_set_opponent_res_prefix();
        c = file_load_shape2d_nofatal_thunk("synt.pvs");
        if (boot_find_pal(c, pal)) {
            video_set_palette(0, 256, pal);
        }
        // Don't keep SYNT.PVS resident for the whole session: the skybox
        // loader reads it from disk when a race needs it. Holding it here
        // eats a chunk of the small DOS heap and makes the bigger custom-car
        // resources fail to load.
        if (c != 0) {
            mmgr_free_asm_(c);
        }
    }

    if (boot_dumpmisc) {
        void far* c = file_load_resfile("misc");
        if (c != 0) {
            file_write_fatal_asm_("MISC.BIN", c,
                mmgr_get_chunk_size_bytes(c));
        }
        boot_quit();
    }

    if (boot_dumpopp) {
        uint16_t i;
        for (i = 0; i < 6; i++) {
            void far* c;
            aOpp1[3] = (char)('1' + i);
            c = file_load_resfile(aOpp1);
            if (c != 0) {
                char fn[10] = "OPP1.BIN";
                if (boot_synthwave) {
                    fn[0] = 'S'; fn[1] = 'Y'; fn[2] = 'N';
                }
                fn[3] = (char)('1' + i);
                file_write_fatal_asm_(fn, c,
                    mmgr_get_chunk_size_bytes(c));
            }
        }
        boot_quit();
    }

    if (boot_dumpdosel) {
        void far* c = file_load_shape2d_nofatal_thunk(aSdosel);
        if (c != 0) {
            uint8_t far* b = (uint8_t far*)c;
            uint16_t cnt = *(uint16_t far*)(b + 4);
            uint32_t size = 0;
            uint16_t i, j;
            for (i = 0; i < cnt; i++) {
                char nm[5];
                char far* sh;
                uint32_t off, end;
                for (j = 0; j < 4; j++) nm[j] = (char)b[6 + i * 4 + j];
                nm[4] = 0;
                sh = locate_shape_alt((const char far*)c, nm);
                off = (uint32_t)((uint8_t far*)sh - b);
                end = off + 16UL +
                      (uint32_t)(*(uint16_t far*)sh) *
                      (uint32_t)(*(uint16_t far*)(sh + 2));
                if (end > size) size = end;
            }
            {
                unsigned long sz = mmgr_get_chunk_size_bytes(c);
                if (sz < size) sz = size;
                file_write_fatal_asm_("DOSEL.BIN", c, sz);
            }
        }
        boot_quit();
    }

    if (boot_dumpmenu) {
        void far* c = file_load_shape2d_nofatal_thunk(aSdmsel);
        if (c != 0) {
            char far* sh = locate_shape_alt((const char far*)c, "scrn");
            if (sh != 0) {
                file_write_fatal_asm_("MENU.BIN", (void far*)sh,
                                      16UL + 64000UL);
            }
        }
        boot_quit();
    }

    if (boot_dumppal) {
        // /dumppal alone captures the game's own palette; together with
        // /synthwave it captures the palette that was just applied.
        if (!boot_synthwave) {
            boot_find_pal(file_load_shape2d_nofatal_thunk("sdmain"), pal);
        }
        file_write_fatal_asm_("PALETTE.BIN", (void far*)pal, 768UL);
        boot_quit();
    }
}

uint16_t cdecl run_intro_looped(void)
{
    if (!boot_parsed) {
        boot_parse_cmdline();
    }
    // Fast-forward the intro when a CLI race/replay target was given. A car
    // alone is not a target: it must not skip the intro or trigger a race
    // boot with an unset track name.
    if (boot_pending || boot_track_set) {
        return 0;
    }
    // Otherwise run the original intro (it auto-advances on its own).
    return run_intro_looped_asm_();
}

int cdecl run_menu(void)
{
    uint16_t k;
    int r;

    if (!boot_parsed) {
        boot_parse_cmdline();
    }
    if (boot_pending) {
        return 4; // -> option menu, where the replay is loaded
    }
    if (boot_track_set) {
        // Apply the requested car/track once, then let the original menu
        // fast-forward itself into "Let's Drive".
        boot_track_set = 0;
        if (boot_car_set) {
            boot_car_set = 0;
            for (k = 0; k < 4; k++) {
                gameconfig.game_playercarid[k] = boot_car[k];
            }
        } else {
            // Default to PMIN (the fastest car most people use).
            gameconfig.game_playercarid[0] = 'P';
            gameconfig.game_playercarid[1] = 'M';
            gameconfig.game_playercarid[2] = 'I';
            gameconfig.game_playercarid[3] = 'N';
        }
        if (boot_track[0] != 0) {
            // Copy the full 9-byte field (do NOT stop at the NUL): otherwise
            // stale trailing bytes from the previous track name linger and end
            // up in the replay header (e.g. "SHORT" + "T" from "DEFAULT").
            for (k = 0; k < 9; k++) {
                gameconfig.game_trackname[k] = boot_track[k];
            }
        }
        file_build_path(TPath, gameconfig.game_trackname, a_trk, g_path_buf);
        file_read_fatal(g_path_buf, td14_elem_map_main);
        // The original menu auto-selects menu item 0 ("Let's Drive") when the
        // idle flag is set, after doing all of its normal setup.
        idle_expired = 1;
        r = run_menu_asm_();
        idle_expired = 0;
        if (boot_dumpvid) {
            file_write_fatal_asm_("MENUVID.BIN", MK_FP(0xA000, 0), 64000UL);
        }
        return r;
    }
    if (boot_car_set) {
        // Car without a track: keep the normal intro/menu flow, but preselect
        // the car for when the player starts a race.
        boot_car_set = 0;
        for (k = 0; k < 4; k++) {
            gameconfig.game_playercarid[k] = boot_car[k];
        }
    }
    return run_menu_asm_();
}

// Replay viewing. When a replay name is given on the CLI, the main loop routes
// run_menu()==4 -> _do_option_menu -> run_option_menu(); load the replay here
// and return nonzero to enter replay mode. file_load_replay fills gameconfig
// from the .rpl header, so the normal track_load/run_game path plays it back.
int cdecl run_option_menu(void)
{
    uint16_t k;

    if (!boot_parsed) {
        boot_parse_cmdline();
    }
    if (boot_pending) {
        boot_pending = 0;
        // dir ("" into TPath) and name (RPath) must be dseg near strings; the
        // asm loader reads them via DS, and builds the path into g_path_buf.
        TPath[0] = 0;
        for (k = 0; boot_replay_name[k] != 0; k++) {
            RPath[k] = boot_replay_name[k];
        }
        RPath[k] = 0;
        file_load_replay(TPath, RPath);
        if (boot_car_set) {
            boot_car_set = 0;
            for (k = 0; k < 4; k++) {
                gameconfig.game_playercarid[k] = boot_car[k];
            }
        }
        boot_track_set = 0; // the replay's own track wins
        return 1;           // enter replay mode
    }
    return run_option_menu_asm_();
}

// Auto-save the just-finished race's replay to "lastrep.rpl" (no dialog), so
// the web page can pick it up. The replay buffer (td13_rpl_header) is a
// persistent allocation, so it is still valid once run_game() returns.
static const char boot_repname[] = "lastrep.rpl";

// Enhanced replay "lastrep.rp2": the standard .RPL bytes plus a 16-byte timing
// trailer. The original game reads the whole file into its ~27KB track buffer
// (which has ample slack after the replay), so it loads a .RP2 renamed to .RPL
// and simply ignores the trailer. Trailer layout (little-endian):
//   0  magic  "RP2\0"
//   4  version (1)
//   5  fps (1)                     -- framespersec (10/20)
//   6  total finish frames (u16)   -- game_total_finish (frame + penalty + base)
//   8  penalty frames (u16)        -- game_penalty
//   10 recorded frames (u16)       -- game_recordedframes
//   12 checksum (u16)              -- sum of bytes 0..11
//   14 reserved
static const char boot_rep2[] = "lastrep.rp2";

static void boot_write_enhanced(void)
{
    uint8_t far* buf = (uint8_t far*)td13_rpl_header;
    uint8_t far* tmp;
    uint32_t std_len = (uint32_t)gameconfig.game_recordedframes + 0x724UL;
    uint16_t total = (uint16_t)state.game_total_finish;
    uint16_t pen = (uint16_t)state.game_penalty;
    uint16_t rec = gameconfig.game_recordedframes;
    uint8_t trailer[16];
    uint16_t k;
    uint16_t sum;
    uint32_t i;

    trailer[0] = 'R';
    trailer[1] = 'P';
    trailer[2] = '2';
    trailer[3] = 0;
    trailer[4] = 1;
    trailer[5] = (uint8_t)framespersec;
    trailer[6] = (uint8_t)(total & 0xff);
    trailer[7] = (uint8_t)(total >> 8);
    trailer[8] = (uint8_t)(pen & 0xff);
    trailer[9] = (uint8_t)(pen >> 8);
    trailer[10] = (uint8_t)(rec & 0xff);
    trailer[11] = (uint8_t)(rec >> 8);
    sum = 0;
    for (k = 0; k < 12; k++) {
        sum += trailer[k];
    }
    trailer[12] = (uint8_t)(sum & 0xff);
    trailer[13] = (uint8_t)(sum >> 8);
    trailer[14] = 0;
    trailer[15] = 0;

    // Build replay + trailer in a private allocation. Appending into the live
    // replay buffer would clobber the following track regions the game still
    // uses (the file is read/written contiguously from td13_rpl_header).
    tmp = mmgr_alloc_resbytes(a_trk, (int32_t)(std_len + 16UL));
    if (tmp == 0) {
        return;
    }
    for (i = 0; i < std_len; i++) {
        tmp[i] = buf[i];
    }
    for (k = 0; k < 16; k++) {
        tmp[std_len + k] = trailer[k];
    }
    // Use g_path_buf (a scratch path the game rebuilds), NOT TPath: TPath is
    // the track path prefix the high-score/next-track load still relies on.
    for (k = 0; k < sizeof(boot_rep2); k++) {
        g_path_buf[k] = boot_rep2[k];
    }
    file_write_fatal_asm_(g_path_buf, (void far*)tmp, std_len + 16UL);
    mmgr_free_asm_(tmp);
}

// Minimal unsigned decimal writer (no CRT sprintf here).
static uint16_t boot_udec(unsigned v, char* dst)
{
    char tmp[6];
    uint16_t n = 0, i;

    if (v == 0) {
        dst[0] = '0';
        return 1;
    }
    while (v > 0 && n < 6) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (i = 0; i < n; i++) {
        dst[i] = tmp[n - 1 - i];
    }
    return n;
}

// Verification output (/info): write the recomputed race timing to TIMING.TXT
// and echo it, so a submission can be checked by re-running the replay. The
// text buffer lives in our own data segment (passed as a far pointer); only
// the filename uses a dseg scratch buffer.
static void boot_report_timing(void)
{
    unsigned total = (unsigned)(uint16_t)state.game_total_finish;
    unsigned pen = (unsigned)(uint16_t)state.game_penalty;
    unsigned fps = (unsigned)framespersec;
    unsigned rec = (unsigned)gameconfig.game_recordedframes;
    uint16_t k;
    uint16_t n = 0;
    char* p = boot_timing_txt;
    static const char name[] = "timing.txt";

    p[n++] = 't'; p[n++] = 'o'; p[n++] = 't'; p[n++] = 'a'; p[n++] = 'l'; p[n++] = '=';
    n += boot_udec(total, p + n);
    p[n++] = '\n';
    p[n++] = 'p'; p[n++] = 'e'; p[n++] = 'n'; p[n++] = 'a'; p[n++] = 'l'; p[n++] = 't'; p[n++] = 'y'; p[n++] = '=';
    n += boot_udec(pen, p + n);
    p[n++] = '\n';
    p[n++] = 'f'; p[n++] = 'p'; p[n++] = 's'; p[n++] = '=';
    n += boot_udec(fps, p + n);
    p[n++] = '\n';
    p[n++] = 'r'; p[n++] = 'e'; p[n++] = 'c'; p[n++] = 'o'; p[n++] = 'r'; p[n++] = 'd'; p[n++] = 'e'; p[n++] = 'd'; p[n++] = '=';
    n += boot_udec(rec, p + n);
    p[n++] = '\n';

    for (k = 0; k < 11; k++) {
        g_path_buf[k] = name[k];
    }
    file_write_fatal_asm_(g_path_buf, (void far*)boot_timing_txt, (unsigned long)n);
    _printf("TIMING total=%u penalty=%u fps=%u recorded=%u\n", total, pen, fps, rec);
}

void cdecl run_game(void)
{
    uint16_t k;

    run_game_asm_();

    if (boot_info) {
        // Verification run: report only, don't overwrite the autosave.
        boot_report_timing();
        return;
    }

    if (gameconfig.game_recordedframes == 0) {
        return; // nothing recorded (e.g. a replay-only session)
    }
    // Plain replay (byte-for-byte the original format; loads in any build).
    for (k = 0; k < sizeof(boot_repname); k++) {
        g_path_buf[k] = boot_repname[k];
    }
    file_write_replay_asm_(g_path_buf);

    // Enhanced replay carrying the timing (our port reads it back).
    boot_write_enhanced();
}

// ---------------------------------------------------------------------------
// Live car position (/pos)
// ---------------------------------------------------------------------------
// While a race runs, write the car's current map square to POS.TXT. The
// square is the one the game itself computes in detect_penalty (seg001):
//   col = car_posWorld1.lx >> 16           (0..29)
//   row = 29 - (car_posWorld1.lz >> 16)    (0..29)
// i.e. the same (col,row) coordinates the track tiles use. A page polls the
// file and moves a marker on the track map. Only rewritten when the square
// changes, so the DOS I/O is a handful of tiny writes per race.
static char boot_pos_name[] = "POS.TXT";

// The car faces `track_angle` until it first moves. Publish that heading as a
// world-space velocity unit vector so the host can angle the marker the same
// way: track_angle 0=N, 0x100=E, 0x200=S, 0x300=W (calibrated against ZCT5,
// whose 0xb5 start drives west). World +x is map-east; +z is map-south.
static void boot_start_vel(int32_t* vx, int32_t* vz)
{
    switch (track_angle & 0x300) {
        case 0x000: *vx = 0;    *vz = 256;  break; // N
        case 0x100: *vx = 256;  *vz = 0;    break; // E
        case 0x200: *vx = 0;    *vz = -256; break; // S
        default:    *vx = -256; *vz = 0;    break; // W (0x300)
    }
}

// How often to rewrite POS.TXT (in simulation frames). Larger = fewer writes;
// the host polls POS.TXT on its own interval regardless.
#define BOOT_POS_INTERVAL 6

static void boot_publish_pos(void)
{
    int32_t lx, lz, vx, vz, wx, wy;
    int x, y;
    char* p;
    unsigned n;

    if (!boot_pos) {
        return;
    }
    lx = state.playerstate.car_posWorld1.lx;
    lz = state.playerstate.car_posWorld1.lz;

    // Smoothed per-frame velocity (EMA). Cheap: one subtract + a few ops/frame.
    if (boot_pos_velinit) {
        boot_pos_vx = (boot_pos_vx * 3 + (lx - boot_pos_lastlx)) / 4;
        boot_pos_vz = (boot_pos_vz * 3 + (lz - boot_pos_lastlz)) / 4;
    }
    boot_pos_lastlx = lx;
    boot_pos_lastlz = lz;
    boot_pos_velinit = 1;

    // Throttle: rewrite at most every BOOT_POS_INTERVAL frames (the first
    // write happens immediately).
    if (boot_pos_have && ++boot_pos_frames < BOOT_POS_INTERVAL) {
        return;
    }
    boot_pos_frames = 0;

    x = (int)(uint8_t)(((uint32_t)lx) >> 16);
    y = 29 - (int)(uint8_t)(((uint32_t)lz) >> 16);
    if (x < 0 || x > 29 || y < 0 || y > 29) {
        return; // off the 30x30 grid (e.g. mid-air) — keep the last marker
    }
    boot_pos_have = 1;
    boot_pos_x = x;
    boot_pos_y = y;

    // Continuous screen coordinates with 8 fractional bits: wx is map-x*256,
    // wy is map-y*256 in the tiles' screen space (y down). wy uses (30 - z) so
    // that wy>>8 == the map row y (the viewer draws file row R at screen 29-R,
    // and the car sits in [row, row+1)).
    wx = lx >> 8;
    wy = (30 << 8) - (lz >> 8);

    // Publish the velocity (scaled) as the direction; while stationary, the
    // start heading. The host turns this into an exact angle.
    vx = boot_pos_vx >> 6;
    vz = boot_pos_vz >> 6;
    if ((vx < 0 ? -vx : vx) + (vz < 0 ? -vz : vz) < 4) {
        boot_start_vel(&vx, &vz);
    }

    p = boot_pos_txt;
    n = 0;
    n += boot_udec((unsigned)x, p + n);
    p[n++] = ' ';
    n += boot_udec((unsigned)y, p + n);
    p[n++] = ' ';
    if (wx < 0) { p[n++] = '-'; wx = -wx; }
    n += boot_udec((unsigned)wx, p + n);
    p[n++] = ' ';
    if (wy < 0) { p[n++] = '-'; wy = -wy; }
    n += boot_udec((unsigned)wy, p + n);
    p[n++] = ' ';
    if (vx < 0) { p[n++] = '-'; vx = -vx; }
    n += boot_udec((unsigned)vx, p + n);
    p[n++] = ' ';
    if (vz < 0) { p[n++] = '-'; vz = -vz; }
    n += boot_udec((unsigned)vz, p + n);
    p[n++] = '\n';
    file_write_fatal_asm_(boot_pos_name, (void far*)boot_pos_txt, (unsigned long)n);
}

// update_gamestate runs once per simulation frame. Wrapping it (the documented
// asm->C aliasing path) lets us publish the position; the asm stays
// byte-faithful and no caller reads the return value.
void cdecl update_gamestate(void)
{
    static uint16_t vid_frames = 0;
    static uint8_t vid_done = 0;

    update_gamestate_asm_();
    boot_publish_pos();

    if (boot_dumpvid && !vid_done && ++vid_frames >= 120) {
        vid_done = 1;
        file_write_fatal_asm_("SCREEN.BIN", MK_FP(0xA000, 0), 64000UL);
    }
}
