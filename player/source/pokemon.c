/* SPDX-License-Identifier: MPL-2.0
 *
 * Generation III party reader.
 *
 * The party is an array of 100-byte structures in RAM. The 48-byte "secure"
 * part is split into four 12-byte substructures (Growth, Attacks, EVs,
 * Misc) whose order depends on personality % 24, XOR-encrypted with
 * personality ^ OT ID, and protected by a 16-bit checksum. Checking that
 * checksum on every slot tells us whether the address is right, which makes
 * this safe on ROM hacks that move things around: if anything does not
 * match, the panel is simply hidden.
 */
#include "pokemon.h"

#include <mgba/core/core.h>

#include <stdio.h>
#include <string.h>

struct PartyLayout {
	const char* code3;
	uint32_t party;
	uint32_t count;
	uint32_t saveBlock2Ptr; /* address of gSaveBlock2Ptr, 0 if unknown */
	uint32_t saveBlock1Ptr; /* address of gSaveBlock1Ptr, 0 if unknown */
	uint32_t mapHeader;     /* address of gMapHeader, 0 if unknown */
	uint32_t moneyOffset;   /* in SaveBlock1 */
	uint32_t keyOffset;     /* money encryption key in SaveBlock2 */
	uint32_t flagsOffset;   /* event flags in SaveBlock1 */
	uint32_t badgeFlag;     /* first badge flag */
	uint32_t mapNamesRef;   /* ROM address holding the map name table pointer */
	uint32_t mapsecBase;    /* first map section of that table */
};

/* Addresses from the pret decompilations (USA builds). FireRed-based hacks
 * built with Complete FireRed Upgrade (Pokemon Unbound and many others) keep
 * the FireRed addresses. */
static const struct PartyLayout layouts[] = {
	/* FireRed / LeafGreen, and hacks built on them. The map name table
	 * pointer sits in a literal pool that hacks such as Unbound keep. */
	{ "BPR", 0x02024284, 0x02024029, 0x0300500C, 0x03005008, 0x02036DFC, 0x290, 0xF20, 0xEE0, 0x820, 0x080C0C94, 0x58 },
	{ "BPG", 0x02024284, 0x02024029, 0x0300500C, 0x03005008, 0x02036DFC, 0x290, 0xF20, 0xEE0, 0x820, 0x080C0C94, 0x58 },
	{ "BPE", 0x020244EC, 0x020244E9, 0x03005D90, 0x03005D8C, 0x02037318, 0x490, 0xAC, 0x1270, 0x867, 0, 0 },
	{ "AXV", 0x03004360, 0x03004350, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ "AXP", 0x03004360, 0x03004350, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};

static const struct PartyLayout* findLayout(const char* gameCode) {
	if (!gameCode || strlen(gameCode) < 3) {
		return NULL;
	}
	for (unsigned i = 0; i < sizeof(layouts) / sizeof(*layouts); ++i) {
		if (!strncmp(gameCode, layouts[i].code3, 3)) {
			return &layouts[i];
		}
	}
	return NULL;
}

bool pokemonSupported(const char* gameCode) {
	return findLayout(gameCode) != NULL;
}

/* Position of the Growth substructure for each personality % 24. */
static const uint8_t growthPos[24] = {
	0, 0, 0, 0, 0, 0, 1, 1, 2, 3, 2, 3, 1, 1, 2, 3, 2, 3, 1, 1, 2, 3, 2, 3,
};

/* Generation III character set to UTF-8 (Western games). */
static const char* charOf(uint8_t c) {
	static char one[2];
	if (c >= 0xBB && c <= 0xD4) {
		one[0] = 'A' + (c - 0xBB);
		one[1] = 0;
		return one;
	}
	if (c >= 0xD5 && c <= 0xEE) {
		one[0] = 'a' + (c - 0xD5);
		one[1] = 0;
		return one;
	}
	if (c >= 0xA1 && c <= 0xAA) {
		one[0] = '0' + (c - 0xA1);
		one[1] = 0;
		return one;
	}
	switch (c) {
	case 0x00: return " ";
	case 0x01: return "\xC3\x80"; /* À */
	case 0x02: return "\xC3\x81"; /* Á */
	case 0x03: return "\xC3\x82"; /* Â */
	case 0x04: return "\xC3\x87"; /* Ç */
	case 0x05: return "\xC3\x88"; /* È */
	case 0x06: return "\xC3\x89"; /* É */
	case 0x07: return "\xC3\x8A"; /* Ê */
	case 0x08: return "\xC3\x8B"; /* Ë */
	case 0x09: return "\xC3\x8C"; /* Ì */
	case 0x0B: return "\xC3\x8E"; /* Î */
	case 0x0C: return "\xC3\x8F"; /* Ï */
	case 0x0D: return "\xC3\x92"; /* Ò */
	case 0x0E: return "\xC3\x93"; /* Ó */
	case 0x0F: return "\xC3\x94"; /* Ô */
	case 0x11: return "\xC3\x99"; /* Ù */
	case 0x12: return "\xC3\x9A"; /* Ú */
	case 0x13: return "\xC3\x9B"; /* Û */
	case 0x14: return "\xC3\x91"; /* Ñ */
	case 0x15: return "\xC3\x9F"; /* ß */
	case 0x16: return "\xC3\xA0"; /* à */
	case 0x17: return "\xC3\xA1"; /* á */
	case 0x19: return "\xC3\xA7"; /* ç */
	case 0x1A: return "\xC3\xA8"; /* è */
	case 0x1B: return "\xC3\xA9"; /* é */
	case 0x1C: return "\xC3\xAA"; /* ê */
	case 0x1D: return "\xC3\xAB"; /* ë */
	case 0x1E: return "\xC3\xAC"; /* ì */
	case 0xAB: return "!";
	case 0xAC: return "?";
	case 0xAD: return ".";
	case 0xAE: return "-";
	case 0xB0: return "...";
	case 0xB1: return "\"";
	case 0xB2: return "\"";
	case 0xB3: return "'";
	case 0xB4: return "'";
	case 0xB5: return "\xE2\x99\x82"; /* male sign */
	case 0xB6: return "\xE2\x99\x80"; /* female sign */
	case 0xB8: return ",";
	case 0xBA: return "/";
	default: return "";
	}
}

static void decodeName(const uint8_t* raw, size_t len, char* out, size_t outSize) {
	size_t pos = 0;
	out[0] = 0;
	for (size_t i = 0; i < len && raw[i] != 0xFF; ++i) {
		const char* s = charOf(raw[i]);
		size_t n = strlen(s);
		if (pos + n + 1 > outSize) {
			break;
		}
		memcpy(out + pos, s, n);
		pos += n;
		out[pos] = 0;
	}
}

static uint32_t read32(const uint8_t* p) {
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t) p[3] << 24);
}

