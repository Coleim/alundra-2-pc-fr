/*
 * card.c - memory cards (PsyQ libmcrd), as 128 KB card images on the host.
 *
 * The images use the standard raw layout of a PlayStation memory card (the ".mcd" files
 * of emulators): block 0 holds the "MC" header and the 15 directory frames, blocks 1..15
 * the files' 8 KB blocks, chained through the directory. So a save made here can be moved
 * to an emulator, or the other way round.
 *
 * Port 1 always has a card (card1.mcd, created formatted when missing); port 2 has one
 * when card2.mcd exists. The library's asynchronous requests (MemCardExist, Accept,
 * ReadFile, WriteFile) are done at once; MemCardSync reports them as finished, as the
 * console's library does once the card answered. As on the console, a card is "new" after
 * power-on until MemCardAccept has looked at it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "host.h"
#include "libaddr.h"
#include "native_modules.h"

#define CARD_BYTES   0x20000u
#define BLOCK_BYTES  0x2000u
#define FRAME_BYTES  0x80u

enum { McErrNone = 0, McErrCardNotExist = 1, McErrCardInvalid = 2, McErrNewCard = 3,
       McErrNotFormat = 4, McErrFileNotExist = 5, McErrAlreadyExist = 6, McErrBlockFull = 7 };
enum { McFuncExist = 1, McFuncAccept = 2, McFuncReadFile = 3, McFuncWriteFile = 4 };

typedef struct {
    uint8_t data[CARD_BYTES];
    int present, new_card;
    char path[1100];
} Card;

static Card s_card[2];
static int s_pending, s_cmd, s_result;
static uint32_t s_callback;

/* ---------------------------------------------------------------- the image */

static uint8_t *frame(Card *c, int block, int f) { return c->data + block * BLOCK_BYTES + f * FRAME_BYTES; }

static void checksum(uint8_t *fr)
{
    uint8_t x = 0;
    for (int i = 0; i < 0x7F; ++i) x ^= fr[i];
    fr[0x7F] = x;
}

static void format(Card *c)
{
    memset(c->data, 0, BLOCK_BYTES);
    uint8_t *h = frame(c, 0, 0);
    h[0] = 'M';
    h[1] = 'C';
    checksum(h);
    for (int i = 1; i <= 15; ++i) {
        uint8_t *d = frame(c, 0, i);
        d[0] = 0xA0;
        d[8] = d[9] = 0xFF;
        checksum(d);
    }
    for (int i = 16; i < 36; ++i) {                 /* no broken sectors */
        uint8_t *d = frame(c, 0, i);
        memset(d, 0xFF, 4);
        d[8] = d[9] = 0xFF;
        checksum(d);
    }
    for (int i = 36; i < 63; ++i) memset(frame(c, 0, i), 0xFF, FRAME_BYTES);
    memcpy(frame(c, 0, 63), h, FRAME_BYTES);
}

static int formatted(const Card *c) { return c->data[0] == 'M' && c->data[1] == 'C'; }

static void save(Card *c)
{
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", c->path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { fprintf(stderr, "a2: cannot write %s\n", tmp); return; }
    const size_t n = fwrite(c->data, 1, CARD_BYTES, f);
    fclose(f);
    if (n != CARD_BYTES) { remove(tmp); fprintf(stderr, "a2: cannot write %s\n", tmp); return; }
    remove(c->path);
    if (rename(tmp, c->path)) fprintf(stderr, "a2: cannot replace %s\n", c->path);
}

static void load(int port)
{
    Card *c = &s_card[port];
    snprintf(c->path, sizeof c->path, "%s/card%d.mcd", a2n_host_save_dir(), port + 1);
    FILE *f = fopen(c->path, "rb");
    c->present = 0;
    if (f) {
        c->present = fread(c->data, 1, CARD_BYTES, f) == CARD_BYTES;
        fclose(f);
        if (!c->present) fprintf(stderr, "a2: %s is not a 128 KB memory card image\n", c->path);
    } else if (port == 0) {
        format(c);
        memset(c->data + BLOCK_BYTES, 0, CARD_BYTES - BLOCK_BYTES);
        save(c);
        c->present = 1;
    }
    c->new_card = c->present;
}

