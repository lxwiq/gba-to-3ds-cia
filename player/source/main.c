/* SPDX-License-Identifier: MPL-2.0
 *
 * gba2cia player: runs one GBA ROM, bundled in the title's RomFS, with the
 * mGBA core. The game fills the top screen; the bottom screen is a touch
 * panel (ui.c). Audio and frame upload follow mGBA's own 3DS frontend.
 *
 * RomFS layout written by gba2cia:
 *   romfs:/game.gba      the ROM
 *   romfs:/player.cfg    key=value settings (config.c)
 *   romfs:/initial.sav   optional save copied to the SD card on first launch
 *
 * Files on the SD card:
 *   sdmc:/3ds/gba2cia/<title id>/game.sav     the save, raw GBA format
 *   sdmc:/3ds/gba2cia/<title id>/game.sav.bak the save as it was at the last launch
 *   sdmc:/3ds/gba2cia/<title id>/state.ss0    quick save state
 *
 * On the first launch, a save from another emulator found on the SD card
 * (file name containing the game title, right size) is copied in; the
 * original file is left untouched.
 */
#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>

#include <mgba/core/blip_buf.h>
#include <mgba/core/core.h>
#include <mgba/core/interface.h>
#include <mgba/gba/core.h>
#include <mgba/internal/gba/audio.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/input.h>
#include <mgba/internal/gba/overrides.h>
#include <mgba/internal/gba/savedata.h>
#include <mgba-util/vfs.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "config.h"
#include "pokemon.h"
#include "ui.h"

#define AUDIO_SAMPLES 384
#define DSP_BUFFERS 4

static struct PlayerConfig cfg;
static struct UiState ui;
static struct mCore* core;
static color_t* outputBuffer;
static C3D_Tex gameTex;
/* The GX transfer (no vertical flip) writes GBA row 0 at v = 1.0 and row 159
 * at v = 1 - 160/256. citro2d wants top > bottom (otherwise it treats the
 * subtexture as rotated), which this is. Checked in the Azahar emulator. */
static Tex3DS_SubTexture gameSub = { 240, 160, 0.0f, 1.0f, 240.0f / 256.0f, 1.0f - 160.0f / 256.0f };
static C3D_RenderTarget* top;
static C3D_RenderTarget* bottom;

/* Party icons: six 32x32 icons side by side in a 256x64 RGBA8 texture. */
static C3D_Tex iconTex;
static uint32_t* iconBuffer;
static Tex3DS_SubTexture iconSub[PARTY_MAX];
static uint16_t iconSpecies[PARTY_MAX];
/* Test mode: screenshots without game artwork. */
static bool testNoIcons;
/* Party and trainer read from the save file at launch. */
static struct Party saveParty;
static struct Trainer saveTrainer;

static bool hasSound;
static ndspWaveBuf dspBuffer[DSP_BUFFERS];
static int16_t* audioData;
static int bufferId;

static char saveDir[64];
static char savePath[96];
static char statePath[96];
static uint32_t frame;
static uint32_t virtualKeys;
static int virtualKeyFrames;
static aptHookCookie aptCookie;

/* ---------------------------------------------------------------- VFile on stdio (RomFS) */

struct VFileStdio {
	struct VFile d;
	FILE* f;
};

static bool vfsClose(struct VFile* vf) {
	struct VFileStdio* v = (struct VFileStdio*) vf;
	fclose(v->f);
	free(v);
	return true;
}

static off_t vfsSeek(struct VFile* vf, off_t offset, int whence) {
	struct VFileStdio* v = (struct VFileStdio*) vf;
	if (fseek(v->f, offset, whence) != 0) {
		return -1;
	}
	return ftell(v->f);
}

static ssize_t vfsRead(struct VFile* vf, void* buffer, size_t size) {
	return fread(buffer, 1, size, ((struct VFileStdio*) vf)->f);
}

static ssize_t vfsReadline(struct VFile* vf, char* buffer, size_t size) {
	return fgets(buffer, size, ((struct VFileStdio*) vf)->f) ? (ssize_t) strlen(buffer) : 0;
}

