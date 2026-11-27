/*
 * cd.c - the CD-ROM drive, reading the game's files extracted from the player's disc.
 *
 * The PsyQ CD libraries (libcd, libds, the streaming library) run unchanged: they talk to
 * this drive through its four registers, get their interrupts through the callback libetc
 * keeps for the CD, and fetch sectors by DMA channel 3. The drive answers the commands the
 * libraries send (GetStat, Setloc, ReadN, ReadS, Pause, Init, Setmode, Setfilter, Getloc*,
 * GetTN, GetTD, SeekL/P, Mute/Demute, Test, GetID, ReadTOC) and decodes the XA-ADPCM audio
 * sectors of streamed music for the sound chip.
 *
 * Data reads complete as fast as the game takes the sectors (instant loading); streamed
 * reads (ReadS: music, video) keep the speed of the drive, 150 sectors a second.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cd.h"
#include "engine.h"
#include "native_modules.h"

/* ---------------------------------------------------------------- the disc */

int a2n_disc_read(uint32_t lba, uint8_t *raw);       /* disc.c */
uint32_t a2n_disc_sectors(void);

static void msf_of(uint32_t lba, uint8_t *m, uint8_t *s, uint8_t *f)
{
    lba += 150;
    *m = (uint8_t)(lba / (60 * 75));
    *s = (uint8_t)((lba / 75) % 60);
    *f = (uint8_t)(lba % 75);
}
static uint8_t bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static uint8_t unbcd(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0xF)); }

/* ---------------------------------------------------------------- controller state */

enum { ST_ERROR = 0x01, ST_MOTOR = 0x02, ST_SEEKERR = 0x04, ST_IDERR = 0x08, ST_SHELL = 0x10,
       ST_READ = 0x20, ST_SEEK = 0x40, ST_PLAY = 0x80 };

typedef struct {
    uint8_t type;                 /* INT1..INT5 */
    uint8_t resp[16];
    int n;
    int sector;                   /* INT1: a sector is waiting in the buffer */
    uint8_t data[2352];           /* that sector */
} Irq;

static struct {
    uint8_t index, irq_enable, irq_flag;
    uint8_t params[16];
    int n_params;
    uint8_t resp[16];
    int n_resp, resp_pos;
    Irq queue[8];
    int n_queue;
    int busy;
    uint8_t stat, mode, filter_file, filter_chan;
    uint32_t setloc, lba;          /* Setloc target; next sector to read */
    int setloc_pending;
    int reading, streaming;        /* ReadN / ReadS in progress */
    int seeking;                   /* ReadN: no data before the next vertical blank */
    int muted;
    uint8_t last_header[8];        /* GetlocL: header + subheader of the last sector */
    uint8_t sector[2352];          /* drive buffer: the last sector read */
    int sector_ready;              /* a data sector waits for the game */
    uint8_t fifo[2352];            /* data fifo, after the request */
    int fifo_n, fifo_pos;
    double stream_credit;          /* sectors due (streamed reads) */
    uint8_t vol_ll, vol_lr, vol_rr, vol_rl;
    uint8_t pend_ll, pend_lr, pend_rr, pend_rl;
} cd;

static int s_log = -1;          /* A2_LOG_CD=1: commands and interrupts on stderr */
#define CD_GAP 4000             /* data reads: transfers of the game's code between two sectors */

/* XA decoder (xa.c) */
void a2n_xa_sector(const uint8_t *raw, int ll, int lr, int rr, int rl);
void a2n_xa_reset(void);

/* the libetc callback for the CD interrupt (InterruptCallback(2, ...)) */
uint32_t a2n_irq_callback(int irq);

static void push_irq(uint8_t type, const uint8_t *resp, int n, int sector)
{
    if (cd.n_queue >= 8) return;
    Irq *q = &cd.queue[cd.n_queue++];
    q->type = type;
    q->n = n;
    memcpy(q->resp, resp, (size_t)n);
    q->sector = sector;
    a2n_event_post(A2N_EV_CD);
}

static void push_stat(uint8_t type)
{
    const uint8_t s = cd.stat;
    push_irq(type, &s, 1, 0);
}