static uint16_t read16(const uint8_t* p) {
	return p[0] | (p[1] << 8);
}


/* ---- Game Freak ROM header (0x08000100): tables shared by every Gen III
 * game and kept up to date by ROM hack tools when they move the data. */
#define GF_MON_ICONS 0x08000138
#define GF_MON_ICON_PALETTE_IDS 0x0800013C
#define GF_MON_ICON_PALETTES 0x08000140
#define GF_MON_SPECIES_NAMES 0x08000144
#define SPECIES_MAX 2048

static bool romPointer(uint32_t p) {
	return p >= 0x08000000 && p < 0x0A000000;
}

static uint32_t rd32(struct mCore* core, uint32_t addr) {
	return core->rawRead32(core, addr, -1);
}

static void speciesName(struct mCore* core, uint16_t species, char* out, size_t outSize) {
	out[0] = 0;
	uint32_t table = rd32(core, GF_MON_SPECIES_NAMES);
	if (!romPointer(table) || species >= SPECIES_MAX) {
		return;
	}
	uint8_t raw[11];
	for (int i = 0; i < 11; ++i) {
		raw[i] = core->rawRead8(core, table + species * 11 + i, -1);
	}
	decodeName(raw, 11, out, outSize);
}

bool pokemonIcon(struct mCore* core, uint16_t species, uint32_t* rgba) {
	uint32_t icons = rd32(core, GF_MON_ICONS);
	uint32_t ids = rd32(core, GF_MON_ICON_PALETTE_IDS);
	uint32_t pals = rd32(core, GF_MON_ICON_PALETTES);
	if (!romPointer(icons) || !romPointer(ids) || !romPointer(pals) || species >= SPECIES_MAX) {
		return false;
	}
	uint32_t gfx = rd32(core, icons + species * 4);
	uint8_t palId = core->rawRead8(core, ids + species, -1);
	uint32_t pal = rd32(core, pals + palId * 8);
	if (!romPointer(gfx) || !romPointer(pal) || palId > 16) {
		return false;
	}
	uint32_t colors[16];
	for (int i = 0; i < 16; ++i) {
		uint16_t c = core->rawRead16(core, pal + i * 2, -1);
		uint32_t r = (c & 0x1F) << 3, g = ((c >> 5) & 0x1F) << 3, b = ((c >> 10) & 0x1F) << 3;
		colors[i] = (r << 24) | (g << 16) | (b << 8) | (i ? 0xFF : 0x00);
	}
	/* 4x4 tiles of 8x8 pixels, 4 bits per pixel, low nibble first. */
	for (int t = 0; t < 16; ++t) {
		int tx = (t & 3) * 8, ty = (t >> 2) * 8;
		for (int y = 0; y < 8; ++y) {
			for (int x = 0; x < 8; x += 2) {
				uint8_t b = core->rawRead8(core, gfx + t * 32 + y * 4 + x / 2, -1);
				rgba[(ty + y) * 32 + tx + x] = colors[b & 0xF];
				rgba[(ty + y) * 32 + tx + x + 1] = colors[b >> 4];
			}
		}
	}
	return true;
}

