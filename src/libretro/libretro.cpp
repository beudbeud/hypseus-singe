/*
 * hypseus_singe_libretro.cpp
 * Libretro core wrapper for Hypseus Singe.
 *
 * Threading model:
 *   - Emulation thread: runs g_game->start() (= cpu::execute()) indefinitely.
 *   - Libretro thread:  retro_run() called by the frontend at ~60 Hz.
 *
 * Synchronisation:
 *   - s_frame_produced: semaphore signalled by emu thread inside vid_blit()
 *     after g_lr_surface has been fully composited.
 *   - s_frame_consumed: semaphore signalled by retro_run() after submitting
 *     the frame to the frontend, allowing emu to continue.
 *
 * Audio:
 *   - SDL audio callback (sound::callback) posts into s_audio_ring then
 *     zeroes the SDL stream so speakers stay silent.
 *   - retro_run() drains the ring and calls audio_batch_cb().
 *
 * Input:
 *   - retro_run() polls the frontend and writes s_pad_state (with mutex).
 *   - SDL_check_input() (libretro stub in input.cpp) reads s_pad_state and
 *     calls g_game->input_enable/disable.
 *
 * ROM format:
 *   - A plain-text ".commands" file containing the hypseus command-line
 *     arguments (space or newline separated), without the executable name.
 *   - Example:  "lair vldp -framefile /path/to/lair.txt"
 *   - -homedir is injected automatically from the system directory.
 */

#include "libretro.h"
#include "config.h"

#include <SDL.h>
#include <SDL_image.h>
#include <SDL_ttf.h>

#include <plog/Log.h>
#include <plog/Appenders/ColorConsoleAppender.h>
#include <plog/Appenders/RollingFileAppender.h>

#include <string>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

/* Emulator headers — order matters: game/ldp types must precede globals.h */
#include "../game/game.h"
#include "../ldp-out/ldp.h"
#include "../ldp-out/ldp-vldp.h"
#define DEFINE_GLOBALS
#include "../globals.h"        /* defines g_game, g_ldp, quitflag, … */
#include "../hypseus.h"
#include "../io/cmdline.h"
#include "../io/input.h"
#include "../io/homedir.h"
#include "../io/conout.h"
#include "../video/video.h"
#include "../sound/sound.h"
#include "../cpu/cpu.h"
#include "../timer/timer.h"

/* -------------------------------------------------------------------------
 * Libretro callbacks (set by the frontend)
 * ---------------------------------------------------------------------- */
static retro_video_refresh_t       video_cb       = nullptr;
static retro_audio_sample_t        audio_cb       = nullptr;
static retro_audio_sample_batch_t  audio_batch_cb = nullptr;
static retro_environment_t         environ_cb     = nullptr;
static retro_input_poll_t          input_poll_cb  = nullptr;
static retro_input_state_t         input_state_cb = nullptr;

/* -------------------------------------------------------------------------
 * Video shared state
 * ---------------------------------------------------------------------- */
static SDL_sem    *s_frame_produced  = nullptr;
static SDL_sem    *s_frame_consumed  = nullptr;
static int         s_vid_w          = 640;
static int         s_vid_h          = 480;
/* Set by libretro_submit_video() (emu thread) when dimensions change; read
 * and cleared by retro_run() (libretro thread) to issue SET_GEOMETRY before
 * passing the first resized frame to the frontend. */
static SDL_atomic_t s_geometry_changed = {0};
#ifdef DEBUG
static int          s_pix_logged       = 0; /* debug pixel log counter, reset on resize */
#endif

/* -------------------------------------------------------------------------
 * Audio ring buffer  (stereo int16_t samples, interleaved L/R)
 * 1 s @ 44100 Hz stereo = 44100 * 2 = 88 200 int16_t values.
 * The drain cap in drain_audio() is ~50 ms so 1 s is more than sufficient.
 * ---------------------------------------------------------------------- */
#define AUDIO_RING_CAP (44100 * 2 * 1)
static int16_t    s_audio_ring[AUDIO_RING_CAP];
static int        s_audio_head  = 0;   /* write index */
static int        s_audio_tail  = 0;   /* read index */
static int        s_audio_count = 0;   /* int16_t values available */
static SDL_mutex *s_audio_mutex = nullptr;

/* -------------------------------------------------------------------------
 * Input shared state
 * Bit N is set when SWITCH_N (from io/input.h) is pressed.
 * ---------------------------------------------------------------------- */
static uint32_t   s_pad_state  = 0;
static uint32_t   s_pad_prev   = 0;   /* previous state seen by emu thread */
static SDL_mutex *s_input_mutex = nullptr;

/* Mouse / pointer / lightgun shared state (guarded by s_input_mutex)     */
static int        s_mouse_x      = 0;   /* absolute screen position       */
static int        s_mouse_y      = 0;
static Sint16     s_mouse_dx     = 0;   /* relative delta this frame       */
static Sint16     s_mouse_dy     = 0;
static bool       s_mouse_moved  = false;
static uint8_t    s_mouse_btns   = 0;   /* bit0=left/trigger, bit1=right   */
static uint8_t    s_mouse_prev   = 0;

/* -------------------------------------------------------------------------
 * Video copy buffer
 * retro_run() copies g_lr_surface here then posts s_frame_consumed before
 * calling video_cb(), so the emu thread unblocks after ~0.15 ms (memcpy)
 * instead of after the full video_cb() latency (~1 ms GPU upload).
 * ---------------------------------------------------------------------- */
static uint32_t *s_video_buf    = nullptr;
static size_t    s_video_buf_sz = 0;

/* -------------------------------------------------------------------------
 * Emulation thread
 * ---------------------------------------------------------------------- */
static SDL_Thread *s_emu_thread  = nullptr;
static bool        s_emu_started = false;
static bool        s_first_run   = true;  /* reset in retro_unload_game for restart */
static std::string s_last_game_path;      /* stored for retro_reset() full reload */

/* -------------------------------------------------------------------------
 * Core options v2
 * ---------------------------------------------------------------------- */
