/* SPDX-License-Identifier: MPL-2.0 */
#include "explored.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char baseDir[96];
static int curGroup = -1, curNum = -1, curW, curH;
static uint8_t* bits;
static bool dirty;
static unsigned framesSinceWrite;

/* The GBA screen shows 15 x 10 metatiles around the player. */
#define SEEN_LEFT 7
#define SEEN_RIGHT 7
#define SEEN_UP 5
#define SEEN_DOWN 5

static void pathFor(int group, int num, char* out, size_t n) {
	snprintf(out, n, "%s/explored/%d-%d.bin", baseDir, group, num);
}

void exploredInit(const char* dir) {
	snprintf(baseDir, sizeof(baseDir), "%s", dir);
	char p[128];
	snprintf(p, sizeof(p), "%s/explored", baseDir);
	mkdir(p, 0777);
}

void exploredFlush(void) {
	if (!bits || !dirty) {
		return;
	}
	char p[128];
	pathFor(curGroup, curNum, p, sizeof(p));
	FILE* f = fopen(p, "wb");
	if (f) {
		uint16_t dims[2] = { (uint16_t) curW, (uint16_t) curH };
		fwrite(dims, sizeof(dims), 1, f);
		fwrite(bits, 1, (curW * curH + 7) / 8, f);
		fclose(f);
	}
	dirty = false;
	framesSinceWrite = 0;
}

static void load(int group, int num, int w, int h) {
	exploredFlush();
	free(bits);
	curGroup = group;
	curNum = num;
	curW = w;
	curH = h;
	bits = calloc(1, (w * h + 7) / 8);
	char p[128];
	pathFor(group, num, p, sizeof(p));
	FILE* f = fopen(p, "rb");
	if (f) {
		uint16_t dims[2];
		if (fread(dims, sizeof(dims), 1, f) == 1 && dims[0] == w && dims[1] == h) {
			fread(bits, 1, (w * h + 7) / 8, f);
		}
		fclose(f);
	}
}

static bool seen(int x, int y) {
	if (x < 0 || y < 0 || x >= curW || y >= curH) {
		return false;
	}
	int i = y * curW + x;
	return (bits[i >> 3] >> (i & 7)) & 1;
}

void exploredApply(struct MapView* map) {
	if (!map->valid) {
		return;
	}
	if (!bits || map->group != curGroup || map->num != curNum || map->width != curW || map->height != curH) {
		load(map->group, map->num, map->width, map->height);
	}
	for (int y = map->py - SEEN_UP; y <= map->py + SEEN_DOWN; ++y) {
		for (int x = map->px - SEEN_LEFT; x <= map->px + SEEN_RIGHT; ++x) {
			if (x >= 0 && y >= 0 && x < curW && y < curH && !seen(x, y)) {
				int i = y * curW + x;
				bits[i >> 3] |= 1 << (i & 7);
				dirty = true;
			}
		}
	}
	for (int y = 0; y < MAP_VIEW_H; ++y) {
		for (int x = 0; x < MAP_VIEW_W; ++x) {
			if (!seen(map->originX + x, map->originY + y)) {
				map->cell[y][x] = CELL_OUTSIDE;
			}
		}
	}
	/* Write at most every ~10 seconds (SD writes are slow). */
	if (dirty && ++framesSinceWrite > 30) {
		exploredFlush();
	}
}
