/* libretro.h - API standard libretro (version 1) - subset used by hypseus-singe */
#ifndef LIBRETRO_H
#define LIBRETRO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RETRO_API_VERSION 1

/* -------------------------------------------------------------------------
 * Devices
 * ---------------------------------------------------------------------- */
#define RETRO_DEVICE_NONE       0
#define RETRO_DEVICE_JOYPAD     1
#define RETRO_DEVICE_MOUSE      2
#define RETRO_DEVICE_KEYBOARD   3
#define RETRO_DEVICE_LIGHTGUN   4
#define RETRO_DEVICE_ANALOG     5
#define RETRO_DEVICE_POINTER    6

/* RetroPad buttons (RETRO_DEVICE_JOYPAD) */
#define RETRO_DEVICE_ID_JOYPAD_B        0
#define RETRO_DEVICE_ID_JOYPAD_Y        1
#define RETRO_DEVICE_ID_JOYPAD_SELECT   2
#define RETRO_DEVICE_ID_JOYPAD_START    3
#define RETRO_DEVICE_ID_JOYPAD_UP       4
#define RETRO_DEVICE_ID_JOYPAD_DOWN     5
#define RETRO_DEVICE_ID_JOYPAD_LEFT     6
#define RETRO_DEVICE_ID_JOYPAD_RIGHT    7
#define RETRO_DEVICE_ID_JOYPAD_A        8
#define RETRO_DEVICE_ID_JOYPAD_X        9
#define RETRO_DEVICE_ID_JOYPAD_L        10
#define RETRO_DEVICE_ID_JOYPAD_R        11
#define RETRO_DEVICE_ID_JOYPAD_L2       12
#define RETRO_DEVICE_ID_JOYPAD_R2       13
#define RETRO_DEVICE_ID_JOYPAD_L3       14
#define RETRO_DEVICE_ID_JOYPAD_R3       15

/* Mouse (RETRO_DEVICE_MOUSE) */
#define RETRO_DEVICE_ID_MOUSE_X               0
#define RETRO_DEVICE_ID_MOUSE_Y               1
#define RETRO_DEVICE_ID_MOUSE_LEFT            2
#define RETRO_DEVICE_ID_MOUSE_RIGHT           3
#define RETRO_DEVICE_ID_MOUSE_MIDDLE          6

/* Pointer (RETRO_DEVICE_POINTER) */
#define RETRO_DEVICE_ID_POINTER_X             0
#define RETRO_DEVICE_ID_POINTER_Y             1
#define RETRO_DEVICE_ID_POINTER_PRESSED       2

/* Lightgun (RETRO_DEVICE_LIGHTGUN) */
#define RETRO_DEVICE_ID_LIGHTGUN_TRIGGER       2
#define RETRO_DEVICE_ID_LIGHTGUN_RELOAD        8
#define RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X     13
#define RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y     14
#define RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN 15

/* -------------------------------------------------------------------------
 * Regions
 * ---------------------------------------------------------------------- */
#define RETRO_REGION_NTSC 0
#define RETRO_REGION_PAL  1

/* -------------------------------------------------------------------------
 * Memory IDs
 * ---------------------------------------------------------------------- */
#define RETRO_MEMORY_SAVE_RAM    0
#define RETRO_MEMORY_RTC         1
#define RETRO_MEMORY_SYSTEM_RAM  2
#define RETRO_MEMORY_VIDEO_RAM   3

/* -------------------------------------------------------------------------
 * Pixel formats
 * ---------------------------------------------------------------------- */
enum retro_pixel_format {
    RETRO_PIXEL_FORMAT_0RGB1555 = 0,  /* default, 15-bit */
    RETRO_PIXEL_FORMAT_XRGB8888 = 1,  /* 32-bit, native endian */
    RETRO_PIXEL_FORMAT_RGB565   = 2,  /* 16-bit, native endian */
    RETRO_PIXEL_FORMAT_UNKNOWN  = INT32_MAX
};

/* -------------------------------------------------------------------------
 * Environment callback commands  (values from libretro-common libretro.h)
 * ---------------------------------------------------------------------- */
#define RETRO_ENVIRONMENT_SET_ROTATION                  1
#define RETRO_ENVIRONMENT_GET_OVERSCAN                  2
#define RETRO_ENVIRONMENT_GET_CAN_DUPE                  3
#define RETRO_ENVIRONMENT_SET_MESSAGE                   6
#define RETRO_ENVIRONMENT_SHUTDOWN                      7
#define RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL         8
#define RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY          9
#define RETRO_ENVIRONMENT_SET_PIXEL_FORMAT              10
#define RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS         11
#define RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK         12
#define RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE    13
#define RETRO_ENVIRONMENT_SET_HW_RENDER                 14
#define RETRO_ENVIRONMENT_GET_VARIABLE                  15
#define RETRO_ENVIRONMENT_SET_VARIABLES                 16
#define RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE           17
#define RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME           18
#define RETRO_ENVIRONMENT_GET_LIBRETRO_PATH             19
#define RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK       21
#define RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK            22
#define RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE          23
#define RETRO_ENVIRONMENT_GET_INPUT_DEVICE_CAPABILITIES 24
#define RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE          25
#define RETRO_ENVIRONMENT_GET_CAMERA_INTERFACE          26
#define RETRO_ENVIRONMENT_GET_LOG_INTERFACE             27
#define RETRO_ENVIRONMENT_GET_PERF_INTERFACE            28
#define RETRO_ENVIRONMENT_GET_LOCATION_INTERFACE        29
#define RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY     30
#define RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY            31
#define RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO            32
#define RETRO_ENVIRONMENT_SET_PROC_ADDRESS_CALLBACK     33
#define RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO            34
#define RETRO_ENVIRONMENT_SET_CONTROLLER_INFO           35
#define RETRO_ENVIRONMENT_SET_MEMORY_MAPS               36
#define RETRO_ENVIRONMENT_SET_GEOMETRY                  37
#define RETRO_ENVIRONMENT_GET_USERNAME                  38
#define RETRO_ENVIRONMENT_GET_LANGUAGE                  39
#define RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER 40
#define RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT         44
#define RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2           67
#define RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL      68