static struct retro_core_option_v2_definition k_option_defs[] = {
    {
        "hypseus_fastboot",
        "Fast Boot (DL/SA/Cliff/GTG)",
        nullptr, nullptr, nullptr, nullptr,
        { {"disabled", nullptr}, {"enabled", nullptr}, {nullptr, nullptr} },
        "disabled"
    },
    {
        "hypseus_blank_searches",
        "Blank Screen During Searches",
        nullptr, nullptr, nullptr, nullptr,
        { {"disabled", nullptr}, {"enabled", nullptr}, {nullptr, nullptr} },
        "disabled"
    },
    {
        "hypseus_blank_skips",
        "Blank Screen During Skips",
        nullptr, nullptr, nullptr, nullptr,
        { {"disabled", nullptr}, {"enabled", nullptr}, {nullptr, nullptr} },
        "disabled"
    },
    {
        "hypseus_seek_frames",
        "Seek Speed (frames/ms, 0=instant)",
        nullptr, nullptr, nullptr, nullptr,
        { {"0", nullptr}, {"20", nullptr}, {"30", nullptr}, {"60", nullptr}, {"100", nullptr}, {nullptr, nullptr} },
        "0"
    },
    {
        "hypseus_latency",
        "Seek Latency ms (DL F2 fix)",
        nullptr, nullptr, nullptr, nullptr,
        { {"0", nullptr}, {"100", nullptr}, {"200", nullptr}, {"300", nullptr}, {"500", nullptr}, {nullptr, nullptr} },
        "0"
    },
    {
        "hypseus_cheat",
        "Cheat Mode (unlimited lives)",
        nullptr, nullptr, nullptr, nullptr,
        { {"disabled", nullptr}, {"enabled", nullptr}, {nullptr, nullptr} },
        "disabled"
    },
    {
        "hypseus_scoreboard",
        "Scoreboard Overlay",
        nullptr, nullptr, nullptr, nullptr,
        { {"enabled", nullptr}, {"disabled", nullptr}, {nullptr, nullptr} },
        "enabled"
    },
    { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, {{nullptr, nullptr}}, nullptr },
};

static struct retro_core_options_v2 k_core_options_v2 = {
    NULL,           /* no categories */
    k_option_defs,
};


/* -------------------------------------------------------------------------
 * Libretro → hypseus input mapping
 * ---------------------------------------------------------------------- */
struct BtnMap { unsigned retro_id; unsigned hypseus_id; };
static const BtnMap k_btn_map[] = {
    { RETRO_DEVICE_ID_JOYPAD_UP,     SWITCH_UP      },
    { RETRO_DEVICE_ID_JOYPAD_DOWN,   SWITCH_DOWN    },
    { RETRO_DEVICE_ID_JOYPAD_LEFT,   SWITCH_LEFT    },
    { RETRO_DEVICE_ID_JOYPAD_RIGHT,  SWITCH_RIGHT   },
    { RETRO_DEVICE_ID_JOYPAD_START,  SWITCH_START1  },
    { RETRO_DEVICE_ID_JOYPAD_SELECT, SWITCH_COIN1   },
    { RETRO_DEVICE_ID_JOYPAD_A,      SWITCH_BUTTON1 },
    { RETRO_DEVICE_ID_JOYPAD_B,      SWITCH_BUTTON2 },
    { RETRO_DEVICE_ID_JOYPAD_X,      SWITCH_BUTTON3 },
    { RETRO_DEVICE_ID_JOYPAD_L,      SWITCH_SKILL1  },
    { RETRO_DEVICE_ID_JOYPAD_R,      SWITCH_SKILL2  },
    { RETRO_DEVICE_ID_JOYPAD_L2,     SWITCH_SERVICE },
    { RETRO_DEVICE_ID_JOYPAD_Y,      SWITCH_PAUSE   },
};
static const int k_btn_map_size = (int)(sizeof(k_btn_map) / sizeof(k_btn_map[0]));

/* =========================================================================
 * Functions called from the emulation thread (via modified subsystems)
 * ====================================================================== */

/*
 * Called from vid_blit() (video/video.cpp, guarded by LIBRETRO_CORE) after
 * SDL_RenderPresent().  Reads pixels from the renderer, stores them in the
 * shared buffer, signals the libretro thread, then waits for it to consume
 * the frame before returning.
 */
void libretro_submit_video()
{
    if (!s_frame_produced || !s_frame_consumed || !video_cb)
        return;

    /* Use viewport (renderer) dimensions, not g_vid_width: the renderer may
     * be resized to probe_width×probe_height (e.g. 720×480) while g_vid_width
     * stays at the cmdline default (640).  Reading more pixels than the buffer
     * is sized for causes a heap overflow. */
    int w = (int)video::get_viewport_width();
    int h = (int)video::get_viewport_height();
    if (w <= 0) w = (int)video::get_video_width();
    if (h <= 0) h = (int)video::get_video_height();

    /* Update stored dimensions; notify frontend of geometry change if needed.
     * Pixel data stays in g_lr_surface — retro_run() reads it directly while
     * the emu thread is blocked on s_frame_consumed (guaranteed exclusion). */
    if (w != s_vid_w || h != s_vid_h) {
        s_vid_w = w;
        s_vid_h = h;
        SDL_AtomicSet(&s_geometry_changed, 1);
#ifdef DEBUG
        s_pix_logged = 0;
#endif
    }

#ifdef DEBUG
    {
        SDL_Surface *lr_surf = video::get_lr_surface();
        ++s_pix_logged;
        if ((s_pix_logged <= 3 || (s_pix_logged % 150) == 0) && lr_surf && lr_surf->pixels) {
            const uint32_t *px = static_cast<const uint32_t *>(lr_surf->pixels);
            int p4 = lr_surf->pitch / 4;
            fprintf(stderr, "[hypseus-libretro] submit_video #%d %dx%d center=0x%08x tl=0x%08x\n",
                    s_pix_logged, w, h, px[(h/2)*p4 + w/2], px[0]);
        }
    }
#endif

    SDL_SemPost(s_frame_produced);
    /* Wait for retro_run() to consume the frame, but don't block forever.
     * If the frontend pauses emulation (menu open), retro_run() stops calling
     * SDL_SemPost(s_frame_consumed). Without a timeout the emu thread stalls
     * for the entire menu duration then bursts on resume, causing lag.
     * Pause the CPU timer during the wait so that when the frontend resumes,
     * the CPU timing loop does not try to catch up on the elapsed wall time.
     * Also nudge the LDP start time so think_delay() does not issue a burst
     * of catch-up pre_think() calls (which would cause VLDP sync lag).
     * Additionally pause OGG audio so its file position does not advance
     * during the menu, which would cause permanent audio/video desync. */
    cpu::pause();
    uint32_t pause_start_ms = SDL_GetTicks();

    bool menu_audio_paused = false;
    while (SDL_SemWaitTimeout(s_frame_consumed, 33) != 0) {
        if (get_quitflag()) {
            if (menu_audio_paused && g_ldp) g_ldp->audio_resume_menu();
            cpu::unpause();
            return;
        }
        if (!menu_audio_paused) {
            /* First timeout: frontend is paused (menu open).
             * Freeze OGG so it does not race ahead of the video. */
            if (g_ldp) g_ldp->audio_pause_menu();
            menu_audio_paused = true;
        }
    }

    uint32_t paused_ms = SDL_GetTicks() - pause_start_ms;
    if (menu_audio_paused) {
        if (g_ldp) g_ldp->audio_resume_menu();
        /* Flush the ring of any samples produced before audio was frozen
         * (at most ~33 ms of pre-pause OGG data) so they are not heard. */
        if (s_audio_mutex) {
            SDL_LockMutex(s_audio_mutex);
            s_audio_head = s_audio_tail = s_audio_count = 0;
            SDL_UnlockMutex(s_audio_mutex);
        }
    }
    cpu::unpause();
    if (paused_ms > 33 && g_ldp)
        g_ldp->nudge_start_time(paused_ms);
}

