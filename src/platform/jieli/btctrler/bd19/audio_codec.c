/* Recovered from BD19 audio_codec.c.o, occurrence 1. */
#include "btctrler/glue.h"
#include <stdint.h>
#include <string.h>
#include <stddef.h>

extern int printf(const char *format, ...);
#ifdef UNITTEST
extern int audio_test_printf(const char *format, ...);
#define printf audio_test_printf
#endif

#if UINTPTR_MAX == UINT32_MAX
_Static_assert(sizeof(PalCodecSreamParam_t) == 20, "SDK stream parameter ABI");
_Static_assert(offsetof(PalCodecSreamParam_t, rdyCback) == 16, "SDK callback offset");
_Static_assert(sizeof(cs47x_Stream_t) == 7708, "SDK stream context ABI");
_Static_assert(offsetof(cs47x_Stream_t, pcm) == 16, "SDK audio buffer offset");
_Static_assert(offsetof(cs47x_Stream_t, prodCtr) == 7696, "SDK producer counter offset");
_Static_assert(offsetof(cs47x_Stream_t, consCtr) == 7700, "SDK consumer counter offset");
_Static_assert(offsetof(cs47x_Stream_t, numProd) == 7704, "SDK sync counter offset");
#endif

/* This common symbol is part of the SDK object's external ABI. */
uint32_t palCodecScratch[400] __attribute__((common, aligned(4)));

static cs47x_Stream_t palCodecStreamTbl[2];

void PalCodecReadLocalSupportedCodecs(uint8_t *pNumStd,
        AudioStdCodecInfo_t *stdCodecs, uint8_t *pNumVs,
        AudioVsCodecInfo_t *vsCodecs)
{
    (void)vsCodecs;
    if (*pNumStd > 1) {
        *pNumStd = 1;
        stdCodecs[0].codecId = 1;
    }
    *pNumVs = 0;
}

uint8_t PalCodecReadLocalSupportedCodecCapabilities(uint8_t codingFmt,
        uint16_t compId, uint16_t vsCodecId, PalAudioDir_t dir)
{
    (void)compId;
    (void)vsCodecId;
    (void)dir;
    return codingFmt == 1;
}

uint8_t PalCodecReadLocalSupportedControllerDelay(uint8_t codingFmt,
        uint16_t compId, uint16_t vsCodecId, PalAudioDir_t dir,
        uint32_t *pMinDly, uint32_t *pMaxDly)
{
    (void)compId;
    (void)vsCodecId;
    if (codingFmt != 1)
        return 0;
    if ((uint32_t)dir > 1)
        return 17;
    *pMinDly = dir == 0 ? 1000 : 100;
    *pMaxDly = dir == 0 ? 2000 : 200;
    return 1;
}

uint8_t PalCodecConfigureDataPath(PalAudioDir_t dir, uint8_t dataPathId)
{
    (void)dir;
    (void)dataPathId;
    return 1;
}

void PalCodecDataInit(void) {}

static cs47x_Stream_t *audioFindContext(uint16_t id)
{
    unsigned i;
    for (i = 0; i < 2; ++i) {
        if (palCodecStreamTbl[i].enabled && palCodecStreamTbl[i].id == id)
            return &palCodecStreamTbl[i];
    }
    return 0;
}

uint8_t PalCodecDataStartStream(uint16_t id, PalCodecSreamParam_t *pParam)
{
    unsigned i;
    if (audioFindContext(id) != 0) {
        printf("Stream already in use, id=%d", (int)id);
        return 0;
    }
    for (i = 0; i < 2; ++i) {
        cs47x_Stream_t *s = &palCodecStreamTbl[i];
        if (!s->enabled) {
            s->enabled = 1;
            s->id = id;
            s->chMask = pParam->chMask;
            s->intervalUsec = pParam->intervalUsec;
            s->dir = (uint8_t)pParam->dir;
            s->rdyCback = pParam->rdyCback;
            uint32_t first = pParam->pktCtr + 1;
            s->consCtr = first;
            s->prodCtr = first;
            return 1;
        }
    }
    return 0;
}

void PalCodecDataStopStream(uint16_t id)
{
    cs47x_Stream_t *s = audioFindContext(id);
    if (s != 0)
        s->enabled = 0;
}

uint16_t PalCodecDataStreamIn(uint16_t id, uint8_t *pBuf,
        uint16_t len, uint32_t *pPktCtr)
{
    cs47x_Stream_t *s = audioFindContext(id);
    (void)pBuf;
    (void)len;
    if (s == 0) {
        printf("Unknown stream id=%u", (unsigned)id);
    } else {
        if (pPktCtr != 0)
            *pPktCtr = s->consCtr;
        ++s->consCtr;
    }
    return 0;
}