void pokemonReadTrainer(struct mCore* core, const char* gameCode, struct Trainer* t) {
	memset(t, 0, sizeof(*t));
	const struct PartyLayout* layout = findLayout(gameCode);
	if (!layout || !layout->saveBlock2Ptr) {
		return;
	}
	uint32_t sb2 = rd32(core, layout->saveBlock2Ptr);
	if (sb2 < 0x02000000 || sb2 >= 0x02040000) {
		return;
	}
	uint8_t raw[8];
	for (int i = 0; i < 8; ++i) {
		raw[i] = core->rawRead8(core, sb2 + i, -1);
	}
	if (raw[0] == 0xFF || raw[0] == 0x00) {
		return; /* no name yet (new game not started) */
	}
	decodeName(raw, 8, t->name, sizeof(t->name));
	t->hours = core->rawRead16(core, sb2 + 0x0E, -1);
	t->minutes = core->rawRead8(core, sb2 + 0x10, -1);
	t->id = core->rawRead16(core, sb2 + 0x0A, -1);
	t->valid = t->name[0] && t->hours < 1000 && t->minutes < 60;
	uint32_t sb1 = layout->saveBlock1Ptr ? rd32(core, layout->saveBlock1Ptr) : 0;
	if (sb1 >= 0x02000000 && sb1 < 0x02040000 && layout->moneyOffset) {
		uint32_t money = rd32(core, sb1 + layout->moneyOffset) ^ rd32(core, sb2 + layout->keyOffset);
		if (money <= 999999) {
			t->money = money;
			t->hasMoney = true;
		}
		t->badges = core->rawRead8(core, sb1 + layout->flagsOffset + layout->badgeFlag / 8, -1);
		t->hasBadges = true;
	}
}

static void mapName(struct mCore* core, const struct PartyLayout* layout, unsigned mapsec, char* out, size_t outSize) {
	out[0] = 0;
	if (!layout->mapNamesRef || mapsec < layout->mapsecBase) {
		return;
	}
	uint32_t table = rd32(core, layout->mapNamesRef);
	if (!romPointer(table)) {
		return;
	}
	uint32_t entry = rd32(core, table + (mapsec - layout->mapsecBase) * 4);
	if (!romPointer(entry)) {
		return;
	}
	uint8_t raw[24];
	for (int i = 0; i < 24; ++i) {
		raw[i] = core->rawRead8(core, entry + i, -1);
	}
	decodeName(raw, sizeof(raw), out, outSize);
}

