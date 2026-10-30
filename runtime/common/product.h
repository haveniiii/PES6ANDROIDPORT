/* PES6PORT — what the port ships as (docs/29): its name and the one version it plays.
 *
 * The app (named "PES6") plays one game version only: the Chilean patch,
 * mods/<PES6_PRODUCT_VERSION>/ (docs/28). The launcher lists nothing else, so the factory game cannot be reached
 * from the interface. For development the other versions stay one variable away:
 *
 *   PES6_VERSION=original   the factory game (what tools/regress.sh and the
 *                           perf/demo scripts run), or any other mods/<id>
 *
 * A build configured with -DPES6_PRODUCT_VERSION= (empty) is the old multi-version
 * port: the original first and every mod with a version.txt (docs/17). */
#ifndef PES6_PRODUCT_H
#define PES6_PRODUCT_H

#define PES6_PRODUCT_NAME "PES6"

#ifndef PES6_PRODUCT_VERSION
#define PES6_PRODUCT_VERSION "pech2026"
#endif

#endif
