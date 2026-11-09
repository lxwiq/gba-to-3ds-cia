/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PLAYER_POKEMON_H
#define PLAYER_POKEMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct mCore;

#define PARTY_MAX 6

struct PartyMember {
	char name[32];      /* UTF-8 nickname */
	char speciesName[32]; /* UTF-8 species name, from the ROM */
	uint16_t species;
	uint8_t level;
	uint16_t hp;
	uint16_t maxHp;
	uint32_t status;
	bool egg;
};

struct Party {
	bool valid;         /* the party block was found and every checksum matched */
	int count;
	struct PartyMember members[PARTY_MAX];
};

/* Whether the game code belongs to a Generation III Pokemon game (or a hack
 * built on one). */
bool pokemonSupported(const char* gameCode);

/* Read and decrypt the player's party from EWRAM/IWRAM. */
void pokemonReadParty(struct mCore* core, const char* gameCode, struct Party* party);

/* Short status label ("PSN", "SLP", ...) or "" when healthy. */
const char* pokemonStatusLabel(const struct PartyMember* m);

struct Trainer {
	bool valid;
	char name[32];
	unsigned hours;
	unsigned minutes;
};

/* Player name and play time from SaveBlock2 (FireRed/LeafGreen/Emerald engine). */
void pokemonReadTrainer(struct mCore* core, const char* gameCode, struct Trainer* trainer);

/* Decode the 32x32 party icon (first animation frame) of a species from the
 * ROM, through the Game Freak ROM header tables, into RGBA8 pixels
 * (0xRRGGBBAA, row-major, transparent background). Returns false when the
 * tables look wrong. */
bool pokemonIcon(struct mCore* core, uint16_t species, uint32_t* rgba32x32);

/* Read the party and trainer from a save file (raw 128 KiB flash image),
 * so they can be shown before the game is loaded. */
bool pokemonReadSave(struct mCore* core, const char* gameCode, const uint8_t* sav, size_t size,
                     struct Party* party, struct Trainer* trainer);

/* Test mode only: write a sample party into RAM to check the bottom screen
 * without a save file. */
void pokemonWriteTestParty(struct mCore* core, const char* gameCode);

#endif
