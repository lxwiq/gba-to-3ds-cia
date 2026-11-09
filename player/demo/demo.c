/* SPDX-License-Identifier: MIT
 *
 * gba2cia demo ROM: an original scene and character, used to show and test
 * the dual screen player without any commercial game.
 *
 * Besides drawing its own top screen, it lays out a small fictional game
 * state in RAM at the addresses the player reads for FireRed-engine games
 * (party, trainer, save blocks, current map), so every bottom screen page
 * has something to show. All names and graphics are made up for this demo.
 */
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;

#define REG_DISPCNT (*(volatile u16*) 0x04000000)
#define REG_VCOUNT (*(volatile u16*) 0x04000006)
#define REG_KEYINPUT (*(volatile u16*) 0x04000130)
#define VRAM ((volatile u16*) 0x06000000)
#define OBJ_PAL ((volatile u16*) 0x05000200)
#define OBJ_TILES ((volatile u32*) 0x06014000) /* tile 512, mode 3 */
#define OAM ((volatile u16*) 0x07000000)

#define RGB(r, g, b) ((u16) ((r) | ((g) << 5) | ((b) << 10)))

/* The compiler emits calls to these for array initialisers. */
void* memset(void* dst, int c, unsigned long n) {
	u8* d = dst;
	while (n--) *d++ = (u8) c;
	return dst;
}

void* memcpy(void* dst, const void* src, unsigned long n) {
	u8* d = dst;
	const u8* s = src;
	while (n--) *d++ = *s++;
	return dst;
}

/* Addresses used by the FireRed engine (and read by the player). */
#define PARTY_COUNT ((volatile u8*) 0x02024029)
#define PARTY ((volatile u8*) 0x02024284)
#define MAP_HEADER ((volatile u32*) 0x02036DFC)
#define SB1_PTR (*(volatile u32*) 0x03005008)
#define SB2_PTR (*(volatile u32*) 0x0300500C)
#define SB1 ((volatile u8*) 0x0203C000)
#define SB2 ((volatile u8*) 0x0203E000)

/* Data placed in the ROM by make_demo.py. */
#define ROM_MAP_LAYOUT 0x080C2000u
#define ROM_MAP_EVENTS 0x080C3000u
#define MAPSEC_DEMO 0x58

static void pixel(int x, int y, u16 c) {
	if (x >= 0 && x < 240 && y >= 0 && y < 160) {
		VRAM[y * 240 + x] = c;
	}
}

static void rect(int x, int y, int w, int h, u16 c) {
	for (int j = y; j < y + h; ++j) {
		for (int i = x; i < x + w; ++i) {
			pixel(i, j, c);
		}
	}
}

static void disc(int cx, int cy, int r, u16 c) {
	for (int j = -r; j <= r; ++j) {
		for (int i = -r; i <= r; ++i) {
			if (i * i + j * j <= r * r) {
				pixel(cx + i, cy + j, c);
			}
		}
	}
}

/* Hill: filled parabola-like bump. */
static void hill(int cx, int base, int halfw, int height, u16 c) {
	for (int i = -halfw; i <= halfw; ++i) {
		int h = height - (height * i * i) / (halfw * halfw);
		for (int j = 0; j < h; ++j) {
			pixel(cx + i, base - j, c);
		}
	}
}

/* 5x7 font for the few letters the title needs. */
static const u8* glyph(char c) {
	static const u8 A[7] = { 14, 17, 17, 31, 17, 17, 17 }, B[7] = { 30, 17, 17, 30, 17, 17, 30 },
	                C[7] = { 14, 17, 16, 16, 16, 17, 14 }, D[7] = { 30, 17, 17, 17, 17, 17, 30 },
	                E[7] = { 31, 16, 16, 30, 16, 16, 31 }, G[7] = { 14, 17, 16, 23, 17, 17, 15 },
	                I[7] = { 14, 4, 4, 4, 4, 4, 14 }, L[7] = { 16, 16, 16, 16, 16, 16, 31 },
	                M[7] = { 17, 27, 21, 21, 17, 17, 17 }, N[7] = { 17, 25, 21, 19, 17, 17, 17 },
	                O[7] = { 14, 17, 17, 17, 17, 17, 14 }, R[7] = { 30, 17, 17, 30, 20, 18, 17 },
	                S[7] = { 15, 16, 16, 14, 1, 1, 30 }, T[7] = { 31, 4, 4, 4, 4, 4, 4 },
	                U[7] = { 17, 17, 17, 17, 17, 17, 14 }, K[7] = { 17, 18, 20, 24, 20, 18, 17 },
	                P[7] = { 30, 17, 17, 30, 16, 16, 16 }, Y[7] = { 17, 17, 10, 4, 4, 4, 4 },
	                W[7] = { 17, 17, 17, 21, 21, 21, 10 }, H[7] = { 17, 17, 17, 31, 17, 17, 17 },
	                THREE[7] = { 30, 1, 1, 14, 1, 1, 30 }, SPACE[7] = { 0 };
	switch (c) {
	case 'A': return A; case 'B': return B; case 'C': return C; case 'D': return D;
	case 'E': return E; case 'G': return G; case 'I': return I; case 'L': return L;
	case 'M': return M; case 'N': return N; case 'O': return O; case 'R': return R;
	case 'S': return S; case 'T': return T; case 'U': return U; case 'K': return K;
	case 'P': return P; case 'Y': return Y; case 'W': return W; case 'H': return H;
	case '3': return THREE;
	default: return SPACE;
	}
}

