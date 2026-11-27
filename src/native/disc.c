/*
 * disc.c - the game's disc, as files: the import of the player's CD image and the sectors
 * the CD drive (cd.c) reads from the imported files.
 *
 * Import (once): the ISO 9660 file system of the image is copied to <data>/files/ (data
 * files as their bytes, streamed audio and video as whole 2352-byte sectors), the few
 * sectors outside any file (volume descriptors, directories) to <data>/meta.bin, and the
 * map to <data>/disc.txt. After that the image is no longer needed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir((p), 0755)
#endif

#include "cd.h"
#include "disc.h"

#define SECTOR 2352
#define USER 2048

typedef struct {
    uint32_t lba, count, size;
    int raw;                      /* stored as whole sectors */
    uint32_t meta_off;            /* meta ranges: first sector in meta.bin */
    int meta;                     /* 1: sectors of meta.bin, 2: copies of one of them (fill) */
    char path[256];
    FILE *f;
} Range;

static Range *s_ranges;
static int s_n_ranges;
static uint32_t s_sectors;
static char s_dir[1024];
static FILE *s_meta;

uint32_t a2n_disc_sectors(void) { return s_sectors; }

static int cmp_range(const void *a, const void *b)
{
    const Range *x = a, *y = b;
    return x->lba < y->lba ? -1 : x->lba > y->lba;
}

int a2n_disc_open(const char *data_dir)
{
    char p[1100];
    snprintf(s_dir, sizeof s_dir, "%s", data_dir);
    snprintf(p, sizeof p, "%s/disc.txt", data_dir);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    char line[512];
    int cap = 64;
    s_ranges = calloc((size_t)cap, sizeof *s_ranges);
    while (fgets(line, sizeof line, f)) {
        Range r = { 0 };
        char kind[16], how[16];
        if (sscanf(line, "sectors %u", &s_sectors) == 1) continue;
        if (sscanf(line, "%15s", kind) != 1) continue;
        if (!strcmp(kind, "file")) {
            if (sscanf(line, "file %u %u %15s %255[^\n]", &r.lba, &r.size, how, r.path) != 4) continue;
            r.raw = !strcmp(how, "raw");
            r.count = (r.size + USER - 1) / USER;
        } else if (!strcmp(kind, "meta")) {
            if (sscanf(line, "meta %u %u %u", &r.lba, &r.count, &r.meta_off) != 3) continue;
            r.meta = 1;
        } else if (!strcmp(kind, "fill")) {
            if (sscanf(line, "fill %u %u %u", &r.lba, &r.count, &r.meta_off) != 3) continue;
            r.meta = 2;
        } else {
            continue;
        }
        if (s_n_ranges == cap) s_ranges = realloc(s_ranges, sizeof *s_ranges * (size_t)(cap *= 2));
        s_ranges[s_n_ranges++] = r;
    }
    fclose(f);
    qsort(s_ranges, (size_t)s_n_ranges, sizeof *s_ranges, cmp_range);
    snprintf(p, sizeof p, "%s/meta.bin", data_dir);
    s_meta = fopen(p, "rb");
    return s_sectors > 0 && s_n_ranges > 0;
}

static void header(uint8_t *raw, uint32_t lba, uint8_t submode)
{
    static const uint8_t sync[12] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    memcpy(raw, sync, 12);
    const uint32_t a = lba + 150;
    const uint8_t m = (uint8_t)(a / 4500), s = (uint8_t)((a / 75) % 60), fr = (uint8_t)(a % 75);
    raw[12] = (uint8_t)(((m / 10) << 4) | (m % 10));
    raw[13] = (uint8_t)(((s / 10) << 4) | (s % 10));
    raw[14] = (uint8_t)(((fr / 10) << 4) | (fr % 10));
    raw[15] = 2;
    raw[16] = raw[20] = 0;
    raw[17] = raw[21] = 0;
    raw[18] = raw[22] = submode;
    raw[19] = raw[23] = 0;
}

