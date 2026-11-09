/* SPDX-License-Identifier: MPL-2.0
 *
 * Bottom screen (320x240), in four pages chosen from a tab bar:
 *   TEAM   the party: name, level, HP, status, species, icon
 *   MAP    location name and a schematic plan of the area around the player,
 *          drawn from the map's collision data (walls, floor, doors, people)
 *   CARD   trainer card: name, ID, play time, money, badges
 *   TOOLS  START, SELECT, speed-up, save and load state, screen size
 * The header shows the trainer and play time, or the game title, and the
 * clock.
 */
#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define W 320
#define H 240
#define HEADER_H 26
#define TAB_Y 204
#define TAB_H (H - TAB_Y)
#define CONTENT_Y (HEADER_H + 1)
#define CONTENT_H (TAB_Y - CONTENT_Y)

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
#define COL_FLOOR    C2D_Color32(0x3A, 0x40, 0x4A, 0xFF)
#define COL_WALL     C2D_Color32(0x12, 0x14, 0x18, 0xFF)

static const char* const tabNames[TAB_COUNT] = { "TEAM", "MAP", "CARD", "TOOLS" };

/* Hit zones: 0..3 tabs, 10..15 tool buttons. */
#define ZONE_TOOL 10

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

static void text(const char* s, float x, float y, float scale, u32 color, u32 flags) {
	C2D_Text t;
	C2D_TextParse(&t, textBuf, s);
	C2D_TextOptimize(&t);
	C2D_DrawText(&t, C2D_WithColor | flags, x, y, textDepth, scale, scale, color);
}

static void tabRect(int i, float* x, float* w) {
	*w = W / (float) TAB_COUNT;
	*x = i * *w;
}

static void toolRect(int i, float* x, float* y, float* w, float* h) {
	const float gap = 6;
	*w = (W - gap * 4) / 3;
	*h = 52;
	*x = gap + (i % 3) * (*w + gap);
	*y = CONTENT_Y + 10 + (i / 3) * (*h + gap);
}

enum UiAction uiHitTest(struct UiState* ui, int x, int y) {
	if (y >= TAB_Y) {
		for (int i = 0; i < TAB_COUNT; ++i) {
			float tx, tw;
			tabRect(i, &tx, &tw);
			if (x >= tx && x < tx + tw) {
				ui->tab = (enum UiTab) i;
				ui->pressed = i;
				ui->pressedFrames = 6;
			}
		}
		return UI_NONE;
	}
	if (ui->tab == TAB_TOOLS) {
		for (int i = 0; i < 6; ++i) {
			float bx, by, bw, bh;
			toolRect(i, &bx, &by, &bw, &bh);
			if (x >= bx && x < bx + bw && y >= by && y < by + bh) {
				ui->pressed = ZONE_TOOL + i;
				ui->pressedFrames = 8;
				return (enum UiAction) (UI_PRESS_START + i);
			}
		}
	}
	return UI_NONE;
}

static void centered(const char* line1, const char* line2) {
	text(line1, W / 2, CONTENT_Y + CONTENT_H / 2 - 18, 0.48f, COL_TEXT, C2D_AlignCenter);
	if (line2) {
		text(line2, W / 2, CONTENT_Y + CONTENT_H / 2 + 2, 0.42f, COL_DIM, C2D_AlignCenter);
	}
}

/* ---------------------------------------------------------------- TEAM */

static void hpBar(float x, float y, float w, const struct PartyMember* m) {
	C2D_DrawRectSolid(x, y, 0.5f, w, 5, COL_BAR_BG);
	float frac = m->maxHp ? (float) m->hp / m->maxHp : 0;
	u32 col = frac > 0.5f ? COL_GREEN : frac > 0.2f ? COL_YELLOW : COL_RED;
	C2D_DrawRectSolid(x, y, 0.55f, w * frac, 5, col);
}