static ssize_t vfsWrite(struct VFile* vf, const void* buffer, size_t size) {
	UNUSED(vf);
	UNUSED(buffer);
	UNUSED(size);
	return -1;
}

static void* vfsMap(struct VFile* vf, size_t size, int flags) {
	UNUSED(flags);
	void* mem = malloc(size);
	if (mem) {
		vf->seek(vf, 0, SEEK_SET);
		vf->read(vf, mem, size);
	}
	return mem;
}

static void vfsUnmap(struct VFile* vf, void* memory, size_t size) {
	UNUSED(vf);
	UNUSED(size);
	free(memory);
}

static void vfsTruncate(struct VFile* vf, size_t size) {
	UNUSED(vf);
	UNUSED(size);
}

static ssize_t vfsSize(struct VFile* vf) {
	struct VFileStdio* v = (struct VFileStdio*) vf;
	long pos = ftell(v->f);
	fseek(v->f, 0, SEEK_END);
	long size = ftell(v->f);
	fseek(v->f, pos, SEEK_SET);
	return size;
}

static bool vfsSync(struct VFile* vf, void* buffer, size_t size) {
	UNUSED(vf);
	UNUSED(buffer);
	UNUSED(size);
	return true;
}

static struct VFile* openStdio(const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	struct VFileStdio* v = calloc(1, sizeof(*v));
	v->f = f;
	v->d.close = vfsClose;
	v->d.seek = vfsSeek;
	v->d.read = vfsRead;
	v->d.readline = vfsReadline;
	v->d.write = vfsWrite;
	v->d.map = vfsMap;
	v->d.unmap = vfsUnmap;
	v->d.truncate = vfsTruncate;
	v->d.size = vfsSize;
	v->d.sync = vfsSync;
	return &v->d;
}

/* ---------------------------------------------------------------- Screenshots (test mode) */

/* Framebuffers are BGR8, stored rotated: column x of the screen is a run of
 * 240 pixels from bottom to top. */
static void writeScreenBmp(gfxScreen_t screen, const char* path) {
	u16 fbW, fbH;
	u8* fb = gfxGetFramebuffer(screen, GFX_LEFT, &fbW, &fbH);
	int width = fbH;  /* 400 or 320 */
	int height = fbW; /* 240 */
	FILE* f = fopen(path, "wb");
	if (!f) {
		return;
	}
	int rowSize = (width * 3 + 3) & ~3;
	uint32_t dataSize = rowSize * height;
	uint8_t header[54] = { 'B', 'M' };
	uint32_t fileSize = 54 + dataSize;
	memcpy(&header[2], &fileSize, 4);
	header[10] = 54;
	header[14] = 40;
	memcpy(&header[18], &width, 4);
	memcpy(&header[22], &height, 4);
	header[26] = 1;
	header[28] = 24;
	memcpy(&header[34], &dataSize, 4);
	fwrite(header, 1, 54, f);
	uint8_t* row = calloc(1, rowSize);
	/* BMP rows go bottom-up: screen row y = height - 1 first. */
	for (int y = height - 1; y >= 0; --y) {
		for (int x = 0; x < width; ++x) {
			const u8* p = &fb[(x * fbW + (fbW - 1 - y)) * 3];
			row[x * 3 + 0] = p[0];
			row[x * 3 + 1] = p[1];
			row[x * 3 + 2] = p[2];
		}
		fwrite(row, 1, rowSize, f);
	}
	free(row);
	fclose(f);
}

static void takeScreenshots(void) {
	mkdir("sdmc:/3ds", 0777);
	mkdir("sdmc:/3ds/gba2cia", 0777);
	mkdir("sdmc:/3ds/gba2cia/test", 0777);
	char path[96];
	snprintf(path, sizeof(path), "sdmc:/3ds/gba2cia/test/%05lu_top.bmp", (unsigned long) frame);
	writeScreenBmp(GFX_TOP, path);
	snprintf(path, sizeof(path), "sdmc:/3ds/gba2cia/test/%05lu_bottom.bmp", (unsigned long) frame);
	writeScreenBmp(GFX_BOTTOM, path);
}

