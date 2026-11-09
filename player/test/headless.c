/* Headless libretro runner used to test the gba2cia player in the Azahar
 * 3DS emulator core without a window. It boots a .cxi, runs N frames and
 * writes the emulator's video output as PPM images.
 *
 *   headless <core.dylib> <game.cxi> <outdir> <frames> <every> [key=value ...]
 *
 * Extra key=value pairs override core options (for example
 * citra_graphics_api=Software).
 */
#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct retro_game_info { const char* path; const void* data; size_t size; const char* meta; };
struct retro_variable { const char* key; const char* value; };
struct retro_system_info { const char* library_name; const char* library_version; const char* valid_extensions; bool need_fullpath; bool block_extract; };
struct retro_game_geometry { unsigned base_width, base_height, max_width, max_height; float aspect_ratio; };
struct retro_system_timing { double fps; double sample_rate; };
struct retro_system_av_info { struct retro_game_geometry geometry; struct retro_system_timing timing; };
struct retro_log_callback { void (*log)(int level, const char* fmt, ...); };

typedef bool (*env_t)(unsigned, void*);
typedef void (*video_t)(const void*, unsigned, unsigned, size_t);
typedef void (*audio_t)(int16_t, int16_t);
typedef size_t (*audio_batch_t)(const int16_t*, size_t);
typedef void (*poll_t)(void);
typedef int16_t (*state_t)(unsigned, unsigned, unsigned, unsigned);

static const char* sysdir;
static const char* outdir;
static int pixfmt = 0;
static unsigned long frameNo;
static int every = 60;

#define MAX_OPTS 128
static struct { char key[96]; char value[128]; } opts[MAX_OPTS];
static int nopts;

static void setOpt(const char* key, const char* value, bool overwrite) {
	for (int i = 0; i < nopts; ++i) {
		if (!strcmp(opts[i].key, key)) {
			if (overwrite) snprintf(opts[i].value, sizeof(opts[i].value), "%s", value);
			return;
		}
	}
	if (nopts < MAX_OPTS) {
		snprintf(opts[nopts].key, sizeof(opts[nopts].key), "%s", key);
		snprintf(opts[nopts].value, sizeof(opts[nopts].value), "%s", value);
		++nopts;
	}
}