int a2n_disc_read(uint32_t lba, uint8_t *raw)
{
    int lo = 0, hi = s_n_ranges - 1, k = -1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (s_ranges[mid].lba <= lba) { k = mid; lo = mid + 1; } else hi = mid - 1;
    }
    memset(raw, 0, SECTOR);
    if (k < 0 || lba >= s_ranges[k].lba + s_ranges[k].count) {
        header(raw, lba, 0x08);                    /* gap: an empty data sector */
        return 1;
    }
    Range *r = &s_ranges[k];
    const uint32_t i = lba - r->lba;
    if (r->meta) {
        if (!s_meta) return 0;
        fseek(s_meta, (long)(r->meta == 2 ? r->meta_off : r->meta_off + i) * SECTOR, SEEK_SET);
        if (fread(raw, 1, SECTOR, s_meta) != SECTOR) return 0;
        if (r->meta == 2) {                        /* the run's last kept sector, at its own place */
            const uint32_t a = lba + 150;
            const uint8_t m = (uint8_t)(a / 4500), sec = (uint8_t)((a / 75) % 60), fr = (uint8_t)(a % 75);
            raw[12] = (uint8_t)(((m / 10) << 4) | (m % 10));
            raw[13] = (uint8_t)(((sec / 10) << 4) | (sec % 10));
            raw[14] = (uint8_t)(((fr / 10) << 4) | (fr % 10));
        }
        return 1;
    }
    if (!r->f) {
        char p[1400];
        snprintf(p, sizeof p, "%s/files/%s", s_dir, r->path);
        r->f = fopen(p, "rb");
        if (!r->f) return 0;
    }
    if (r->raw) {
        fseek(r->f, (long)i * SECTOR, SEEK_SET);
        return fread(raw, 1, SECTOR, r->f) == SECTOR;
    }
    header(raw, lba, i + 1 == r->count ? 0x89 : 0x08);
    fseek(r->f, (long)i * USER, SEEK_SET);
    const size_t n = fread(raw + 24, 1, USER, r->f);
    (void)n;
    return 1;
}

/* ---------------------------------------------------------------- import */

typedef struct {
    uint32_t lba, size;
    int raw;
    char path[256];
} Entry;

static FILE *s_img;

static int read_user(uint32_t lba, uint32_t size, uint8_t *out)
{
    for (uint32_t done = 0; done < size; done += USER, ++lba) {
        uint8_t sec[SECTOR];
        if (fseek(s_img, (long)lba * SECTOR, SEEK_SET) || fread(sec, 1, SECTOR, s_img) != SECTOR) return 0;
        memcpy(out + done, sec + 24, size - done < USER ? size - done : USER);
    }
    return 1;
}

static int walk(uint32_t lba, uint32_t size, const char *dir, Entry **list, int *n, int *cap, int depth)
{
    if (depth > 8 || size > (1u << 20)) return 0;
    uint8_t *buf = malloc(size + USER);
    if (!buf || !read_user(lba, size, buf)) { free(buf); return 0; }
    for (uint32_t i = 0; i < size;) {
        const uint8_t len = buf[i];
        if (!len) { i = (i / USER + 1) * USER; continue; }
        const uint8_t *rec = buf + i;
        uint32_t elba, esize;
        memcpy(&elba, rec + 2, 4);
        memcpy(&esize, rec + 10, 4);
        const uint8_t flags = rec[25], nl = rec[32];
        if (!(nl == 1 && (rec[33] == 0 || rec[33] == 1))) {
            char name[128];
            int k = 0;
            for (; k < nl && k < 127 && rec[33 + k] != ';'; ++k) name[k] = (char)rec[33 + k];
            name[k] = 0;
            char path[256];
            snprintf(path, sizeof path, "%s%s%s", dir, dir[0] ? "/" : "", name);
            const uint8_t *su = rec + 33 + nl + ((nl + 1) % 2);
            const int su_len = len - (int)(su - rec);
            int xa = 0;
            if (su_len >= 14 && su[6] == 'X' && su[7] == 'A') xa = (su[4] << 8) | su[5];
            if (flags & 2) {
                walk(elba, esize, path, list, n, cap, depth + 1);
            } else {
                if (*n == *cap) *list = realloc(*list, sizeof **list * (size_t)(*cap *= 2));
                Entry *e = &(*list)[(*n)++];
                e->lba = elba;
                e->size = esize;
                e->raw = (xa & 0x3000) != 0;
                snprintf(e->path, sizeof e->path, "%s", path);
            }
        }
        i += len;
    }
    free(buf);
    return 1;
}