/* ---------------------------------------------------------------- Audio */

static void postAudioBuffer(struct mAVStream* stream, blip_t* left, blip_t* right) {
	UNUSED(stream);
	if (!hasSound || ui.speed > 1) {
		blip_clear(left);
		blip_clear(right);
		return;
	}
	int start = bufferId;
	while (dspBuffer[bufferId].status == NDSP_WBUF_QUEUED || dspBuffer[bufferId].status == NDSP_WBUF_PLAYING) {
		bufferId = (bufferId + 1) & (DSP_BUFFERS - 1);
		if (bufferId == start) {
			blip_clear(left);
			blip_clear(right);
			return;
		}
	}
	int16_t* data = &audioData[AUDIO_SAMPLES * 2 * bufferId];
	memset(&dspBuffer[bufferId], 0, sizeof(dspBuffer[bufferId]));
	dspBuffer[bufferId].data_pcm16 = data;
	dspBuffer[bufferId].nsamples = AUDIO_SAMPLES;
	blip_read_samples(left, data, AUDIO_SAMPLES, true);
	blip_read_samples(right, data + 1, AUDIO_SAMPLES, true);
	DSP_FlushDataCache(data, AUDIO_SAMPLES * 2 * sizeof(int16_t));
	ndspChnWaveBufAdd(0, &dspBuffer[bufferId]);
}

static struct mAVStream avStream = { .postAudioBuffer = postAudioBuffer };

static void audioInit(void) {
	hasSound = R_SUCCEEDED(ndspInit());
	if (!hasSound) {
		return;
	}
	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	ndspSetOutputCount(1);
	ndspChnReset(0);
	ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
	ndspChnSetInterp(0, NDSP_INTERP_NONE);
	ndspChnSetRate(0, 0x8000);
	ndspChnWaveBufClear(0);
	audioData = linearMemAlign(AUDIO_SAMPLES * DSP_BUFFERS * 2 * sizeof(int16_t), 0x80);
	memset(audioData, 0, AUDIO_SAMPLES * DSP_BUFFERS * 2 * sizeof(int16_t));
	memset(dspBuffer, 0, sizeof(dspBuffer));
}

/* ---------------------------------------------------------------- Saves */

static void flushSave(void) {
	if (!core) {
		return;
	}
	struct GBA* gba = core->board;
	struct GBASavedata* sd = &gba->memory.savedata;
	if (sd->vf && sd->data && (sd->mapMode & MAP_WRITE)) {
		sd->vf->sync(sd->vf, sd->data, GBASavedataSize(sd));
		GBASavedataRTCWrite(sd);
	}
}

/* A save the game never wrote to: missing, empty, or erased flash (all 0xFF
 * or all 0x00). The first launch creates such a file, so a bundled starting
 * save must be allowed to replace it. */
static bool saveIsBlank(const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f) {
		return true;
	}
	static u8 buf[0x4000];
	size_t n;
	bool sawFF = false, saw00 = false, other = false;
	while (!other && (n = fread(buf, 1, sizeof(buf), f)) > 0) {
		for (size_t i = 0; i < n; ++i) {
			if (buf[i] == 0xFF) {
				sawFF = true;
			} else if (buf[i] == 0x00) {
				saw00 = true;
			} else {
				other = true;
				break;
			}
		}
	}
	fclose(f);
	return !other && !(sawFF && saw00);
}

static void copyFile(const char* from, const char* to) {
	FILE* in = fopen(from, "rb");
	if (!in) {
		return;
	}
	FILE* out = fopen(to, "wb");
	if (out) {
		static u8 buf[0x4000];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
			fwrite(buf, 1, n, out);
		}
		fclose(out);
	}
	fclose(in);
}