/*
 * Called from sound::callback() (sound/sound.cpp, guarded by LIBRETRO_CORE)
 * after mixing.  Copies samples into the ring buffer and zeroes the SDL
 * stream so the SDL audio device stays silent.
 *
 * All-zero frames are not added to the ring.  During disc seeks every audio
 * chip (including the OGG layer) outputs zeros; buffering that silence and
 * submitting it to RetroArch would cause ~seek-duration audio lag after every
 * seek — the most visible symptom being attract-mode A/V desync in Space Ace.
 * Sound-chip audio that happens to be non-zero is unaffected by this check.
 */
void libretro_audio_post(uint8_t *stream, int len)
{
    if (!s_audio_mutex) return;

    const int16_t *src = reinterpret_cast<const int16_t *>(stream);
    int n = len / (int)sizeof(int16_t);

    /* Fast scan: exit early if the whole callback buffer is silence */
    bool has_audio = false;
    for (int i = 0; i < n; ++i) {
        if (src[i]) { has_audio = true; break; }
    }

    if (has_audio) {
        SDL_LockMutex(s_audio_mutex);
        /* Write in at most 2 contiguous segments to handle ring wrap-around */
        int contig = AUDIO_RING_CAP - s_audio_head;
        int n1 = (n < contig) ? n : contig;
        memcpy(&s_audio_ring[s_audio_head], src, (size_t)n1 * sizeof(int16_t));
        if (n > n1)
            memcpy(s_audio_ring, src + n1, (size_t)(n - n1) * sizeof(int16_t));
        s_audio_head = (s_audio_head + n) % AUDIO_RING_CAP;
        if (s_audio_count + n <= AUDIO_RING_CAP) {
            s_audio_count += n;
        } else {
            /* Overflow: advance tail past overwritten data */
            int overflow = s_audio_count + n - AUDIO_RING_CAP;
            s_audio_tail = (s_audio_tail + overflow) % AUDIO_RING_CAP;
            s_audio_count = AUDIO_RING_CAP;
        }
        SDL_UnlockMutex(s_audio_mutex);
    }

    memset(stream, 0, len); /* silence the SDL audio device */
}

/*
 * Called (via ldp_vldp_set_audio_play_hook) at the start of every
 * ldp_vldp::audio_play().  Discards silence that accumulated in the ring
 * during the preceding disc seek so RetroArch does not receive ~50 ms of
 * silence before the real OGG samples, which would cause a systematic
 * audio-lag-behind-video after every seek (most noticeable in attract loops).
 */
void libretro_audio_flush()
{
    if (!s_audio_mutex) return;
    SDL_LockMutex(s_audio_mutex);
    s_audio_head = s_audio_tail = s_audio_count = 0;
    SDL_UnlockMutex(s_audio_mutex);
}

/*
 * Called from SDL_check_input() (io/input.cpp, guarded by LIBRETRO_CORE).
 * Reads the shared input state and fires input_enable / input_disable on
 * transitions, mirroring what process_keydown/process_keyup would do.
 */
void libretro_process_input()
{
    if (!s_input_mutex || !g_game) return;

    SDL_LockMutex(s_input_mutex);
    uint32_t cur = s_pad_state;
    SDL_UnlockMutex(s_input_mutex);

    uint32_t changed = cur ^ s_pad_prev;
    if (!changed) return;

    for (int i = 0; i < k_btn_map_size; ++i) {
        uint32_t bit = 1u << k_btn_map[i].hypseus_id;
        if (!(changed & bit)) continue;

        if (cur & bit)
            g_game->input_enable((uint8_t)k_btn_map[i].hypseus_id, NOMOUSE);
        else
            g_game->input_disable((uint8_t)k_btn_map[i].hypseus_id, NOMOUSE);
    }
    s_pad_prev = cur;

    /* ------------------------------------------------------------------
     * Mouse / pointer / lightgun — only for games that declare mouse use
     * ---------------------------------------------------------------- */
    if (!g_game->get_mouse_enabled()) return;

    SDL_LockMutex(s_input_mutex);
    bool    moved = s_mouse_moved;
    int     mx    = s_mouse_x;
    int     my    = s_mouse_y;
    Sint16  dx    = s_mouse_dx;
    Sint16  dy    = s_mouse_dy;
    uint8_t btns  = s_mouse_btns;
    uint8_t prev  = s_mouse_prev;
    s_mouse_dx = s_mouse_dy = 0;
    s_mouse_moved = false;
    s_mouse_prev  = btns;
    SDL_UnlockMutex(s_input_mutex);

    if (moved)
        g_game->OnMouseMotion((Uint16)mx, (Uint16)my, dx, dy, NOMOUSE);

    uint8_t mchanged = btns ^ prev;
    if (mchanged & 1) {
        if (btns & 1) g_game->input_enable(SWITCH_BUTTON1, NOMOUSE);
        else          g_game->input_disable(SWITCH_BUTTON1, NOMOUSE);
    }
    if (mchanged & 2) {
        if (btns & 2) g_game->input_enable(SWITCH_BUTTON2, NOMOUSE);
        else          g_game->input_disable(SWITCH_BUTTON2, NOMOUSE);
    }
}

/* =========================================================================
 * Emulation thread entry
 * ====================================================================== */
static int emu_thread_func(void * /*unused*/)
{
    g_game->start(); /* blocks until set_quitflag() */
    return 0;
}

/* =========================================================================
 * Helpers
 * ====================================================================== */
static void drain_audio()
{
    if (!s_audio_mutex || !audio_batch_cb) return;

    /* Allow up to 4 video-frames of audio backlog.  Beyond that we drop the
     * oldest samples to prevent the ring from growing stale and causing lag.
     * 44100 Hz / 59.94 fps * 2 ch * 4 frames ≈ 5886 int16_t; round up. */
    static const int k_max = 5888;
    static int16_t tmp[5888];
    int avail = 0;

    SDL_LockMutex(s_audio_mutex);
    if (s_audio_count > k_max) {
        int drop = s_audio_count - k_max;
        s_audio_tail  = (s_audio_tail + drop) % AUDIO_RING_CAP;
        s_audio_count -= drop;
    }
    avail = s_audio_count & ~1; /* round down to stereo pairs */
    if (avail > 0) {
        /* Copy in at most 2 contiguous segments to handle ring wrap-around */
        int contig = AUDIO_RING_CAP - s_audio_tail;
        int n1 = (avail < contig) ? avail : contig;
        memcpy(tmp, &s_audio_ring[s_audio_tail], (size_t)n1 * sizeof(int16_t));
        if (avail > n1)
            memcpy(tmp + n1, s_audio_ring, (size_t)(avail - n1) * sizeof(int16_t));
        s_audio_tail  = (s_audio_tail + avail) % AUDIO_RING_CAP;
        s_audio_count -= avail;
    }
    SDL_UnlockMutex(s_audio_mutex);

    if (avail >= 2)
        audio_batch_cb(tmp, (size_t)(avail / 2));
}

