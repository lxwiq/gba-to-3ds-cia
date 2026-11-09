/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PLAYER_CONFIG_H
#define PLAYER_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

enum ScaleMode {
	SCALE_FULL = 0,   /* 400x240, the whole top screen */
	SCALE_ASPECT = 1, /* 360x240, original aspect ratio */
	SCALE_NATIVE = 2, /* 240x160, pixel for pixel */
	SCALE_MAX
};

#define TEST_MAX_EVENTS 64

enum TestEventKind {
	TEST_PRESS,
	TEST_TOUCH,
	TEST_SHOT,
	TEST_PARTY,
	TEST_EXIT
};

struct TestEvent {
	uint32_t frame;
	enum TestEventKind kind;
	uint32_t keys;  /* GBA key mask for TEST_PRESS */
	int x, y;       /* TEST_TOUCH */
};

struct PlayerConfig {
	char title[65];
	char titleId[17];
	char gameCode[5];
	int saveType;  /* enum SavedataType */
	bool rtc;
	enum ScaleMode scale;
	bool hasInitialSave;

	bool test;
	int testEventCount;
	struct TestEvent testEvents[TEST_MAX_EVENTS];
};

/* Reads romfs:/player.cfg. Missing keys keep their defaults. */
void configLoad(struct PlayerConfig* cfg);

#endif