static void make_dirs(const char *path)
{
    char p[1400];
    snprintf(p, sizeof p, "%s", path);
    for (char *c = p + 1; *c; ++c)
        if (*c == '/') { *c = 0; MKDIR(p); *c = '/'; }
}

static uint64_t fnv(const uint8_t *p, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

/* image: a .cue (its first FILE) or a .bin; progress(0..1) may be NULL */
const char *a2n_disc_import(const char *image, const char *data_dir, void (*progress)(double, void *), void *ctx)
{
    static char err[256];
    char bin[1400];
    snprintf(bin, sizeof bin, "%s", image);
    const size_t il = strlen(image);
    if (il > 4 && (!strcmp(image + il - 4, ".cue") || !strcmp(image + il - 4, ".CUE"))) {
        FILE *c = fopen(image, "r");
        if (!c) return "cannot open the .cue file";
        char line[512], name[400] = "";
        while (fgets(line, sizeof line, c))
            if (sscanf(line, " FILE \"%399[^\"]\"", name) == 1) break;
        fclose(c);
        if (!name[0]) return "no FILE line in the .cue file";
        const char *slash = strrchr(image, '/');
#ifdef _WIN32
        const char *bs = strrchr(image, '\\');
        if (bs && (!slash || bs > slash)) slash = bs;
#endif
        if (slash) snprintf(bin, sizeof bin, "%.*s/%s", (int)(slash - image), image, name);
        else snprintf(bin, sizeof bin, "%s", name);
    }
    s_img = fopen(bin, "rb");
    if (!s_img) { snprintf(err, sizeof err, "cannot open %s", bin); return err; }
    fseek(s_img, 0, SEEK_END);
    const long bytes = ftell(s_img);
    if (bytes <= 0 || bytes % SECTOR) { fclose(s_img); return "not a MODE2/2352 image (size)"; }
    const uint32_t sectors = (uint32_t)(bytes / SECTOR);
    uint8_t pvd[USER];
    if (!read_user(16, USER, pvd) || memcmp(pvd + 1, "CD001", 5)) { fclose(s_img); return "not a PlayStation disc"; }
    uint32_t rlba, rsize;
    memcpy(&rlba, pvd + 156 + 2, 4);
    memcpy(&rsize, pvd + 156 + 10, 4);
    int n = 0, cap = 32;
    Entry *list = malloc(sizeof *list * (size_t)cap);
    walk(rlba, rsize, "", &list, &n, &cap, 0);

    /* the French release: SLES_026.01 */
    const Entry *boot = NULL;
    for (int i = 0; i < n; ++i) if (!strcmp(list[i].path, "SLES_026.01")) boot = &list[i];
    if (!boot) { free(list); fclose(s_img); return "this is not Alundra 2 (France, SLES-02601)"; }
    {
        uint8_t *exe = malloc(boot->size);
        read_user(boot->lba, boot->size, exe);
        const uint64_t h = fnv(exe, boot->size);
        free(exe);
        if (h != A2N_EXE_FNV) { free(list); fclose(s_img); return "SLES_026.01 does not match the French release"; }
    }

    char p[1400];
    MKDIR(data_dir);
    snprintf(p, sizeof p, "%s/files", data_dir);
    MKDIR(p);
    uint8_t *covered = calloc(sectors, 1);
    uint64_t total = 0, done = 0;
    for (int i = 0; i < n; ++i) total += list[i].size;
    uint8_t sec[SECTOR];
    for (int i = 0; i < n; ++i) {
        const Entry *e = &list[i];
        snprintf(p, sizeof p, "%s/files/%s", data_dir, e->path);
        make_dirs(p);
        FILE *o = fopen(p, "wb");
        if (!o) { free(list); free(covered); fclose(s_img); return "cannot write the data folder"; }
        const uint32_t count = (e->size + USER - 1) / USER;
        for (uint32_t k = 0; k < count; ++k) {
            fseek(s_img, (long)(e->lba + k) * SECTOR, SEEK_SET);
            if (fread(sec, 1, SECTOR, s_img) != SECTOR) memset(sec, 0, sizeof sec);
            if (e->raw) fwrite(sec, 1, SECTOR, o);
            else fwrite(sec + 24, 1, k + 1 == count ? e->size - k * USER : USER, o);
            if (e->lba + k < sectors) covered[e->lba + k] = 1;
            done += USER;
            if (progress && !(k & 1023)) progress((double)done / (double)(total ? total : 1), ctx);
        }
        fclose(o);
    }
    /* sectors outside the files */
    snprintf(p, sizeof p, "%s/meta.bin", data_dir);
    FILE *meta = fopen(p, "wb");
    snprintf(p, sizeof p, "%s/disc.txt", data_dir);
    FILE *idx = fopen(p, "w");
    if (!meta || !idx) { free(list); free(covered); fclose(s_img); return "cannot write the data folder"; }
    fprintf(idx, "# Alundra 2 (France, SLES-02601): files of the disc, see src/native/disc.c\n");
    fprintf(idx, "sectors %u\n", sectors);
    for (int i = 0; i < n; ++i)
        fprintf(idx, "file %u %u %s %s\n", list[i].lba, list[i].size, list[i].raw ? "raw" : "data", list[i].path);
    /* sectors outside the files: each distinct sector once in meta.bin; a sector equal to the
     * previous one but for its position (long empty runs) as a copy of it ("fill") */
    uint32_t meta_off = 0;
    uint8_t prev[SECTOR];
    for (uint32_t l = 0; l < sectors;) {
        if (covered[l]) { ++l; continue; }
        uint32_t e = l;
        while (e < sectors && !covered[e]) ++e;
        uint32_t open_lba = 0, open_count = 0, open_off = 0;
        for (uint32_t k = l; k < e;) {
            fseek(s_img, (long)k * SECTOR, SEEK_SET);
            if (fread(sec, 1, SECTOR, s_img) != SECTOR) memset(sec, 0, sizeof sec);
            fwrite(sec, 1, SECTOR, meta);
            if (open_count && open_lba + open_count == k) {
                open_count++;
            } else {
                if (open_count) fprintf(idx, "meta %u %u %u\n", open_lba, open_count, open_off);
                open_lba = k;
                open_count = 1;
                open_off = meta_off;
            }
            memcpy(prev, sec, SECTOR);
            uint32_t same = 0;
            for (uint32_t j = k + 1; j < e; ++j) {
                fseek(s_img, (long)j * SECTOR, SEEK_SET);
                if (fread(sec, 1, SECTOR, s_img) != SECTOR) break;
                if (memcmp(sec, prev, 12) || memcmp(sec + 15, prev + 15, SECTOR - 15)) break;
                same++;
            }
            if (same) {
                fprintf(idx, "meta %u %u %u\n", open_lba, open_count, open_off);
                fprintf(idx, "fill %u %u %u\n", k + 1, same, meta_off);
                open_count = 0;
            }
            meta_off++;
            k += 1 + same;
        }
        if (open_count) fprintf(idx, "meta %u %u %u\n", open_lba, open_count, open_off);
        l = e;
    }
    fclose(meta);
    fclose(idx);
    free(covered);
    free(list);
    fclose(s_img);
    if (progress) progress(1.0, ctx);
    return NULL;
}