void PalCodecDataStreamOut(uint16_t id, const uint8_t *pBuf,
        uint16_t len, uint32_t pktCtr)
{
    cs47x_Stream_t *s = audioFindContext(id);
    uint32_t next;
    int32_t delta;
    (void)pBuf;
    (void)len;
    if (s == 0) {
        printf("Unknown stream id=%u", (unsigned)id);
        return;
    }
    printf("Missed audio packet; conceal with mute packet, id=%u, pktCtr[15:0]=%u",
            (unsigned)s->id, (unsigned)pktCtr);
    next = (s->prodCtr + 1) % 12;
    __builtin_memset(s->pcm[next], 0, 640);
    if (s->prodCtr + 1 == pktCtr) {
        delta = (int32_t)(pktCtr - s->consCtr);
        if (delta < 0) {
            printf("Output audio stream underrun, id=%u, pktCtr[15:0]=%u",
                    (unsigned)s->id, (unsigned)s->consCtr);
        } else if (delta > 12) {
            printf("Output audio stream overrun, id=%u, pktCtr[15:0]=%u",
                    (unsigned)s->id, (unsigned)s->consCtr);
            s->consCtr = pktCtr - 12;
        }
        s->prodCtr = pktCtr;
        ++s->numProd;
        if (s->numProd == 6) {
            printf("Output stream synchronized, id=%u pktCtr[15:0]=%u",
                    (unsigned)id, (unsigned)pktCtr);
            s->consCtr += 2;
        }
    } else {
        printf("Output stream synchronization initialized/lost, id=%u pktCtr[15:0]=%u",
                (unsigned)id, (unsigned)pktCtr);
        s->consCtr = pktCtr;
        s->prodCtr = pktCtr;
        s->numProd = 0;
    }
}

#ifdef UNITTEST
#include "test.h"
#include <assert.h>

static unsigned test_prints;
static void (*print_hook)(const char *);
static unsigned data_ready_calls;
int printf(const char *fmt, ...)
{
    ++test_prints;
    if (print_hook) print_hook(fmt);
    return 0;
}

#undef printf

static void data_ready(uint16_t id) { (void)id; ++data_ready_calls; }

static void mutate_log_counters(const char *fmt)
{
    if (strncmp(fmt, "Missed audio", 12) == 0) {
        palCodecStreamTbl[0].prodCtr = 4;
        palCodecStreamTbl[0].consCtr = 5;
    } else if (strncmp(fmt, "Output stream synchronized", 26) == 0) {
        palCodecStreamTbl[0].consCtr = 100;
    }
}

static void restart_in_diagnostic(const char *fmt)
{
    if (strncmp(fmt, "Missed audio", 12) == 0) {
        PalCodecSreamParam_t p = {0, 1, 1000, 20, data_ready};
        PalCodecDataStopStream(7);
        assert(PalCodecDataStartStream(99, &p) == 1);
    }
}

static void reset_test_state(void)
{
    __builtin_memset(palCodecStreamTbl, 0, sizeof(palCodecStreamTbl));
    test_prints = 0;
    print_hook = 0;
    data_ready_calls = 0;
}