/* deliver the next interrupt once the game acknowledged the previous one */
static void deliver(void)
{
    if (cd.irq_flag || !cd.n_queue) return;
    const Irq q = cd.queue[0];
    memmove(cd.queue, cd.queue + 1, sizeof cd.queue[0] * (size_t)(--cd.n_queue));
    cd.irq_flag = q.type;
    if (s_log > 0) {
        fprintf(stderr, "a2.cd: int%d resp %02x n=%d", q.type, q.resp[0], q.n);
        if (q.sector)
            fprintf(stderr, " +sector %02x:%02x:%02x sub %02x %02x %02x %02x str %04x sec %u/%u frame %u",
                    q.data[12], q.data[13], q.data[14], q.data[16], q.data[17], q.data[18], q.data[19],
                    q.data[24] | q.data[25] << 8, q.data[28] | q.data[29] << 8, q.data[30] | q.data[31] << 8,
                    q.data[32] | q.data[33] << 8);
        fprintf(stderr, "\n");
    }
    memcpy(cd.resp, q.resp, (size_t)q.n);
    cd.n_resp = q.n;
    cd.resp_pos = 0;
    cd.busy = 0;
    if (q.sector) {
        memcpy(cd.sector, q.data, sizeof cd.sector);
        memcpy(cd.last_header, q.data + 12, 8);
        cd.sector_ready = 1;
    }
    const uint32_t cb = a2n_irq_callback(2);
    if ((cd.irq_enable & cd.irq_flag & 0x1F) && cb) a2n_call_interrupt(cb, 0, 0, 0);
}

/* ---------------------------------------------------------------- reading */

static int sector_matches_filter(const uint8_t *raw)
{
    return raw[16] == cd.filter_file && raw[17] == cd.filter_chan;
}

/* read the next sector at the head; returns 1 if it raised a data interrupt */
static int read_one(void)
{
    if (cd.lba >= a2n_disc_sectors()) {
        cd.reading = cd.streaming = 0;
        cd.stat &= (uint8_t)~ST_READ;
        const uint8_t r[2] = { (uint8_t)(cd.stat | ST_ERROR), 0x04 };
        push_irq(5, r, 2, 0);                               /* end of disc */
        return 1;
    }
    uint8_t raw[2352];
    if (!a2n_disc_read(cd.lba, raw)) memset(raw, 0, sizeof raw);
    cd.lba++;
    const uint8_t submode = raw[18];
    const int form2_audio = raw[15] == 2 && (submode & 0x04);
    if ((cd.mode & 0x40) && form2_audio) {                   /* XA-ADPCM to the sound chip */
        if (!(cd.mode & 0x08) || sector_matches_filter(raw)) {
            if (!cd.muted) a2n_xa_sector(raw, cd.vol_ll, cd.vol_lr, cd.vol_rr, cd.vol_rl);
        }
        return 0;
    }
    if ((cd.mode & 0x08) && form2_audio) return 0;          /* filtered out */
    if (cd.n_queue >= 8) return 0;
    push_stat(1);
    Irq *q = &cd.queue[cd.n_queue - 1];
    q->sector = 1;
    memcpy(q->data, raw, sizeof raw);
    return 1;
}

/* streamed reads advance with time; data reads start at the vertical blank after the read
 * command (the seek: the game may look at the drive's status once before data comes) */
void a2n_cd_vblank(void)
{
    if (!cd.reading) return;
    if (!cd.streaming && cd.seeking) {
        cd.seeking = 0;
        a2n_cd_timer_set(CD_GAP);
    }
    if (cd.streaming) {
        cd.stream_credit += (cd.mode & 0x80) ? 3.0 : 1.5;  /* 150 / 75 sectors a second, PAL */
        while (cd.stream_credit >= 1.0 && cd.reading) {
            cd.stream_credit -= 1.0;
            if (cd.n_queue < 6) read_one();
        }
    }
}

/* data reads: a sector every CD_GAP transfers of the game's code, once the previous one
 * was taken (far faster than the drive, while the main code still runs between sectors
 * and can stop the read in time) */

void a2n_cd_timer(void)
{
    if (!cd.reading || cd.streaming) return;
    if (!cd.irq_flag && !cd.n_queue) read_one();
    if (cd.reading && !cd.streaming) a2n_cd_timer_set(CD_GAP);
}

void a2n_cd_event(void)
{
    deliver();
    if (cd.n_queue && !cd.irq_flag) a2n_event_post(A2N_EV_CD);
}

/* ---------------------------------------------------------------- commands */

static void start_read(int streaming)
{
    if (cd.setloc_pending) { cd.lba = cd.setloc; cd.setloc_pending = 0; }
    cd.reading = 1;
    cd.streaming = streaming;
    cd.stream_credit = 0;
    cd.sector_ready = 0;
    cd.stat = (uint8_t)((cd.stat | ST_MOTOR | ST_READ) & ~ST_PLAY);
    push_stat(3);
    cd.seeking = !streaming;
    a2n_cd_timer_set(0);
}

