/* PES6PORT — rollback study probe (docs/27 §5), desktop only, off unless PES6_RB is set. */
#pragma once
#include <stdint.h>

void pes6_rb_setup(void);                 /* registers the vblank task when PES6_RB is set */
uint32_t pes6_rb_pad(uint32_t *analog);   /* PES6_RB=replay: a pad made only from guest state */
