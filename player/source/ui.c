/* SPDX-License-Identifier: MPL-2.0
 *
 * Bottom screen (320x240): header with title and clock, the Pokemon party
 * when available, and a dock of large touch buttons.
 */
#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define W 320
#define H 240
#define HEADER_H 26
#define DOCK_Y 196
#define DOCK_H (H - DOCK_Y)
#define BUTTONS 6

static C2D_TextBuf textBuf;
/* Depth of text: above panels (0.1-0.55), below the toast box (0.8). */
static float textDepth = 0.6f;

/* Palette: neutral greys and one accent, as in the web app. */
#define COL_BG       C2D_Color32(0x16, 0x18, 0x1C, 0xFF)
#define COL_PANEL    C2D_Color32(0x1F, 0x22, 0x28, 0xFF)
#define COL_PANEL_HI C2D_Color32(0x2A, 0x2E, 0x36, 0xFF)
#define COL_LINE     C2D_Color32(0x2C, 0x30, 0x38, 0xFF)
#define COL_TEXT     C2D_Color32(0xE6, 0xE8, 0xEB, 0xFF)
#define COL_DIM      C2D_Color32(0x9A, 0xA0, 0xA8, 0xFF)
#define COL_ACCENT   C2D_Color32(0x5B, 0x9C, 0xF0, 0xFF)
#define COL_GREEN    C2D_Color32(0x3F, 0xB9, 0x50, 0xFF)
#define COL_YELLOW   C2D_Color32(0xD2, 0x99, 0x22, 0xFF)
#define COL_RED      C2D_Color32(0xF8, 0x51, 0x49, 0xFF)
#define COL_BAR_BG   C2D_Color32(0x0E, 0x10, 0x13, 0xFF)

static const char* buttonLabel(const struct UiState* ui, int i, char* tmp, size_t n) {
	switch (i) {
	case 0: return "START";
	case 1: return "SELECT";
	case 2: snprintf(tmp, n, "%dx", ui->speed); return tmp;
	case 3: return "SAVE";
	case 4: return ui->loadArmed ? "SURE?" : "LOAD";
	case 5:
		switch (ui->scale) {
		case SCALE_FULL: return "FULL";
		case SCALE_ASPECT: return "4:3";
		default: return "1:1";
		}
	}
	return "";
}

static void buttonRect(int i, float* x, float* y, float* w, float* h) {
	const float gap = 4;
	float bw = (W - gap * (BUTTONS + 1)) / BUTTONS;
	*x = gap + i * (bw + gap);
	*y = DOCK_Y + 6;
	*w = bw;
	*h = DOCK_H - 12;
}

void uiInit(void) {
	textBuf = C2D_TextBufNew(4096);
}

void uiExit(void) {
	C2D_TextBufDelete(textBuf);
}

void uiToast(struct UiState* ui, const char* message) {
	snprintf(ui->toast, sizeof(ui->toast), "%s", message);
	ui->toastFrames = 150;
}

enum UiAction uiHitTest(struct UiState* ui, int x, int y) {
	for (int i = 0; i < BUTTONS; ++i) {
		float bx, by, bw, bh;
		buttonRect(i, &bx, &by, &bw, &bh);
		if (x >= bx && x < bx + bw && y >= by - 4 && y < by + bh + 4) {
			ui->pressedButton = i;
			ui->pressedFrames = 8;
			return (enum UiAction) (UI_PRESS_START + i);
		}
	}
	return UI_NONE;
}

static void text(const char* s, float x, float y, float scale, u32 color, u32 flags) {
	C2D_Text t;
	C2D_TextParse(&t, textBuf, s);
	C2D_TextOptimize(&t);
	C2D_DrawText(&t, C2D_WithColor | flags, x, y, textDepth, scale, scale, color);
}

static void hpBar(float x, float y, float w, const struct PartyMember* m) {
	C2D_DrawRectSolid(x, y, 0.5f, w, 5, COL_BAR_BG);
	float frac = m->maxHp ? (float) m->hp / m->maxHp : 0;
	u32 col = frac > 0.5f ? COL_GREEN : frac > 0.2f ? COL_YELLOW : COL_RED;
	C2D_DrawRectSolid(x, y, 0.55f, w * frac, 5, col);
}

/* Party as a 2 x 3 grid of cards: icon, name, level, HP bar, HP, status. */
static void drawParty(const struct UiState* ui) {
	const float top = HEADER_H + 4;
	const float gap = 4;
	const float cw = (W - gap * 3) / 2;
	const float ch = (DOCK_Y - top - gap * 3) / 3;
	for (int i = 0; i < PARTY_MAX; ++i) {
		float x = gap + (i & 1) * (cw + gap);
		float y = top + gap + (i >> 1) * (ch + gap);
		if (i >= ui->party.count) {
			C2D_DrawRectSolid(x, y, 0.4f, cw, ch, COL_BG);
			C2D_DrawRectSolid(x, y + ch - 1, 0.45f, cw, 1, COL_LINE);
			continue;
		}
		const struct PartyMember* m = &ui->party.members[i];
		C2D_DrawRectSolid(x, y, 0.4f, cw, ch, COL_PANEL);
		if (ui->iconValid[i]) {
			C2D_DrawImageAt(ui->icons[i], x + 2, y + (ch - 32) / 2 - 2, 0.6f, NULL, 1.0f, 1.0f);
		}
		float tx = x + 38;
		text(m->name, tx, y + 3, 0.45f, COL_TEXT, 0);
		if (m->egg) {
			continue;
		}
		char lv[12];
		snprintf(lv, sizeof(lv), "Lv%u", m->level);
		text(lv, x + cw - 5, y + 3, 0.4f, COL_DIM, C2D_AlignRight);
		hpBar(tx, y + 22, cw - 44, m);
		char hp[16];
		snprintf(hp, sizeof(hp), "%u/%u", m->hp, m->maxHp);
		text(hp, tx, y + 29, 0.38f, COL_DIM, 0);
		const char* st = pokemonStatusLabel(m);
		if (st[0]) {
			text(st, x + cw - 5, y + 29, 0.38f, COL_RED, C2D_AlignRight);
		} else if (m->speciesName[0] && strcmp(m->speciesName, m->name) != 0) {
			text(m->speciesName, x + cw - 5, y + 29, 0.38f, COL_DIM, C2D_AlignRight);
		}
	}
}