/*
 * Parse a .commands text file into a vector of argument strings, then
 * build argc/argv suitable for parse_cmd_line().
 * The file may use spaces and/or newlines as separators.
 * Prepends "hypseus" as argv[0].
 */
static bool parse_commands_file(const char *path,
                                std::vector<std::string> &out_args)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;

    out_args.clear();
    out_args.push_back("hypseus");

    char buf[4096] = {};
    while (fgets(buf, sizeof(buf), f)) {
        /* tokenise each line */
        char *tok = strtok(buf, " \t\r\n");
        while (tok) {
            if (tok[0] != '\0')
                out_args.push_back(tok);
            tok = strtok(nullptr, " \t\r\n");
        }
    }
    fclose(f);
    return out_args.size() > 1; /* must have at least one arg besides argv[0] */
}

/* =========================================================================
 * Libretro API implementation
 * ====================================================================== */

void retro_set_environment(retro_environment_t cb)
{
    environ_cb = cb;

    /* Signal that we require a game to be loaded */
    bool no_game = false;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);

    cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &k_core_options_v2);
}

void retro_set_video_refresh(retro_video_refresh_t cb)  { video_cb       = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb)    { audio_cb       = cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb)        { input_poll_cb  = cb; }
void retro_set_input_state(retro_input_state_t cb)      { input_state_cb = cb; }

unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_get_system_info(struct retro_system_info *info)
{
    memset(info, 0, sizeof(*info));
    info->library_name     = "Hypseus Singe";
    info->library_version  = hypseus_VERSION;
    info->valid_extensions = "commands|daphne";
    info->need_fullpath    = true;
    info->block_extract    = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
    memset(info, 0, sizeof(*info));
    info->geometry.base_width   = (unsigned)s_vid_w;
    info->geometry.base_height  = (unsigned)s_vid_h;
    info->geometry.max_width    = 1920;
    info->geometry.max_height   = 1080;
    info->geometry.aspect_ratio = (float)s_vid_w / (float)s_vid_h;
    /* VLDP runs at 29.97 * 2 = 59.94 fields/s (VBLANKS_PER_KILOSECOND = 59940).
     * Declaring the actual rate lets RetroArch compute the correct audio/video
     * ratio (44100/59.94 ≈ 735.74 samples/frame) and prevents the ~1 ms/s
     * A/V drift that occurs when 60.0 is declared instead. */
    info->timing.fps            = 29.97 * 2.0;
    info->timing.sample_rate    = sound::FREQ;
}

void retro_init(void)
{
    /* Create synchronisation primitives */
    s_frame_produced = SDL_CreateSemaphore(0);
    s_frame_consumed = SDL_CreateSemaphore(0);
    s_audio_mutex    = SDL_CreateMutex();
    s_input_mutex    = SDL_CreateMutex();
    s_audio_head = s_audio_tail = s_audio_count = 0;
}

void retro_deinit(void)
{
    if (s_frame_produced) { SDL_DestroySemaphore(s_frame_produced); s_frame_produced = nullptr; }
    if (s_frame_consumed) { SDL_DestroySemaphore(s_frame_consumed); s_frame_consumed = nullptr; }
    if (s_audio_mutex)    { SDL_DestroyMutex(s_audio_mutex);        s_audio_mutex    = nullptr; }
    if (s_input_mutex)    { SDL_DestroyMutex(s_input_mutex);        s_input_mutex    = nullptr; }
    s_vid_w = 640; s_vid_h = 480;
}

