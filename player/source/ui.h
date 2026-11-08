/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PLAYER_UI_H
#define PLAYER_UI_H

#include <citro2d.h>
#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "pokemon.h"

enum UiAction {
	UI_NONE = 0,
	UI_PRESS_START,
	UI_PRESS_SELECT,
	UI_SPEED,
	UI_SAVE_STATE,
	UI_LOAD_STATE,
	UI_SCREEN
};

struct UiState {
	const char* title;
	int speed;               /* 1, 2 or 4 */
	enum ScaleMode scale;
	bool pokemon;            /* the game is a Gen III Pokemon game */
	struct Party party;
	bool sound;              /* DSP firmware present */
	char toast[96];
	int toastFrames;
	bool loadArmed;          /* first tap on LOAD done, waiting for confirmation */
	int loadArmedFrames;
	int pressedButton;       /* index of the button drawn pressed, -1 none */
	int pressedFrames;
};

void uiInit(void);
void uiExit(void);
void uiToast(struct UiState* ui, const char* message);
/* Map a touch position to an action. */
enum UiAction uiHitTest(struct UiState* ui, int x, int y);
/* Draw the bottom screen. Must be called inside a citro2d scene. */
void uiDraw(struct UiState* ui);
/* Draw a centred message with a progress bar (loading). */
void uiDrawLoading(const char* title, const char* line, float progress);

#endif
