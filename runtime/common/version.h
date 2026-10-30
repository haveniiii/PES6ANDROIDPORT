/* PES6PORT — game versions: the factory game or a mod that changes it (docs/17).
 *
 * A version other than "original" is a directory mods/<id>/ with a version.txt
 * (first line: its title), and optionally textures/ (replacement textures, see
 * ge.c) and save/<gameName><saveName>/ (the save it starts from). Each version
 * keeps its own saves -- the game writes edits there -- so choosing one sets:
 *
 *   PES6_SAVEDATA  <io root>/SAVEDATA                  for the original
 *                  <io root>/versions/<id>/SAVEDATA    for a mod, seeded from
 *                                                      mods/<id>/save the first time
 *   PES6_MOD       ""  /  <id>                         which textures apply
 *
 * The host shows the launcher (launcher.h) to pick one, or takes PES6_VERSION. */
#ifndef PES6_VERSION_H
#define PES6_VERSION_H

typedef struct {
    char id[64];        /* "original" or the mod directory's name */
    char title[96];
} pes6_version;

/* The versions available. Returns how many. In the product build (product.h)
 * the product's version, then the original (the launcher's "Liga chilena" switch),
 * plus the one PES6_VERSION names; without the product's data only that one, so
 * 0 when none is named. Otherwise [0] is the original. */
int pes6_versions_list(const char *mods_dir, pes6_version *out, int max);

/* Index of the version last chosen (remembered under the io root), or 0. */
int pes6_version_last(const char *io_root, const pes6_version *v, int n);

/* Index of the version named `id` (PES6_VERSION), or -1. */
int pes6_version_find(const pes6_version *v, int n, const char *id);

/* Make `v` the one the game runs: environment, first-time save, remembered
 * choice. Returns 0, or -1 if its save could not be prepared. */
int pes6_version_apply(const pes6_version *v, const char *io_root, const char *mods_dir);

#endif