bool retro_load_game(const struct retro_game_info *info)
{
    if (!info || !info->path) return false;

    s_last_game_path = info->path;

    /* ------------------------------------------------------------------ */
    /* 1. Resolve directories                                              */
    /* ------------------------------------------------------------------ */
    const char *sys_dir = nullptr;
    environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sys_dir);
    if (!sys_dir) sys_dir = ".";

    /* Normalise game path early so we can derive the parent directory */
    std::string dir_path_tmp = info->path;
    while (!dir_path_tmp.empty() &&
           (dir_path_tmp.back() == '/' || dir_path_tmp.back() == '\\'))
        dir_path_tmp.pop_back();

    /* Parent of the .daphne directory (contains roms/, vldp/, …)        */
    size_t parent_slash = dir_path_tmp.find_last_of("/\\");
    std::string parent_dir = (parent_slash != std::string::npos)
                             ? dir_path_tmp.substr(0, parent_slash) : ".";

    /* homedir = <system_dir>/hypseus  (user data: fonts, logs, ram, roms…) */
    std::string homedir = std::string(sys_dir) + "/hypseus";

    /* datadir: first candidate that has the hypseus LED BMPs wins.
     * Checking for pics/led0.bmp (not just pics/) avoids selecting a
     * daphne-compatible pics/ directory that lacks hypseus-specific assets.
     * Injected as -datadir so hypseus chdir()s there, making ALL relative
     * asset paths (pics/, bezels/, …) resolve correctly. */
    std::vector<std::string> datadir_candidates = {
        std::string(sys_dir) + "/hypseus",
        "/usr/share/hypseus",
        "/usr/share/daphne",
        parent_dir,
    };
    std::string datadir;
    for (auto &c : datadir_candidates) {
        struct stat st;
        std::string led = c + "/pics/led0.bmp";
        if (stat(led.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            datadir = c;
            break;
        }
    }

    /* romdir = <parent>/roms if that directory exists, otherwise unset   */
    std::string roms_in_parent = parent_dir + "/roms";
    std::string romdir;
    {
        struct stat st;
        if (stat(roms_in_parent.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
            romdir = roms_in_parent;
    }

    /* ------------------------------------------------------------------ */
    /* 2. Locate .commands and build argv                                  */
    /*                                                                     */
    /* Accepted layouts:                                                   */
    /*   a) <game>.commands           — parse directly                     */
    /*   b) <game>.daphne/ (or dir)   — look for <game>.commands inside,  */
    /*                                   then auto-generate from directory */
    /*   c) <game>.zip / <game>.zlua  — Singe zip ROM: use -zlua directly */
    /* ------------------------------------------------------------------ */

    /* dir_path already normalised above */
    std::string dir_path = dir_path_tmp;

    /* Extract game name: basename of path without extension */
    size_t slash_pos  = dir_path.find_last_of("/\\");
    std::string base  = (slash_pos != std::string::npos)
                        ? dir_path.substr(slash_pos + 1) : dir_path;
    size_t dot_pos    = base.rfind('.');
    std::string gamename = (dot_pos != std::string::npos)
                           ? base.substr(0, dot_pos) : base;

    /* Lowercase file extension helper */
    auto lower_ext = [](const std::string &s) -> std::string {
        size_t d = s.rfind('.');
        if (d == std::string::npos) return "";
        std::string e = s.substr(d);
        for (auto &c : e) c = (char)tolower((unsigned char)c);
        return e;
    };
    std::string path_ext = lower_ext(base);

    /* Detect whether info->path points to a zip/zlua file directly.
     * In that case the adjacent sibling files (framefile, etc.) live in
     * parent_dir, not inside dir_path. */
    bool is_zip_path = (path_ext == ".zip" || path_ext == ".zlua");

    /* Directory used to search for sibling files (.commands, .txt, .singe…) */
    std::string search_dir = is_zip_path ? parent_dir : dir_path;

    std::vector<std::string> str_args;

    auto file_exists = [](const std::string &p) {
        struct stat st;
        return (stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode));
    };

    /* Case (a): path IS the .commands file, or <search_dir>/<game>.commands */
    std::string cmd_file = search_dir + "/" + gamename + ".commands";
    bool loaded_commands = parse_commands_file(cmd_file.c_str(), str_args);

    /* Also try path itself as commands — but only for text-like extensions
     * (.commands or no extension).  Never try to parse a binary zip as text. */
    if (!loaded_commands && path_ext != ".zip" && path_ext != ".zlua")
        loaded_commands = parse_commands_file(dir_path.c_str(), str_args);

    if (!loaded_commands)
    {
        /* Auto-generate from detected content */
        str_args.clear();
        str_args.push_back("hypseus");

        std::string framefile    = search_dir + "/" + gamename + ".txt";
        std::string singe_script = search_dir + "/" + gamename + ".singe";
        std::string zlua_pkg     = search_dir + "/" + gamename + ".zlua";
        std::string zip_pkg      = search_dir + "/" + gamename + ".zip";

        if (is_zip_path) {
            /* Case (c): path is a .zip or .zlua — use it directly with -zlua */
            str_args.push_back("singe");
            str_args.push_back("vldp");
            str_args.push_back("-framefile"); str_args.push_back(framefile);
            str_args.push_back("-zlua");      str_args.push_back(dir_path);
        } else if (file_exists(singe_script)) {
            str_args.push_back("singe");
            str_args.push_back("vldp");
            str_args.push_back("-framefile"); str_args.push_back(framefile);
            str_args.push_back("-script");    str_args.push_back(singe_script);
        } else if (file_exists(zlua_pkg)) {
            str_args.push_back("singe");
            str_args.push_back("vldp");
            str_args.push_back("-framefile"); str_args.push_back(framefile);
            str_args.push_back("-zlua");      str_args.push_back(zlua_pkg);
        } else if (file_exists(zip_pkg)) {
            /* .zip inside a game directory treated the same as .zlua */
            str_args.push_back("singe");
            str_args.push_back("vldp");
            str_args.push_back("-framefile"); str_args.push_back(framefile);
            str_args.push_back("-zlua");      str_args.push_back(zip_pkg);
        } else {
            /* DAPHNE ROM game */
            str_args.push_back(gamename);
            str_args.push_back("vldp");
            str_args.push_back("-framefile"); str_args.push_back(framefile);
        }
    }

    /* Inject -homedir unless already present */
    bool has_homedir = false;
    for (auto &a : str_args)
        if (a == "-homedir") { has_homedir = true; break; }
    if (!has_homedir) {
        str_args.push_back("-homedir");
        str_args.push_back(homedir);
    }

    /* Inject -romdir when ROMs live alongside the .daphne directory */
    bool has_romdir = false;
    for (auto &a : str_args)
        if (a == "-romdir") { has_romdir = true; break; }
    if (!has_romdir && !romdir.empty()) {
        str_args.push_back("-romdir");
        str_args.push_back(romdir);
    }

    /* Inject -datadir so hypseus can chdir() there and resolve pics/ paths */
    bool has_datadir = false;
    for (auto &a : str_args)
        if (a == "-datadir") { has_datadir = true; break; }
    if (!has_datadir && !datadir.empty()) {
        str_args.push_back("-datadir");
        str_args.push_back(datadir);
    }

    /* Inject -singedir for Singe games: enables espath so lua_espath()
     * rewrites "singe/gamename/file" -> "<parent_dir>/gamename.daphne/file"
     * using the parent of the .daphne directory as the base. */
    {
        bool has_singedir = false;
        bool is_singe = false;
        for (auto &a : str_args) {
            if (a == "-singedir") { has_singedir = true; break; }
            if (a == "singe") is_singe = true;
        }
        if (!has_singedir && is_singe && !parent_dir.empty()) {
            str_args.push_back("-singedir");
            str_args.push_back(parent_dir + "/");
        }
    }

    /* ------------------------------------------------------------------ */
    /* 3b. Apply core options (inject args not already present)           */
    /* ------------------------------------------------------------------ */
    {
        auto has_arg = [&](const char *a) -> bool {
            for (auto &s : str_args) if (s == a) return true;
            return false;
        };
        auto getcore = [&](const char *key) -> const char * {
            struct retro_variable v = { key, nullptr };
            environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &v);
            return v.value ? v.value : "";
        };

        if (!has_arg("-fastboot") && strcmp(getcore("hypseus_fastboot"), "enabled") == 0)
            str_args.push_back("-fastboot");

        if (!has_arg("-blank_searches") && strcmp(getcore("hypseus_blank_searches"), "enabled") == 0)
            str_args.push_back("-blank_searches");

        if (!has_arg("-blank_skips") && strcmp(getcore("hypseus_blank_skips"), "enabled") == 0)
            str_args.push_back("-blank_skips");

        if (!has_arg("-cheat") && strcmp(getcore("hypseus_cheat"), "enabled") == 0)
            str_args.push_back("-cheat");

        {
            const char *v = getcore("hypseus_seek_frames");
            if (!has_arg("-seek_frames_per_ms") && v[0] && strcmp(v, "0") != 0) {
                str_args.push_back("-seek_frames_per_ms");
                str_args.push_back(std::string(v));
            }
        }
        {
            const char *v = getcore("hypseus_latency");
            if (!has_arg("-latency") && v[0] && strcmp(v, "0") != 0) {
                str_args.push_back("-latency");
                str_args.push_back(std::string(v));
            }
        }
    }

    /* Logging enabled for debug — disable with -nolog once stable */
    /* str_args.push_back("-nolog"); */

    /* Build C-style argv */
    std::vector<char *> argv;
    argv.reserve(str_args.size());
    for (auto &s : str_args) argv.push_back(const_cast<char *>(s.c_str()));
    int argc = (int)argv.size();

    /* ------------------------------------------------------------------ */
    /* 3. Announce pixel format                                             */
    /* ------------------------------------------------------------------ */
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);