#define RETRO_NUM_CORE_OPTION_VALUES_MAX 128

/* -------------------------------------------------------------------------
 * Structs
 * ---------------------------------------------------------------------- */
struct retro_message {
    const char *msg;
    unsigned    frames;
};

struct retro_game_info {
    const char *path;   /* Full path of game */
    const void *data;   /* Memory buffer of loaded file (may be NULL) */
    size_t      size;   /* Size of memory buffer */
    const char *meta;   /* String of implementation-specific metadata */
};

struct retro_system_info {
    const char *library_name;
    const char *library_version;
    const char *valid_extensions;
    bool        need_fullpath;
    bool        block_extract;
};

struct retro_game_geometry {
    unsigned base_width;
    unsigned base_height;
    unsigned max_width;
    unsigned max_height;
    float    aspect_ratio;
};

struct retro_system_timing {
    double fps;
    double sample_rate;
};

struct retro_system_av_info {
    struct retro_game_geometry geometry;
    struct retro_system_timing timing;
};

struct retro_variable {
    const char *key;
    const char *value;
};

struct retro_core_option_value {
    const char *value;
    const char *label; /* human-readable label, may be NULL */
};

struct retro_core_option_v2_category {
    const char *key;
    const char *desc;
    const char *info;
};

struct retro_core_option_v2_definition {
    const char *key;
    const char *desc;
    const char *desc_categorized; /* may be NULL */
    const char *info;             /* may be NULL */
    const char *info_categorized; /* may be NULL */
    const char *category_key;     /* may be NULL */
    struct retro_core_option_value values[RETRO_NUM_CORE_OPTION_VALUES_MAX];
    const char *default_value;
};

struct retro_core_options_v2 {
    struct retro_core_option_v2_category    *categories; /* NULL = no categories */
    struct retro_core_option_v2_definition  *definitions;
};

struct retro_input_descriptor {
    unsigned    port;
    unsigned    device;
    unsigned    index;
    unsigned    id;
    const char *description;
};

struct retro_controller_description {
    const char *desc;
    unsigned    id;
};

struct retro_controller_info {
    const struct retro_controller_description *types;
    unsigned num_types;
};

enum retro_log_level {
    RETRO_LOG_DEBUG = 0,
    RETRO_LOG_INFO,
    RETRO_LOG_WARN,
    RETRO_LOG_ERROR,
    RETRO_LOG_DUMMY = INT32_MAX
};

typedef void (*retro_log_printf_t)(enum retro_log_level level, const char *fmt, ...);

struct retro_log_callback {
    retro_log_printf_t log;
};

/* -------------------------------------------------------------------------
 * Callback typedefs
 * ---------------------------------------------------------------------- */
typedef void (*retro_video_refresh_t)(const void *data, unsigned width,
                                      unsigned height, size_t pitch);

typedef void (*retro_audio_sample_t)(int16_t left, int16_t right);

typedef size_t (*retro_audio_sample_batch_t)(const int16_t *data, size_t frames);

typedef void (*retro_input_poll_t)(void);

typedef int16_t (*retro_input_state_t)(unsigned port, unsigned device,
                                        unsigned index, unsigned id);

typedef bool (*retro_environment_t)(unsigned cmd, void *data);

/* -------------------------------------------------------------------------
 * Core function declarations
 * ---------------------------------------------------------------------- */
void retro_init(void);
void retro_deinit(void);
unsigned retro_api_version(void);
void retro_get_system_info(struct retro_system_info *info);
void retro_get_system_av_info(struct retro_system_av_info *info);
void retro_set_environment(retro_environment_t cb);
void retro_set_video_refresh(retro_video_refresh_t cb);
void retro_set_audio_sample(retro_audio_sample_t cb);
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb);
void retro_set_input_poll(retro_input_poll_t cb);
void retro_set_input_state(retro_input_state_t cb);
void retro_set_controller_port_device(unsigned port, unsigned device);
void retro_reset(void);
void retro_run(void);
size_t retro_serialize_size(void);
bool retro_serialize(void *data, size_t size);
bool retro_unserialize(const void *data, size_t size);
void retro_cheat_reset(void);
void retro_cheat_set(unsigned index, bool enabled, const char *code);
bool retro_load_game(const struct retro_game_info *game);
bool retro_load_game_special(unsigned game_type,
                              const struct retro_game_info *info,
                              size_t num_info);
void retro_unload_game(void);
unsigned retro_get_region(void);
void *retro_get_memory_data(unsigned id);
size_t retro_get_memory_size(unsigned id);

#ifdef __cplusplus
}
#endif
#endif /* LIBRETRO_H */