static void saveState(void) {
	size_t size = core->stateSize(core);
	void* buf = malloc(size);
	if (!buf) {
		uiToast(&ui, "Not enough memory for a save state");
		return;
	}
	bool ok = core->saveState(core, buf);
	FILE* f = ok ? fopen(statePath, "wb") : NULL;
	if (f) {
		ok = fwrite(buf, 1, size, f) == size;
		fclose(f);
	} else {
		ok = false;
	}
	free(buf);
	uiToast(&ui, ok ? "State saved" : "Could not save the state");
}

static void loadState(void) {
	FILE* f = fopen(statePath, "rb");
	if (!f) {
		uiToast(&ui, "No saved state yet");
		return;
	}
	size_t size = core->stateSize(core);
	void* buf = malloc(size);
	bool ok = buf && fread(buf, 1, size, f) == size && core->loadState(core, buf);
	fclose(f);
	free(buf);
	uiToast(&ui, ok ? "State loaded" : "Could not load the state");
}

/* ---------------------------------------------------------------- Save import */

/* Finds a save made by another emulator or injector somewhere on the SD card
 * (mGBA, RetroArch, a ROM folder...) the first time the game starts. Only a
 * copy is used; the original file is never modified. */

struct SaveCandidate {
	char path[256];
	time_t mtime;
	bool found;
};

static size_t expectedSaveSize(int type) {
	switch (type) {
	case SAVEDATA_FLASH1M: return 0x20000;
	case SAVEDATA_FLASH512: return 0x10000;
	case SAVEDATA_SRAM: return 0x8000;
	case SAVEDATA_EEPROM: return 0x2000;
	case SAVEDATA_EEPROM512: return 0x200;
	default: return 0;
	}
}

/* Lower-case words of the title worth matching ("unbound" for "Pokemon Unbound"). */
static int titleKeywords(char words[4][32]) {
	int n = 0;
	char tmp[65];
	snprintf(tmp, sizeof(tmp), "%s", cfg.title);
	char* save = NULL;
	for (char* w = strtok_r(tmp, " -_.:", &save); w && n < 4; w = strtok_r(NULL, " -_.:", &save)) {
		if (strlen(w) < 3 || !strcasecmp(w, "pokemon") || !strcasecmp(w, "the") || !strcasecmp(w, "version")) {
			continue;
		}
		int i = 0;
		for (; w[i] && i < 31; ++i) {
			words[n][i] = tolower((unsigned char) w[i]);
		}
		words[n][i] = 0;
		++n;
	}
	return n;
}

static bool nameMatches(const char* name, char words[4][32], int nwords) {
	char lower[256];
	int i = 0;
	for (; name[i] && i < 255; ++i) {
		lower[i] = tolower((unsigned char) name[i]);
	}
	lower[i] = 0;
	const char* ext = strrchr(lower, '.');
	if (!ext || (strcmp(ext, ".sav") && strcmp(ext, ".srm") && strcmp(ext, ".saveram") && strcmp(ext, ".sa1"))) {
		return false;
	}
	for (int w = 0; w < nwords; ++w) {
		if (!strstr(lower, words[w])) {
			return false;
		}
	}
	return nwords > 0;
}

static void scanDir(const char* dir, int depth, size_t want, char words[4][32], int nwords, struct SaveCandidate* best) {
	DIR* d = opendir(dir);
	if (!d) {
		return;
	}
	struct dirent* e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') {
			continue;
		}
		char path[256];
		snprintf(path, sizeof(path), "%s%s%s", dir, dir[strlen(dir) - 1] == '/' ? "" : "/", e->d_name);
		struct stat st;
		if (stat(path, &st) != 0) {
			continue;
		}
		if (S_ISDIR(st.st_mode)) {
			/* System folders hold no emulator saves and can be huge. */
			if (depth > 0 && strcasecmp(e->d_name, "Nintendo 3DS") && strcasecmp(e->d_name, "luma") &&
			    strcasecmp(e->d_name, "gba2cia")) {
				scanDir(path, depth - 1, want, words, nwords, best);
			}
			continue;
		}
		if ((size_t) st.st_size < want || (size_t) st.st_size > want + 0x200 || !nameMatches(e->d_name, words, nwords)) {
			continue;
		}
		if (!best->found || st.st_mtime > best->mtime) {
			snprintf(best->path, sizeof(best->path), "%s", path);
			best->mtime = st.st_mtime;
			best->found = true;
		}
	}
	closedir(d);
}

