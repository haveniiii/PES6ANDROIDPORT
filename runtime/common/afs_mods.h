/* PES6PORT — mods that replace members of the disc's .afs archives (docs/28):
 * mods/<id>/afs/<archive>/<slot>.bin. */
#ifndef PES6_AFS_MODS_H
#define PES6_AFS_MODS_H

/* Lay the members of mods/<id>/afs over the disc image. Needs the HLE's disc
 * set up (psp_hle_init). Returns how many members were replaced. */
int pes6_afs_mods_load(const char *mods_dir, const char *id);

/* Silence the PSP's own goal shout ("It's in! Yes!", the game's only voice: sceSas banks
 * 8941..8949 of 0_text.afs, one per language), at run time: each bank is read from the player's
 * disc, its VAG ADPCM zeroed but for the flags (length and loops unchanged) and laid over the image
 * in memory -- the ISO is not touched and nothing ships with the app. For a match with our
 * commentary on (docs/39 §2.1): Brackley's shout and the PSP's no longer speak over each other.
 * Returns the banks silenced (5), or -1. */
int pes6_afs_goal_voice_off(void);

#endif
