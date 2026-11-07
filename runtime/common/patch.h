/* PES6PORT — patches: mods/<id>/ with a mod.json, their import and the player's own commentary
 * bank (docs/38).
 *
 * A patch is a game version (version.h): mods/<id>/ with mod.json (or, from before, version.txt),
 * and its save/, afs/, textures/, faces/, commentary/. The importer takes what the player already
 * has -- a .zip (with a mod.json, or "raw": PSP .afs archives, a save folder), a loose PSP .afs, or
 * a PS2 commentary bank -- checks all of it first and only then puts it in place: it is accepted
 * whole or refused with the reason, never left half done. Nothing of the game's own data is kept:
 * a PSP .afs is compared with the disc's and only the members that differ are stored.
 *
 * Messages are for the pre-game menu (lang.h; its font has no other accents than Spanish ones). */
#ifndef PES6_PATCH_H
#define PES6_PATCH_H

#include <stdint.h>

enum {                                /* mod.json "replaces" */
    PATCH_SAVE = 1, PATCH_AFS = 2, PATCH_TEXTURES = 4, PATCH_FACES = 8, PATCH_COMMENTARY = 16,
};
enum { PATCH_VOICE_NONE, PATCH_VOICE_OWN, PATCH_VOICE_BASE };   /* mod.json "commentary" */

typedef struct {
    char id[64];
    char name[72];
    char author[72];
    char version[32];
    char desc[408];
    char base[16];
    unsigned replaces;                /* PATCH_* */
    int voice;                        /* PATCH_VOICE_* */
    int has_json;                     /* 0: an older version.txt patch */
    int64_t bytes;                    /* on disk (pes6_patch_list fills it) */
} pes6_patch;

#define PES6_PATCH_FORMAT 1
#define PES6_PATCH_BASE "ULES00476"
#define PES6_PATCH_MAX 32

/* The patches in `mods` (directories with mod.json or version.txt), sorted by id. */
int pes6_patch_list(const char *mods, pes6_patch *out, int max);
/* One of them; 0, or -1 if `mods/<id>` is not a patch. */
int pes6_patch_read(const char *mods, const char *id, pes6_patch *p);
/* Remove mods/<id> (not the player's saves of it). 0 or -1. */
int pes6_patch_delete(const char *mods, const char *id);
/* Leftovers of an import that did not finish (mods/.import-*): removed. */
void pes6_patch_clean(const char *mods);

/* Parse a mod.json text into *p, checking the rules of docs/38 §2.2 that need no files.
 * Returns 0, or -1 with the reason in msg. */
int pes6_patch_parse(const char *json, pes6_patch *p, char *msg, int cap);

/* The player's own game, which imports are made against: the ISO (path and where in it the image
 * starts: 0, or the offset inside the APK; NULL: no disc yet) to compare a PSP .afs with, and the
 * original game's system save folder (<io root>/SAVEDATA/ULES004760013000; NULL or missing: none
 * yet) that a PC option file is laid over (pcof.h). */
typedef struct { const char *path; int64_t offset; const char *save; } pes6_disc;

/* An import in progress, for a thread and the menu (all fields readable while it runs). */
typedef struct {
    volatile int done;                /* 0 running, 1 accepted, -1 refused */
    volatile int64_t at, total;       /* bytes, for the progress */
    int bank;                         /* it was a commentary bank (went to audio_dir) */
    char id[64];                      /* the patch installed */
    char msg[200];                    /* what happened: accepted, or why not */
} pes6_patch_job;

/* Import the file open on `fd` (closed when done) named `name` (for its type and the id of a raw
 * one). A PS2 commentary bank goes to audio_dir (pes6_bank_*). In the calling thread: returns
 * 0 or -1, with job->msg. */
int pes6_patch_import(pes6_patch_job *job, int fd, const char *name, const char *mods,
                      const pes6_disc *disc, const char *audio_dir);
/* The same in a thread of its own; job->done says when it is over. 0, or -1 if no thread. */
int pes6_patch_import_start(pes6_patch_job *job, int fd, const char *name, const char *mods,
                            const pes6_disc *disc, const char *audio_dir);
/* Only a commentary bank (the menu's Commentator row): anything else is refused. */
int pes6_bank_import(pes6_patch_job *job, int fd, const char *name, const char *audio_dir);
int pes6_bank_import_start(pes6_patch_job *job, int fd, const char *name, const char *audio_dir);

/* The player's bank in audio_dir: its path into `afs`, its catalogue's name into `catalogue`
 * ("e_sound.txt") and who speaks into `label`. 0, or -1 if there is none. */
int pes6_bank_installed(const char *audio_dir, char *afs, int afs_cap, char *catalogue, int cat_cap,
                        char *label, int label_cap);
int pes6_bank_delete(const char *audio_dir);

/* Which commentary the version `id` ("" or "original": the factory game) plays (docs/38 §5):
 * PATCH_VOICE_BASE the player's bank, OWN mods/<id>/commentary, NONE silence. */
int pes6_patch_voice(const char *mods, const char *id);
/* The player's bank and its catalogue (<mods>/catalogues/<name>, shipped with the app) for
 * pes6_commentary_open. 0, or -1 if there is no bank or no catalogue for it. */
int pes6_bank_paths(const char *audio_dir, const char *mods, char *afs, int afs_cap, char *catalogue, int cat_cap);

#endif