static Card *card_of(uint32_t chan)
{
    const int port = (int)((chan >> 4) & 1);
    return (chan & 0xF) == 0 && s_card[port].present ? &s_card[port] : NULL;
}

/* directory entry of a file, 0..14, or -1 */
static int find(Card *c, uint32_t name_addr)
{
    char name[21];
    for (int i = 0; i < 20; ++i) name[i] = (char)a2n_r8(name_addr + (uint32_t)i);
    name[20] = 0;
    for (int i = 0; i < 15; ++i) {
        const uint8_t *d = frame(c, 0, 1 + i);
        if (d[0] == 0x51 && !strncmp((const char *)d + 0x0A, name, 20)) return i;
    }
    return -1;
}

static int file_size(Card *c, int e)
{
    const uint8_t *d = frame(c, 0, 1 + e);
    return d[4] | d[5] << 8 | d[6] << 16 | d[7] << 24;
}

static int next_block(Card *c, int e)
{
    const uint8_t *d = frame(c, 0, 1 + e);
    const int n = d[8] | d[9] << 8;
    return n < 15 ? n : -1;
}

/* copy between the file and guest memory, from byte `ofs` */
static int transfer(Card *c, int e, uint32_t addr, uint32_t ofs, uint32_t bytes, int to_card)
{
    if (ofs + bytes > (uint32_t)file_size(c, e)) return 0;
    int b = e;
    for (uint32_t skip = ofs / BLOCK_BYTES; skip--; ) if ((b = next_block(c, b)) < 0) return 0;
    uint32_t pos = ofs % BLOCK_BYTES;
    for (uint32_t done = 0; done < bytes; ) {
        if (b < 0) return 0;
        uint8_t *p = c->data + (1 + b) * BLOCK_BYTES + pos;
        uint32_t n = BLOCK_BYTES - pos;
        if (n > bytes - done) n = bytes - done;
        for (uint32_t i = 0; i < n; ++i) {
            if (to_card) p[i] = (uint8_t)a2n_r8(addr + done + i);
            else a2n_w8(addr + done + i, p[i]);
        }
        done += n;
        pos = 0;
        b = next_block(c, b);
    }
    return 1;
}

/* ---------------------------------------------------------------- the library */

static int finish(CPUState *cpu, int cmd, int result)
{
    if (s_pending) return a2n_ret(cpu, 0);          /* a request is still unanswered */
    s_pending = 1;
    s_cmd = cmd;
    s_result = result;
    return a2n_ret(cpu, 1);
}

static int n_MemCardExist(CPUState *cpu)
{
    Card *c = card_of(cpu->gpr[4]);
    return finish(cpu, McFuncExist, !c ? McErrCardNotExist : c->new_card ? McErrNewCard : McErrNone);
}

static int n_MemCardAccept(CPUState *cpu)
{
    Card *c = card_of(cpu->gpr[4]);
    int r;
    if (!c) r = McErrCardNotExist;
    else if (!formatted(c)) r = McErrNotFormat;
    else r = c->new_card ? McErrNewCard : McErrNone;
    if (c) c->new_card = 0;
    return finish(cpu, McFuncAccept, r);
}

static int rw_file(CPUState *cpu, int to_card)
{
    Card *c = card_of(cpu->gpr[4]);
    const uint32_t name = cpu->gpr[5], addr = cpu->gpr[6], ofs = cpu->gpr[7];
    const uint32_t bytes = a2n_arg(cpu, 4);
    int r = McErrNone;
    if (!c) r = McErrCardNotExist;
    else if (!formatted(c)) r = McErrNotFormat;
    else {
        const int e = find(c, name);
        if (e < 0) r = McErrFileNotExist;
        else if (!transfer(c, e, addr, ofs, bytes, to_card)) r = McErrCardInvalid;
        else if (to_card) save(c);
    }
    return finish(cpu, to_card ? McFuncWriteFile : McFuncReadFile, r);
}

static int n_MemCardReadFile(CPUState *cpu) { return rw_file(cpu, 0); }
static int n_MemCardWriteFile(CPUState *cpu) { return rw_file(cpu, 1); }