static void importSaveFromSd(void) {
	size_t want = expectedSaveSize(cfg.saveType);
	char words[4][32];
	int nwords = titleKeywords(words);
	if (!want || !nwords) {
		return;
	}
	struct SaveCandidate best = { 0 };
	scanDir("sdmc:/", 4, want, words, nwords, &best);
	if (!best.found) {
		return;
	}
	copyFile(best.path, savePath);
	/* Show the file name only, shortened to fit the bottom screen. */
	const char* name = strrchr(best.path, '/');
	name = name ? name + 1 : best.path;
	char msg[96];
	if (strlen(name) > 44) {
		snprintf(msg, sizeof(msg), "Save imported: %.41s...", name);
	} else {
		snprintf(msg, sizeof(msg), "Save imported: %s", name);
	}
	uiToast(&ui, msg);
	ui.toastFrames = 600;
}

/* ---------------------------------------------------------------- Lifecycle */

static void aptHookFunc(APT_HookType hook, void* param) {
	UNUSED(param);
	switch (hook) {
	case APTHOOK_ONSUSPEND:
	case APTHOOK_ONSLEEP:
	case APTHOOK_ONEXIT:
		flushSave();
		break;
	default:
		break;
	}
}

/* ---------------------------------------------------------------- Party icons */

static void iconsInit(void) {
	C3D_TexInitVRAM(&iconTex, 256, 64, GPU_RGBA8);
	C3D_TexSetFilter(&iconTex, GPU_NEAREST, GPU_NEAREST);
	C3D_TexSetWrap(&iconTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	iconBuffer = linearMemAlign(256 * 64 * 4, 0x80);
	memset(iconBuffer, 0, 256 * 64 * 4);
	for (int i = 0; i < PARTY_MAX; ++i) {
		/* Same orientation rule as the game picture: row 0 at v = 1. */
		iconSub[i] = (Tex3DS_SubTexture) { 32, 32, i * 32 / 256.0f, 1.0f, (i + 1) * 32 / 256.0f, 1.0f - 32 / 64.0f };
		ui.icons[i] = (C2D_Image) { &iconTex, &iconSub[i] };
		ui.iconValid[i] = false;
		iconSpecies[i] = 0;
	}
}

static void iconsUpdate(void) {
	bool changed = false;
	static uint32_t px[32 * 32];
	for (int i = 0; i < PARTY_MAX; ++i) {
		const struct PartyMember* m = &ui.party.members[i];
		uint16_t species = (ui.party.valid && i < ui.party.count && !m->egg) ? m->species : 0;
		if (species == iconSpecies[i]) {
			continue;
		}
		iconSpecies[i] = species;
		changed = true;
		bool ok = species && !testNoIcons && pokemonIcon(core, species, px);
		ui.iconValid[i] = ok;
		for (int y = 0; y < 32; ++y) {
			for (int x = 0; x < 32; ++x) {
				iconBuffer[y * 256 + i * 32 + x] = ok ? px[y * 32 + x] : 0;
			}
		}
	}
	if (changed) {
		GSPGPU_FlushDataCache(iconBuffer, 256 * 64 * 4);
		C3D_SyncDisplayTransfer((u32*) iconBuffer, GX_BUFFER_DIM(256, 64), iconTex.data, GX_BUFFER_DIM(256, 64),
		                        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
		                            GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_TILED(1));
	}
}

/* ---------------------------------------------------------------- Rendering */

static void drawFrame(bool showGame) {
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	C2D_TargetClear(top, C2D_Color32(0, 0, 0, 0xFF));
	C2D_SceneBegin(top);
	if (showGame) {
		C2D_Image img = { &gameTex, &gameSub };
		switch (ui.scale) {
		case SCALE_FULL:
			C2D_DrawImageAt(img, 0, 0, 0.5f, NULL, 400.0f / 240.0f, 240.0f / 160.0f);
			break;
		case SCALE_ASPECT:
			C2D_DrawImageAt(img, 20, 0, 0.5f, NULL, 1.5f, 1.5f);
			break;
		default:
			C2D_DrawImageAt(img, 80, 40, 0.5f, NULL, 1.0f, 1.0f);
			break;
		}
	}
	C2D_TargetClear(bottom, C2D_Color32(0x16, 0x18, 0x1C, 0xFF));
	C2D_SceneBegin(bottom);
	uiDraw(&ui);
	C3D_FrameEnd(0);
}

static void uploadFrame(void) {
	GSPGPU_FlushDataCache(outputBuffer, 256 * GBA_VIDEO_VERTICAL_PIXELS * sizeof(color_t));
	C3D_SyncDisplayTransfer((u32*) outputBuffer, GX_BUFFER_DIM(256, GBA_VIDEO_VERTICAL_PIXELS), gameTex.data,
	                        GX_BUFFER_DIM(256, 256),
	                        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) |
	                            GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) | GX_TRANSFER_OUT_TILED(1));
}

