#include "save.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "log.h"

#define SAVE_MAGIC      0x4A425356u  /* "JBSV" */
#define SLOT_VERSION    3u
#define PROGRESS_VERSION 2u
#define LEGACY_VERSION  1u
#define SAVE_DIR        "sdmc:/switch/junkbot"
#define SLOT_PATH       SAVE_DIR "/slot%d.sav"
#define PROGRESS_PATH   SAVE_DIR "/progress.sav"
#define NO_BEST         0xFFFFu

typedef struct {
    /* Kept for save-format compatibility only. Completion is derived from
       best[]: a best move count can only exist after a clear. */
    uint8_t  beaten[JB_MAX_LEVELS];
    uint16_t best[JB_MAX_LEVELS];
    uint16_t par[JB_MAX_LEVELS];
    uint8_t  unlockAll;
    uint8_t  reserved[3];
} SaveRecord;

/* Version 2 stored all slots in one progress.sav. It is migration-only now:
   explicit writes go to one slotN.sav file per slot. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint8_t  activeSlot;
    uint8_t  reserved[3];
    SaveRecord slots[JB_SAVE_SLOTS];
    uint32_t checksum;
} SaveFile;

/* Version 3: one self-contained file per save slot. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t slot;
    SaveRecord record;
    uint32_t checksum;
} SlotFile;

/* Version 1 was a single root-level save. It is read once for migration only;
   the port never writes outside sdmc:/switch/junkbot/. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint8_t  beaten[JB_MAX_LEVELS];
    uint16_t best[JB_MAX_LEVELS];
    uint16_t par[JB_MAX_LEVELS];
    uint8_t  unlockAll;
    uint8_t  reserved[3];
    uint32_t checksum;
} LegacySave;

static SaveFile s_file;
static bool s_loaded = false;

static uint32_t checksum_bytes(const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static uint32_t file_checksum(const SaveFile *file) {
    return checksum_bytes(file, offsetof(SaveFile, checksum));
}

static uint32_t slot_file_checksum(const SlotFile *file) {
    return checksum_bytes(file, offsetof(SlotFile, checksum));
}

static uint32_t legacy_checksum(const LegacySave *save) {
    return checksum_bytes(save, offsetof(LegacySave, checksum));
}

static void reset_record(SaveRecord *record) {
    memset(record, 0, sizeof(*record));
    for (int i = 0; i < JB_MAX_LEVELS; i++) record->best[i] = NO_BEST;
}

static void reset_file(void) {
    memset(&s_file, 0, sizeof(s_file));
    s_file.magic = SAVE_MAGIC;
    s_file.version = PROGRESS_VERSION;
    s_file.activeSlot = 0;
    for (int i = 0; i < JB_SAVE_SLOTS; i++) reset_record(&s_file.slots[i]);
}

static SaveRecord *active_record(void) {
    int slot = s_file.activeSlot;
    if (slot < 0 || slot >= JB_SAVE_SLOTS) slot = 0;
    return &s_file.slots[slot];
}

static const SaveRecord *const_active_record(void) {
    return active_record();
}

static SaveRecord *slot_record(int slot) {
    if (slot < 0 || slot >= JB_SAVE_SLOTS) return NULL;
    return &s_file.slots[slot];
}

static const SaveRecord *const_slot_record(int slot) {
    return slot_record(slot);
}

static bool ensure_dir(void) {
    if (mkdir("sdmc:/switch", 0777) != 0 && errno != EEXIST) return false;
    if (mkdir(SAVE_DIR, 0777) != 0 && errno != EEXIST) return false;
    return true;
}

static void slot_path(int slot, char *path, size_t size) {
    if (slot < 0 || slot >= JB_SAVE_SLOTS) slot = 0;
    snprintf(path, size, SLOT_PATH, slot + 1);
}

static bool read_slot_file(int slot, SaveRecord *out) {
    char path[128];
    slot_path(slot, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    SlotFile tmp;
    size_t got = fread(&tmp, 1, sizeof(tmp), f);
    fclose(f);
    if (got != sizeof(tmp) || tmp.magic != SAVE_MAGIC ||
        tmp.version != SLOT_VERSION || tmp.slot != (uint32_t)slot ||
        tmp.checksum != slot_file_checksum(&tmp)) {
        LOG("save slot %d file %s is invalid", slot + 1, path);
        return false;
    }
    *out = tmp.record;
    return true;
}

static bool write_slot_file(int slot) {
    if (!s_loaded) return false;
    const SaveRecord *record = slot_record(slot);
    if (!record || !ensure_dir()) {
        LOG("save slot %d directory FAILED: %s", slot + 1, SAVE_DIR);
        return false;
    }
    SlotFile file;
    memset(&file, 0, sizeof(file));
    file.magic = SAVE_MAGIC;
    file.version = SLOT_VERSION;
    file.slot = (uint32_t)slot;
    file.record = *record;
    file.checksum = slot_file_checksum(&file);

    char path[128];
    slot_path(slot, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) {
        LOG("save slot %d write FAILED: %s", slot + 1, path);
        return false;
    }
    bool ok = fwrite(&file, 1, sizeof(file), f) == sizeof(file);
    if (fclose(f) != 0) ok = false;
    LOG("save slot %d %s: %s", slot + 1, path, ok ? "written" : "FAILED");
    return ok;
}

/* Version 2's single progress.sav is migrated into memory once; explicit
   writes from then on create the per-slot files. The old file is never
   rewritten. */