#define LR_LOG(msg) fprintf(stderr, "[hypseus-libretro] " msg "\n")
#define LR_FAIL(step) do { \
    fprintf(stderr, "[hypseus-libretro] FAILED at: " step " — %s\n", SDL_GetError()); \
} while(0)

    /* ------------------------------------------------------------------ */
    /* 4. Initialise SDL subsystems needed by hypseus                      */
    /* ------------------------------------------------------------------ */
    LR_LOG("SDL_Init");
    if (SDL_Init(SDL_INIT_NOPARACHUTE) < 0) {
        LR_FAIL("SDL_Init"); return false;
    }
    if (SDL_InitSubSystem(SDL_INIT_TIMER | SDL_INIT_AUDIO) < 0) {
        LR_FAIL("SDL_InitSubSystem"); return false;
    }

    int imgflags = IMG_INIT_PNG | IMG_INIT_JPG;
    if (IMG_Init(imgflags) != imgflags) {
        LR_FAIL("IMG_Init"); SDL_QuitSubSystem(SDL_INIT_TIMER | SDL_INIT_AUDIO); return false;
    }
    if (TTF_Init() != 0) {
        LR_FAIL("TTF_Init"); IMG_Quit(); SDL_QuitSubSystem(SDL_INIT_TIMER | SDL_INIT_AUDIO); return false;
    }

    /* ------------------------------------------------------------------ */
    /* 5. Parse command line (allocates g_game and g_ldp)                  */
    /* ------------------------------------------------------------------ */
    LR_LOG("parse_cmd_line");
    if (!parse_cmd_line(argc, argv.data())) {
        LR_FAIL("parse_cmd_line");
        TTF_Quit(); IMG_Quit(); SDL_QuitSubSystem(SDL_INIT_TIMER | SDL_INIT_AUDIO);
        return false;
    }

    /* ------------------------------------------------------------------ */
    /* 6. Initialise plog so subsystem LOGE/LOGI calls are visible         */
    /* ------------------------------------------------------------------ */
    {
        std::string log_dir = g_homedir.get_homedir() + "/logs";
        mkdir(log_dir.c_str(), 0755);
        std::string log_file = log_dir + "/hypseus.log";
        static plog::ColorConsoleAppender<plog::TxtFormatter> s_plog_console;
        static plog::RollingFileAppender<plog::TxtFormatter>  s_plog_file(log_file.c_str(), 500000, 3);
        plog::init(plog::debug, &s_plog_console).addAppender(&s_plog_file);
        fprintf(stderr, "[hypseus-libretro] plog -> %s\n", log_file.c_str());
    }

    /* ------------------------------------------------------------------ */
    /* 7. Hook audio post-processing before sound::init()                  */
    /* ------------------------------------------------------------------ */
    sound::set_audio_capture_hook(libretro_audio_post);
    video::vid_set_frame_ready_hook(libretro_submit_video);
    SDL_set_libretro_input(libretro_process_input);
    ldp_vldp_set_audio_play_hook(libretro_audio_flush);
    {
        struct retro_variable v = { "hypseus_scoreboard", nullptr };
        environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &v);
        video::vid_set_scoreboard_visible(!v.value || strcmp(v.value, "disabled") != 0);
    }

    /* ------------------------------------------------------------------ */
    /* 8. Initialise subsystems in the same order as standalone main()     */
    /* ------------------------------------------------------------------ */
    LR_LOG("video::load_bmps");
    if (!video::load_bmps()) {
        /* Non-fatal: scoreboard BMPs are only needed for ROM games.
         * Singe games use their own Lua overlay system and work without them. */
        fprintf(stderr, "[hypseus-libretro] WARNING: load_bmps failed — "
                        "scoreboard overlays will be unavailable\n");
    }

    /* Use a small SDL audio buffer (512 samples ≈ 11.6 ms) so the capture
     * callback fires roughly every retro_run() call (16.67 ms) instead of
     * every 46 ms (2048-sample default).  This makes audio delivery to
     * audio_batch_cb() more uniform and reduces A/V desync caused by bursty
     * sample delivery. */
    sound::set_buf_size(512);

    LR_LOG("sound::init");
    if (!sound::init()) {
        LR_FAIL("sound::init");
        video::free_bmps();
        TTF_Quit(); IMG_Quit(); SDL_QuitSubSystem(SDL_INIT_TIMER | SDL_INIT_AUDIO);
        return false;
    }

    LR_LOG("SDL_input_init");
    if (!SDL_input_init()) {
        LR_FAIL("SDL_input_init");
        sound::shutdown();
        video::free_bmps();
        TTF_Quit(); IMG_Quit(); SDL_QuitSubSystem(SDL_INIT_TIMER | SDL_INIT_AUDIO);
        return false;
    }

    LR_LOG("g_game->load_roms");
    if (!g_game->load_roms()) {
        LR_FAIL("g_game->load_roms");
        SDL_input_shutdown();
        sound::shutdown();
        video::free_bmps();
        TTF_Quit(); IMG_Quit(); SDL_QuitSubSystem(SDL_INIT_TIMER | SDL_INIT_AUDIO);
        return false;
    }

    LR_LOG("g_game->init_video");
    if (!g_game->init_video()) {
        LR_FAIL("g_game->init_video");
        SDL_input_shutdown();
        sound::shutdown();
        video::free_bmps();
        TTF_Quit(); IMG_Quit(); SDL_QuitSubSystem(SDL_INIT_TIMER | SDL_INIT_AUDIO);
        return false;
    }

    SDL_Delay(200); /* let video settle before VLDP init */

    LR_LOG("g_ldp->pre_init");
    if (!g_ldp->pre_init()) {
        LR_FAIL("g_ldp->pre_init");
        g_game->shutdown_video();
        SDL_input_shutdown();
        sound::shutdown();
        video::free_bmps();
        TTF_Quit(); IMG_Quit(); SDL_QuitSubSystem(SDL_INIT_TIMER | SDL_INIT_AUDIO);
        return false;
    }

    LR_LOG("g_game->pre_init");
    if (!g_game->pre_init()) {
        LR_FAIL("g_game->pre_init");
        g_ldp->pre_shutdown();
        g_game->shutdown_video();
        SDL_input_shutdown();
        sound::shutdown();
        video::free_bmps();
        TTF_Quit(); IMG_Quit(); SDL_QuitSubSystem(SDL_INIT_TIMER | SDL_INIT_AUDIO);
        return false;
    }
    LR_LOG("all init done, starting emu thread");

    /* ------------------------------------------------------------------ */
    /* 9. Update geometry with actual video dimensions                      */
    /* SET_GEOMETRY is used (not SET_SYSTEM_AV_INFO) to avoid triggering   */
    /* a full video-driver reinitialization in the frontend.               */
    /* ------------------------------------------------------------------ */
    s_vid_w = (int)video::get_viewport_width();
    s_vid_h = (int)video::get_viewport_height();
    if (s_vid_w <= 0) s_vid_w = (int)video::get_video_width();
    if (s_vid_h <= 0) s_vid_h = (int)video::get_video_height();
    if (s_vid_w <= 0) s_vid_w = 640;
    if (s_vid_h <= 0) s_vid_h = 480;

    struct retro_game_geometry geom;
    geom.base_width   = (unsigned)s_vid_w;
    geom.base_height  = (unsigned)s_vid_h;
    geom.max_width    = 1920;
    geom.max_height   = 1080;
    geom.aspect_ratio = (float)s_vid_w / (float)s_vid_h;
    environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geom);

    /* ------------------------------------------------------------------ */
    /* 10. Launch emulation thread                                          */
    /* ------------------------------------------------------------------ */
    reset_quitflag();
    s_emu_started = true;
    s_emu_thread  = SDL_CreateThread(emu_thread_func, "hypseus_emu", nullptr);
    if (!s_emu_thread) {
        s_emu_started = false;
        set_quitflag();
        return false;
    }

    return true;
}