static void applyScaleFilter(void) {
	GPU_TEXTURE_FILTER_PARAM f = ui.scale == SCALE_NATIVE ? GPU_NEAREST : GPU_LINEAR;
	C3D_TexSetFilter(&gameTex, f, f);
}

static void loadingProgress(size_t done, size_t total, void* context) {
	UNUSED(context);
	static int tick;
	if ((++tick & 15) != 0) {
		return;
	}
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	C2D_TargetClear(top, C2D_Color32(0, 0, 0, 0xFF));
	C2D_TargetClear(bottom, C2D_Color32(0x16, 0x18, 0x1C, 0xFF));
	C2D_SceneBegin(bottom);
	uiDrawLoading(cfg.title, "Loading", total ? (float) done / total : 0);
	C3D_FrameEnd(0);
}

static void fatal(const char* message) {
	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & (KEY_START | KEY_A | KEY_B)) {
			break;
		}
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C2D_TargetClear(top, C2D_Color32(0, 0, 0, 0xFF));
		C2D_TargetClear(bottom, C2D_Color32(0x16, 0x18, 0x1C, 0xFF));
		C2D_SceneBegin(bottom);
		uiDrawLoading("Error", message, 0);
		C3D_FrameEnd(0);
	}
}

/* ---------------------------------------------------------------- Input */

static uint32_t mapKeys(u32 held) {
	uint32_t k = 0;
	if (held & KEY_A) k |= 1 << GBA_KEY_A;
	if (held & KEY_B) k |= 1 << GBA_KEY_B;
	if (held & KEY_SELECT) k |= 1 << GBA_KEY_SELECT;
	if (held & KEY_START) k |= 1 << GBA_KEY_START;
	if (held & (KEY_DRIGHT | KEY_CPAD_RIGHT)) k |= 1 << GBA_KEY_RIGHT;
	if (held & (KEY_DLEFT | KEY_CPAD_LEFT)) k |= 1 << GBA_KEY_LEFT;
	if (held & (KEY_DUP | KEY_CPAD_UP)) k |= 1 << GBA_KEY_UP;
	if (held & (KEY_DDOWN | KEY_CPAD_DOWN)) k |= 1 << GBA_KEY_DOWN;
	if (held & KEY_R) k |= 1 << GBA_KEY_R;
	if (held & KEY_L) k |= 1 << GBA_KEY_L;
	return k;
}

static void pressVirtual(uint32_t keys) {
	virtualKeys = keys;
	virtualKeyFrames = 6;
}

static void handleAction(enum UiAction a) {
	switch (a) {
	case UI_PRESS_START:
		pressVirtual(1 << GBA_KEY_START);
		break;
	case UI_PRESS_SELECT:
		pressVirtual(1 << GBA_KEY_SELECT);
		break;
	case UI_SPEED:
		ui.speed = ui.speed == 1 ? 2 : ui.speed == 2 ? 4 : 1;
		break;
	case UI_SAVE_STATE:
		saveState();
		break;
	case UI_LOAD_STATE:
		if (ui.loadArmed) {
			ui.loadArmed = false;
			loadState();
		} else {
			ui.loadArmed = true;
			ui.loadArmedFrames = 120;
			uiToast(&ui, "Tap LOAD again to load the saved state");
		}
		break;
	case UI_SCREEN:
		ui.scale = (enum ScaleMode) ((ui.scale + 1) % SCALE_MAX);
		applyScaleFilter();
		break;
	default:
		break;
	}
}