/* MemCardSync(mode, *cmds, *result): 1 finished, 0 running, -1 nothing asked */
static int n_MemCardSync(CPUState *cpu)
{
    if (!s_pending) return a2n_ret(cpu, 0xFFFFFFFFu);
    s_pending = 0;
    if (cpu->gpr[5]) a2n_w32(cpu->gpr[5], (uint32_t)s_cmd);
    if (cpu->gpr[6]) a2n_w32(cpu->gpr[6], (uint32_t)s_result);
    return a2n_ret(cpu, 1);
}

static int n_MemCardCreateFile(CPUState *cpu)
{
    Card *c = card_of(cpu->gpr[4]);
    const int blocks = (int)cpu->gpr[6];
    if (!c) return a2n_ret(cpu, McErrCardNotExist);
    if (!formatted(c)) return a2n_ret(cpu, McErrNotFormat);
    if (find(c, cpu->gpr[5]) >= 0) return a2n_ret(cpu, McErrAlreadyExist);
    int free_list[15], n_free = 0;
    for (int i = 0; i < 15; ++i)
        if ((frame(c, 0, 1 + i)[0] & 0xF0) == 0xA0) free_list[n_free++] = i;
    if (blocks < 1 || blocks > n_free) return a2n_ret(cpu, McErrBlockFull);
    for (int k = 0; k < blocks; ++k) {
        uint8_t *d = frame(c, 0, 1 + free_list[k]);
        memset(d, 0, FRAME_BYTES);
        d[0] = blocks == 1 ? 0x51 : k == 0 ? 0x51 : k + 1 == blocks ? 0x53 : 0x52;
        if (k == 0) {
            const uint32_t size = (uint32_t)blocks * BLOCK_BYTES;
            memcpy(d + 4, &size, 4);
            for (int i = 0; i < 20; ++i) d[0x0A + i] = (uint8_t)a2n_r8(cpu->gpr[5] + (uint32_t)i);
            d[0x1E] = 0;
        }
        const int next = k + 1 < blocks ? free_list[k + 1] : 0xFFFF;
        d[8] = (uint8_t)next;
        d[9] = (uint8_t)(next >> 8);
        checksum(d);
    }
    save(c);
    return a2n_ret(cpu, McErrNone);
}

static int n_MemCardDeleteFile(CPUState *cpu)
{
    Card *c = card_of(cpu->gpr[4]);
    if (!c) return a2n_ret(cpu, McErrCardNotExist);
    if (!formatted(c)) return a2n_ret(cpu, McErrNotFormat);
    int e = find(c, cpu->gpr[5]);
    if (e < 0) return a2n_ret(cpu, McErrFileNotExist);
    for (int guard = 0; e >= 0 && guard < 15; ++guard) {
        uint8_t *d = frame(c, 0, 1 + e);
        const int next = next_block(c, e);
        if (d[0] >= 0x51 && d[0] <= 0x53) d[0] = (uint8_t)(d[0] - 0x51 + 0xA1);
        checksum(d);
        e = next;
    }
    save(c);
    return a2n_ret(cpu, McErrNone);
}

static int n_MemCardFormat(CPUState *cpu)
{
    Card *c = card_of(cpu->gpr[4]);
    if (!c) return a2n_ret(cpu, McErrCardNotExist);
    format(c);
    c->new_card = 0;
    save(c);
    return a2n_ret(cpu, McErrNone);
}

static int n_MemCardUnformat(CPUState *cpu)
{
    Card *c = card_of(cpu->gpr[4]);
    if (!c) return a2n_ret(cpu, McErrCardNotExist);
    memset(c->data, 0, BLOCK_BYTES);
    save(c);
    return a2n_ret(cpu, McErrNone);
}

static int match(const char *pat, const char *s)
{
    for (; *pat; ++pat, ++s) {
        if (*pat == '*') {
            for (const char *t = s;; ++t) {
                if (match(pat + 1, t)) return 1;
                if (!*t) return 0;
            }
        }
        if (!*s || (*pat != '?' && *pat != *s)) return 0;
    }
    return !*s;
}

