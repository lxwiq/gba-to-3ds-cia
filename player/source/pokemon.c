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
};

/* Addresses from the pret decompilations (USA builds). FireRed-based hacks
 * built with Complete FireRed Upgrade (Pokemon Unbound and many others) keep
 * the FireRed addresses. */
static const struct PartyLayout layouts[] = {
	{ "BPR", 0x02024284, 0x02024029, 0x0300500C }, /* FireRed */
	{ "BPG", 0x02024284, 0x02024029, 0x0300500C }, /* LeafGreen */
	{ "BPE", 0x020244EC, 0x020244E9, 0x03005D90 }, /* Emerald */
	{ "AXV", 0x03004360, 0x03004350, 0 },          /* Ruby */
	{ "AXP", 0x03004360, 0x03004350, 0 },          /* Sapphire */
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
	t->valid = t->name[0] && t->hours < 1000 && t->minutes < 60;
}

void pokemonReadParty(struct mCore* core, const char* gameCode, struct Party* party) {
	memset(party, 0, sizeof(*party));
	const struct PartyLayout* layout = findLayout(gameCode);
	if (!layout) {
		return;
	}
	int count = core->rawRead8(core, layout->count, -1);
	if (count < 0 || count > PARTY_MAX) {
		return;
	}
	uint8_t raw[100];
	for (int slot = 0; slot < count; ++slot) {
		uint32_t base = layout->party + slot * 100;
		for (int i = 0; i < 100; ++i) {
			raw[i] = core->rawRead8(core, base + i, -1);
		}
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
		if (sum != read16(&raw[0x1C]) || personality == 0) {
			return; /* wrong address or corrupted data: hide the panel */
		}
		struct PartyMember* m = &party->members[slot];
		const uint8_t* growth = &secure[growthPos[personality % 24] * 12];
		m->species = read16(growth);
		m->egg = (raw[0x13] & 0x04) != 0;
		m->status = read32(&raw[0x50]);
		m->level = raw[0x54];
		m->hp = read16(&raw[0x56]);
		m->maxHp = read16(&raw[0x58]);
		if (m->species == 0 || m->maxHp == 0 || m->hp > m->maxHp || m->level > 100) {
			return;
		}
		if (m->egg) {
			snprintf(m->name, sizeof(m->name), "Egg");
		} else {
			decodeName(&raw[0x08], 10, m->name, sizeof(m->name));
			speciesName(core, m->species, m->speciesName, sizeof(m->speciesName));
		}
	}
	party->count = count;
	party->valid = count > 0;
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
