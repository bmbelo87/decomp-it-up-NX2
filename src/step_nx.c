/* NX2: charts "STEP/<id>/<MODO>.NX" (magic "NX10"), sem compressao nem cifra.
 *
 * Layout (derivado dos arquivos de E:\20_nx2\game\STEP e do leitor 0x807f5d0 do piu NX2;
 * o motor que consome as notas nao esta no nx2src):
 *   +0x00 "NX10"
 *   +0x04 u32 (0; 2 nos HF; 10 nos LM)
 *   +0x08 u32 colunas por linha (5 NO/HD/CR/PR/DV, 10 NM/FR, 6 HF)            [confirmado nos 2133 arquivos]
 *   +0x0C u32 N secoes
 *   +0x10 u32 off_secao[N]  (a partir do inicio do arquivo)
 *   secao: u32 M; u32 off_bloco[M]   (M = 1, exceto DV: M ramos)
 *   bloco: +0x00 f32 inicio absoluto (ms)   +0x04 f32 BPM          +0x08 f32 beats por linha
 *          +0x0C f32 delay antes do segmento (ms)   +0x10 f32 velocidade (1.0 = normal; FIRE: BPM 544 x 0.25)
 *          +0x14 u32 0   +0x18 u16 linhas por beat   +0x1A u16 beats por compasso
 *          +0x1C u32 N linhas (slots)   +0x20 u32 off_linha[N]   (0 = linha vazia)
 *   linha: `colunas` x u16. Nibble baixo: 3 tap, 4 inicio de hold, 6 corpo, 7 fim. Os bits altos
 *          (0xB0 na maioria) e os codigos 0x...F1/0x...F2 ainda NAO foram decodificados.  [hipotese]
 * Colunas 0..4 = DL UL CN UR DR do P1, 5..9 = o mesmo do P2 (ordem herdada do .SEE: hipotese).
 * Tempo de uma linha: igual ao .SEE (delayDiv 1000): inicio do segmento = soma(linhas*spr + delay). */
#include "step.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void Log_Print(const char* fmt, ...);

static const struct { const char* name; int sec; } k_nxModes[] = {
    { "PR", 0 }, { "NO", 1 }, { "HD", 2 }, { "NM", 3 }, { "CR", 4 }, { "FR", 5 }, { "HF", 6 }, { "DV", 7 },
};

static uint32_t rd32(const uint8_t* b, size_t len, size_t o, bool* ok)
{
    uint32_t v = 0;
    if (o + 4 > len) { *ok = false; return 0; }
    memcpy(&v, b + o, 4);
    return v;
}

static float rdf(const uint8_t* b, size_t len, size_t o, bool* ok)
{
    float v = 0;
    if (o + 4 > len) { *ok = false; return 0; }
    memcpy(&v, b + o, 4);
    return v;
}

static uint8_t nxCell(uint16_t v)
{
    switch (v & 0xF) {
    case 3: return NT_TAP;
    case 4: return NT_HOLD_H;
    case 6: return NT_HOLD_B;
    case 7: return NT_HOLD_T;
    default: return 0;
    }
}

/* colunas -> StepHalf; HF (6 colunas) ocupa CN1 UR1 DR1 DL2 UL2 CN2 = indices 2..7 [hipotese] */
static void nxFillRow(StepRow* r, const uint16_t* c, int cols)
{
    uint8_t v[10] = { 0 };
    if (cols == 6) { for (int i = 0; i < 6; i++) v[2 + i] = nxCell(c[i]); }
    else            { for (int i = 0; i < cols && i < 10; i++) v[i] = nxCell(c[i]); }
    r->half1.dl = v[0]; r->half1.ul = v[1]; r->half1.cn = v[2]; r->half1.ur = v[3]; r->half1.dr = v[4];
    if (cols == 5) r->half2 = r->half1;   /* single: P2 le a mesma metade (como no .SEE) */
    else { r->half2.dl = v[5]; r->half2.ul = v[6]; r->half2.cn = v[7]; r->half2.ur = v[8]; r->half2.dr = v[9]; }
}