static void logcb(int level, const char* fmt, ...) {
	if (level < 1) return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static bool env(unsigned cmd, void* data) {
	cmd &= 0xFFFF;
	switch (cmd) {
	case 9:  /* GET_SYSTEM_DIRECTORY */
	case 30: /* GET_CORE_ASSETS_DIRECTORY */
	case 31: /* GET_SAVE_DIRECTORY */
		*(const char**) data = sysdir;
		return true;
	case 10: /* SET_PIXEL_FORMAT */
		pixfmt = *(int*) data;
		return pixfmt == 1 || pixfmt == 2;
	case 16: { /* SET_VARIABLES: "Description; default|other|..." */
		for (const struct retro_variable* v = data; v->key; ++v) {
			const char* semi = strchr(v->value, ';');
			char def[128] = "";
			if (semi) {
				const char* s = semi + 1;
				while (*s == ' ') ++s;
				size_t n = strcspn(s, "|");
				snprintf(def, sizeof(def), "%.*s", (int) n, s);
			}
			fprintf(stderr, "[opt] %s = %s   (%s)\n", v->key, def, v->value);
			setOpt(v->key, def, false);
		}
		return true;
	}
	case 15: { /* GET_VARIABLE */
		struct retro_variable* v = data;
		for (int i = 0; i < nopts; ++i) {
			if (!strcmp(opts[i].key, v->key)) {
				v->value = opts[i].value;
				return true;
			}
		}
		v->value = NULL;
		return false;
	}
	case 17: /* GET_VARIABLE_UPDATE */
		*(bool*) data = false;
		return true;
	case 27: /* GET_LOG_INTERFACE */
		((struct retro_log_callback*) data)->log = logcb;
		return true;
	case 52: /* GET_CORE_OPTIONS_VERSION: use legacy variables */
		*(unsigned*) data = 0;
		return true;
	case 32: /* SET_SYSTEM_AV_INFO */
	case 37: /* SET_GEOMETRY */
	case 11: /* SET_INPUT_DESCRIPTORS */
	case 35: /* SET_CONTROLLER_INFO */
	case 3:  /* GET_CAN_DUPE */
		if (cmd == 3) *(bool*) data = true;
		return true;
	case 14: /* SET_HW_RENDER: refuse, use the software renderer */
		fprintf(stderr, "[env] core asked for hardware rendering; refused\n");
		return false;
	default:
		return false;
	}
}

static void video(const void* buf, unsigned w, unsigned h, size_t pitch) {
	++frameNo;
	if (!buf || (frameNo % every) != 0) return;
	char path[512];
	snprintf(path, sizeof(path), "%s/frame_%05lu.ppm", outdir, frameNo);
	FILE* f = fopen(path, "wb");
	if (!f) return;
	fprintf(f, "P6\n%u %u\n255\n", w, h);
	for (unsigned y = 0; y < h; ++y) {
		const uint8_t* row = (const uint8_t*) buf + y * pitch;
		for (unsigned x = 0; x < w; ++x) {
			uint8_t rgb[3];
			if (pixfmt == 1) {
				uint32_t p = ((const uint32_t*) row)[x];
				rgb[0] = p >> 16; rgb[1] = p >> 8; rgb[2] = p;
			} else {
				uint16_t p = ((const uint16_t*) row)[x];
				rgb[0] = ((p >> 11) & 31) << 3; rgb[1] = ((p >> 5) & 63) << 2; rgb[2] = (p & 31) << 3;
			}
			fwrite(rgb, 1, 3, f);
		}
	}
	fclose(f);
	fprintf(stderr, "[frame] %lu %ux%u\n", frameNo, w, h);
}

static void audio(int16_t l, int16_t r) { (void) l; (void) r; }
static size_t audioBatch(const int16_t* d, size_t n) { (void) d; return n; }
static void poll(void) {}
static int16_t inputState(unsigned port, unsigned device, unsigned index, unsigned id) {
	(void) port; (void) device; (void) index; (void) id;
	return 0;
}

#define LOAD(name) name = dlsym(core, #name); if (!name) { fprintf(stderr, "missing %s\n", #name); return 1; }

int main(int argc, char** argv) {
	if (argc < 6) {
		fprintf(stderr, "usage: headless core game outdir frames every [key=value...]\n");
		return 2;
	}
	void* core = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	if (!core) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
	const char* game = argv[2];
	outdir = argv[3];
	sysdir = argv[3];
	long frames = strtol(argv[4], NULL, 10);
	every = atoi(argv[5]);
	for (int i = 6; i < argc; ++i) {
		char* eq = strchr(argv[i], '=');
		if (eq) { *eq = 0; setOpt(argv[i], eq + 1, true); }
	}

	void (*retro_set_environment)(env_t); void (*retro_set_video_refresh)(video_t);
	void (*retro_set_audio_sample)(audio_t); void (*retro_set_audio_sample_batch)(audio_batch_t);
	void (*retro_set_input_poll)(poll_t); void (*retro_set_input_state)(state_t);
	void (*retro_init)(void); void (*retro_run)(void); void (*retro_deinit)(void);
	bool (*retro_load_game)(const struct retro_game_info*);
	void (*retro_get_system_av_info)(struct retro_system_av_info*);
	LOAD(retro_set_environment); LOAD(retro_set_video_refresh); LOAD(retro_set_audio_sample);
	LOAD(retro_set_audio_sample_batch); LOAD(retro_set_input_poll); LOAD(retro_set_input_state);
	LOAD(retro_init); LOAD(retro_run); LOAD(retro_deinit); LOAD(retro_load_game); LOAD(retro_get_system_av_info);

	retro_set_environment(env);
	retro_set_video_refresh(video);
	retro_set_audio_sample(audio);
	retro_set_audio_sample_batch(audioBatch);
	retro_set_input_poll(poll);
	retro_set_input_state(inputState);
	retro_init();
	struct retro_game_info info = { game, NULL, 0, NULL };
	if (!retro_load_game(&info)) { fprintf(stderr, "load failed\n"); return 1; }
	struct retro_system_av_info av;
	retro_get_system_av_info(&av);
	fprintf(stderr, "[av] %ux%u fps %.2f\n", av.geometry.base_width, av.geometry.base_height, av.timing.fps);
	for (long i = 0; i < frames; ++i) {
		retro_run();
	}
	fprintf(stderr, "[done] %lu frames\n", frameNo);
	_exit(0);
}
