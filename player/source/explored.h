/* SPDX-License-Identifier: MPL-2.0 */
#ifndef PLAYER_EXPLORED_H
#define PLAYER_EXPLORED_H

#include <stdbool.h>

#include "pokemon.h"

/* Areas of each map the player has seen on the top screen, kept on the SD
 * card so the MAP page never shows what has not been explored. */
void exploredInit(const char* dir);
/* Mark what is visible around the player, then hide the rest of the view. */
void exploredApply(struct MapView* map);
void exploredFlush(void);

#endif
