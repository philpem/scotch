/* Load every vendored Acorn/Replay decompressor module under the ARMulator
 * harness and confirm it initialises cleanly. This guards both the harness (the
 * 26-bit MOV pc,Rn return path in particular) and the checked-in modules.
 *
 * usage: test_codec_modules_init <vendor/armovie-codecs dir>
 */

#include "replay/codecif.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const modules[] = {
    /* Acorn: compressed */
    "Decomp7", "Decomp17", "Decomp19", "Decomp20", "Decomp20new", "MovingLine",
    /* Acorn: uncompressed */
    "Decomp2", "Decomp3", "Decomp4", "Decomp5", "Decomp6", "Decomp8",
    "Decomp9", "Decomp10", "Decomp11", "Decomp16", "Decomp21", "Decomp22",
    "Decomp23", "Decomp24", "Decomp25", "Decomp26", "Decomp27",
    /* Non-Acorn (freeware): Eidos Escape, Henrik Bjerregaard Pedersen */
    "Decomp100", "Decomp800", "Decomp802"
};

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b;
    long s;
    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (s = ftell(f)) < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    b = malloc((size_t)s ? (size_t)s : 1);
    if (b == NULL || (s > 0 && fread(b, 1, (size_t)s, f) != (size_t)s)) {
        fclose(f);
        free(b);
        return NULL;
    }
    fclose(f);
    *len = (size_t)s;
    return b;
}

int main(int argc, char **argv)
{
    const char *dir;
    size_t i;
    int fails = 0;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <codecs-dir>\n", argv[0]);
        return 2;
    }
    dir = argv[1];

    for (i = 0; i < sizeof modules / sizeof modules[0]; i++) {
        char path[1024], err[256];
        size_t len;
        uint8_t *module;
        ReplayCodecIf *cif;

        snprintf(path, sizeof path, "%s/%s/Decompress,ffd", dir, modules[i]);
        module = read_file(path, &len);
        if (module == NULL) {
            printf("MISSING %s\n", modules[i]);
            fails++;
            continue;
        }
        cif = replay_codecif_open(module, len, 160, 128, REPLAY_ARM_MODE_26,
                                  err, sizeof err);
        if (cif == NULL) {
            printf("FAIL    %s: %s\n", modules[i], err);
            fails++;
        } else {
            printf("ok      %s\n", modules[i]);
            replay_codecif_close(cif);
        }
        free(module);
    }

    if (fails != 0) {
        printf("\n%d module(s) failed to initialise\n", fails);
        return 1;
    }
    printf("\nall %zu modules initialised\n", sizeof modules / sizeof modules[0]);
    return 0;
}