void pokemonReadMap(struct mCore* core, const char* gameCode, struct MapView* map) {
	memset(map, 0, sizeof(*map));
	const struct PartyLayout* layout = findLayout(gameCode);
	if (!layout || !layout->mapHeader || !layout->saveBlock1Ptr) {
		return;
	}
	uint32_t sb1 = rd32(core, layout->saveBlock1Ptr);
	uint32_t mapLayout = rd32(core, layout->mapHeader);
	if (sb1 < 0x02000000 || sb1 >= 0x02040000 || !romPointer(mapLayout)) {
		return;
	}
	int width = (int) rd32(core, mapLayout);
	int height = (int) rd32(core, mapLayout + 4);
	uint32_t blocks = rd32(core, mapLayout + 12);
	if (width <= 0 || height <= 0 || width > 512 || height > 512 || !romPointer(blocks)) {
		return;
	}
	int px = (int16_t) core->rawRead16(core, sb1, -1);
	int py = (int16_t) core->rawRead16(core, sb1 + 2, -1);
	if (px < 0 || py < 0 || px >= width || py >= height) {
		return;
	}
	map->width = width;
	map->height = height;
	map->px = px;
	map->py = py;
	map->originX = px - MAP_VIEW_W / 2;
	map->originY = py - MAP_VIEW_H / 2;
	for (int y = 0; y < MAP_VIEW_H; ++y) {
		for (int x = 0; x < MAP_VIEW_W; ++x) {
			int mx = map->originX + x, my = map->originY + y;
			if (mx < 0 || my < 0 || mx >= width || my >= height) {
				map->cell[y][x] = CELL_OUTSIDE;
				continue;
			}
			uint16_t b = core->rawRead16(core, blocks + (my * width + mx) * 2, -1);
			map->cell[y][x] = (b & 0x0C00) ? CELL_WALL : CELL_FLOOR;
		}
	}
	/* Warps (doors, stairs) and people from the map events. */
	uint32_t events = rd32(core, layout->mapHeader + 4);
	if (romPointer(events)) {
		unsigned objects = core->rawRead8(core, events, -1);
		unsigned warps = core->rawRead8(core, events + 1, -1);
		uint32_t objectList = rd32(core, events + 4);
		uint32_t warpList = rd32(core, events + 8);
		for (unsigned i = 0; i < warps && i < 64 && romPointer(warpList); ++i) {
			int wx = (int16_t) core->rawRead16(core, warpList + i * 8, -1) - map->originX;
			int wy = (int16_t) core->rawRead16(core, warpList + i * 8 + 2, -1) - map->originY;
			if (wx >= 0 && wy >= 0 && wx < MAP_VIEW_W && wy < MAP_VIEW_H) {
				map->cell[wy][wx] = CELL_WARP;
			}
		}
		for (unsigned i = 0; i < objects && i < 64 && romPointer(objectList); ++i) {
			int ox = (int16_t) core->rawRead16(core, objectList + i * 24 + 4, -1) - map->originX;
			int oy = (int16_t) core->rawRead16(core, objectList + i * 24 + 6, -1) - map->originY;
			if (ox >= 0 && oy >= 0 && ox < MAP_VIEW_W && oy < MAP_VIEW_H && map->cell[oy][ox] != CELL_WARP) {
				map->cell[oy][ox] = CELL_PERSON;
			}
		}
	}
	mapName(core, layout, core->rawRead8(core, layout->mapHeader + 0x14, -1), map->name, sizeof(map->name));
	map->valid = true;
}

/* Decode one 100-byte party structure. Returns false when the values do not
 * make sense (wrong address, corrupted data).
 *
 * Two layouts exist:
 * - the original games: the 48-byte substructure block is XOR-encrypted with
 *   personality ^ OT ID, shuffled by personality % 24 and checksummed;
 * - Complete FireRed Upgrade hacks (Pokemon Unbound...): stored in plain,
 *   in fixed order (Growth first), checksum left at 0. */
static bool decodeMember(struct mCore* core, const uint8_t* raw, struct PartyMember* m) {
	memset(m, 0, sizeof(*m));
	uint32_t personality = read32(&raw[0x00]);
	uint32_t otId = read32(&raw[0x04]);
	uint32_t key = personality ^ otId;
	uint8_t secure[48];
	for (int i = 0; i < 48; i += 4) {
		uint32_t w = read32(&raw[0x20 + i]) ^ key;
		secure[i] = w;
		secure[i + 1] = w >> 8;
		secure[i + 2] = w >> 16;
		secure[i + 3] = w >> 24;
	}
	uint16_t sum = 0;
	for (int i = 0; i < 48; i += 2) {
		sum += read16(&secure[i]);
	}
	if (personality == 0) {
		return false;
	}
	if (sum == read16(&raw[0x1C])) {
		m->species = read16(&secure[growthPos[personality % 24] * 12]);
	} else {
		/* Plain layout: accept it only if everything else is consistent
		 * (checked below, plus a known species name). */
		m->species = read16(&raw[0x20]);
		char probe[32];
		speciesName(core, m->species, probe, sizeof(probe));
		if (!probe[0] || m->species >= SPECIES_MAX) {
			return false;
		}
	}
	m->egg = (raw[0x13] & 0x04) != 0;
	m->status = read32(&raw[0x50]);
	m->level = raw[0x54];
	m->hp = read16(&raw[0x56]);
	m->maxHp = read16(&raw[0x58]);
	if (m->species == 0 || m->maxHp == 0 || m->hp > m->maxHp || m->level > 100) {
		return false;
	}
	if (m->egg) {
		snprintf(m->name, sizeof(m->name), "Egg");
	} else {
		decodeName(&raw[0x08], 10, m->name, sizeof(m->name));
		speciesName(core, m->species, m->speciesName, sizeof(m->speciesName));
	}
	return true;
}