static void command(uint8_t c)
{
    const uint8_t *p = cd.params;
    if (s_log < 0) s_log = getenv("A2_LOG_CD") ? atoi(getenv("A2_LOG_CD")) : 0;
    if (s_log) {
        fprintf(stderr, "a2.cd: cmd %02x", c);
        for (int i = 0; i < cd.n_params; ++i) fprintf(stderr, " %02x", p[i]);
        fprintf(stderr, " (vblank %llu)\n", (unsigned long long)a2n_vblank_count());
    }
    cd.busy = 1;
    switch (c) {
    case 0x01:                                              /* GetStat */
        push_stat(3);
        cd.stat &= (uint8_t)~ST_SHELL;
        break;
    case 0x02:                                              /* Setloc */
        cd.setloc = (uint32_t)((unbcd(p[0]) * 60 + unbcd(p[1])) * 75 + unbcd(p[2])) - 150;
        cd.setloc_pending = 1;
        push_stat(3);
        break;
    case 0x03:                                              /* Play (CD audio): none on this disc */
        cd.stat |= ST_PLAY;
        push_stat(3);
        break;
    case 0x06:                                              /* ReadN */
        start_read(0);
        break;
    case 0x1B:                                              /* ReadS */
        start_read(1);
        break;
    case 0x08:                                              /* Stop */
    case 0x09:                                              /* Pause */
        cd.reading = cd.streaming = 0;
        cd.sector_ready = 0;
        push_stat(3);
        cd.stat &= (uint8_t)~(ST_READ | ST_PLAY | ST_SEEK);
        if (c == 0x08) cd.stat &= (uint8_t)~ST_MOTOR;
        push_stat(2);
        break;
    case 0x0A:                                              /* Init */
        cd.reading = cd.streaming = 0;
        cd.mode = 0;
        cd.stat = ST_MOTOR;
        push_stat(3);
        push_stat(2);
        break;
    case 0x0B: cd.muted = 1; push_stat(3); break;           /* Mute */
    case 0x0C: cd.muted = 0; push_stat(3); break;           /* Demute */
    case 0x0D:                                              /* Setfilter */
        cd.filter_file = p[0];
        cd.filter_chan = p[1];
        push_stat(3);
        break;
    case 0x0E:                                              /* Setmode */
        cd.mode = p[0];
        push_stat(3);
        break;
    case 0x0F: {                                            /* Getparam */
        const uint8_t r[5] = { cd.stat, cd.mode, 0, cd.filter_file, cd.filter_chan };
        push_irq(3, r, 5, 0);
        break;
    }
    case 0x10:                                              /* GetlocL */
        push_irq(3, cd.last_header, 8, 0);
        break;
    case 0x11: {                                            /* GetlocP */
        const uint32_t at = cd.lba ? cd.lba - 1 : 0;
        uint8_t m, s, f, am, as, af;
        msf_of(at, &am, &as, &af);
        msf_of(at, &m, &s, &f);                             /* one track: relative = absolute - 2 s */
        const uint32_t rel = at;
        m = (uint8_t)(rel / (60 * 75)); s = (uint8_t)((rel / 75) % 60); f = (uint8_t)(rel % 75);
        const uint8_t r[8] = { 1, 1, bcd(m), bcd(s), bcd(f), bcd(am), bcd(as), bcd(af) };
        push_irq(3, r, 8, 0);
        break;
    }
    case 0x13: {                                            /* GetTN */
        const uint8_t r[3] = { cd.stat, 1, 1 };
        push_irq(3, r, 3, 0);
        break;
    }
    case 0x14: {                                            /* GetTD */
        uint8_t m, s, f;
        if (p[0] == 0) msf_of(a2n_disc_sectors(), &m, &s, &f);
        else { m = 0; s = 2; f = 0; }
        const uint8_t r[3] = { cd.stat, bcd(m), bcd(s) };
        push_irq(3, r, 3, 0);
        break;
    }
    case 0x15: case 0x16:                                   /* SeekL, SeekP */
        if (cd.setloc_pending) { cd.lba = cd.setloc; cd.setloc_pending = 0; }
        cd.reading = cd.streaming = 0;
        cd.stat = (uint8_t)((cd.stat | ST_MOTOR) & ~(ST_READ | ST_PLAY));
        push_stat(3);
        push_stat(2);
        break;
    case 0x19: {                                            /* Test */
        if (p[0] == 0x20) {
            const uint8_t r[4] = { 0x97, 0x01, 0x10, 0xC2 };
            push_irq(3, r, 4, 0);
        } else {
            push_stat(3);
        }
        break;
    }
    case 0x1A: {                                            /* GetID: licensed, Europe */
        push_stat(3);
        const uint8_t r[8] = { ST_MOTOR, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'E' };
        push_irq(2, r, 8, 0);
        break;
    }
    case 0x1E:                                              /* ReadTOC */
        push_stat(3);
        push_stat(2);
        break;
    default: {
        const uint8_t r[2] = { (uint8_t)(cd.stat | ST_ERROR), 0x40 };
        push_irq(5, r, 2, 0);
        fprintf(stderr, "a2: CD command %02X not handled\n", c);
        break;
    }
    }
    cd.n_params = 0;
}

