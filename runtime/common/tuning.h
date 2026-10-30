/* PES6PORT — gameplay parameters (pes6_tuning.h, docs/25 §2). */
#ifndef PES6_TUNING_H
#define PES6_TUNING_H

#include <stdint.h>
#include <stdio.h>

/* Read the overrides: <io_root>/tuning.txt ("name = value" lines, '#' comments), then the
 * environment PES6_TUNE="name=value,name=value". Call before the game runs any code. */
void pes6_tuning_init(const char *io_root);

/* Write the data parameters of a program just loaded ("EBOOT.elf", "game.ovl"...) into guest
 * memory, each only if the memory holds its original value. */
void pes6_tuning_apply_data(const char *program);

/* The parameters that differ from the original, one line each (for logs and reports). */
void pes6_tuning_report(FILE *out);

#endif