static bool read_progress_save(SaveFile *out) {
    FILE *f = fopen(PROGRESS_PATH, "rb");
    if (!f) return false;
    SaveFile tmp;
    size_t got = fread(&tmp, 1, sizeof(tmp), f);
    fclose(f);
    if (got != sizeof(tmp) || tmp.magic != SAVE_MAGIC ||
        tmp.version != PROGRESS_VERSION || tmp.checksum != file_checksum(&tmp))
        return false;
    if (tmp.activeSlot >= JB_SAVE_SLOTS) tmp.activeSlot = 0;
    *out = tmp;
    return true;
}

static bool read_legacy_save(SaveFile *out) {
    static const char *const paths[] = {
        "sdmc:/junkbot.sav", "sdmc:/switch/junkbot.sav"
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        FILE *f = fopen(paths[i], "rb");
        if (!f) continue;
        LegacySave tmp;
        size_t got = fread(&tmp, 1, sizeof(tmp), f);
        fclose(f);
        if (got != sizeof(tmp) || tmp.magic != SAVE_MAGIC ||
            tmp.version != LEGACY_VERSION || tmp.checksum != legacy_checksum(&tmp)) {
            LOG("legacy save %s is invalid", paths[i]);
            continue;
        }
        reset_record(&out->slots[0]);
        /* The legacy beaten bytes are migrated for fidelity but ignored:
           completion comes from best[] from now on. */
        memcpy(out->slots[0].beaten, tmp.beaten, sizeof(tmp.beaten));
        memcpy(out->slots[0].best, tmp.best, sizeof(tmp.best));
        memcpy(out->slots[0].par, tmp.par, sizeof(tmp.par));
        out->slots[0].unlockAll = tmp.unlockAll;
        out->activeSlot = 0;
        LOG("migrated legacy save %s into SLOT 1", paths[i]);
        return true;
    }
    return false;
}

void save_init(void) {
    reset_file();
    s_loaded = true;

    int loaded = 0;
    for (int i = 0; i < JB_SAVE_SLOTS; i++) {
        if (read_slot_file(i, &s_file.slots[i])) loaded++;
    }
    if (loaded) {
        LOG("loaded %d/%d slot file(s)", loaded, JB_SAVE_SLOTS);
    } else if (read_progress_save(&s_file)) {
        LOG("migrated %s into SLOT files on next explicit save", PROGRESS_PATH);
    } else if (read_legacy_save(&s_file)) {
        LOG("legacy save migrated into SLOT 1; written on next explicit save");
    } else {
        LOG("no save file found; %d empty slots initialised", JB_SAVE_SLOTS);
    }
    /* Deliberately no write here: the first explicit save creates the file. */
}