/* ---------------------------------------------------------------- Main */

int main(void) {
	romfsInit();
	configLoad(&cfg);

	gfxInit(GSP_BGR8_OES, GSP_BGR8_OES, false);
	if (cfg.test) {
		/* Single buffering, so the framebuffer always holds the last frame
		 * and screenshots are exact. */
		gfxSetDoubleBuffering(GFX_TOP, false);
		gfxSetDoubleBuffering(GFX_BOTTOM, false);
	}
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
	C2D_Prepare();
	top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	uiInit();
	osSetSpeedupEnable(true);
	aptHook(&aptCookie, aptHookFunc, NULL);
	audioInit();

	memset(&ui, 0, sizeof(ui));
	ui.title = cfg.title;
	ui.speed = 1;
	ui.scale = cfg.scale;
	ui.pressed = -1;
	ui.tab = TAB_TEAM;
	ui.sound = hasSound;
	ui.pokemon = pokemonSupported(cfg.gameCode);

	iconsInit();
	C3D_TexInitVRAM(&gameTex, 256, 256, GPU_RGB565);
	C3D_TexSetWrap(&gameTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	applyScaleFilter();
	outputBuffer = linearMemAlign(256 * 256 * sizeof(color_t), 0x80);
	memset(outputBuffer, 0, 256 * 256 * sizeof(color_t));
	uploadFrame();

	snprintf(saveDir, sizeof(saveDir), "sdmc:/3ds/gba2cia/%s", cfg.titleId);
	mkdir("sdmc:/3ds", 0777);
	mkdir("sdmc:/3ds/gba2cia", 0777);
	mkdir(saveDir, 0777);
	snprintf(savePath, sizeof(savePath), "%s/game.sav", saveDir);
	snprintf(statePath, sizeof(statePath), "%s/state.ss0", saveDir);
	if (!saveIsBlank(savePath)) {
		/* Keep the save as it was before this session. */
		char backup[112];
		snprintf(backup, sizeof(backup), "%s.bak", savePath);
		copyFile(savePath, backup);
	} else {
		if (cfg.hasInitialSave) {
			copyFile("romfs:/initial.sav", savePath);
			uiToast(&ui, "Save installed from the title");
		} else {
			importSaveFromSd();
		}
	}

	core = GBACoreCreate();
	if (!core || !core->init(core)) {
		fatal("Could not start the emulator core");
		goto exit;
	}
	mCoreInitConfig(core, NULL);
	struct mCoreOptions opts = {
		.useBios = false,
		.skipBios = true,
		.volume = 0x100,
		.sampleRate = 32768,
		.audioBuffers = AUDIO_SAMPLES,
		.fpsTarget = 60,
	};
	mCoreConfigLoadDefaults(&core->config, &opts);
	mCoreLoadConfig(core);
	core->setVideoBuffer(core, outputBuffer, 256);
	core->setAudioBufferSize(core, AUDIO_SAMPLES);
	core->setAVStream(core, &avStream);

	struct VFile* rom = openStdio("romfs:/game.gba");
	if (!rom || !mCorePreloadVFCB(core, rom, loadingProgress, NULL)) {
		fatal("The game could not be loaded from the title");
		goto exit;
	}
	/* mGBA paths use the SD card archive without the "sdmc:" prefix. */
	char sdSavePath[96];
	snprintf(sdSavePath, sizeof(sdSavePath), "/3ds/gba2cia/%s/game.sav", cfg.titleId);
	struct VFile* save = VFileOpen(sdSavePath, O_CREAT | O_RDWR);
	if (save) {
		core->loadSave(core, save);
	}

	/* The GBA core already provides a wall-clock RTC source (the 3DS clock);
	 * the override below plugs the clock chip into the cartridge. */
	core->reset(core);
	/* Show the party and trainer of the save right away, before the game
	 * itself has loaded them. */
	if (ui.pokemon) {
		FILE* sf = fopen(savePath, "rb");
		if (sf) {
			static uint8_t sav[0x20000];
			size_t n = fread(sav, 1, sizeof(sav), sf);
			fclose(sf);
			pokemonReadSave(core, cfg.gameCode, sav, n, &saveParty, &saveTrainer);
			ui.party = saveParty;
			ui.trainer = saveTrainer;
			ui.fromSave = saveParty.valid || saveTrainer.valid;
		}
	}
	testNoIcons = cfg.test;

	struct GBACartridgeOverride override = {
		.savetype = cfg.saveType,
		.hardware = cfg.rtc ? HW_RTC : HW_NO_OVERRIDE,
		.idleLoop = IDLE_LOOP_NONE,
	};
	memcpy(override.id, cfg.gameCode, 4);
	GBAOverrideApply(core->board, &override);

	double ratio = GBAAudioCalculateRatio(1, 268111856.f / 4481136.f, 1);
	blip_set_rates(core->getAudioChannel(core, 0), core->frequency(core), 32768 * ratio);
	blip_set_rates(core->getAudioChannel(core, 1), core->frequency(core), 32768 * ratio);

	int testIndex = 0;
	bool quit = false;
	while (!quit && aptMainLoop()) {
		hidScanInput();
		u32 held = hidKeysHeld();
		u32 down = hidKeysDown();
		uint32_t keys = mapKeys(held);

		if (down & KEY_TOUCH) {
			touchPosition t;
			hidTouchRead(&t);
			handleAction(uiHitTest(&ui, t.px, t.py));
		}
		if (down & KEY_X) {
			handleAction(UI_SPEED);
		}

		/* Scripted input (test mode). */
		bool shot = false;
		while (cfg.test && testIndex < cfg.testEventCount && cfg.testEvents[testIndex].frame <= frame) {
			const struct TestEvent* ev = &cfg.testEvents[testIndex++];
			switch (ev->kind) {
			case TEST_PRESS:
				pressVirtual(ev->keys);
				break;
			case TEST_TOUCH:
				handleAction(uiHitTest(&ui, ev->x, ev->y));
				break;
			case TEST_SHOT:
				shot = true;
				break;
			case TEST_PARTY:
				pokemonWriteTestParty(core, cfg.gameCode);
				break;
			case TEST_EXIT:
				quit = true;
				break;
			}
		}

		if (virtualKeyFrames > 0) {
			keys |= virtualKeys;
			--virtualKeyFrames;
		}
		core->setKeys(core, keys);
		for (int i = 0; i < ui.speed; ++i) {
			core->runFrame(core);
		}
		++frame;

		if (ui.pokemon && (frame % 20) == 0) {
			struct Party live;
			struct Trainer liveTrainer;
			pokemonReadParty(core, cfg.gameCode, &live);
			pokemonReadTrainer(core, cfg.gameCode, &liveTrainer);
			if (live.valid) {
				ui.party = live;
				ui.trainer = liveTrainer.valid ? liveTrainer : saveTrainer;
				ui.fromSave = false;
			} else {
				ui.party = saveParty;
				ui.trainer = saveTrainer;
				ui.fromSave = saveParty.valid || saveTrainer.valid;
			}
			iconsUpdate();
			pokemonReadMap(core, cfg.gameCode, &ui.map);
		}

		uploadFrame();
		drawFrame(true);
		if (shot) {
			gspWaitForVBlank();
			takeScreenshots();
		}
	}

exit:
	flushSave();
	if (core) {
		core->deinit(core);
	}
	aptUnhook(&aptCookie);
	if (hasSound) {
		ndspExit();
	}
	uiExit();
	C2D_Fini();
	C3D_Fini();
	gfxExit();
	romfsExit();
	return 0;
}
