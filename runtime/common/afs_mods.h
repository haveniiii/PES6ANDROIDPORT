/* PES6PORT — mods that replace members of the disc's .afs archives (docs/28):
 * mods/<id>/afs/<archive>/<slot>.bin. */
#ifndef PES6_AFS_MODS_H
#define PES6_AFS_MODS_H

/* Lay the members of mods/<id>/afs over the disc image. Needs the HLE's disc
 * set up (psp_hle_init). Returns how many members were replaced. */
int pes6_afs_mods_load(const char *mods_dir, const char *id);

#endif
