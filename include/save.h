#ifndef JUNKBOT_SAVE_H
#define JUNKBOT_SAVE_H

#include <stdbool.h>
#include <stddef.h>

#include "game.h"

#define JB_SAVE_SLOTS 3

/* Persistent progress: three independent files under
   sdmc:/switch/junkbot/ (slot1.sav, slot2.sav, slot3.sav). A level is "beaten"
   when it has been cleared once; the best (lowest) move count is kept as the
   high score. Par is achieved when best <= level par. */
void save_init(void);
/* Writes the active slot's file. Only called after an explicit save action. */
void save_flush(void);

int  save_slot_count(void);
int  save_active_slot(void);
bool save_set_active_slot(int slot);          /* memory only, never writes */
bool save_slot_used(int slot);
int  save_slot_cleared_count(int slot);
int  save_slot_par_count(int slot);
void save_slot_summary(int slot, char *out, size_t size);
int  save_slot_max_building(int slot);  /* 1-based, for the file-select row */
void save_erase_slot(int slot);
/* Destructive title-screen option: removes every slot file, the legacy
   progress files, and resets all in-memory progress. */
void save_erase_all(void);
bool save_copy_slot(int src, int dst);        /* false for empty/invalid copy */

/* Wipe the active slot and persist it (compatibility helper). */
void save_erase(void);

/* Slot-aware queries used by the file-select preview. */
bool save_slot_is_beaten(int slot, int index);
int  save_slot_best_moves(int slot, int index);
bool save_slot_got_par(int slot, int index);
bool save_slot_building_unlocked(int slot, int building);
bool save_slot_unlock_all(int slot);         /* Konami flag for this file */
int  save_slot_max_playable_index(int slot);

/* These operate on the active slot and are what gameplay uses. Completion is
   derived from the best-move record: no separate cleared flag is needed. */
bool save_is_beaten(int index);
int  save_best_moves(int index);              /* -1 when never cleared */
bool save_got_par(int index);
void save_record_win(int index, int moves, int par);
int  save_building_cleared(int building);
bool save_building_unlocked(int building);
int  save_max_playable_index(void);

/* Konami code unlocks every building in the active slot. */
void save_set_unlock_all(void);
bool save_unlock_all(void);

#endif
