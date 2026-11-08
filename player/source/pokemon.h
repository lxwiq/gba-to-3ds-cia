/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PLAYER_POKEMON_H
#define PLAYER_POKEMON_H

#include <stdbool.h>
#include <stdint.h>

struct mCore;

#define PARTY_MAX 6

struct PartyMember {
	char name[32];      /* UTF-8 nickname */
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

#endif