static bool nxParse(const uint8_t* b, size_t len, int sec, StepChart* ch)
{
    bool ok = true;
    if (len < 0x10 || memcmp(b, "NX10", 4) != 0) return false;
    uint32_t cols = rd32(b, len, 8, &ok), nSec = rd32(b, len, 12, &ok);
    if (!ok || (cols != 5 && cols != 6 && cols != 10) || nSec == 0 || nSec > 100000) return false;

    /* 1a passada: total de linhas */
    uint64_t total = 0;
    for (uint32_t s = 0; s < nSec; s++) {
        uint32_t so = rd32(b, len, 16 + 4 * s, &ok), bo = rd32(b, len, so + 4, &ok);
        total += rd32(b, len, bo + 0x1C, &ok);
        if (!ok) return false;
    }
    if (total == 0 || total > 4000000) return false;

    StepRow* rows = (StepRow*)calloc((size_t)total, sizeof(StepRow));
    if (!rows) return false;

    memset(ch, 0, sizeof(*ch));
    ch->sectionIndex = sec;
    ch->delayDiv = 1000;
    ch->panelCount = (cols == 5) ? STEP_PANELS_SINGLE : STEP_PANELS_DOUBLE;
    ch->rows = rows;

    uint32_t row0 = 0, nseg = 0, merged = 0;
    int notes = 0;
    for (uint32_t s = 0; s < nSec; s++) {
        uint32_t so = rd32(b, len, 16 + 4 * s, &ok), bo = rd32(b, len, so + 4, &ok);
        float bpm = rdf(b, len, bo + 4, &ok), delay = rdf(b, len, bo + 0xC, &ok), spd = rdf(b, len, bo + 0x10, &ok);
        uint32_t n = rd32(b, len, bo + 0x1C, &ok);
        uint16_t split = 0, meas = 0;
        if (bo + 0x1C > len) ok = false;
        if (!ok) break;
        memcpy(&split, b + bo + 0x18, 2); memcpy(&meas, b + bo + 0x1A, 2);
        for (uint32_t k = 0; k < n; k++) {
            uint32_t ro = rd32(b, len, bo + 0x20 + 4 * k, &ok);
            if (!ok) break;
            if (ro == 0) continue;
            if (ro + 2u * cols > len) continue;
            uint16_t c[10];
            memcpy(c, b + ro, 2 * cols);
            nxFillRow(&rows[row0 + k], c, (int)cols);
            for (uint32_t i = 0; i < cols; i++) { uint16_t t = c[i] & 0xF; if (t == 3 || t == 4) notes++; }
        }
        int32_t d = (int32_t)(delay + (delay >= 0 ? 0.5f : -0.5f));
        int32_t sp = (int32_t)(spd * 1000.0f + (spd >= 0 ? 0.5f : -0.5f));

        /* mescla com o anterior se for identico e sem delay (o jogo guarda ate STEP_MAX_BLOCK_Y segmentos) */
        if (nseg > 0 && d == 0 &&
            ch->segments[nseg - 1].bpm == bpm && ch->segments[nseg - 1].beatSplit == split &&
            ch->segments[nseg - 1].beatPerMeasure == meas && ch->segments[nseg - 1].speed == sp) {
            ch->segments[nseg - 1].rowCount += n;
            merged++;
        } else if (nseg < STEP_MAX_BLOCK_Y) {
            ch->segments[nseg].bpm = bpm; ch->segments[nseg].beatSplit = split; ch->segments[nseg].beatPerMeasure = meas;
            ch->segments[nseg].delay = d; ch->segments[nseg].rowStart = row0; ch->segments[nseg].rowCount = n;
            ch->segments[nseg].speed = sp; ch->segments[nseg].stopFlag = 0;
            nseg++;
        } else {
            ch->segments[nseg - 1].rowCount += n;   /* estourou: o resto fica no ultimo (log abaixo) */
            merged++;
        }
        row0 += n;
    }
    if (!ok || nseg == 0) { free(rows); ch->rows = NULL; return false; }

    ch->rowCount = row0;
    ch->segmentCount = nseg;
    ch->bpm = ch->segments[0].bpm;
    ch->beatSplit = ch->segments[0].beatSplit;
    ch->beatPerMeasure = ch->segments[0].beatPerMeasure;
    ch->delay = ch->segments[0].delay;
    ch->hasSplit = nseg > 1;
    ch->totalNotes = notes;
    if (nSec > nseg) Log_Print("NX10: %u secoes -> %u segmentos (%u mescladas)\n", nSec, nseg, merged);
    return true;
}

/* dir = ".../STEP/<id>" (sem barra final) */
bool Step_LoadNX(const char* dir, StepSong* song)
{
    memset(song, 0, sizeof(StepSong));
    for (size_t m = 0; m < sizeof(k_nxModes) / sizeof(k_nxModes[0]); m++) {
        char p[1024];
        snprintf(p, sizeof(p), "%s/%s.NX", dir, k_nxModes[m].name);
        FILE* f = fopen(p, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t* buf = (sz > 0) ? (uint8_t*)malloc((size_t)sz) : NULL;
        bool rd = buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz;
        fclose(f);
        if (rd && song->chartCount < 9 && nxParse(buf, (size_t)sz, k_nxModes[m].sec, &song->charts[song->chartCount])) {
            StepChart* c = &song->charts[song->chartCount];
            Log_Print("NX10: %s -> secao %d, %u linhas, %u segmentos, %d notas, bpm %.2f\n",
                      p, c->sectionIndex, c->rowCount, c->segmentCount, c->totalNotes, c->bpm);
            song->chartCount++;
        } else {
            Log_Print("NX10: falha ao ler '%s'\n", p);
        }
        free(buf);
    }
    return song->chartCount > 0;
}

/* true se existe pelo menos um .NX no diretorio do chart (STEP/<id>/) */
bool Step_NXExists(const char* dir)
{
    for (size_t m = 0; m < sizeof(k_nxModes) / sizeof(k_nxModes[0]); m++) {
        char p[1024];
        snprintf(p, sizeof(p), "%s/%s.NX", dir, k_nxModes[m].name);
        FILE* f = fopen(p, "rb");
        if (f) { fclose(f); return true; }
    }
    return false;
}
