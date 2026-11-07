/* PES6PORT — the patches' importer on the desktop (docs/38 §9): the same patch.c as the app.
 *
 *   pes6patch list   <mods> [<iso>]
 *   pes6patch import <mods> <iso|-> <file> [<audio dir> [<save dir>]]
 *                    a .zip, .afs or PC option file; - for no disc; <save dir> the original's
 *                    system save (default build/SAVEDATA/ULES004760013000)
 *   pes6patch pcof   <option file> <base DATA.BIN> <out DATA.BIN>   the converter alone (pcof.h)
 *   pes6patch bank   <audio dir> <file.afs>                the commentary bank
 *   pes6patch delete <mods> <id>
 *
 * Exit status 0 when accepted, 1 when refused (the reason on stdout), 2 on bad usage. */
#include "lang.h"
#include "pcof.h"
#include "patch.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int usage(void) {
    fprintf(stderr, "usage: pes6patch list <mods> | import <mods> <iso|-> <file> [<audio> [<save>]] | bank <audio> <file> |\n"
                    "       delete <mods> <id> | pcof <option file> <base DATA.BIN> <out DATA.BIN>\n");
    return 2;
}

int main(int argc, char **argv) {
    pes6_lang_init(getenv("LANG"));
    if (argc < 3) return usage();
    const char *cmd = argv[1];
    if (!strcmp(cmd, "list")) {
        static pes6_patch p[PES6_PATCH_MAX];
        const int n = pes6_patch_list(argv[2], p, PES6_PATCH_MAX);
        for (int i = 0; i < n; i++)
            printf("%-24s %-32s %-20s %-10s %8.1f MB  replaces=%02x voice=%d%s\n", p[i].id, p[i].name, p[i].author,
                   p[i].version, (double)p[i].bytes / 1048576.0, p[i].replaces, p[i].voice, p[i].has_json ? "" : " (version.txt)");
        return 0;
    }
    if (!strcmp(cmd, "delete") && argc == 4) return pes6_patch_delete(argv[2], argv[3]) == 0 ? 0 : 1;
    if (!strcmp(cmd, "pcof") && argc == 5) {
        static uint8_t of[PCOF_SIZE + 1], psp[PCOF_SIZE + 1];
        FILE *a = fopen(argv[2], "rb"), *b = fopen(argv[3], "rb");
        const size_t na = a ? fread(of, 1, sizeof of, a) : 0, nb = b ? fread(psp, 1, sizeof psp, b) : 0;
        if (a) fclose(a);
        if (b) fclose(b);
        const int kind = pes6_pcof_probe(of, na);
        if (kind < 0 || nb != PCOF_SIZE) {
            printf("REFUSED %s\n", nb != PCOF_SIZE ? "base: not a PSP system save" : kind == -PCOF_CHECKSUM ? "checksums" : "size");
            return 1;
        }
        if (kind == 1) pes6_pcof_decrypt(of);
        pes6_pcof_to_psp(of, psp);
        FILE *o = fopen(argv[4], "wb");
        if (!o || fwrite(psp, 1, PCOF_SIZE, o) != PCOF_SIZE) return 1;
        fclose(o);
        printf("ACCEPTED %s option file\n", kind ? "encrypted" : "plain");
        return 0;
    }
    if ((!strcmp(cmd, "import") && argc >= 5) || (!strcmp(cmd, "bank") && argc == 4)) {
        const char *file = argv[argc == 4 && !strcmp(cmd, "bank") ? 3 : 4];
        const int fd = open(file, O_RDONLY);
        if (fd < 0) { perror(file); return 2; }
        const char *name = strrchr(file, '/') ? strrchr(file, '/') + 1 : file;
        static pes6_patch_job job;
        int rc;
        if (!strcmp(cmd, "bank")) {
            rc = pes6_bank_import(&job, fd, name, argv[2]);
        } else {
            const char *save = argc > 6 ? argv[6] : "build/SAVEDATA/ULES004760013000";
            const pes6_disc disc = { strcmp(argv[3], "-") ? argv[3] : NULL, 0, strcmp(save, "-") ? save : NULL };
            rc = pes6_patch_import(&job, fd, name, argv[2], &disc, argc > 5 ? argv[5] : "audio");
        }
        printf("%s %s\n", rc == 0 ? "ACCEPTED" : "REFUSED", job.msg);
        return rc == 0 ? 0 : 1;
    }
    return usage();
}
