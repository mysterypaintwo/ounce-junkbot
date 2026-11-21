#ifndef JB_ASSETS_H
#define JB_ASSETS_H

#include <SDL.h>
#include <SDL_image.h>
#include <stdbool.h>
#include <stddef.h>

#define JB_OTR_FILENAME "junkbot.otr"
#define JB_DIR_FILENAME "junkbot.dir"

/* Mount the external asset package and the small code-only romfs. If the
   package is missing, assets_init() still succeeds so prep/error screens can
   run; callers must check assets_has_package() before loading game data. */
bool assets_init(const char *arg0);
void assets_exit(void);
bool assets_has_package(void);
const char *assets_package_source(void);
const char *assets_error(void);

/* Preferred on-disk locations, derived from argv[0] when possible. */
const char *assets_package_path(void);
const char *assets_dir_path(void);

/* Read a virtual path from the package first, then from code-only romfs. */
void *assets_read(const char *vpath, size_t *outSize);
char *assets_read_text(const char *vpath);
bool assets_write(const char *vpath, const void *data, size_t size);
bool assets_has(const char *vpath);

/* SDL_RWops over a private copy of an entry. SDL_RWclose() frees the copy. */
SDL_RWops *assets_open_rw(const char *vpath);

/* Try image extensions in a stable order. `base` has no extension. */
SDL_Texture *assets_load_texture(SDL_Renderer *renderer, const char *base);

/* Pack a staging directory into a deterministic JOTR archive. */
bool otr_pack_directory(const char *srcDir, const char *outPath,
                        const char *sourceLabel, char *error, size_t errorSize);

/* List virtual paths under a prefix. Used for level lookup. */
int assets_list(const char *prefix, char ***outNames);
void assets_list_free(char **names, int count);

#endif