/* MemCardGetDirentry(chan, name, dir[], *files, ofs, max): files matching a pattern */
static int n_MemCardGetDirentry(CPUState *cpu)
{
    Card *c = card_of(cpu->gpr[4]);
    const uint32_t dir = cpu->gpr[6], files = cpu->gpr[7];
    const int ofs = (int)a2n_arg(cpu, 4), max = (int)a2n_arg(cpu, 5);
    if (files) a2n_w32(files, 0);
    if (!c) return a2n_ret(cpu, McErrCardNotExist);
    if (!formatted(c)) return a2n_ret(cpu, McErrNotFormat);
    char pat[64];
    int k = 0;
    for (; k < 63; ++k) if (!(pat[k] = (char)a2n_r8(cpu->gpr[5] + (uint32_t)k))) break;
    pat[k] = 0;
    int seen = 0, n = 0;
    for (int i = 0; i < 15 && n < max; ++i) {
        const uint8_t *d = frame(c, 0, 1 + i);
        if (d[0] != 0x51) continue;
        char name[21];
        memcpy(name, d + 0x0A, 20);
        name[20] = 0;
        if (!match(pat, name)) continue;
        if (seen++ < ofs) continue;
        const uint32_t e = dir + 0x28u * (uint32_t)n;
        for (int j = 0; j < 0x28; ++j) a2n_w8(e + (uint32_t)j, 0);
        for (int j = 0; j < 20; ++j) a2n_w8(e + (uint32_t)j, (uint8_t)name[j]);
        a2n_w32(e + 0x14, 0x50);                     /* attributes */
        a2n_w32(e + 0x18, (uint32_t)file_size(c, i));
        a2n_w32(e + 0x20, (uint32_t)(i + 1));        /* first block */
        n++;
    }
    if (files) a2n_w32(files, (uint32_t)n);
    return a2n_ret(cpu, McErrNone);
}

static int n_MemCardCallback(CPUState *cpu)
{
    const uint32_t old = s_callback;
    s_callback = cpu->gpr[4];
    return a2n_ret(cpu, old);
}

static int n_nothing(CPUState *cpu) { return a2n_ret(cpu, 0); }

/* the PC file server of the development kit: not there */
static int n_no_pc(CPUState *cpu) { return a2n_ret(cpu, 0xFFFFFFFFu); }

void a2n_card_init(void)
{
    load(0);
    load(1);
    s_pending = 0;
    a2n_native(LIB_MemCardInit, "MemCardInit", n_nothing);
    a2n_native(LIB_MemCardEnd, "MemCardEnd", n_nothing);
    a2n_native(LIB_MemCardStart, "MemCardStart", n_nothing);
    a2n_native(LIB_MemCardStop, "MemCardStop", n_nothing);
    a2n_native(LIB_MemCardExist, "MemCardExist", n_MemCardExist);
    a2n_native(LIB_MemCardAccept, "MemCardAccept", n_MemCardAccept);
    a2n_native(LIB_MemCardReadFile, "MemCardReadFile", n_MemCardReadFile);
    a2n_native(LIB_MemCardWriteFile, "MemCardWriteFile", n_MemCardWriteFile);
    a2n_native(LIB_MemCardSync, "MemCardSync", n_MemCardSync);
    a2n_native(LIB_MemCardCreateFile, "MemCardCreateFile", n_MemCardCreateFile);
    a2n_native(LIB_MemCardDeleteFile, "MemCardDeleteFile", n_MemCardDeleteFile);
    a2n_native(LIB_MemCardFormat, "MemCardFormat", n_MemCardFormat);
    a2n_native(LIB_MemCardUnformat, "MemCardUnformat", n_MemCardUnformat);
    a2n_native(LIB_MemCardGetDirentry, "MemCardGetDirentry", n_MemCardGetDirentry);
    a2n_native(LIB_MemCardCallback, "MemCardCallback", n_MemCardCallback);
    a2n_native(LIB_PCopen, "PCopen", n_no_pc);
    a2n_native(LIB_PCcreat, "PCcreat", n_no_pc);
    a2n_native(LIB_PClseek, "PClseek", n_no_pc);
    a2n_native(0x8003CF78u, "PCclose", n_nothing);
    a2n_native(0x8003D15Cu, "PCinit", n_no_pc);
    a2n_native(0x8003D168u, "PCread", n_no_pc);
    a2n_native(0x8003D240u, "PCwrite", n_no_pc);
}