int main(void)
{
    uint8_t count, vsCount;
    AudioStdCodecInfo_t codec = { 0xA5 };
    uint32_t min = 0x11223344, max = 0x55667788, seq = 0;
    PalCodecSreamParam_t p = { 0x102, 0x1234, 0x56789, 9, 0 };
    unsigned i;

    count = 1; vsCount = 0xCC;
    PalCodecReadLocalSupportedCodecs(&count, &codec, &vsCount, 0);
    assert(count == 1 && codec.codecId == 0xA5 && vsCount == 0);
    count = 2; codec.codecId = 0xA5; vsCount = 0xCC;
    PalCodecReadLocalSupportedCodecs(&count, &codec, &vsCount, 0);
    assert(count == 1 && codec.codecId == 1 && vsCount == 0);
    assert(PalCodecReadLocalSupportedCodecCapabilities(1, 7, 8, 0) == 1);
    assert(PalCodecReadLocalSupportedCodecCapabilities(2, 7, 8, 0) == 0);
    assert(PalCodecReadLocalSupportedControllerDelay(1, 0, 0, 0, &min, &max) == 1);
    assert(min == 1000 && max == 2000);
    min = 0x11223344; max = 0x55667788;
    assert(PalCodecReadLocalSupportedControllerDelay(1, 0, 0, 1, &min, &max) == 1);
    assert(min == 100 && max == 200);
    assert(PalCodecReadLocalSupportedControllerDelay(1, 0, 0, 2, &min, &max) == 17);
    assert(min == 100 && max == 200);
    assert(PalCodecReadLocalSupportedControllerDelay(2, 0, 0, 0, &min, &max) == 0);

    reset_test_state();
    PalCodecDataInit();
    assert(PalCodecDataStartStream(7, &p) == 1);
    assert(PalCodecDataStartStream(7, &p) == 0 && test_prints == 1);
    assert(PalCodecDataStreamIn(7, 0, 65535, &seq) == 0 && seq == 10);
    PalCodecDataStreamOut(7, 0, 65535, 10);
    assert(palCodecStreamTbl[0].prodCtr == 10);
    assert(palCodecStreamTbl[0].pcm[11][0] == 0);
    assert(palCodecStreamTbl[0].rdyCback == 0);
    for (i = 0; i < 6; ++i) {
        uint32_t pkt = palCodecStreamTbl[0].prodCtr + 1;
        PalCodecDataStreamOut(7, 0, 0, pkt);
    }
    assert(palCodecStreamTbl[0].numProd == 6);
    assert(palCodecStreamTbl[0].consCtr == 12);
    PalCodecDataStopStream(7);
    assert(PalCodecDataStartStream(8, &p) == 1);
    assert(palCodecStreamTbl[0].consCtr == 10 && palCodecStreamTbl[0].prodCtr == 10);
    assert(palCodecStreamTbl[0].numProd == 6);
    palCodecStreamTbl[0].prodCtr = 20;
    palCodecStreamTbl[0].consCtr = 0;
    PalCodecDataStreamOut(8, 0, 0, 21);
    assert(palCodecStreamTbl[0].prodCtr == 21 && palCodecStreamTbl[0].consCtr == 9);
    assert(palCodecStreamTbl[0].rdyCback == 0);
    assert(PalCodecDataStreamIn(7, 0, 0, &seq) == 0);
    assert(test_prints > 1);
    /* Input capacity zero preserves the first codec byte too. */
    count = 0; codec.codecId = 0xa5; vsCount = 0xff;
    PalCodecReadLocalSupportedCodecs(&count, &codec, &vsCount, 0);
    assert(count == 0 && codec.codecId == 0xa5 && vsCount == 0);

    reset_test_state();
    p.rdyCback = data_ready;
    p.pktCtr = UINT32_MAX;
    assert(PalCodecDataStartStream(7, &p) == 1);
    assert(palCodecStreamTbl[0].prodCtr == 0 && palCodecStreamTbl[0].consCtr == 0);
    assert(PalCodecDataStartStream(8, &p) == 1);
    assert(PalCodecDataStartStream(9, &p) == 0); /* Full two-slot table. */
    palCodecStreamTbl[0].consCtr = UINT32_MAX;
    assert(PalCodecDataStreamIn(7, 0, 0, &seq) == 0 && seq == UINT32_MAX);
    assert(palCodecStreamTbl[0].consCtr == 0);
    /* Init is a no-op even with active, dirty streams. */
    palCodecStreamTbl[0].numProd = 5;
    PalCodecDataInit();
    assert(palCodecStreamTbl[0].enabled && palCodecStreamTbl[0].numProd == 5);
    PalCodecDataStreamOut(7, 0, 0, 1);
    assert(palCodecStreamTbl[0].consCtr == 2 && data_ready_calls == 0);

    reset_test_state();
    p.pktCtr = 0;
    assert(PalCodecDataStartStream(7, &p) == 1);
    __builtin_memset(palCodecStreamTbl[0].pcm, 0x5a, sizeof(palCodecStreamTbl[0].pcm));
    palCodecStreamTbl[0].numProd = 5;
    print_hook = mutate_log_counters;
    PalCodecDataStreamOut(7, 0, 0, 5);
    assert(palCodecStreamTbl[0].prodCtr == 5 && palCodecStreamTbl[0].consCtr == 102);
    for (unsigned row = 0; row < 12; ++row)
        for (unsigned col = 0; col < 320; ++col)
            assert(palCodecStreamTbl[0].pcm[row][col] == (row == 5 ? 0 : 0x5a5a));
    assert(data_ready_calls == 0);

    /* A printf callback can stop/reuse the selected private context. The
     * SDK continues with that pointer and reloads its counters after logging. */
    print_hook = restart_in_diagnostic;
    PalCodecDataStreamOut(7, 0, 0, 22);
    assert(palCodecStreamTbl[0].id == 99 && palCodecStreamTbl[0].prodCtr == 22);
    assert(palCodecStreamTbl[0].consCtr == 21 && data_ready_calls == 0);
    print_hook = 0;
    assert(sizeof(palCodecScratch) == 1600);
    return TEST_DONE("btctrler_audio_codec_bd19");
}
#endif