void retro_unload_game(void)
{
    if (!s_emu_started) return;

    /* Ask the emulation loop to stop */
    set_quitflag();

    /* Unblock the emu thread if it is waiting on s_frame_consumed */
    if (s_frame_consumed) SDL_SemPost(s_frame_consumed);

    /* Wait for the emulation thread to finish */
    int thread_result = 0;
    SDL_WaitThread(s_emu_thread, &thread_result);
    s_emu_thread  = nullptr;
    s_emu_started = false;

    /* Drain semaphore counts left over from the stopped thread so that
     * a subsequent retro_load_game starts with clean synchronisation state. */
    if (s_frame_produced) while (SDL_SemTryWait(s_frame_produced) == 0) {}
    if (s_frame_consumed) while (SDL_SemTryWait(s_frame_consumed) == 0) {}

    /* Free the video copy buffer; it will be reallocated on next load. */
    delete[] s_video_buf;
    s_video_buf    = nullptr;
    s_video_buf_sz = 0;

    /* Reset per-session state for clean restart */
    reset_quitflag();
    s_first_run  = true;
    s_vid_w = 640; s_vid_h = 480;
    s_pad_state  = 0;
    s_pad_prev   = 0;
    s_mouse_x = s_mouse_y = 0;
    s_mouse_dx = s_mouse_dy = 0;
    s_mouse_moved = false;
    s_mouse_btns = s_mouse_prev = 0;
    SDL_AtomicSet(&s_geometry_changed, 0);
    if (s_audio_mutex) {
        SDL_LockMutex(s_audio_mutex);
        s_audio_head = s_audio_tail = s_audio_count = 0;
        SDL_UnlockMutex(s_audio_mutex);
    }

    /* Shutdown subsystems */
    if (g_game) g_game->pre_shutdown();
    if (g_ldp)  g_ldp->pre_shutdown();

    if (g_game) g_game->shutdown_video();
    SDL_input_shutdown();
    sound::shutdown();
    video::free_bmps();
    video::deinit_display();
    video::shutdown_display();

    if (g_game) { delete g_game; g_game = nullptr; }
    if (g_ldp)  { delete g_ldp;  g_ldp  = nullptr; }

    TTF_Quit();
    IMG_Quit();
    /* Do NOT call SDL_Quit() here: that would destroy the semaphores and
     * mutexes allocated in retro_init(), which are not recreated until
     * retro_deinit()/retro_init() cycle. Quit only the subsystems started
     * in retro_load_game so they can be re-initialised on the next load. */
    SDL_QuitSubSystem(SDL_INIT_TIMER | SDL_INIT_AUDIO);
}

void retro_run(void)
{
    /* On the very first retro_run after load, flush silence accumulated
     * during the ~400 ms between sound::init() and here. */
    if (s_first_run) {
        s_first_run = false;
        SDL_LockMutex(s_audio_mutex);
        s_audio_head = s_audio_tail = s_audio_count = 0;
        SDL_UnlockMutex(s_audio_mutex);
    }

    /* ------------------------------------------------------------------
     * 1. Poll input from frontend and update shared state
     * ---------------------------------------------------------------- */
    input_poll_cb();

    uint32_t new_state = 0;
    for (int i = 0; i < k_btn_map_size; ++i) {
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, k_btn_map[i].retro_id))
            new_state |= (1u << k_btn_map[i].hypseus_id);
    }

    SDL_LockMutex(s_input_mutex);
    s_pad_state = new_state;

    /* Poll mouse/pointer/lightgun only for games that use pointing devices.
     * Unconditional polling is expensive on the udev driver (aarch64). */
    if (g_game && g_game->get_mouse_enabled()) {
        Sint16 mdx = (Sint16)input_state_cb(0, RETRO_DEVICE_MOUSE,   0, RETRO_DEVICE_ID_MOUSE_X);
        Sint16 mdy = (Sint16)input_state_cb(0, RETRO_DEVICE_MOUSE,   0, RETRO_DEVICE_ID_MOUSE_Y);
        bool   mbl = (bool)  input_state_cb(0, RETRO_DEVICE_MOUSE,   0, RETRO_DEVICE_ID_MOUSE_LEFT);
        bool   mbr = (bool)  input_state_cb(0, RETRO_DEVICE_MOUSE,   0, RETRO_DEVICE_ID_MOUSE_RIGHT);

        int16_t px = (int16_t)input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
        int16_t py = (int16_t)input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);
        bool    pp = (bool)   input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED);

        int16_t lgx = (int16_t)input_state_cb(0, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X);
        int16_t lgy = (int16_t)input_state_cb(0, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y);
        bool    lgt = (bool)   input_state_cb(0, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER);
        bool    lgo = (bool)   input_state_cb(0, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN);

        bool moved = false;
        if (lgt && !lgo) {
            int nx = std::max(0, std::min(s_vid_w - 1, (int)((lgx + 32767) * s_vid_w / 65534)));
            int ny = std::max(0, std::min(s_vid_h - 1, (int)((lgy + 32767) * s_vid_h / 65534)));
            if (nx != s_mouse_x || ny != s_mouse_y) {
                s_mouse_dx = (Sint16)(nx - s_mouse_x); s_mouse_dy = (Sint16)(ny - s_mouse_y);
                s_mouse_x = nx; s_mouse_y = ny; moved = true;
            }
        } else if (pp) {
            int nx = std::max(0, std::min(s_vid_w - 1, (int)((px + 32767) * s_vid_w / 65534)));
            int ny = std::max(0, std::min(s_vid_h - 1, (int)((py + 32767) * s_vid_h / 65534)));
            if (nx != s_mouse_x || ny != s_mouse_y) {
                s_mouse_dx = (Sint16)(nx - s_mouse_x); s_mouse_dy = (Sint16)(ny - s_mouse_y);
                s_mouse_x = nx; s_mouse_y = ny; moved = true;
            }
        } else if (mdx || mdy) {
            s_mouse_dx = mdx; s_mouse_dy = mdy;
            s_mouse_x = std::max(0, std::min(s_vid_w - 1, s_mouse_x + mdx));
            s_mouse_y = std::max(0, std::min(s_vid_h - 1, s_mouse_y + mdy));
            moved = true;
        }
        s_mouse_moved = moved;
        uint8_t btns = 0;
        if (mbl || pp || (lgt && !lgo)) btns |= 1;
        if (mbr)                        btns |= 2;
        s_mouse_btns = btns;
    }

    SDL_UnlockMutex(s_input_mutex);

    /* ------------------------------------------------------------------
     * 2. Wait for a video frame from the emulation thread (50 ms timeout)
     * ---------------------------------------------------------------- */
    bool got_frame = (SDL_SemWaitTimeout(s_frame_produced, 50) == 0);

    /* Notify the frontend of a geometry change before submitting the first
     * frame at the new resolution, so the video driver can resize its
     * buffers before it receives pixel data of unexpected dimensions. */
    if (SDL_AtomicGet(&s_geometry_changed)) {
        SDL_AtomicSet(&s_geometry_changed, 0);
        struct retro_game_geometry geom;
        geom.base_width   = (unsigned)s_vid_w;
        geom.base_height  = (unsigned)s_vid_h;
        geom.max_width    = 1920;
        geom.max_height   = 1080;
        geom.aspect_ratio = (float)s_vid_w / (float)s_vid_h;
        environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geom);
    }

    if (got_frame) {
        /* Copy g_lr_surface into s_video_buf, then post s_frame_consumed
         * *before* calling video_cb().  This decouples the emu thread from
         * video_cb() latency (GPU texture upload can take ~1 ms on some
         * drivers).  The emu unblocks after the memcpy (~0.15 ms) instead
         * of after video_cb() completes, recovering the ~4 fps gap vs
         * standalone at 60 Hz.  s_video_buf is only touched here (retro_run
         * thread) so no locking is needed. */
        SDL_Surface *lr = video::get_lr_surface();
        if (lr && lr->pixels) {
            size_t frame_sz = (size_t)lr->pitch * (size_t)lr->h;
            if (frame_sz > s_video_buf_sz) {
                delete[] s_video_buf;
                s_video_buf    = new uint32_t[(frame_sz + 3) / 4];
                s_video_buf_sz = frame_sz;
            }
            memcpy(s_video_buf, lr->pixels, frame_sz);
            SDL_SemPost(s_frame_consumed);
            video_cb(s_video_buf, (unsigned)s_vid_w, (unsigned)s_vid_h, (size_t)lr->pitch);
        } else {
            SDL_SemPost(s_frame_consumed);
            video_cb(NULL, (unsigned)s_vid_w, (unsigned)s_vid_h, 0);
        }
    } else {
        /* Timeout: signal duplicate frame */
        video_cb(NULL, (unsigned)s_vid_w, (unsigned)s_vid_h, 0);
    }

    /* ------------------------------------------------------------------
     * 3. Submit buffered audio
     * ---------------------------------------------------------------- */
    drain_audio();
}