static void drawTeam(const struct UiState* ui) {
	if (!ui->party.valid) {
		centered(ui->pokemon ? "Your party appears here" : "No party for this game",
		         ui->pokemon ? "once you continue or start a game." : NULL);
		return;
	}
	const float gap = 4;
	const float cw = (W - gap * 3) / 2;
	const float ch = (CONTENT_H - gap * 4) / 3;
	for (int i = 0; i < PARTY_MAX; ++i) {
		float x = gap + (i & 1) * (cw + gap);
		float y = CONTENT_Y + gap + (i >> 1) * (ch + gap);
		if (i >= ui->party.count) {
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

/* ---------------------------------------------------------------- MAP */

static void drawMap(const struct UiState* ui) {
	const struct MapView* m = &ui->map;
	if (!m->valid) {
		centered("The map appears once you are in the game.", NULL);
		return;
	}
	char line[64];
	text(m->name[0] ? m->name : "Unknown place", 8, CONTENT_Y + 3, 0.48f, COL_TEXT, 0);
	snprintf(line, sizeof(line), "X %d  Y %d", m->px, m->py);
	text(line, W - 8, CONTENT_Y + 4, 0.4f, COL_DIM, C2D_AlignRight);

	const float cell = 6;
	const float ox = (W - MAP_VIEW_W * cell) / 2;
	const float oy = CONTENT_Y + 22;
	C2D_DrawRectSolid(ox - 2, oy - 2, 0.2f, MAP_VIEW_W * cell + 4, MAP_VIEW_H * cell + 4, COL_BAR_BG);
	for (int y = 0; y < MAP_VIEW_H; ++y) {
		for (int x = 0; x < MAP_VIEW_W; ++x) {
			float cx = ox + x * cell, cy = oy + y * cell;
			switch (m->cell[y][x]) {
			case CELL_FLOOR:
				C2D_DrawRectSolid(cx, cy, 0.3f, cell, cell, COL_FLOOR);
				break;
			case CELL_WALL:
				C2D_DrawRectSolid(cx, cy, 0.3f, cell, cell, COL_WALL);
				break;
			case CELL_WARP:
				C2D_DrawRectSolid(cx, cy, 0.3f, cell, cell, COL_FLOOR);
				C2D_DrawRectSolid(cx + 1, cy + 1, 0.35f, cell - 2, cell - 2, COL_YELLOW);
				break;
			case CELL_PERSON:
				C2D_DrawRectSolid(cx, cy, 0.3f, cell, cell, COL_FLOOR);
				C2D_DrawRectSolid(cx + 1.5f, cy + 1.5f, 0.35f, cell - 3, cell - 3, COL_DIM);
				break;
			default:
				break;
			}
		}
	}
	/* The player, always in the middle of the view. */
	float pxl = ox + (m->px - m->originX) * cell, pyl = oy + (m->py - m->originY) * cell;
	C2D_DrawRectSolid(pxl - 1, pyl - 1, 0.4f, cell + 2, cell + 2, COL_TEXT);
	C2D_DrawRectSolid(pxl, pyl, 0.45f, cell, cell, COL_ACCENT);
	/* Legend */
	float ly = oy + MAP_VIEW_H * cell + 6;
	C2D_DrawRectSolid(10, ly + 2, 0.4f, 6, 6, COL_ACCENT);
	text("You", 20, ly, 0.36f, COL_DIM, 0);
	C2D_DrawRectSolid(60, ly + 2, 0.4f, 6, 6, COL_YELLOW);
	text("Door, stairs", 70, ly, 0.36f, COL_DIM, 0);
	C2D_DrawRectSolid(150, ly + 2, 0.4f, 6, 6, COL_DIM);
	text("Person", 160, ly, 0.36f, COL_DIM, 0);
	snprintf(line, sizeof(line), "Map %dx%d", m->width, m->height);
	text(line, W - 8, ly, 0.36f, COL_DIM, C2D_AlignRight);
}

/* ---------------------------------------------------------------- CARD */

static void cardRow(float y, const char* label, const char* value) {
	text(label, 18, y, 0.45f, COL_DIM, 0);
	text(value, W - 18, y, 0.45f, COL_TEXT, C2D_AlignRight);
	C2D_DrawRectSolid(14, y + 19, 0.3f, W - 28, 1, COL_LINE);
}

static void drawCard(const struct UiState* ui) {
	const struct Trainer* t = &ui->trainer;
	if (!t->valid) {
		centered("The trainer card appears", "once you continue or start a game.");
		return;
	}
	C2D_DrawRectSolid(8, CONTENT_Y + 6, 0.2f, W - 16, CONTENT_H - 12, COL_PANEL);
	char v[32];
	float y = CONTENT_Y + 14;
	cardRow(y, "Name", t->name);
	y += 26;
	snprintf(v, sizeof(v), "%05u", t->id);
	cardRow(y, "ID No.", v);
	y += 26;
	snprintf(v, sizeof(v), "%u:%02u", t->hours, t->minutes);
	cardRow(y, "Play time", v);
	y += 26;
	if (t->hasMoney) {
		snprintf(v, sizeof(v), "%u", t->money);
		cardRow(y, "Money", v);
		y += 26;
	}
	if (t->hasBadges) {
		text("Badges", 18, y, 0.45f, COL_DIM, 0);
		for (int i = 0; i < 8; ++i) {
			float bx = W - 18 - (8 - i) * 18 + 4;
			bool have = (t->badges >> i) & 1;
			C2D_DrawRectSolid(bx, y + 3, 0.4f, 12, 12, have ? COL_YELLOW : COL_BAR_BG);
		}
	}
	if (ui->fromSave) {
		text("From your last save", W / 2, CONTENT_Y + CONTENT_H - 26, 0.38f, COL_DIM, C2D_AlignCenter);
	}
}

/* ---------------------------------------------------------------- TOOLS */

static const char* toolLabel(const struct UiState* ui, int i, char* tmp, size_t n) {
	switch (i) {
	case 0: return "START";
	case 1: return "SELECT";
	case 2: snprintf(tmp, n, "SPEED %dx", ui->speed); return tmp;
	case 3: return "SAVE STATE";
	case 4: return ui->loadArmed ? "TAP AGAIN" : "LOAD STATE";
	default:
		switch (ui->scale) {
		case SCALE_FULL: return "SCREEN FULL";
		case SCALE_ASPECT: return "SCREEN 3:2";
		default: return "SCREEN 1:1";
		}
	}
}

static void drawTools(const struct UiState* ui) {
	for (int i = 0; i < 6; ++i) {
		float x, y, w, h;
		toolRect(i, &x, &y, &w, &h);
		bool pressed = ui->pressed == ZONE_TOOL + i;
		bool active = (i == 2 && ui->speed > 1) || (i == 4 && ui->loadArmed);
		C2D_DrawRectSolid(x, y, 0.3f, w, h, pressed ? COL_ACCENT : COL_PANEL);
		C2D_DrawRectSolid(x, y + h - 2, 0.35f, w, 2, active ? COL_ACCENT : COL_LINE);
		char tmp[24];
		text(toolLabel(ui, i, tmp, sizeof(tmp)), x + w / 2, y + h / 2 - 7, 0.42f, pressed ? COL_BG : COL_TEXT,
		     C2D_AlignCenter);
	}
	float y = CONTENT_Y + 10 + 2 * 58 + 4;
	text("X: speed-up   HOME: save in game first", W / 2, y, 0.38f, COL_DIM, C2D_AlignCenter);
	if (!ui->sound) {
		text("No sound: dump the DSP firmware once", W / 2, y + 15, 0.38f, COL_YELLOW, C2D_AlignCenter);
		text("(Rosalina menu > Miscellaneous options).", W / 2, y + 28, 0.38f, COL_YELLOW, C2D_AlignCenter);
	}
}

/* ---------------------------------------------------------------- frame */

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
	} else {
		text(ui->title, 8, 5, 0.5f, COL_TEXT, 0);
	}
	time_t now = time(NULL);
	struct tm* tm = gmtime(&now);
	char clock[8];
	snprintf(clock, sizeof(clock), "%02d:%02d", tm->tm_hour, tm->tm_min);
	text(clock, W - 8, 5, 0.5f, COL_DIM, C2D_AlignRight);

	switch (ui->tab) {
	case TAB_TEAM: drawTeam(ui); break;
	case TAB_MAP: drawMap(ui); break;
	case TAB_CARD: drawCard(ui); break;
	default: drawTools(ui); break;
	}

	/* Tab bar */
	C2D_DrawRectSolid(0, TAB_Y, 0.2f, W, TAB_H, COL_PANEL);
	C2D_DrawRectSolid(0, TAB_Y, 0.2f, W, 1, COL_LINE);
	for (int i = 0; i < TAB_COUNT; ++i) {
		float x, w;
		tabRect(i, &x, &w);
		bool current = ui->tab == (enum UiTab) i;
		if (current) {
			C2D_DrawRectSolid(x, TAB_Y + 1, 0.25f, w, TAB_H - 1, COL_PANEL_HI);
			C2D_DrawRectSolid(x + 10, TAB_Y + 1, 0.3f, w - 20, 3, COL_ACCENT);
		}
		text(tabNames[i], x + w / 2, TAB_Y + TAB_H / 2 - 7, 0.45f, current ? COL_TEXT : COL_DIM, C2D_AlignCenter);
	}

	if (ui->pressedFrames > 0) {
		--ui->pressedFrames;
	} else {
		ui->pressed = -1;
	}
	if (ui->loadArmed && --ui->loadArmedFrames <= 0) {
		ui->loadArmed = false;
	}

	/* Toast, above everything. */
	if (ui->toastFrames > 0) {
		--ui->toastFrames;
		C2D_DrawRectSolid(8, TAB_Y - 30, 0.8f, W - 16, 24, COL_PANEL_HI);
		C2D_DrawRectSolid(8, TAB_Y - 30, 0.85f, 3, 24, COL_ACCENT);
		textDepth = 0.95f;
		text(ui->toast, W / 2, TAB_Y - 25, 0.4f, COL_TEXT, C2D_AlignCenter);
		textDepth = 0.6f;
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