void pokemonReadParty(struct mCore* core, const char* gameCode, struct Party* party) {
	memset(party, 0, sizeof(*party));
	const struct PartyLayout* layout = findLayout(gameCode);
	if (!layout) {
		return;
	}
	int count = core->rawRead8(core, layout->count, -1);
	if (count <= 0 || count > PARTY_MAX) {
		return;
	}
	uint8_t raw[100];
	for (int slot = 0; slot < count; ++slot) {
		uint32_t base = layout->party + slot * 100;
		for (int i = 0; i < 100; ++i) {
			raw[i] = core->rawRead8(core, base + i, -1);
		}
		if (!decodeMember(core, raw, &party->members[slot])) {
			return; /* hide the panel */
		}
	}
	party->count = count;
	party->valid = true;
}

/* ---- Save file (Generation III flash layout) ----
 * 128 KiB: two copies (slot A at 0x0000, slot B at 0xE000) of 14 sections
 * of 4 KiB. Each section ends with: u16 id (0xFF4), u16 checksum, u32
 * signature 0x08012025 (0xFF8), u32 save counter (0xFFC). The newest copy
 * has the highest counter. Section 0 holds the trainer (name, play time),
 * section 1 the party (FireRed/LeafGreen: count at 0x34, party at 0x38;
 * Ruby/Sapphire/Emerald: 0x234 and 0x238). */
bool pokemonReadSave(struct mCore* core, const char* gameCode, const uint8_t* sav, size_t size,
                     struct Party* party, struct Trainer* trainer) {
	memset(party, 0, sizeof(*party));
	memset(trainer, 0, sizeof(*trainer));
	if (!findLayout(gameCode) || size < 0x1C000) {
		return false;
	}
	bool frlg = !strncmp(gameCode, "BPR", 3) || !strncmp(gameCode, "BPG", 3);
	/* A copy is valid when its 14 sections carry the ids 0..13. The
	 * signature is 0x08012025 in the original games and 0x01121999 in
	 * Complete FireRed Upgrade hacks, so it is not checked. */
	int best = -1;
	uint32_t bestCounter = 0;
	for (int slot = 0; slot < 2; ++slot) {
		const uint8_t* sec = sav + slot * 0xE000;
		uint16_t seen = 0;
		for (int i = 0; i < 14; ++i) {
			uint16_t id = read16(&sec[i * 0x1000 + 0xFF4]);
			if (id < 14) {
				seen |= 1 << id;
			}
		}
		if (seen != 0x3FFF) {
			continue;
		}
		uint32_t counter = read32(&sec[0xFFC]);
		if (best < 0 || counter > bestCounter) {
			best = slot;
			bestCounter = counter;
		}
	}
	if (best < 0) {
		return false;
	}
	const uint8_t* s0 = NULL;
	const uint8_t* s1 = NULL;
	for (int i = 0; i < 14; ++i) {
		const uint8_t* sec = sav + best * 0xE000 + i * 0x1000;
		uint16_t id = read16(&sec[0xFF4]);
		if (id == 0) {
			s0 = sec;
		} else if (id == 1) {
			s1 = sec;
		}
	}
	if (s0 && s0[0] != 0xFF && s0[0] != 0x00) {
		decodeName(s0, 8, trainer->name, sizeof(trainer->name));
		trainer->hours = read16(&s0[0x0E]);
		trainer->minutes = s0[0x10];
		trainer->id = read16(&s0[0x0A]);
		trainer->valid = trainer->name[0] && trainer->hours < 1000 && trainer->minutes < 60;
	}
	/* SaveBlock1 is split over sections 1 to 4 (0xF80 bytes each). */
	const struct PartyLayout* layout = findLayout(gameCode);
	if (s0 && s1 && layout->moneyOffset && layout->moneyOffset < 0xF80 - 4 && layout->keyOffset < 0xF80 - 4) {
		uint32_t money = read32(&s1[layout->moneyOffset]) ^ read32(&s0[layout->keyOffset]);
		if (money <= 999999) {
			trainer->money = money;
			trainer->hasMoney = true;
		}
	}
	if (layout->flagsOffset) {
		uint32_t at = layout->flagsOffset + layout->badgeFlag / 8;
		unsigned sectionId = 1 + at / 0xF80;
		for (int i = 0; i < 14 && sectionId <= 4; ++i) {
			const uint8_t* sec = sav + best * 0xE000 + i * 0x1000;
			if (read16(&sec[0xFF4]) == sectionId) {
				trainer->badges = sec[at % 0xF80];
				trainer->hasBadges = true;
			}
		}
	}
	if (!s1) {
		return trainer->valid;
	}
	uint32_t count = read32(&s1[frlg ? 0x34 : 0x234]);
	const uint8_t* list = &s1[frlg ? 0x38 : 0x238];
	if (count == 0 || count > PARTY_MAX) {
		return trainer->valid;
	}
	for (uint32_t i = 0; i < count; ++i) {
		if (!decodeMember(core, list + i * 100, &party->members[i])) {
			memset(party, 0, sizeof(*party));
			return trainer->valid;
		}
	}
	party->count = count;
	party->valid = true;
	return true;
}

