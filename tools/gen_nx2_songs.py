#!/usr/bin/env python3
"""
gen_nx2_songs.py - gera src/nx2_songs.c a partir da tabela de musicas do piu do NX2 (ELF i386).

Tabela 0x0810E520: 220 registros x 0x5C bytes (NX: 192 x 0x48). Delimitada por varredura
(ponteiros de string validos + seq 0..219 monotonico); fim em 0x08113430.
  +0x00 u32 sequencia            (CONFIRMADO: 0..219)
  +0x04 u32 id                   (CONFIRMADO: os 220 ids tem pasta STEP/<id>/)
  +0x08/+0x0C artista KR/EN, +0x10/+0x14 titulo KR/EN (ponteiros UTF-8)
  +0x18 ponteiro BPM em texto
  +0x1C u32 canal 0..6           (0 NX, 1 K-POP, 2 POP, 3 BANYA, 4 FULLSONG, 5 REMIX, 6 ANOTHER; mesmos da NX)
  +0x20..+0x30 5 x s32 niveis NORMAL HARD CRAZY FREESTYLE NIGHTMARE (-1 = nao existe)
  Do +0x34 em diante a ordem vem de SongList_t (nx2src songmanager.h, "extraida do executavel NX2",
  sizeof = 92 = 0x5C, confere com o stride medido) e foi conferida contra os dados do piu:
  +0x34 bCensor  +0x35 bCensorLock  +0x36 bEnable_m  +0x37 bHidden_m  +0x38 bEnable_play  +0x39 bHidden_play
        (186 musicas: enable_m=1/hidden_m=0/enable_play=1/hidden_play=0; 34: enable_m=0/hidden_m=1/
         enable_play=0/hidden_play=1 -> bate com o 34 de +0x38=0x100)
  +0x3C s32 TexID (-1 em todos os 220 no arquivo; preenchido em execucao)
  +0x40 5 bytes bEnableMode_m[5]  (NN HD CR FS NM)
  +0x45 5 bytes bEnableMode_play[5] (0 em todos no arquivo)
  +0x4A byte bEnableDemoPlay (28 musicas)
  +0x4C s16 time (82..283 = duracao em s p/ kcal/VO2)   +0x4E s16 SongMileage (50..1600, custo de desbloqueio)
  +0x50 5 x s16 ModeMileage[5]
  Os NOMES dos campos vem do nx2src; a ORDEM/OFFSETS foram verificados nos dados. O que cada flag
  faz em execucao (m vs play) ainda nao foi confirmado no piu.

Mapa de recursos 0x0810DE08: pares { u32 id_recurso, u32 id_chart }, termina com id_chart 0
(214 pares). Mesma estrutura da NX (0x0813F860).

Uso:
  python tools/gen_nx2_songs.py <piu> <saida.c>
"""
import struct
import sys