static void drawHelp(const struct UiState* ui) {
	const float x = 14;
	float y = HEADER_H + 14;
	if (ui->pokemon) {
		text("Your party appears here once you continue", x, y, 0.45f, COL_TEXT, 0);
		y += 16;
		text("or start a game.", x, y, 0.45f, COL_TEXT, 0);
		y += 26;
	}
	text("SAVE / LOAD  quick save state (one slot)", x, y, 0.45f, COL_DIM, 0);
	y += 18;
	text("SPEED        fast-forward 1x, 2x, 4x (or X)", x, y, 0.45f, COL_DIM, 0);
	y += 18;
	text("FULL / 4:3 / 1:1  top screen size", x, y, 0.45f, COL_DIM, 0);
	y += 18;
	text("HOME         save the game in game first", x, y, 0.45f, COL_DIM, 0);
	if (!ui->sound) {
		y += 26;
		text("No sound: dump the DSP firmware once", x, y, 0.45f, COL_YELLOW, 0);
		y += 16;
		text("(Rosalina menu > Miscellaneous options).", x, y, 0.45f, COL_YELLOW, 0);
	}
}

void uiDraw(struct UiState* ui) {
	C2D_TextBufClear(textBuf);
	C2D_DrawRectSolid(0, 0, 0.1f, W, H, COL_BG);

	/* Header */
	C2D_DrawRectSolid(0, 0, 0.2f, W, HEADER_H, COL_PANEL);
	C2D_DrawRectSolid(0, HEADER_H, 0.2f, W, 1, COL_LINE);
	if (ui->trainer.valid) {
		char who[64];
		snprintf(who, sizeof(who), "%s   %u:%02u", ui->trainer.name, ui->trainer.hours, ui->trainer.minutes);
		text(who, 8, 5, 0.5f, COL_TEXT, 0);
		if (ui->fromSave) {
			text("last save", W - 52, 7, 0.4f, COL_DIM, C2D_AlignRight);
		}
	} else {
		text(ui->title, 8, 5, 0.5f, COL_TEXT, 0);
	}
	time_t now = time(NULL);
	struct tm* tm = gmtime(&now);
	char clock[8];
	snprintf(clock, sizeof(clock), "%02d:%02d", tm->tm_hour, tm->tm_min);
	text(clock, W - 8, 5, 0.5f, COL_DIM, C2D_AlignRight);

	if (ui->party.valid) {
		drawParty(ui);
	} else {
		drawHelp(ui);
	}

	/* Toast */
	if (ui->toastFrames > 0) {
		--ui->toastFrames;
		C2D_DrawRectSolid(8, DOCK_Y - 30, 0.8f, W - 16, 24, COL_PANEL_HI);
		C2D_DrawRectSolid(8, DOCK_Y - 30, 0.85f, 3, 24, COL_ACCENT);
		textDepth = 0.95f;
		text(ui->toast, W / 2, DOCK_Y - 25, 0.4f, COL_TEXT, C2D_AlignCenter);
		textDepth = 0.6f;
	}

	/* Dock */
	C2D_DrawRectSolid(0, DOCK_Y, 0.2f, W, DOCK_H, COL_PANEL);
	C2D_DrawRectSolid(0, DOCK_Y, 0.2f, W, 1, COL_LINE);
	if (ui->pressedFrames > 0) {
		--ui->pressedFrames;
	} else {
		ui->pressedButton = -1;
	}
	if (ui->loadArmed && --ui->loadArmedFrames <= 0) {
		ui->loadArmed = false;
	}
	for (int i = 0; i < BUTTONS; ++i) {
		float x, y, w, h;
		buttonRect(i, &x, &y, &w, &h);
		bool pressed = ui->pressedButton == i;
		bool active = (i == 2 && ui->speed > 1) || (i == 4 && ui->loadArmed);
		C2D_DrawRectSolid(x, y, 0.3f, w, h, pressed ? COL_ACCENT : active ? COL_PANEL_HI : COL_BG);
		C2D_DrawRectSolid(x, y + h - 2, 0.35f, w, 2, active ? COL_ACCENT : COL_LINE);
		char tmp[16];
		text(buttonLabel(ui, i, tmp, sizeof(tmp)), x + w / 2, y + h / 2 - 6, 0.36f,
		     pressed ? COL_BG : COL_TEXT, C2D_AlignCenter);
	}
}

void uiDrawLoading(const char* title, const char* line, float progress) {
	C2D_TextBufClear(textBuf);
	C2D_DrawRectSolid(0, 0, 0.1f, W, H, COL_BG);
	text(title, W / 2, 80, 0.6f, COL_TEXT, C2D_AlignCenter);
	text(line, W / 2, 110, 0.45f, COL_DIM, C2D_AlignCenter);
	C2D_DrawRectSolid(40, 140, 0.2f, W - 80, 6, COL_BAR_BG);
	C2D_DrawRectSolid(40, 140, 0.3f, (W - 80) * progress, 6, COL_ACCENT);
}