static void print(const char* s, int x, int y, int scale, u16 c, u16 shadow) {
	for (; *s; ++s, x += 6 * scale) {
		const u8* g = glyph(*s);
		for (int row = 0; row < 7; ++row) {
			for (int col = 0; col < 5; ++col) {
				if (g[row] & (16 >> col)) {
					rect(x + col * scale + 1, y + row * scale + 1, scale, scale, shadow);
					rect(x + col * scale, y + row * scale, scale, scale, c);
				}
			}
		}
	}
}

static void drawScene(void) {
	/* Sky gradient */
	for (int y = 0; y < 110; ++y) {
		u16 c = RGB(6 + y / 10, 12 + y / 8, 24 + y / 16 > 31 ? 31 : 24 + y / 16);
		rect(0, y, 240, 1, c);
	}
	disc(196, 34, 16, RGB(31, 26, 10));
	disc(196, 34, 12, RGB(31, 30, 18));
	/* Clouds */
	disc(40, 30, 8, RGB(30, 31, 31)); disc(52, 26, 10, RGB(30, 31, 31)); disc(64, 31, 7, RGB(30, 31, 31));
	disc(130, 48, 6, RGB(28, 30, 31)); disc(140, 45, 8, RGB(28, 30, 31)); disc(150, 49, 5, RGB(28, 30, 31));
	/* Hills and ground */
	hill(50, 112, 90, 40, RGB(9, 18, 12));
	hill(180, 112, 100, 30, RGB(7, 16, 11));
	rect(0, 110, 240, 50, RGB(10, 22, 9));
	for (int y = 110; y < 160; y += 6) {
		for (int x = (y / 6) % 2 * 8; x < 240; x += 16) {
			rect(x, y, 2, 2, RGB(12, 25, 10));
		}
	}
	/* A path and a small house */
	rect(0, 128, 240, 10, RGB(24, 20, 13));
	rect(150, 92, 40, 30, RGB(27, 24, 20));
	for (int i = 0; i < 24; ++i) {
		rect(146 + i, 92 - i, 48 - 2 * i, 1, RGB(20, 8, 7));
	}
	rect(166, 106, 10, 16, RGB(12, 8, 5));
	rect(154, 98, 8, 8, RGB(14, 22, 30));
	/* Title */
	print("GBA TO 3DS CIA", 22, 8, 2, RGB(31, 31, 31), RGB(4, 6, 12));
	print("DUAL SCREEN DEMO", 72, 30, 1, RGB(30, 30, 24), RGB(4, 6, 12));
	print("D PAD TO WALK", 82, 146, 1, RGB(28, 30, 28), RGB(4, 8, 4));
}

/* 16x16 sprite: a round mascot with an antenna, 4 bits per pixel. */
static void makeSprite(void) {
	OBJ_PAL[1] = RGB(31, 18, 12); /* body */
	OBJ_PAL[2] = RGB(18, 6, 4);   /* outline */
	OBJ_PAL[3] = RGB(3, 3, 6);    /* eyes */
	OBJ_PAL[4] = RGB(31, 31, 28); /* shine */
	OBJ_PAL[5] = RGB(31, 28, 6);  /* antenna tip */
	u8 px[16][16] = { { 0 } };
	for (int y = 0; y < 16; ++y) {
		for (int x = 0; x < 16; ++x) {
			int dx = 2 * x - 15, dy = 2 * y - 19;
			int d = dx * dx + dy * dy;
			if (d <= 13 * 13) px[y][x] = 1;
			if (d > 11 * 11 && d <= 13 * 13) px[y][x] = 2;
		}
	}
	px[1][8] = 5; px[2][8] = 2; px[3][8] = 2;
	px[8][5] = 3; px[9][5] = 3; px[8][10] = 3; px[9][10] = 3;
	px[6][5] = 4; px[6][6] = 4;
	for (int t = 0; t < 4; ++t) {
		int tx = (t & 1) * 8, ty = (t >> 1) * 8;
		for (int y = 0; y < 8; ++y) {
			u32 row = 0;
			for (int x = 0; x < 8; ++x) {
				row |= (u32) px[ty + y][tx + x] << (x * 4);
			}
			OBJ_TILES[t * 8 + y] = row;
		}
	}
}

