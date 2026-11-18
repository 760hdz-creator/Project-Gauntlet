/* For each out/texraw dump: candidate hashes of the index data (linear) and palette variants. */
#define XXH_INLINE_ALL
#include "xxhash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
int main(int argc, char **argv) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA("../texraw/*.bin", &fd);
    if (h == INVALID_HANDLE_VALUE) return 1;
    do {
        char path[512];
        snprintf(path, sizeof path, "../texraw/%s", fd.cFileName);
        FILE *f = fopen(path, "rb");
        unsigned hdr[3];
        fread(hdr, 4, 3, f);
        unsigned w = hdr[0], hh = hdr[1], pl = hdr[2], n = w * hh;
        unsigned char *idx = malloc(n);
        unsigned pal[256] = {0};
        fread(idx, 1, n, f);
        fread(pal, 4, pl, f);
        fclose(f);
        /* palette variants: PS2 CLUT = R,G,B,A bytes (u32 0xAABBGGRR), A halved (0x80 = opaque) */
        unsigned p1[256], p2[256];
        for (unsigned k = 0; k < 256; k++) {
            unsigned c = pal[k], a = c >> 24, r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
            p1[k] = r | g << 8 | b << 16 | a << 24;
            unsigned a2 = a == 255 ? 0x80 : (a + 1) / 2;
            p2[k] = r | g << 8 | b << 16 | a2 << 24;
        }
        printf("%s %u %u %u idx3 %016llx idx64 %016llx pal3a %016llx pal3b %016llx pal3c %016llx\n", fd.cFileName, w, hh, pl,
               (unsigned long long)XXH3_64bits(idx, n), (unsigned long long)XXH64(idx, n, 0),
               (unsigned long long)XXH3_64bits(pal, pl * 4), (unsigned long long)XXH3_64bits(p1, pl * 4),
               (unsigned long long)XXH3_64bits(p2, pl * 4));
        free(idx);
    } while (FindNextFileA(h, &fd));
    return 0;
}