/* =========================================================================
 * Minimal stubs for required but unused API functions
 * ====================================================================== */

void retro_reset(void)
{
    /* RetroArch "Restart" calls retro_reset(), not retro_unload + retro_load.
     * cpu::reset() alone is not enough — VLDP, audio, input, and all game
     * state need a full restart.  Perform a full unload/reload cycle using
     * the stored game path from the last retro_load_game() call. */
    if (s_last_game_path.empty()) return;

    retro_unload_game();

    struct retro_game_info info = {};
    info.path = s_last_game_path.c_str();
    retro_load_game(&info);
}

void retro_set_controller_port_device(unsigned /*port*/, unsigned /*device*/) {}

/* -------------------------------------------------------------------------
 * Save states
 * Format: [header 20B] + SS_MAX_CPUS × [context 128B + mem 64KB]
 * ---------------------------------------------------------------------- */
static const uint32_t SS_MAGIC    = 0x48595053; /* "HYPS" */
static const uint32_t SS_VERSION  = 1;
static const int      SS_MAX_CPUS = 4;
static const size_t   SS_MEM_SIZE = 0x10000;    /* 64 KB — covers full Z80/6809 space */

size_t retro_serialize_size(void)
{
    return 5 * sizeof(uint32_t)
           + (size_t)SS_MAX_CPUS * ((size_t)cpu::MAX_CONTEXT_SIZE + SS_MEM_SIZE);
}

bool retro_serialize(void *data, size_t size)
{
    if (size < retro_serialize_size() || !s_emu_started || !g_ldp) return false;

    /* No pause/delay: retro_serialize is called every frame when rewind is
     * enabled. A best-effort snapshot (possible microsecond drift) is fine. */
    uint8_t *p = (uint8_t *)data;
    auto w32 = [&](uint32_t v) { memcpy(p, &v, 4); p += 4; };

    /* Count active CPU slots */
    uint32_t ncpus = 0;
    for (int i = 0; i < SS_MAX_CPUS; ++i)
        if (cpu::get_struct(i)) ++ncpus;

    w32(SS_MAGIC);
    w32(SS_VERSION);
    w32(ncpus);
    w32(g_ldp->get_current_frame());
    w32((uint32_t)g_ldp->get_status());

    for (int i = 0; i < SS_MAX_CPUS; ++i) {
        cpu::def *c = cpu::get_struct(i);
        uint8_t ctx[cpu::MAX_CONTEXT_SIZE] = {};
        uint8_t mem[SS_MEM_SIZE]           = {};
        if (c) {
            if (c->getcontext_callback) c->getcontext_callback(ctx);
            if (c->mem) memcpy(mem, c->mem, SS_MEM_SIZE);
        }
        memcpy(p, ctx, cpu::MAX_CONTEXT_SIZE); p += cpu::MAX_CONTEXT_SIZE;
        memcpy(p, mem, SS_MEM_SIZE);           p += SS_MEM_SIZE;
    }

    return true;
}

bool retro_unserialize(const void *data, size_t size)
{
    if (size < retro_serialize_size() || !s_emu_started || !g_ldp) return false;

    cpu::pause();
    /* Brief yield so the emu thread completes its current instruction
     * before we overwrite registers and memory. */
    SDL_Delay(5);

    const uint8_t *p = (const uint8_t *)data;
    auto r32 = [&]() -> uint32_t { uint32_t v; memcpy(&v, p, 4); p += 4; return v; };

    if (r32() != SS_MAGIC || r32() != SS_VERSION) { cpu::unpause(); return false; }

    uint32_t ncpus     = r32();
    uint32_t ldp_frame = r32();
    int      ldp_stat  = (int)r32();

    for (int i = 0; i < SS_MAX_CPUS; ++i) {
        const uint8_t *ctx = p; p += cpu::MAX_CONTEXT_SIZE;
        const uint8_t *mem = p; p += SS_MEM_SIZE;
        cpu::def *c = cpu::get_struct(i);
        if (c && (uint32_t)i < ncpus) {
            if (c->setcontext_callback) c->setcontext_callback((void *)ctx);
            if (c->mem) memcpy(c->mem, mem, SS_MEM_SIZE);
        }
    }

    /* Restore laserdisc position */
    char frame_str[16];
    snprintf(frame_str, sizeof(frame_str), "%u", (unsigned)ldp_frame);
    g_ldp->pre_search(frame_str, true); /* blocking seek */
    if (ldp_stat == LDP_PLAYING) g_ldp->pre_play();

    cpu::unpause();
    return true;
}

void   retro_cheat_reset(void) {}
void   retro_cheat_set(unsigned, bool, const char *) {}

bool   retro_load_game_special(unsigned, const struct retro_game_info *, size_t)
{ return false; }

unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

void  *retro_get_memory_data(unsigned) { return nullptr; }
size_t retro_get_memory_size(unsigned) { return 0; }