void save_flush(void) { write_slot_file(save_active_slot()); }

int save_slot_count(void) { return JB_SAVE_SLOTS; }

int save_active_slot(void) {
    int slot = s_file.activeSlot;
    return (slot >= 0 && slot < JB_SAVE_SLOTS) ? slot : 0;
}

bool save_set_active_slot(int slot) {
    if (!slot_record(slot)) return false;
    s_file.activeSlot = (uint8_t)slot;
    LOG("active save slot -> %d (in memory only)", slot + 1);
    return true;
}

bool save_slot_used(int slot) {
    const SaveRecord *record = const_slot_record(slot);
    if (!record) return false;
    if (record->unlockAll) return true;
    for (int i = 0; i < JB_MAX_LEVELS; i++)
        if (record->best[i] != NO_BEST) return true;
    return false;
}

int save_slot_cleared_count(int slot) {
    const SaveRecord *record = const_slot_record(slot);
    if (!record) return 0;
    int last = g_levelCount > 0 ? g_levelCount : JB_MAX_LEVELS;
    int count = 0;
    for (int i = 0; i < last && i < JB_MAX_LEVELS; i++)
        if (record->best[i] != NO_BEST) count++;
    return count;
}

int save_slot_par_count(int slot) {
    const SaveRecord *record = const_slot_record(slot);
    if (!record) return 0;
    int last = g_levelCount > 0 ? g_levelCount : JB_MAX_LEVELS;
    int count = 0;
    for (int i = 0; i < last && i < JB_MAX_LEVELS; i++) {
        if (record->best[i] == NO_BEST) continue;
        if (record->par[i] > 0 && record->best[i] <= record->par[i]) count++;
    }
    return count;
}

void save_slot_summary(int slot, char *out, size_t size) {
    if (!out || size == 0) return;
    if (!const_slot_record(slot)) {
        snprintf(out, size, "INVALID");
        return;
    }
    if (!save_slot_used(slot)) {
        snprintf(out, size, "EMPTY - A to start a new file");
        return;
    }
    int last = g_levelCount > 0 ? g_levelCount : JB_MAX_LEVELS;
    snprintf(out, size, "%d/%d cleared   %d GOLD%s",
             save_slot_cleared_count(slot), last, save_slot_par_count(slot),
             const_slot_record(slot)->unlockAll ? "   ALL BUILDINGS OPEN" : "");
}

void save_erase_slot(int slot) {
    SaveRecord *record = slot_record(slot);
    if (!record) return;
    reset_record(record);
    LOG("save slot %d erased", slot + 1);
    write_slot_file(slot);
}

void save_erase_all(void) {
    static const char *const paths[] = {
        "sdmc:/switch/junkbot/slot1.sav",
        "sdmc:/switch/junkbot/slot2.sav",
        "sdmc:/switch/junkbot/slot3.sav",
        PROGRESS_PATH,
        "sdmc:/junkbot.sav",
        "sdmc:/switch/junkbot.sav",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        if (remove(paths[i]) == 0)
            LOG("erased %s", paths[i]);
    }
    reset_file();
    LOG("all Junkbot save data erased");
}

bool save_copy_slot(int src, int dst) {
    SaveRecord *from = slot_record(src);
    SaveRecord *to = slot_record(dst);
    if (!from || !to || src == dst) return false;
    *to = *from;
    LOG("save slot %d copied to slot %d", src + 1, dst + 1);
    write_slot_file(dst);
    return true;
}

bool save_slot_is_beaten(int slot, int index) {
    /* A best move count can only be written by a clear. */
    return save_slot_best_moves(slot, index) >= 0;
}

int save_slot_best_moves(int slot, int index) {
    const SaveRecord *record = const_slot_record(slot);
    if (!record || index < 0 || index >= JB_MAX_LEVELS ||
        record->best[index] == NO_BEST)
        return -1;
    return (int)record->best[index];
}