/* ---------------------------------------------------------------- registers */

static uint32_t cd_read(uint32_t a, int size)
{
    (void)size;
    switch (a & 3) {
    case 0:
        return cd.index | (cd.n_params == 0 ? 0x08 : 0) | (cd.n_params < 16 ? 0x10 : 0) |
               (cd.resp_pos < cd.n_resp ? 0x20 : 0) | (cd.fifo_pos < cd.fifo_n ? 0x40 : 0) |
               (cd.busy ? 0x80 : 0);
    case 1:
        return cd.resp_pos < cd.n_resp ? cd.resp[cd.resp_pos++] : 0;
    case 2: {
        uint32_t v = cd.fifo_pos < cd.fifo_n ? cd.fifo[cd.fifo_pos++] : 0;
        if (size >= 2) v |= (uint32_t)(cd.fifo_pos < cd.fifo_n ? cd.fifo[cd.fifo_pos++] : 0) << 8;
        return v;
    }
    default:
        return (cd.index & 1) ? (uint32_t)(cd.irq_flag | 0xE0) : (uint32_t)(cd.irq_enable | 0xE0);
    }
}

/* the sector as the request register loads it into the data fifo (a request while the
 * fifo already holds the sector keeps reading it: the library takes the header first) */
static void load_fifo(void)
{
    if (!cd.sector_ready) return;
    if (cd.mode & 0x20) {                                   /* whole sector after the sync */
        memcpy(cd.fifo, cd.sector + 12, 2340);
        cd.fifo_n = 2340;
    } else {
        memcpy(cd.fifo, cd.sector + 24, 2048);
        cd.fifo_n = 2048;
    }
    cd.fifo_pos = 0;
    cd.sector_ready = 0;
}

static void cd_write(uint32_t a, uint32_t v, int size)
{
    (void)size;
    const uint8_t b = (uint8_t)v;
    switch ((a & 3) | (cd.index << 2)) {
    case 0: case 4: case 8: case 12:
        cd.index = b & 3;
        break;
    case 1:                                                  /* command */
        command(b);
        break;
    case 2:                                                  /* parameter */
        if (cd.n_params < 16) cd.params[cd.n_params++] = b;
        break;
    case 3:                                                  /* request */
        if (b & 0x80) load_fifo();
        else cd.fifo_n = cd.fifo_pos = 0;
        if (s_log > 0) fprintf(stderr, "a2.cd: request %02x -> fifo %d bytes (ready %d)\n", b, cd.fifo_n, cd.sector_ready);
        break;
    case 6:                                                  /* interrupt enable */
        cd.irq_enable = b & 0x1F;
        break;
    case 7:                                                  /* interrupt acknowledge */
        if (b & 0x40) cd.n_params = 0;
        if (b & 0x1F) {
            cd.irq_flag &= (uint8_t)~(b & 0x1F);
            if (!(cd.irq_flag & 7)) cd.irq_flag = 0;
            if (!cd.irq_flag) a2n_event_post(A2N_EV_CD);
        }
        break;
    case 10: cd.pend_ll = b; break;                          /* CD audio volume */
    case 11: cd.pend_lr = b; break;
    case 13: cd.pend_rr = b; break;
    case 14: cd.pend_rl = b; break;
    case 15:
        if (b & 0x20) {
            cd.vol_ll = cd.pend_ll; cd.vol_lr = cd.pend_lr;
            cd.vol_rr = cd.pend_rr; cd.vol_rl = cd.pend_rl;
        }
        break;
    default:
        break;
    }
}

/* DMA channel 3: the data fifo into RAM */
void a2n_cd_dma(uint32_t addr, uint32_t words)
{
    for (uint32_t i = 0; i < words; ++i) {
        uint32_t w = 0;
        for (int k = 0; k < 4; ++k)
            w |= (uint32_t)(cd.fifo_pos < cd.fifo_n ? cd.fifo[cd.fifo_pos++] : 0) << (8 * k);
        a2n_w32(addr + 4 * i, w);
    }
}

void a2n_cd_init(void)
{
    memset(&cd, 0, sizeof cd);
    cd.stat = ST_MOTOR;
    cd.irq_enable = 0x1F;
    cd.vol_ll = cd.vol_rr = cd.pend_ll = cd.pend_rr = 0x80;
    a2n_xa_reset();
    a2n_io_register(0x1F801800u, 0x1F801804u, cd_read, cd_write);
}
