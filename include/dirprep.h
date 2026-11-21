#ifndef JB_DIRPREP_H
#define JB_DIRPREP_H

#include <stdbool.h>
#include <stddef.h>

typedef void (*JbPrepProgress)(const char *stage, int done, int total, void *user);

/* Extract a user-supplied junkbot.dir into a staging tree whose paths match
   junkbot.otr virtual paths. The tree is packed by otr_pack_directory(). */
bool dirprep_extract(const char *dirPath, const char *stageDir,
                     JbPrepProgress progress, void *user,
                     char *error, size_t errorSize);

#endif