const char* pokemonStatusLabel(const struct PartyMember* m) {
	if (m->egg) {
		return "";
	}
	if (m->hp == 0) {
		return "FNT";
	}
	uint32_t s = m->status;
	if (s & 0x7) return "SLP";
	if (s & 0x88) return "PSN";
	if (s & 0x10) return "BRN";
	if (s & 0x20) return "FRZ";
	if (s & 0x40) return "PAR";
	return "";
}

void pokemonWriteTestParty(struct mCore* core, const char* gameCode) {
	const struct PartyLayout* layout = findLayout(gameCode);
	if (!layout) {
		return;
	}
	static const uint16_t species[PARTY_MAX] = { 10, 11, 13, 16, 19, 21 };
	static const uint8_t levels[PARTY_MAX] = { 54, 52, 50, 47, 49, 55 };
	static const uint16_t hp[PARTY_MAX][2] = { { 160, 160 }, { 81, 158 }, { 22, 155 }, { 120, 120 }, { 0, 230 }, { 170, 182 } };
	static const uint32_t status[PARTY_MAX] = { 0, 0x08, 0, 0x40, 0, 0 };
	for (int slot = 0; slot < PARTY_MAX; ++slot) {
		uint8_t m[100];
		memset(m, 0, sizeof(m));
		uint32_t personality = 24u * (1000u + slot); /* % 24 == 0: Growth first */
		uint32_t otId = 0x0000ABCD;
		memcpy(&m[0], &personality, 4);
		memcpy(&m[4], &otId, 4);
		/* Nickname "TEST A" .. "TEST F" (generic test data). */
		static const uint8_t testName[6] = { 0xCE, 0xBF, 0xCD, 0xCE, 0x00, 0xBB };
		memset(&m[8], 0xFF, 10);
		memcpy(&m[8], testName, 6);
		m[8 + 5] = 0xBB + slot;
		m[0x12] = 2;    /* English */
		m[0x13] = 0x02; /* has species */
		memset(&m[0x14], 0xFF, 7);
		m[0x14] = 0xCE; m[0x15] = 0xBF; m[0x16] = 0xCD; m[0x17] = 0xCE; /* "TEST" */
		uint8_t secure[48];
		memset(secure, 0, sizeof(secure));
		memcpy(&secure[0], &species[slot], 2); /* Growth: species */
		secure[9] = 70;                        /* friendship */
		uint16_t sum = 0;
		for (int i = 0; i < 48; i += 2) {
			sum += secure[i] | (secure[i + 1] << 8);
		}
		memcpy(&m[0x1C], &sum, 2);
		uint32_t key = personality ^ otId;
		for (int i = 0; i < 48; i += 4) {
			uint32_t w;
			memcpy(&w, &secure[i], 4);
			w ^= key;
			memcpy(&m[0x20 + i], &w, 4);
		}
		memcpy(&m[0x50], &status[slot], 4);
		m[0x54] = levels[slot];
		memcpy(&m[0x56], &hp[slot][0], 2);
		memcpy(&m[0x58], &hp[slot][1], 2);
		for (int i = 0; i < 100; ++i) {
			core->rawWrite8(core, layout->party + slot * 100 + i, -1, m[i]);
		}
	}
	core->rawWrite8(core, layout->count, -1, PARTY_MAX);
}
