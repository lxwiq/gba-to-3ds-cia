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
};

/* Addresses from the pret decompilations (USA builds). FireRed-based hacks
 * built with Complete FireRed Upgrade (Pokemon Unbound and many others) keep
 * the FireRed addresses. */
static const struct PartyLayout layouts[] = {
	{ "BPR", 0x02024284, 0x02024029 }, /* FireRed */
	{ "BPG", 0x02024284, 0x02024029 }, /* LeafGreen */
	{ "BPE", 0x020244EC, 0x020244E9 }, /* Emerald */
	{ "AXV", 0x03004360, 0x03004350 }, /* Ruby */
	{ "AXP", 0x03004360, 0x03004350 }, /* Sapphire */
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