/* ---- Fictional game state for the bottom screen ---- */

static u8 encode(char c) {
	if (c >= 'A' && c <= 'Z') return 0xBB + (c - 'A');
	if (c >= 'a' && c <= 'z') return 0xD5 + (c - 'a');
	return 0x00;
}

static void putName(volatile u8* dst, const char* s, int len) {
	int i = 0;
	for (; s[i] && i < len; ++i) dst[i] = encode(s[i]);
	for (; i < len; ++i) dst[i] = 0xFF;
}

static void put16(volatile u8* p, u16 v) { p[0] = v; p[1] = v >> 8; }
static void put32(volatile u8* p, u32 v) { put16(p, v); put16(p + 2, v >> 16); }

static void writeMember(int slot, u16 species, const char* nick, u8 level, u16 hp, u16 maxHp, u32 status) {
	volatile u8* m = PARTY + slot * 100;
	for (int i = 0; i < 100; ++i) m[i] = 0;
	u32 personality = 24u * (3000u + slot); /* % 24 == 0: Growth first */
	u32 otId = 0x00004D2E;
	put32(m, personality);
	put32(m + 4, otId);
	putName(m + 8, nick, 10);
	m[0x12] = 2;
	m[0x13] = 2;
	putName(m + 0x14, "Demo", 7);
	u16 secure[24] = { 0 };
	secure[0] = species;
	secure[4] = 70 << 8; /* friendship */
	u16 sum = 0;
	for (int i = 0; i < 24; ++i) sum += secure[i];
	put16(m + 0x1C, sum);
	u32 key = personality ^ otId;
	for (int i = 0; i < 12; ++i) {
		u32 w = (secure[2 * i] | ((u32) secure[2 * i + 1] << 16)) ^ key;
		put32(m + 0x20 + i * 4, w);
	}
	put32(m + 0x50, status);
	m[0x54] = level;
	put16(m + 0x56, hp);
	put16(m + 0x58, maxHp);
}

static void writeGameState(void) {
	writeMember(0, 1, "Sprout", 24, 71, 71, 0);
	writeMember(1, 2, "Cinder", 22, 38, 66, 0);
	writeMember(2, 3, "Drip", 23, 12, 70, 0x08);
	writeMember(3, 4, "Rocky", 21, 80, 80, 0);
	writeMember(4, 5, "Puff", 19, 0, 54, 0);
	writeMember(5, 6, "Spike", 20, 50, 61, 0x40);
	*PARTY_COUNT = 6;

	for (int i = 0; i < 0x1000; ++i) { SB1[i] = 0; SB2[i] = 0; }
	putName(SB2, "Demo", 8);
	put16(SB2 + 0x0A, 31415);  /* trainer ID */
	put16(SB2 + 0x0E, 12);     /* play time hours */
	SB2[0x10] = 34;           /* minutes */
	put32(SB2 + 0xF20, 0);     /* money key */
	SB1[4] = 1;                /* map group */
	SB1[5] = 1;                /* map number */
	put32(SB1 + 0x290, 24680); /* money */
	SB1[0xEE0 + 0x820 / 8] = 0x07; /* three badges */
	SB1_PTR = 0x0203C000;
	SB2_PTR = 0x0203E000;
	MAP_HEADER[0] = ROM_MAP_LAYOUT;
	MAP_HEADER[1] = ROM_MAP_EVENTS;
	((volatile u8*) MAP_HEADER)[0x14] = MAPSEC_DEMO;
}

static void setPosition(int x, int y) {
	put16(SB1, (u16) x);
	put16(SB1 + 2, (u16) y);
}

static void waitVBlank(void) {
	while (REG_VCOUNT >= 160) {}
	while (REG_VCOUNT < 160) {}
}

int main(void) {
	REG_DISPCNT = 3 | 0x0400 | 0x1000 | 0x0040; /* mode 3, BG2, OBJ, 1D */
	drawScene();
	makeSprite();
	writeGameState();
	for (int i = 1; i < 128; ++i) OAM[i * 4] = 0x0200; /* hide other sprites */
	int x = 104, y = 114;
	for (;;) {
		waitVBlank();
		u16 keys = ~REG_KEYINPUT;
		if ((keys & 0x10) && x < 224) ++x;
		if ((keys & 0x20) && x > 0) --x;
		if ((keys & 0x40) && y > 60) --y;
		if ((keys & 0x80) && y < 140) ++y;
		OAM[0] = (y & 0xFF);                 /* 4bpp, square */
		OAM[1] = (x & 0x1FF) | (1 << 14);    /* 16x16 */
		OAM[2] = 512;
		/* Map position: the fictional map is 40 x 30 metatiles. */
		setPosition(6 + x / 8, 4 + (y - 60) / 6);
	}
}