bool save_slot_got_par(int slot, int index) {
    const SaveRecord *record = const_slot_record(slot);
    if (!record) return false;
    int best = save_slot_best_moves(slot, index);
    return best >= 0 && record->par[index] > 0 && best <= record->par[index];
}

static int slot_building_cleared(const SaveRecord *record, int building) {
    if (!record || building < 0) return 0;
    int count = 0;
    int first = building * JB_LEVELS_PER_GROUP;
    int last = first + JB_LEVELS_PER_GROUP;
    if (last > g_levelCount) last = g_levelCount;
    for (int i = first; i < last && i < JB_MAX_LEVELS; i++)
        if (record->best[i] != NO_BEST) count++;
    return count;
}

bool save_slot_building_unlocked(int slot, int building) {
    const SaveRecord *record = const_slot_record(slot);
    if (!record) return false;
    if (building <= 0) return true;
    if (save_slot_unlock_all(slot)) return true;
    /* Ten clears in the previous building open the next one. */
    const int needed = 10;
    return slot_building_cleared(record, building - 1) >= needed;
}

int save_slot_max_playable_index(int slot) {
    int buildings = (g_levelCount + JB_LEVELS_PER_GROUP - 1) / JB_LEVELS_PER_GROUP;
    int last = 0;
    for (int b = 0; b < buildings; b++) {
        if (!save_slot_building_unlocked(slot, b)) break;
        last = (b + 1) * JB_LEVELS_PER_GROUP - 1;
    }
    if (last >= g_levelCount) last = g_levelCount - 1;
    return last;
}

int save_slot_max_building(int slot) {
    int last = save_slot_max_playable_index(slot);
    if (last < 0) last = 0;
    int building = last / JB_LEVELS_PER_GROUP + 1;
    int total = (g_levelCount + JB_LEVELS_PER_GROUP - 1) / JB_LEVELS_PER_GROUP;
    if (total < 1) total = 1;
    if (building > total) building = total;
    return building;
}

void save_erase(void) { save_erase_slot(save_active_slot()); }

bool save_is_beaten(int index) { return save_slot_is_beaten(save_active_slot(), index); }

int save_best_moves(int index) { return save_slot_best_moves(save_active_slot(), index); }

bool save_got_par(int index) { return save_slot_got_par(save_active_slot(), index); }

void save_record_win(int index, int moves, int par) {
    SaveRecord *record = active_record();
    if (!record || index < 0 || index >= JB_MAX_LEVELS) return;
    bool changed = false;
    if (par > 0 && record->par[index] != (uint16_t)par) {
        record->par[index] = (uint16_t)par;
        changed = true;
    }
    if (moves >= 0 && moves < 0xFFFF) {
        uint16_t m = (uint16_t)moves;
        if (record->best[index] == NO_BEST || m < record->best[index]) {
            record->best[index] = m;
            changed = true;
        }
    }
    if (changed) {
        LOG("slot %d: saved win for level %d: moves=%d best=%d",
            save_active_slot() + 1, index, moves,
            save_best_moves(index));
        save_flush();
    }
}

int save_building_cleared(int building) {
    return slot_building_cleared(const_active_record(), building);
}

bool save_building_unlocked(int building) {
    return save_slot_building_unlocked(save_active_slot(), building);
}

int save_max_playable_index(void) {
    return save_slot_max_playable_index(save_active_slot());
}

void save_set_unlock_all(void) {
    SaveRecord *record = active_record();
    if (!record || record->unlockAll) return;
    record->unlockAll = 1;
    LOG("slot %d: all buildings unlocked (Konami code)", save_active_slot() + 1);
    save_flush();
}

bool save_slot_unlock_all(int slot) {
    const SaveRecord *record = const_slot_record(slot);
    return record && record->unlockAll != 0;
}

bool save_unlock_all(void) { return save_slot_unlock_all(save_active_slot()); }