SONG_VA = 0x0810E520
SONG_COUNT = 220
SONG_SIZE = 0x5C
RESMAP_VA = 0x0810DE08
CHANNEL_NAMES = ("NX", "K-POP", "POP", "BANYA", "FULLSONG", "REMIX", "ANOTHER")


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    b = open(sys.argv[1], "rb").read()
    phoff, = struct.unpack_from("<I", b, 0x1C)
    phnum, = struct.unpack_from("<H", b, 0x2C)
    segs = []
    for i in range(phnum):
        t, o, va, _, fsz, _, _, _ = struct.unpack_from("<8I", b, phoff + i * 32)
        if t == 1:
            segs.append((va, o, fsz))

    def off(va):
        for s, o, n in segs:
            if s <= va < s + n:
                return va - s + o
        raise ValueError("VA fora do arquivo: 0x%08X" % va)

    def cstr(va):
        if va == 0:
            return ""
        o = off(va)
        return b[o:b.index(b"\0", o)].decode("utf-8", "replace")

    resmap = []
    while True:
        res, chart = struct.unpack_from("<II", b, off(RESMAP_VA + len(resmap) * 8))
        if chart == 0:
            break
        resmap.append((res, chart))
    res_of = {}
    for res, chart in resmap:
        res_of.setdefault(chart, res)

    songs = []
    for i in range(SONG_COUNT):
        o = off(SONG_VA + i * SONG_SIZE)
        seq, sid, ak, ae, tk, te, bpm, ch = struct.unpack_from("<II6I", b, o)
        bpm_txt = cstr(bpm).strip()
        try:
            bpm_num = float(bpm_txt.split("~")[0])
        except ValueError:
            bpm_num = 0.0
        res = res_of.get(sid, -1)
        tm, mil = struct.unpack_from("<hh", b, o + 0x4C)
        songs.append(dict(
            seq=seq, id=sid, ak=cstr(ak), ae=cstr(ae), tk=cstr(tk), te=cstr(te),
            bpm=bpm_num, bpmText=bpm_txt, ch=ch,
            lv=struct.unpack_from("<5i", b, o + 0x20),
            fl=tuple(b[o + 0x34:o + 0x3A]), tex=struct.unpack_from("<i", b, o + 0x3C)[0],
            lock=b[o + 0x40:o + 0x45], mplay=tuple(b[o + 0x45:o + 0x4A]), demo=b[o + 0x4A],
            length=tm, mileage=mil, modemil=struct.unpack_from("<5h", b, o + 0x50),
            base=(0xC04 if res == 0xC1C04 else res) if res >= 0x100 else -1))   # resmap do piu aponta DC04 -> 0xC1C04, que nao existe em TITLE/PREVIEW/EYE_SMALL (so C04.*)
    chans = [[s["id"] for s in songs if s["ch"] == c] for c in range(len(CHANNEL_NAMES))]
    cmax = max(len(c) for c in chans) + 1

    def c_escape(s):
        out = []
        for ch in s.encode("utf-8"):
            if ch in (0x22, 0x5C):
                out.append("\\" + chr(ch))
            elif 32 <= ch < 127:
                out.append(chr(ch))
            else:
                out.append("\\%03o" % ch)
        return '"' + "".join(out) + '"'

    L = []
    L.append("/* GERADO por tools/gen_nx2_songs.py a partir do piu (Pump It Up NX2) - nao editar a mao.")
    L.append(" *   g_songTable     <- 0x0810e520 (%d x 0x5C bytes)" % SONG_COUNT)
    L.append(" *   g_songChannels  <- campo +0x1C de cada registro")
    L.append(" *   g_nxResMap        <- 0x0810de08 (%d pares recurso/chart)" % len(resmap))
    L.append(" * Substitui src/nx_songs.c (mesmos simbolos). */")
    L.append('#include "pumpy.h"')
    L.append("")
    L.append("#if SONG_TABLE_COUNT != %d || SONG_CHANNEL_COUNT != %d || SONG_CHANNEL_MAX != %d || NX_RESMAP_COUNT != %d" % (len(songs), len(CHANNEL_NAMES), cmax, len(resmap)))
    L.append('#error "pumpy.h: contagens fora do NX2 (rode tools/gen_nx2_songs.py)"')
    L.append("#endif")
    L.append("")
    L.append("TrackEntry g_songTable[SONG_TABLE_COUNT] = {")
    for i, s in enumerate(songs):
        L.append("    { 0x%X, %s, %s, %s, %s, %.4f, { %s }, %d, %d, { %s }, %s, %d, %d, %d, %d, %s, %d }, /* %d */" % (
            s["id"], c_escape(s["ak"]), c_escape(s["ae"]), c_escape(s["tk"]), c_escape(s["te"]), s["bpm"],
            ", ".join(str(x) for x in s["lv"]), s["fl"][4], s["fl"][5], ", ".join(str(x) for x in s["lock"]),
            ("0x%X" % s["base"]) if s["base"] >= 0 else "-1", s["ch"], s["seq"], s["demo"], s["fl"][4],
            c_escape(s["bpmText"]), s["length"], i))
    L.append("};")
    L.append("")
    L.append("/* campos do SongList_t que o TrackEntry da NX nao tem (mesma ordem de g_songTable) */")
    L.append("const Nx2SongExtra g_nx2SongExtra[SONG_TABLE_COUNT] = {")
    for i, s in enumerate(songs):
        L.append("    { %s, { %s }, %d, { %s } }, /* %d */" % (", ".join(str(x) for x in s["fl"]),
                 ", ".join(str(x) for x in s["mplay"]), s["mileage"], ", ".join(str(x) for x in s["modemil"]), i))
    L.append("};")
    L.append("")
    L.append("const int g_songChannels[SONG_CHANNEL_COUNT][SONG_CHANNEL_MAX] = {")
    for c, ids in enumerate(chans):
        L.append("    /* %s (%d) */ { %s }," % (CHANNEL_NAMES[c], len(ids),
                 ", ".join("0x%X" % x for x in ids + [0] * (cmax - len(ids)))))
    L.append("};")
    L.append("")
    L.append("const uint32_t g_nxResMap[NX_RESMAP_COUNT][2] = {")
    for k in range(0, len(resmap), 6):
        L.append("    " + " ".join("{ 0x%X, 0x%X }," % p for p in resmap[k:k + 6]))
    L.append("};")
    L.append("")
    open(sys.argv[2], "w", encoding="utf-8", newline="\n").write("\n".join(L))
    for c, ids in enumerate(chans):
        print("canal %d %-8s %3d musicas" % (c, CHANNEL_NAMES[c], len(ids)))
    print("SONG_CHANNEL_MAX = %d; NX_RESMAP_COUNT = %d; gravado: %s" % (cmax, len(resmap), sys.argv[2]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
