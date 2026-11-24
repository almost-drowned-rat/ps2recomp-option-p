/* chd2iso.c - a CHD disc image as the .iso the setup's tools read.
 *
 * The game reads a CHD itself (the engine's Disc); only the translation's Python tools need an .iso. On the PC the
 * launcher writes it (launcher.c chd_job, data/build/work/disc_from_chd.iso); an Android app may exec programs only
 * from its nativeLibraryDir, so its setup runs this one, built as lib<name>_exe.so (CMakeLists.txt here,
 * CHD2ISO_OUTPUT_NAME). Agent L's chd2iso_exe.c (ZOE Android 05758d7, Odin 2 device test PASS 2026-11-24), taken
 * into option-p's Android standard: every PS2 Android port reads CHD from its first version.
 *
 * DVD CHDs (chdman createdvd, 2048-byte units) and CD CHDs (createcd: raw 2352-byte frames + subcode, the 2048 data
 * bytes of mode 1 or mode 2 form 1) are read.
 *
 *   chd2iso <in.chd> <out.iso>
 *
 * <out.iso>.progress: "<done> <total>" in sectors, rewritten every 0.1 % (the setup's bar reads it, as it reads the
 * game's --unpack-disc progress). Exit 0 = done, 1 = failed (out.iso removed), 2 = usage. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libchdr/cdrom.h"
#include "libchdr/chd.h"

int main(int argc, char **argv)
{
    chd_file *chd = NULL;
    const chd_header *h;
    unsigned char *hunk = NULL;
    FILE *out = NULL;
    unsigned long long sectors, at;
    unsigned units, data_at = 0, last = ~0u;
    char prog[4096];
    int rc = 1;
    if (argc != 3) {
        fprintf(stderr, "usage: %s <in.chd> <out.iso>\n", argv[0]);
        return 2;
    }
    snprintf(prog, sizeof prog, "%s.progress", argv[2]);
    if (chd_open(argv[1], CHD_OPEN_READ, NULL, &chd) != CHDERR_NONE || !chd) {
        fprintf(stderr, "chd2iso: %s is no readable CHD\n", argv[1]);
        return 1;
    }
    h = chd_get_header(chd);
    if (!h || !h->hunkbytes || !h->unitbytes || h->hunkbytes % h->unitbytes ||
        (h->unitbytes != 2048 && h->unitbytes != CD_FRAME_SIZE)) {
        fprintf(stderr, "chd2iso: %s is no disc CHD (unit %u bytes)\n", argv[1], h ? h->unitbytes : 0);
        goto done;
    }
    units = h->hunkbytes / h->unitbytes;
    sectors = h->logicalbytes / h->unitbytes;
    hunk = (unsigned char *)malloc(h->hunkbytes);
    out = fopen(argv[2], "wb");
    if (!hunk || !out || !sectors) {
        fprintf(stderr, "chd2iso: cannot write %s\n", argv[2]);
        goto done;
    }
    for (at = 0; at < sectors; at++) {
        const unsigned u = (unsigned)(at % units);
        const unsigned char *unit;
        unsigned permille;
        if (u == 0 && chd_read(chd, (unsigned)(at / units), hunk) != CHDERR_NONE) {
            fprintf(stderr, "chd2iso: sector %llu unreadable\n", at);
            goto done;
        }
        unit = hunk + (size_t)u * h->unitbytes;
        if (h->unitbytes == CD_FRAME_SIZE) {                    /* a CD's raw sector: its 2048 data bytes */
            if (!data_at)
                data_at = unit[15] == 2 ? 24 : 16;
            unit += data_at;
        }
        if (fwrite(unit, 1, 2048, out) != 2048) {
            fprintf(stderr, "chd2iso: writing %s failed (storage?)\n", argv[2]);
            goto done;
        }
        permille = (unsigned)((at + 1) * 1000 / sectors);
        if (permille != last) {                                 /* every 0.1 %: the setup's bar */
            FILE *pf = fopen(prog, "w");
            if (pf) {
                fprintf(pf, "%llu %llu\n", at + 1, sectors);
                fclose(pf);
            }
            last = permille;
        }
    }
    rc = 0;
done:
    if (out && fclose(out) != 0)
        rc = 1;
    if (rc && out)
        remove(argv[2]);
    remove(prog);
    free(hunk);
    chd_close(chd);
    return rc;
}
