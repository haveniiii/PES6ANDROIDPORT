/* PES6PORT — what the port ships as: its name, and optionally the one patch it plays.
 *
 * Since 7/11 (docs/38) the app is PES6 "plus": the launcher lists the original and every patch
 * installed in mods/ (patch.h), so PES6_PRODUCT_VERSION is empty. Set to a mods/<id> (CMake
 * -DPES6_PRODUCT_VERSION=<id>) it is the old product of docs/29: that patch first, the original
 * as the menu's "Chilean league: No", nothing else. For development, either way:
 *
 *   PES6_VERSION=original   the factory game (what tools/regress.sh and the
 *                           perf/demo scripts run), or any other mods/<id> */
#ifndef PES6_PRODUCT_H
#define PES6_PRODUCT_H

#define PES6_PRODUCT_NAME "PES6"

#ifndef PES6_PRODUCT_VERSION
#define PES6_PRODUCT_VERSION ""
#endif

#endif
