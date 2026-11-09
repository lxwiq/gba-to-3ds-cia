/* SPDX-License-Identifier: MPL-2.0 */
#include "config.h"

#include <mgba/internal/gba/savedata.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void trim(char* s) {
	char* end = s + strlen(s);
	while (end > s && isspace((unsigned char) end[-1])) {
		*--end = '\0';
	}
	char* start = s;
	while (*start && isspace((unsigned char) *start)) {
		++start;
	}
	if (start != s) {
		memmove(s, start, strlen(start) + 1);
	}
}

static void copyField(char* dst, size_t size, const char* src) {
	snprintf(dst, size, "%s", src);
}

static int parseSaveType(const char* v) {
	if (!strcmp(v, "none")) return SAVEDATA_FORCE_NONE;
	if (!strcmp(v, "sram")) return SAVEDATA_SRAM;
	if (!strcmp(v, "flash512k")) return SAVEDATA_FLASH512;
	if (!strcmp(v, "flash1m")) return SAVEDATA_FLASH1M;
	if (!strcmp(v, "eeprom64k")) return SAVEDATA_EEPROM;
	if (!strcmp(v, "eeprom4k")) return SAVEDATA_EEPROM512;
	return SAVEDATA_AUTODETECT;
}

static uint32_t parseKeyName(const char* k) {
	static const char* names[] = { "A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L" };
	for (unsigned i = 0; i < sizeof(names) / sizeof(*names); ++i) {
		if (!strcmp(k, names[i])) {
			return 1u << i;
		}
	}
	return 0;
}

/* Test script: comma separated events.
 *   <frame>:press:<KEY>[+KEY...]   hold keys for 6 frames
 *   <frame>:touch:<x>:<y>           tap the bottom screen
 *   <frame>:shot                    write both screens to sdmc:/3ds/gba2cia/test/
 *   <frame>:exit                    quit
 */
static void parseTestScript(struct PlayerConfig* cfg, char* script) {
	char* save = NULL;
	for (char* item = strtok_r(script, ",", &save); item && cfg->testEventCount < TEST_MAX_EVENTS;
	     item = strtok_r(NULL, ",", &save)) {
		struct TestEvent ev = { 0 };
		char* fsave = NULL;
		char* frame = strtok_r(item, ":", &fsave);
		char* kind = strtok_r(NULL, ":", &fsave);
		if (!frame || !kind) {
			continue;
		}
		ev.frame = strtoul(frame, NULL, 10);
		if (!strcmp(kind, "press")) {
			ev.kind = TEST_PRESS;
			char* keys = strtok_r(NULL, ":", &fsave);
			char* ksave = NULL;
			for (char* k = keys ? strtok_r(keys, "+", &ksave) : NULL; k; k = strtok_r(NULL, "+", &ksave)) {
				ev.keys |= parseKeyName(k);
			}
		} else if (!strcmp(kind, "touch")) {
			ev.kind = TEST_TOUCH;
			char* x = strtok_r(NULL, ":", &fsave);
			char* y = strtok_r(NULL, ":", &fsave);
			ev.x = x ? atoi(x) : 0;
			ev.y = y ? atoi(y) : 0;
		} else if (!strcmp(kind, "shot")) {
			ev.kind = TEST_SHOT;
		} else if (!strcmp(kind, "party")) {
			ev.kind = TEST_PARTY;
		} else if (!strcmp(kind, "exit")) {
			ev.kind = TEST_EXIT;
		} else {
			continue;
		}
		cfg->testEvents[cfg->testEventCount++] = ev;
	}
}

void configLoad(struct PlayerConfig* cfg) {
	memset(cfg, 0, sizeof(*cfg));
	copyField(cfg->title, sizeof(cfg->title), "GBA game");
	copyField(cfg->titleId, sizeof(cfg->titleId), "0000000000000000");
	cfg->saveType = SAVEDATA_AUTODETECT;
	cfg->scale = SCALE_FULL;

	FILE* f = fopen("romfs:/player.cfg", "r");
	if (!f) {
		return;
	}
	static char line[2048];
	while (fgets(line, sizeof(line), f)) {
		char* eq = strchr(line, '=');
		if (!eq || line[0] == '#') {
			continue;
		}
		*eq = '\0';
		char* key = line;
		char* value = eq + 1;
		trim(key);
		trim(value);
		if (!strcmp(key, "title")) {
			copyField(cfg->title, sizeof(cfg->title), value);
		} else if (!strcmp(key, "title_id")) {
			copyField(cfg->titleId, sizeof(cfg->titleId), value);
		} else if (!strcmp(key, "game_code")) {
			copyField(cfg->gameCode, sizeof(cfg->gameCode), value);
		} else if (!strcmp(key, "save")) {
			cfg->saveType = parseSaveType(value);
		} else if (!strcmp(key, "rtc")) {
			cfg->rtc = !strcmp(value, "1") || !strcmp(value, "on");
		} else if (!strcmp(key, "scale")) {
			if (!strcmp(value, "aspect")) {
				cfg->scale = SCALE_ASPECT;
			} else if (!strcmp(value, "native")) {
				cfg->scale = SCALE_NATIVE;
			} else {
				cfg->scale = SCALE_FULL;
			}
		} else if (!strcmp(key, "initial_save")) {
			cfg->hasInitialSave = !strcmp(value, "1");
		} else if (!strcmp(key, "test")) {
			cfg->test = !strcmp(value, "1");
		} else if (!strcmp(key, "test_script")) {
			parseTestScript(cfg, value);
		}
	}
	fclose(f);
}
