/*
 * nx2_world.c — WORLD MAX do NX2: mapa-múndi (CSelectWorld, nx2src ProcModules/select_mission.cpp + CWorldMap.cpp).
 *
 * Fluxo do nx2src (flowcontrol.cpp): Station -> SELECTMS (esta tela) -> W_BRIEFING -> Play -> WGRADE -> Eyecatch -> SELECTMS.
 *
 * Esta fatia (fase 2a): mapa de ladrilhos (MAPDATA.DAT, 13 x 9 de 512 x 256), pontos, conexões, setas de direção,
 * movimento com animação (15 quadros), vidas, tempo (60 s), BGM por região, mini-mapa, janela de informação
 * (região, nível, break, jogadores, switch) e saída (fade de 30 quadros).
 * W_BRIEFING (worldmaxbriefing.cpp): tela do objetivo da missão + ícones dos comandos; depois a missão é lançada.
 * NÃO feito ainda: warp, caixa de missão (M_BOX), gelo, vulcão, bomba, comandos escondidos, painel BOARD/BARI,
 * itens do briefing (setas de mileage/vida/modo/loteria), WGRADE (resultado da missão).
 * Confiança: o nx2src está confirmado; o piu tem o mesmo fluxo (SELECTMS, W_BRIEFING, WGRADE, MISSIONCLEAR, CSelectWorld)
 * mas esta tela ainda não foi comparada função a função com o piu.
 */
#include "pumpy.h"
#include "bga.h"
#include "nx2_mission.h"
#include "movie.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

extern unsigned Title_GetJoinedMask(void);

/* ---- índices dos BGAs, na ordem de carga (g_BGA[0..6] do original) ---- */
enum { B_UI = 0, B_ICON, B_MBOX, B_BOARD, B_EVENTEFF, B_W, B_BARI };

#define TILE_ROW_MAX 13
#define TILE_COL_MAX 9
#define ANOTHER_TILE_MAX 50
#define SELECT_TIME 60

enum { E_DISABLE = 0, E_LOCKED, E_CONNECTED };

/* SFX_WORLDMAX.LUA */
enum { WS_M, WS_B, WS_S, WS_C, WS_R, WS_H, WS_G, WS_P, WS_N, WS_CENTER, WS_MOVE, WS_NOSTART, WS_NOMOVE, WS_WARP, WS_VOL, WS_ICE, WS_MINE, WS_START, WS_COUNT };
static const char* const k_wav[WS_COUNT] = {
    "MIRTAIN.WAV", "BARHARN.WAV", "SHANTOMIA.WAV", "CRYOMISTION.WAV", "ROOTINIA.WAV", "HARENA.WAV", "MORIGIN.WAV",
    "PRUE OCEAN.WAV", "NX2 ZONE.WAV", "WCENTER.WAV", "WMOVE.WAV", "WNOTSTART.WAV", "WNOTMOVE.WAV", "WARP.WAV", "VOLCANO.WAV",
    "ICE.WAV", "WMINE.WAV", "START.WAV",
};
static int  s_snd[WS_COUNT];
static bool s_sndInit;
static void snd(int k, bool loop) { if (k >= 0 && s_snd[k] >= 0) Audio_Play(s_snd[k], loop); }

/* BgTable (select_mission.cpp): região -> BGM */
static const struct { const char* land; int bgm; } k_bg[] = {
    { "Mirtain", WS_M }, { "Barharn", WS_B }, { "Shantomia", WS_S }, { "Rootinia", WS_R },
    { "Cryomiston", WS_C }, { "Harena", WS_H }, { "Morigin", WS_G }, { "Prue Ocean", WS_P },
};
static const char* const k_landList[9] = { "Mirtain", "Barharn", "Shantomia", "Cryomiston", "Rootinia", "Harena", "Morigin", "Prue Ocean", "NX2 ZONE" };

/* ---- estado ---- */
static bool s_ok;
static int  s_phase;                 /* 0 Start, 1 Run, 2 End */
static int  s_player;
static int  s_cnt;                   /* m_Cnt (End) */
static double s_timeAcc;             /* m_iTimeCnt em segundos (o original: m_iTimeCnt / 80 por quadro) */
static int  s_time;
static bool s_selected, s_wasMoving, s_switch, s_bEnd;
static int  s_worldLife = 3;         /* g_StateManager.m_nWorldLife */
static bool s_session;               /* já existe uma lista corrente (m_CurrentStage != 0) */
static NxMission* s_curM;
static NxMission* s_prevM;
static int  s_bgm = -1;

/* textura dos ladrilhos */
static int s_tex[TILE_ROW_MAX][TILE_COL_MAX];
static struct { int row, col, texId; } s_alt[ANOTHER_TILE_MAX], s_orig[ANOTHER_TILE_MAX];
static bool s_another;

/* mapa */
static int s_posX, s_posY, s_startX, s_startY, s_endX, s_endY, s_mvCnt;
static bool s_move;

/* camadas copiadas do BGA (SetLayerSprite) */
static BGALayerSrc s_timeNum[10], s_levelSpr[5], s_breakSpr[2], s_playerSpr[3], s_switchSpr[2], s_landSpr[9], s_miniSpr[2];
static BGALayerSrc s_pt[2], s_line, s_arrow[2], s_lock[12];
static float s_alpha = 1.0f;

/* ---- perfis (ps / pms do nx2src) ---- */
static Nx2Stat* ps(void)  { return Nx2_Stat(s_player); }
static Nx2Stat* pms(void) { return &g_nx2Opt.m_Stat; }

/* CheckBarricade (CWorldMap.cpp): devolve 0 se a barricada já foi aberta */
static int checkBarricade(int bar)
{
    if (!bar) return bar;
    if (pms()->bWMBarricadeClear[bar - 1]) return 0;
    if (ps()->bWMBarricadeClear[bar - 1]) return 0;
    return bar;
}

static bool streq(const char* a, const char* b) { return _stricmp(a, b) == 0; }
static bool isType(const NxMission* m, const char* utf8) { return streq(m->type, utf8); }
#define T_NORMAL  "\xEC\x9D\xBC\xEB\xB0\x98"                 /* 일반 */
#define T_DUMMY   "\xEB\x8D\x94\xEB\xAF\xB8"                 /* 더미 */
#define T_WARP    "\xEC\x9B\x8C\xED\x94\x84"                 /* 워프 */
#define T_BOSS    "\xEB\xB3\xB4\xEC\x8A\xA4"                 /* 보스 */
#define T_SUBBOSS "\xEC\xA4\x91\xEA\xB0\x84\xEB\xB3\xB4\xEC\x8A\xA4"   /* 중간보스 */
#define T_EVENT   "\xEC\x9D\xB4\xEB\xB2\xA4\xED\x8A\xB8"     /* 이벤트 */

/* ---- texto TTF (CInfoFont / CTTFont do nx2src, com stb_truetype no lugar do FreeType) ----
 * Layout do CTTFont: y = topo da caixa de "size" px, linha de base em y + size; cada glifo em bitmap_left/bitmap_top;
 * avanço = largura + bitmap_left (se <= 0, size / 2); sem kerning. Sombra: cópia preta (alpha 128) a +2,+2. */
typedef struct { unsigned char* data; stbtt_fontinfo info; int size; bool ok; } WFont;
typedef struct { unsigned tex; int w, h; } WLine;
#define WTEXT_LINES 8
#define WTEXT_PAD 16
static WFont s_fBig, s_fSmall, s_fMed;
static WLine s_nameLine, s_descLine[WTEXT_LINES];
static WLine s_objLine[WTEXT_LINES];
static int   s_objLines;
static int   s_descLines;
static char  s_fontFile[16];

static bool wfontLoad(WFont* f, const char* file, int size)
{
    memset(f, 0, sizeof(*f));
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/%s", g_game.currentDirectory, file);
    FILE* fp = fopen(path, "rb");
    if (!fp) { Log_Print("NX2WORLD: fonte %s nao existe\n", path); return false; }
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    f->data = (unsigned char*)malloc((size_t)n);
    if (!f->data || fread(f->data, 1, (size_t)n, fp) != (size_t)n) { fclose(fp); free(f->data); f->data = NULL; return false; }
    fclose(fp);
    if (!stbtt_InitFont(&f->info, f->data, stbtt_GetFontOffsetForIndex(f->data, 0))) { free(f->data); f->data = NULL; return false; }
    f->size = size;
    f->ok = true;
    return true;
}
static void wfontFree(WFont* f) { free(f->data); memset(f, 0, sizeof(*f)); }

static int utf8Next(const unsigned char** pp)
{
    const unsigned char* p = *pp;
    int c;
    if (*p < 0x80) { c = *p; p += 1; }
    else if (*p < 0xE0) { c = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
    else if (*p < 0xF0) { c = ((p[0] & 0xF) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
    else { c = '?'; p += 4; }
    *pp = p;
    return c;
}

static WLine wlineMake(const WFont* f, const char* utf8, int len)
{
    WLine out = { 0, 0, 0 };
    if (!f->ok || !utf8 || len <= 0) return out;
    float sc = stbtt_ScaleForMappingEmToPixels(&f->info, (float)f->size);
    char tmp[1536];
    if (len > (int)sizeof(tmp) - 1) len = (int)sizeof(tmp) - 1;
    memcpy(tmp, utf8, (size_t)len); tmp[len] = 0;
    int total = 0;
    for (const unsigned char* p = (const unsigned char*)tmp; *p; ) {
        int cp = utf8Next(&p), x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(&f->info, cp, sc, sc, &x0, &y0, &x1, &y1);
        int adv = (x1 - x0) + x0;
        total += adv > 0 ? adv : f->size / 2;
    }
    int w = total + WTEXT_PAD * 2, h = f->size + f->size / 2 + 2;
    unsigned char* a = (unsigned char*)calloc((size_t)w * h, 1);
    unsigned char* rgba = (unsigned char*)malloc((size_t)w * h * 4);
    if (!a || !rgba) { free(a); free(rgba); return out; }
    int pen = WTEXT_PAD;
    for (const unsigned char* p = (const unsigned char*)tmp; *p; ) {
        int cp = utf8Next(&p), x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(&f->info, cp, sc, sc, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        int ox = pen + x0, oy = f->size + y0;       /* topo do glifo: linha de base (size) + y0 */
        if (gw > 0 && gh > 0 && ox >= 0 && oy >= 0 && ox + gw <= w && oy + gh <= h)
            stbtt_MakeCodepointBitmap(&f->info, a + (size_t)oy * w + ox, gw, gh, w, sc, sc, cp);
        int adv = gw + x0;
        pen += adv > 0 ? adv : f->size / 2;
    }
    for (int i = 0; i < w * h; i++) { rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255; rgba[i * 4 + 3] = a[i]; }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    free(a); free(rgba);
    out.tex = tex; out.w = w; out.h = h;
    return out;
}
static void wlineFree(WLine* l) { if (l->tex) glDeleteTextures(1, &l->tex); memset(l, 0, sizeof(*l)); }

static void wlineQuad(const WLine* l, float x, float y, float r, float g, float b, float a)
{
    float x0 = x - WTEXT_PAD, yT = 480.0f - y, yB = yT - (float)l->h;
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, l->tex);
    glColor4f(r, g, b, a);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(x0, yT);
    glTexCoord2f(1, 0); glVertex2f(x0 + (float)l->w, yT);
    glTexCoord2f(1, 1); glVertex2f(x0 + (float)l->w, yB);
    glTexCoord2f(0, 1); glVertex2f(x0, yB);
    glEnd();
}
/* DrawTextAt(x, y, shadow = true) */
static void wlineDraw(const WLine* l, int x, int y)
{
    if (!l->tex) return;
    wlineQuad(l, (float)x + 2.f, (float)y + 2.f, 0.f, 0.f, 0.f, 128.f / 255.f);
    wlineQuad(l, (float)x, (float)y, 1.f, 1.f, 1.f, 1.f);
}

static void textClear(void)
{
    wlineFree(&s_nameLine);
    for (int i = 0; i < WTEXT_LINES; i++) { wlineFree(&s_descLine[i]); wlineFree(&s_objLine[i]); }
    s_descLines = 0;
    s_objLines = 0;
}

/* SetMissionInfoWnd (texto): nome (fonte grande, 22) e descrição curta em várias linhas separadas por @ (fonte 14).
 * Idioma: KR descKor, EN descEn, ES descSp; PT usa nxpt.ttf e o campo descCn (no mission.txt o DESC_CN_S está em português). */
static void textSet(NxMission* m)
{
    textClear();
    if (!m) return;
    int lang = g_nx2Opt.g_Language;
    const char* d = NULL;
    switch (lang) {
    case 0: d = m->desc[NXM_LANG_KOR][0]; break;
    case 2: d = m->desc[NXM_LANG_SP][0]; break;
    case 3: d = m->desc[NXM_LANG_CN][0]; break;
    default: d = m->desc[NXM_LANG_EN][0]; break;
    }
    s_nameLine = wlineMake(&s_fBig, m->name, (int)strlen(m->name));
    if (!d) return;
    const char* ptr = d;
    while (s_descLines < WTEXT_LINES) {
        const char* at = strchr(ptr, '@');
        int n = at ? (int)(at - ptr) : (int)strlen(ptr);
        s_descLine[s_descLines++] = wlineMake(&s_fSmall, ptr, n);
        if (!at) break;
        ptr = at + 1;
    }
}

/* API para outras telas (USB_INPUT: posição do WORLD MAX no perfil): linhas de 14 px da nx.ttf, sombra +2,+2 */
static WFont s_fExt;
static WLine s_extLine[8];
void Nx2Ttf_Set(int slot, const char* utf8)
{
    if (slot < 0 || slot >= 8) return;
    wlineFree(&s_extLine[slot]);
    if (!s_fExt.ok && !s_fExt.data) wfontLoad(&s_fExt, "nx.ttf", 14);
    if (utf8 && utf8[0]) s_extLine[slot] = wlineMake(&s_fExt, utf8, (int)strlen(utf8));
}
/* DrawLicenceText: centrada em cx (largura = soma dos avanços) */
void Nx2Ttf_DrawCentered(int slot, int cx, int y)
{
    if (slot < 0 || slot >= 8 || !s_extLine[slot].tex) return;
    int len = s_extLine[slot].w - 2 * WTEXT_PAD;
    wlineDraw(&s_extLine[slot], cx - len / 2, y);
}
void Nx2Ttf_Clear(void) { for (int i = 0; i < 8; i++) wlineFree(&s_extLine[i]); }

/* ---- desenho de peças de sprite ---- */

/* Texture_DrawUV recebe y de cima para baixo (yUp = 480 - y - h). Aqui as contas são em Y para cima (como o S3D do nx2src);
 * yb = borda de baixo do quadro */
static void quadUp(int tex, float x, float yb, float w, float h, float u1, float v1, float u2, float v2, float a)
{
    Texture_DrawUV(tex, x, 480.0f - yb - h, w, h, u1, v1, u2, v2, 1.f, 1.f, 1.f, a);
}

/* tile 'pic' do sprite da camada com a origem em (ox, oy) (Y para cima); o quadro vtx = (srcX, srcY)..(+srcW, +srcH) vai
 * de cima para baixo (S3DScalef(1, -1, 1) do original) */
static void drawTile(const BGALayerSrc* s, int pic, float ox, float oy, float a)
{
    if (!s || !s->isSPR || s->sprTileCount <= 0) return;
    if (pic < 0 || pic >= s->sprTileCount) pic = 0;
    int idx = s->sprTileStart + pic;
    if (idx < 0 || idx >= g_game.sprTileCount) return;
    SPRTileDef* t = &g_game.sprTiles[idx];
    if (t->texId < 0) return;
    int tw = Texture_GetWidth(t->texId); if (tw <= 0) tw = 256;
    int th = Texture_GetHeight(t->texId); if (th <= 0) th = 256;
    quadUp(t->texId, ox + (float)t->srcX, oy - (float)(t->srcY + t->srcH), (float)t->srcW, (float)t->srcH,
           t->u1 * tw, t->v1 * th, t->u2 * tw, t->v2 * th, a);
}

/* DrawSceneAt(x, y, nome): translada em Y para cima e desenha a cena */
static void sceneAt(int bga, float x, float y, const char* name)
{
    glPushMatrix();
    glTranslatef(x, y, 0.0f);
    BGA_ScenePlay(bga, name, true);
    glPopMatrix();
}

/* ---- ladrilhos (CWorldMap::LoadTexture) ---- */
static void loadTiles(void)
{
    memset(s_tex, -1, sizeof(s_tex));
    for (int i = 0; i < ANOTHER_TILE_MAX; i++) { s_alt[i].texId = -1; s_orig[i].texId = -1; }
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/BGA/MAPDATA.DAT", g_game.currentDirectory);
    if (!RES_Open(path)) { Log_Print("NX2WORLD: falha ao abrir BGA/MAPDATA.DAT\n"); return; }
    int row = 0, col = 0, cnt = 1, cnt2 = 0;
    for (;;) {
        char fn[32];
        snprintf(fn, sizeof(fn), "m-%03d.tga", cnt);
        s_tex[row][col] = loadTextureFromRES(fn);
        cnt++;
        col++;
        if (col >= TILE_COL_MAX) { col = 0; row++; }
        snprintf(fn, sizeof(fn), "m-%03d'.tga", cnt);
        int alt = (row < TILE_ROW_MAX) ? loadTextureFromRES(fn) : -1;
        if (alt != -1 && cnt2 < ANOTHER_TILE_MAX) { s_alt[cnt2].row = row; s_alt[cnt2].col = col; s_alt[cnt2].texId = alt; cnt2++; }
        if (row >= TILE_ROW_MAX) break;
    }
    RES_Close();
    /* SetMapTexTable: guarda os ladrilhos originais dos que têm versão "another" */
    for (int i = 0; i < ANOTHER_TILE_MAX; i++) {
        if (s_alt[i].texId != -1) {
            s_orig[i].row = s_alt[i].row; s_orig[i].col = s_alt[i].col;
            s_orig[i].texId = s_tex[s_alt[i].row][s_alt[i].col];
        }
        if (s_alt[i].texId == -1) break;
    }
    int n = 0;
    for (int r = 0; r < TILE_ROW_MAX; r++) for (int c = 0; c < TILE_COL_MAX; c++) if (s_tex[r][c] >= 0) n++;
    Log_Print("NX2WORLD: %d/117 ladrilhos, %d alternativos\n", n, cnt2);
}

static void toggleMapTexture(void)
{
    for (int i = 0; i < ANOTHER_TILE_MAX; i++) {
        if (s_another) { if (s_alt[i].texId != -1) s_tex[s_alt[i].row][s_alt[i].col] = s_alt[i].texId; else break; }
        else           { if (s_orig[i].texId != -1) s_tex[s_orig[i].row][s_orig[i].col] = s_orig[i].texId; else break; }
    }
}

static void drawMapTile(int px, int py, int id)
{
    if (id < 0) return;
    quadUp(id, (float)px, (float)(480 - py - 256), 512.f, 256.f, 0.f, 0.f,
           (float)Texture_GetWidth(id), (float)Texture_GetHeight(id), 1.f);
    if (getenv("NX2WORLD_DBG")) Render_RectOutline((float)px, (float)py, 512.f, 256.f, 255, 0, 0, 255);
}

/* CWorldMap::DrawMapTile() */
static void drawMapTiles(void)
{
    int cy = 64;
    if (s_posX < 0) s_posX = 0;
    if (s_posY < 0) s_posY = 0;
    if (s_posX > 512 * (TILE_COL_MAX - 1) - 128) s_posX = 512 * (TILE_COL_MAX - 1) - 128;
    if (s_posY > 256 * (TILE_ROW_MAX - 1) - cy) s_posY = 256 * (TILE_ROW_MAX - 1) - cy;
    int ci = s_posX / 512, ri = s_posY / 256;
    if (ci > TILE_COL_MAX - 2) ci = TILE_COL_MAX - 2;
    if (ci < 0) ci = 0;
    if (ri > TILE_ROW_MAX - 2) ri = TILE_ROW_MAX - 2;
    if (ri < 0) ri = 0;
    int boX = s_posX % 512, boY = s_posY % 256;
    drawMapTile(0 - boX, 0 - boY, s_tex[ri][ci]);
    drawMapTile(512 - boX, 0 - boY, s_tex[ri][ci + 1]);
    if (ci <= TILE_COL_MAX - 3) drawMapTile(1024 - boX, 0 - boY, s_tex[ri][ci + 2]);
    drawMapTile(0 - boX, 256 - boY, s_tex[ri + 1][ci]);
    drawMapTile(512 - boX, 256 - boY, s_tex[ri + 1][ci + 1]);
    if (ci <= TILE_COL_MAX - 3) drawMapTile(1024 - boX, 256 - boY, s_tex[ri + 1][ci + 2]);
    if (ri <= TILE_ROW_MAX - 3) {
        drawMapTile(0 - boX, 512 - boY, s_tex[ri + 2][ci]);
        drawMapTile(512 - boX, 512 - boY, s_tex[ri + 2][ci + 1]);
        if (ci <= TILE_COL_MAX - 3) drawMapTile(1024 - boX, 512 - boY, s_tex[ri + 2][ci + 2]);
    }
}

/* ---- movimento (CWorldMap::SetMove / MoveMapByPoint / SetStartMission) ---- */
static void setMove(NxMission* t)
{
    if (!t) return;
    int cx = 320, cy = 160;
    s_move = true;
    s_startX = s_posX; s_startY = s_posY;
    s_endX = s_posX - (s_posX + cx - t->x);
    s_endY = s_posY - (s_posY + cy - t->y);
    s_mvCnt = 0;
}
static void moveMapByPoint(void)
{
    if (!s_move) return;
    if (s_mvCnt < 15) {
        float pc = (float)s_mvCnt / 15.0f + 0.1f;
        if (pc >= 1.0f) pc = 1.0f;
        s_posX = s_startX + (int)((float)(s_endX - s_startX) * pc);
        s_posY = s_startY + (int)((float)(s_endY - s_startY) * pc);
        s_mvCnt++;
    } else s_move = false;
}
static void setStartMission(NxMission* m)
{
    if (!m) return;
    s_curM = m;
    m->state = NXM_READY;
    s_posX = m->x - 320;
    s_posY = m->y - 160;
}

/* CWorldMap::SetEnabledConnection: libera as conexões pelo que o perfil já cumpriu */
static void setEnabledConnection(void)
{
    static const struct { int bit, out, in; } pathtbl[4] = {
        { 1, NXM_CONN_LU, NXM_CONN_RL }, { 2, NXM_CONN_LL, NXM_CONN_RU }, { 4, NXM_CONN_RU, NXM_CONN_LL }, { 8, NXM_CONN_RL, NXM_CONN_LU },
    };
    for (int i = 0; i < NxMission_CurCount(); i++) {
        NxMission* m = NxMission_CurAt(i);
        m->cleared = false;
        for (int b = 0; b < 4; b++) m->conn[b].enabled = false;
    }
    for (int i = 0; i < NxMission_CurCount(); i++) {
        NxMission* m = NxMission_CurAt(i);
        int trackNum = 0;
        sscanf(&m->step[2], "%3d", &trackNum);
        if (trackNum < 0 || trackNum >= NX2_MISSION_MAX) continue;
        int bm = ps()->bmWMClear[trackNum] | pms()->bmWMClear[trackNum];
        for (int a = 0; a < 4; a++) {
            if ((bm & pathtbl[a].bit) || (ps()->iWMChallenge[trackNum] >= m->challenge[pathtbl[a].out])) {
                m->cleared = (ps()->bmWMClear[trackNum] | pms()->bmWMClear[trackNum]) ? true : false;
                NxMission* c = m->next[pathtbl[a].out];
                if (c) {
                    m->show = true;
                    m->conn[pathtbl[a].out].enabled = true;
                    c->show = true;
                    c->conn[pathtbl[a].in].enabled = true;
                }
            }
        }
    }
}

static int isPossible(int dir)
{
    if (checkBarricade(s_curM->conn[dir].lock)) return E_LOCKED;
    return s_curM->conn[dir].enabled ? E_CONNECTED : E_DISABLE;
}

static NxMission* getNext(int dir)
{
    NxMission* n = s_curM ? s_curM->next[dir] : NULL;
    if (!n) return NULL;
    NxMission* p = s_curM;
    s_curM = n;
    p->state = NXM_NONE;
    s_curM->state = NXM_READY;
    setMove(s_curM);
    return s_curM;
}

/* ---- pontos, setas e linhas ---- */
static void drawDirection(float px, float py, NxMission* m)
{
    static int cnt = 0, picIdx = 0;
    for (int i = 0; i < 4; i++) {
        const BGALayerSrc* spr = (i % 2) ? &s_arrow[0] : &s_arrow[1];   /* pSpr = i%2 ? m_pArrowBlue : m_pArrowRed */
        if (m->next[i] && m->conn[i].enabled && !checkBarricade(m->conn[i].lock)) {
            float x = (float)(m->next[i]->x - m->x), y = (float)(m->next[i]->y - m->y);
            float len = sqrtf(x * x + y * y);
            float rot = acosf(x / len) / (3.14159f / 180.0f);
            if (y > 0) rot = 360.f - rot;
            glPushMatrix();
            glTranslatef(px, 480.f - py, 0.f);
            rot -= 90;
            glRotatef(rot, 0, 0, 1);
            glTranslatef(-16.f, 32.f + 32.f, 0.f);
            drawTile(spr, picIdx, 0.f, 0.f, s_alpha);
            glPopMatrix();
        }
    }
    if (cnt > 10) { cnt = 0; picIdx++; if (picIdx > 2) picIdx = 0; }
    cnt++;
}

static void drawIcePoint(float px, float py, NxMission* m, int state)
{
    static int cnt = 0, picIdx = 0;
    if (!s_move && state == NXM_READY)    { drawDirection(px, py, m); sceneAt(B_UI, px, -py, "selet point"); }
    if (!s_move && state == NXM_SELECTED) { drawDirection(px, py, m); sceneAt(B_UI, px, -py, "selet point on"); }
    drawTile(&s_pt[m->cleared ? 1 : 0], picIdx, px - 16.f, 480.f - py + 45.f, s_alpha);   /* (ícone de gelo: fase 2b) */
    if (cnt > 30) { cnt = 0; picIdx++; if (picIdx > 2) picIdx = 0; }
    cnt++;
}

static void drawMissionPoint(float px, float py, NxMission* m, int state)
{
    if (isType(m, T_NORMAL) || isType(m, T_DUMMY)) {
        float a = isType(m, T_DUMMY) ? 0.5f : 1.0f;
        drawTile(&s_pt[m->cleared ? 1 : 0], 0, px - 13.f, 480.f - py + 13.f, a * s_alpha);
        if (!s_move && state == NXM_READY)    { drawDirection(px, py, m); sceneAt(B_UI, px, -py, "selet point"); }
        if (!s_move && state == NXM_SELECTED) { drawDirection(px, py, m); sceneAt(B_UI, px, -py, "selet point on"); }
        return;
    }
    char buf[64];
    if (isType(m, T_BOSS)) {
        snprintf(buf, sizeof(buf), "%s%s", m->scene, m->cleared ? "c" : "");
        sceneAt(B_ICON, px, -py, buf);
    } else if (isType(m, T_EVENT)) {
        if (streq(m->land, "Harena")) {
            snprintf(buf, sizeof(buf), "%s%s", m->scene, m->cleared ? "_on" : "_off");
            sceneAt(B_ICON, px, -py, buf);
        } else sceneAt(B_ICON, px, -py, m->scene);
    } else sceneAt(B_ICON, px, -py, m->scene);

    if (!s_move && state == NXM_READY) {
        drawDirection(px, py, m);
        sceneAt(B_UI, px, -py, streq(m->step, "EF999") ? "selet point" : isType(m, T_WARP) ? "select warp" : "select boss");
    } else if (!s_move && state == NXM_SELECTED) {
        drawDirection(px, py, m);
        sceneAt(B_UI, px, -py, streq(m->step, "EF999") ? "selet point on" : isType(m, T_WARP) ? "select warp on" : "select boss on");
    }
}

static void drawConnectionLine(NxMission* o, NxMission* d, int connID)
{
    if (!o || !d) return;
    float x = (float)(d->x - o->x), y = (float)(d->y - o->y);
    float len = sqrtf(x * x + y * y);
    float rot = acosf(x / len) / (3.14159f / 180.0f);
    if (y > 0) rot = 360.f - rot;
    if (!o->conn[connID].enabled) return;
    if (s_line.isSPR && s_line.sprTileCount > 0) {
        SPRTileDef* t = &g_game.sprTiles[s_line.sprTileStart];
        if (t->texId >= 0) {
            int tw = Texture_GetWidth(t->texId); if (tw <= 0) tw = 256;
            int th = Texture_GetHeight(t->texId); if (th <= 0) th = 256;
            glPushMatrix();
            glTranslatef((float)(o->x - s_posX), 480.f - (float)o->y + (float)s_posY, 0.f);
            glRotatef(rot, 0, 0, 1);
            glTranslatef(0.f, 2.5f, 0.f);
            /* TexRect(rcTex0, rcTex1, rcVtx0, rcVtx1, rcTex2, rcTex3, len, rcVtx3) com Y invertido */
            quadUp(t->texId, (float)t->srcX, -(float)(t->srcY + t->srcH), len - (float)t->srcX, (float)t->srcH,
                   t->u1 * tw, t->v1 * th, t->u2 * tw, t->v2 * th, 1.f);
            glPopMatrix();
        }
    }
    int idx = checkBarricade(o->conn[connID].lock);
    if (idx > 0 && idx <= 12)
        drawTile(&s_lock[idx - 1], 0, (float)(o->x - s_posX) - 23.f + (int)(x / 2), 480.f - (float)o->y + (float)s_posY + 23.f - (int)(y / 2), 1.f);
}

static void drawWorldMap(void)
{
    drawMapTiles();
    /* DrawMissionConnections: só as de id[2] maior para o filho (cada linha uma vez) */
    for (int i = 0; i < NxMission_CurCount(); i++) {
        NxMission* m = NxMission_CurAt(i);
        for (int d = 0; d < 4; d++)
            if (m->next[d] && m->id[2] > m->next[d]->id[2]) drawConnectionLine(m, m->next[d], d);
    }
    for (int i = 0; i < NxMission_CurCount(); i++) {   /* DrawMissionPoints */
        NxMission* m = NxMission_CurAt(i);
        if (!m->show) continue;
        float px = (float)(m->x - s_posX), py = (float)(m->y - s_posY);
        if (m->iced) drawIcePoint(px, py, m, m->state);
        else drawMissionPoint(px, py, m, m->state);
    }
}

/* ---- janela de informação, tempo, vidas ---- */
static void setMissionInfoWnd(NxMission* m)
{
    if (!m) return;
    for (int i = 0; i < 9; i++)
        if (!strcmp(m->land, k_landList[i])) {
            BGA_SetLayerSrc(B_W, 21, &s_landSpr[i]);
            BGA_SetLayerSrc(B_W, 22, &s_landSpr[i]);
            break;
        }
    int player = m->players - 1; if (player < 0) player = 0; if (player > 2) player = 2;
    int lv = m->level - 1; if (lv < 0) lv = 0; if (lv > 4) lv = 4;
    BGA_SetLayerSrc(B_W, 10, &s_breakSpr[m->nxBreak ? 1 : 0]);
    BGA_SetLayerSrc(B_W, 11, &s_playerSpr[player]);
    BGA_SetLayerSrc(B_W, 9, &s_levelSpr[lv]);
    BGA_SetLayerSrc(B_W, 8, &s_switchSpr[s_switch ? 1 : 0]);
    BGA_SceneReset(B_W, "life 3_1"); BGA_SceneReset(B_W, "life 3_2"); BGA_SceneReset(B_W, "life 3_3");   /* SetLifeGage */
    textSet(m);
}

static void drawTime(int t)
{
    if (t > 99) t = 99;
    if (t < 0) t = 0;
    BGA_SetLayerSrc(B_W, 35, &s_timeNum[t % 10]); t /= 10;
    BGA_SetLayerSrc(B_W, 34, &s_timeNum[t % 10]);
}

static void drawLifeGage(void)
{
    static const char* const k_on[3] = { "life_1", "life_2", "life_3" };
    static const char* const k_blk[3] = { "life 3_1", "life 3_2", "life 3_3" };
    int need = s_curM->life, cur = s_worldLife;
    if (s_move) {
        for (int i = 0; i < cur && i < 3; i++) BGA_ScenePlay(B_W, k_on[i], true);
        return;
    }
    if (cur - need < 0) { BGA_ScenePlay(B_W, "alram", true); return; }
    for (int i = 0; i < cur - need && i < 3; i++) BGA_ScenePlay(B_W, k_on[i], true);
    int pos = (cur == 3) ? 0 : (cur == 2) ? 1 : 2;
    for (int i = 0; i < need && pos + i < 3; i++) BGA_ScenePlay(B_W, k_blk[pos + i], true);
}

static void drawMiniMapPoint(int px, int py)
{
    float x = (float)px * 0.0423f, y = (float)py * 0.0423f;
    sceneAt(B_W, 430.f + x, -286.f - y, "arrow hold");
}

/* ---- BGM por região ---- */
static void stopBgm(void) { if (s_bgm >= 0 && s_snd[s_bgm] >= 0) Audio_Stop(s_snd[s_bgm]); s_bgm = -1; }
static void playBgmFor(const char* land)
{
    int want = -1;
    if (_stricmp(land, "Prue Ocean") != 0) {                 /* Prue Ocean usa a música de Rootinia (EFF_R_BGM) */
        for (size_t a = 0; a < sizeof(k_bg) / sizeof(k_bg[0]); a++) if (streq(land, k_bg[a].land)) { want = k_bg[a].bgm; break; }
    } else want = WS_R;
    if (want < 0) return;
    stopBgm();
    s_bgm = want;
    snd(s_bgm, true);
}
static void setBgm(void)
{
    if (_stricmp(s_prevM->land, s_curM->land) == 0) return;
    if (_stricmp(s_curM->land, "Prue Ocean") != 0) {
        for (size_t a = 0; a < sizeof(k_bg) / sizeof(k_bg[0]); a++)
            if (streq(s_curM->land, k_bg[a].land)) { stopBgm(); s_bgm = k_bg[a].bgm; snd(s_bgm, true); }
    }
}

/* CMission::setPointSprite: nome da cena do ícone de cada tipo (os normais usam os sprites 61/62 do WORLD_IN) */
static void setPointScenes(void)
{
    for (int i = 0; i < NxMission_Count(); i++) {
        NxMission* m = NxMission_At(i);
        if (isType(m, T_NORMAL) || isType(m, T_DUMMY)) continue;
        char b[32];
        memset(b, 0, sizeof(b));
        strncpy(b, m->land, sizeof(b) - 1);
        if (isType(m, T_WARP)) strncat(b, "_w", sizeof(b) - strlen(b) - 1);
        else if (isType(m, T_BOSS)) strncat(b, streq(m->step, "EF085") ? "Off_fb" : "_fb", sizeof(b) - strlen(b) - 1);
        else if (isType(m, T_SUBBOSS)) strncat(b, "_sb", sizeof(b) - strlen(b) - 1);
        else if (isType(m, T_EVENT)) strncat(b, "_e", sizeof(b) - strlen(b) - 1);
        strncpy(m->scene, b, sizeof(m->scene) - 1);
    }
}

/* ---- entrada ---- */
static const struct { int btn; int dir; } k_move[4] = { { PAD_DL, NXM_CONN_LL }, { PAD_UL, NXM_CONN_LU }, { PAD_DR, NXM_CONN_RL }, { PAD_UR, NXM_CONN_RU } };

static void startEnd(void)
{
    if (g_nx2HasProfile[s_player] && s_curM) {          /* select_mission.cpp: ps->szLand / szMission = onde o perfil ficou */
        memset(ps()->szLand, 0, sizeof(ps()->szLand)); memset(ps()->szMission, 0, sizeof(ps()->szMission));
        strncpy(ps()->szLand, s_curM->land, sizeof(ps()->szLand) - 1);
        strncpy(ps()->szMission, s_curM->name, sizeof(ps()->szMission) - 1);
    }
    s_bEnd = true;
    s_time = 0;
    s_cnt = 0;
    s_phase = 2;
    snd(WS_START, false);
}

/* teste (NX2WORLD_KEYS=1795...): uma tecla do roteiro a cada 40 quadros; 1 DL, 7 UL, 9 UR, 3 DR, 5 centro */
static int s_dbgFrame;
static bool padHit(int p, PadButton b)
{
    if (Input_IsPadHit(p, b)) return true;
    const char* k = getenv("NX2WORLD_KEYS");
    if (!k) return false;
    int n = (int)strlen(k);
    if (s_dbgFrame < 40 || s_dbgFrame % 40 != 0 || s_dbgFrame / 40 > n) return false;
    char c = k[s_dbgFrame / 40 - 1];
    return (c == '1' && b == PAD_DL) || (c == '7' && b == PAD_UL) || (c == '9' && b == PAD_UR) || (c == '3' && b == PAD_DR) || (c == '5' && b == PAD_C);
}

static void processInputInMap(int p)
{
    s_dbgFrame++;
    for (int i = 0; i < 4; i++) {
        if (!padHit(p, (PadButton)k_move[i].btn)) continue;
        int dir = k_move[i].dir;
        if (isPossible(dir) == E_CONNECTED) {
            s_prevM = s_curM;
            if (getNext(dir)) { s_wasMoving = true; s_selected = false; snd(WS_MOVE, false); }
            setBgm();
        } else snd(WS_NOMOVE, false);
    }
    if (padHit(p, PAD_C)) {
        if (!s_selected) {
            s_selected = true;
            if (s_worldLife - s_curM->life < 0) { s_selected = false; snd(WS_NOMOVE, false); }
            else {
                s_curM->state = NXM_SELECTED;
                snd(WS_CENTER, false);
                /* warp, switch (EF999) e caixa de missão (evento): fase 2b */
                if (isType(s_curM, T_WARP) || streq(s_curM->step, "EF999") || isType(s_curM, T_EVENT)) {
                    s_selected = false;
                    s_curM->state = NXM_READY;
                    Log_Print("NX2WORLD: missão '%s' (%s) é warp/switch/evento: ainda não portado\n", s_curM->step, s_curM->type);
                }
            }
        } else {
            startEnd();   /* confirma a missão */
        }
    }
}

/* ---------------------------------------------------------------------------
 * W_BRIEFING (worldmaxbriefing.cpp) e lançamento da missão (PlayEngine "RUN <hex> <modo> <comando> <condições>")
 * ------------------------------------------------------------------------- */
enum { PH_BRIEF_START = 3, PH_BRIEF_RUN = 4, PH_BRIEF_END = 5 };
enum { BA = 0, BB = 1, BC = 2, BD = 3 };      /* WORLD_ITEM, WORLD_ITEM_BG, USER_COMMAND, WORLD_IN */
#define BRIEF_TIME 20

bool g_nx2WorldBreak = true;
bool g_nx2WGLottery;
int Nx2World_Player(void) { return s_player; }
const NxMission* Nx2World_CurMission(void) { return s_curM; }
int Nx2World_LivesLeft(void) { return s_worldLife; }
static char s_runMode[8], s_runCmd[128];
static unsigned s_runTrack;
static BGALayerSrc s_bTime[10], s_cmdSpr[8], s_cmdBase[8];
static int  s_cmdType[8];                  /* CMD_TYPE_SPEED = 0, CMD_TYPE_MODE = 1 */
static bool s_cmdOn[8];                    /* m_bCommand: ícone aceso ("in_iconNN") ou apagado ("out_iconNN") */
/* itens do briefing (só com pendrive e missão com ITEM): velocidade (UL), vida (UR), modo (LL), loteria (LR) */
enum { AR_IN = 0, AR_OUT, AR_NONE, AR_CLK, AR_ROLLBACK };
enum { AU_UL = 0, AU_UR, AU_LL, AU_LR };
#define SPEED_MILEAGE 45
#define MODE_MILEAGE  145
static const int k_lifeMileage[4] = { 60, 30, 60, 120 };
static const int k_lifeRollback[4] = { 60, 90, 150, 270 };
static const char* const k_arrIn[4]  = { "in_nw", "in_ne", "in_sw", "in_se" };
static const char* const k_arrOut[4] = { "out_nw", "out_ne", "out_sw", "out_se" };
static const char* const k_arrClk[4] = { "click_nw", "click_ne", "click_sw", "click_se" };
static const char* s_arrCur[4];
static int  s_arrState[4];
static int  s_curSpeed, s_preSpeed, s_curLife, s_preLife, s_rbCnt;
static bool s_modeOff, s_lottery;
static int  s_totalMil, s_useMil, s_lotMil;
static BGALayerSrc s_spdSpr[5], s_lifeSpr[5], s_modeSpr[2], s_lotSpr[2], s_milA[10], s_milB[10];
int g_nx2WGStart, g_nx2WGDisp, g_nx2WGLimit, g_nx2WGBonus;   /* gauge da missão: @ % # e o bônus da vida comprada (0/900/1500/2700/5100) */
static int  s_cmdMax, s_bTimeLeft, s_bCnt, s_bFrames;
static bool s_bSel, s_bUsb, s_bItem, s_bLaunched;
static double s_bElapsed;
static int  s_bSnd[4] = { -1, -1, -1, -1 };

/* commandLayer.h */
enum { L_2X = 15, L_3X, L_4X, L_8X, L_AC, L_DC, L_EW, L_RV, L_ZERO_EZ, L_V, L_NS, L_FD, L_FL, L_NX_CEL, L_OLD, L_HWATOO, L_CANON,
       L_CARD, L_SHEEP, L_HORSE, L_DOG, L_GIRL, L_EXCEED = 38, L_NX, L_MIRROR, L_UA, L_RS = 43, L_RG, L_05X = 46, L_1X, L_5X, L_6X,
       L_DROP = 69, L_SNAKE, L_APPEAR, L_SINK, L_RISE, L_HARDJ, L_RNDSKIN };
/* CmdParamTable (playengine.cpp:580) e o sprite de cada letra (worldmaxbriefing.cpp ParseArgument) */
static const char k_cmdParam[] = "vnwfmruj" "dax^!" "bgt$" "()pzhi*";
static const int  k_cmdLayer[24] = { L_V, L_NS, L_FL, L_FD, L_MIRROR, L_RS, L_UA, L_RG, L_DC, L_AC, L_EXCEED, L_NX, L_DROP,
                                      -1, -1, -1, -1, L_SINK, L_RISE, L_APPEAR, L_SNAKE, L_HARDJ, L_RNDSKIN, -1 };

/* ParseArgument: um ícone por comando; velocidade vira ícone pela soma dos dígitos */
static void briefingParseIcons(const char* tok)
{
    int idx = 0, speed = 0;
    memset(s_cmdSpr, 0, sizeof(s_cmdSpr));
    s_cmdMax = 0;
    int ctype[8] = { 0 };
    bool have[8] = { false };
    for (int b = 0; tok[b]; b++) {
        char c = (char)tolower((unsigned char)tok[b]);
        if (isdigit((unsigned char)c)) { speed += c - '0'; continue; }
        if (c == 'k') { b++; continue; }
        if (c == 's' || c == 'e') {
            if (idx < 8 && BGA_GetLayerSrc(BC, c == 's' ? L_RV : L_EW, &s_cmdSpr[idx])) { ctype[idx] = 0; have[idx++] = true; }
            continue;
        }
        if (c == '#' || c == '%' || c == '@') { b++; while (isdigit((unsigned char)tok[b])) b++; b--; continue; }
        const char* f = strchr(k_cmdParam, c);
        if (f && *f) {
            int i = (int)(f - k_cmdParam);
            if (k_cmdLayer[i] != -1 && idx < 8 && BGA_GetLayerSrc(BC, k_cmdLayer[i], &s_cmdSpr[idx])) {
                ctype[idx] = (c == 'a' || c == 'd') ? 0 : 1;      /* AC e DC contam como velocidade (CMD_TYPE_SPEED) */
                have[idx++] = true;
            }
        }
    }
    static const int k_spd[6] = { 8, 6, 5, 4, 3, 2 }, k_spdIcon[6] = { L_8X, L_6X, L_5X, L_4X, L_3X, L_2X };
    for (int a = 0; a < 6; a++)
        if (speed >= k_spd[a] * 4) { if (idx < 8 && BGA_GetLayerSrc(BC, k_spdIcon[a], &s_cmdSpr[idx])) { ctype[idx] = 0; have[idx] = true; } break; }
    if (speed == 2 && idx < 8 && BGA_GetLayerSrc(BC, L_05X, &s_cmdSpr[idx])) { ctype[idx] = 0; have[idx] = true; }
    /* os ícones ficam compactados na ordem em que entraram */
    BGALayerSrc tmp[8]; int n = 0;
    memset(tmp, 0, sizeof(tmp));
    for (int i = 0; i < 8; i++) if (have[i]) { tmp[n] = s_cmdSpr[i]; s_cmdType[n] = ctype[i]; s_cmdOn[n] = true; n++; }
    memcpy(s_cmdSpr, tmp, sizeof(tmp));
    memcpy(s_cmdBase, tmp, sizeof(tmp));
    s_cmdMax = n;
}

static void briefingDrawTime(int t)
{
    if (t < 0) t = 0;
    if (t > 99) t = 99;
    BGA_SetLayerSrc(BA, 3, &s_bTime[t % 10]); t /= 10;
    BGA_SetLayerSrc(BA, 2, &s_bTime[t % 10]);
}

/* ---- itens do briefing (worldmaxbriefing.cpp) ---- */
static int wrap5(int i) { while (i > 4) i -= 5; while (i < 0) i += 5; return i; }

static void arrowSetSprites(int kind, int state)
{
    const BGALayerSrc* spr = (kind == AU_UL) ? s_spdSpr : s_lifeSpr;
    int cur = (kind == AU_UL) ? s_curSpeed : s_curLife;
    if (kind == AU_UL || kind == AU_UR) {
        if (state == AR_IN || state == AR_OUT) {
            static const int k_slot[2][5] = { { 18, 19, 21, 17, 16 }, { 26, 27, 29, 25, 24 } };
            const int* sl = k_slot[kind == AU_UL ? 0 : 1];
            BGA_SetLayerSrc(BA, sl[0], &spr[wrap5(cur + 2)]); BGA_SetLayerSrc(BA, sl[1], &spr[wrap5(cur + 1)]);
            BGA_SetLayerSrc(BA, sl[2], &spr[wrap5(cur)]);     BGA_SetLayerSrc(BA, sl[3], &spr[wrap5(cur - 1)]);
            BGA_SetLayerSrc(BA, sl[4], &spr[wrap5(cur - 2)]);
        } else {
            static const int k_slot[2][8] = { { 34, 37, 38, 43, 42, 39, 36, 35 }, { 45, 48, 49, 54, 53, 50, 47, 46 } };
            static const int k_off[8] = { 2, 1, 0, 0, -1, -1, -2, -3 };
            const int* sl = k_slot[kind == AU_UL ? 0 : 1];
            for (int i = 0; i < 8; i++) BGA_SetLayerSrc(BA, sl[i], &spr[wrap5(cur + k_off[i])]);
        }
    } else if (kind == AU_LL) {
        if (state == AR_IN || state == AR_OUT) BGA_SetLayerSrc(BA, 31, &s_modeSpr[s_modeOff ? 1 : 0]);
        else { BGA_SetLayerSrc(BA, 57, &s_modeSpr[s_modeOff ? 1 : 0]); BGA_SetLayerSrc(BA, 58, &s_modeSpr[s_modeOff ? 1 : 0]); }
    } else {
        if (state == AR_IN || state == AR_OUT) BGA_SetLayerSrc(BA, 32, &s_lotSpr[s_lottery ? 1 : 0]);
        else { BGA_SetLayerSrc(BA, 63, &s_lotSpr[s_lottery ? 1 : 0]); BGA_SetLayerSrc(BA, 64, &s_lotSpr[s_lottery ? 1 : 0]); }
    }
}
static void reorganize(int kind, int state) { s_arrState[kind] = state; arrowSetSprites(kind, state); }

static void setTotalMileage(int v)
{
    if (v > 9999999) v = 9999999;
    for (int i = 0; i < 7; i++) { BGA_SetLayerSrc(BA, 10 - i, &s_milA[v % 10]); v /= 10; }
}
static void setUseMileage(int v)
{
    for (int i = 0; i < 4; i++) { BGA_SetLayerSrc(BA, 14 - i, &s_milB[v % 10]); v /= 10; }
}

static void updateCmdSpr(void)
{
    static const int k_spdLayer[4] = { L_1X, L_2X, L_3X, L_4X };
    if (s_curSpeed != 0) {
        for (int i = 0; i < 8; i++) if (s_cmdType[i] == 0) s_cmdOn[i] = false;
        for (int i = 0; i < 8; i++)
            if (s_cmdType[i] == 0 && i < s_cmdMax) { s_cmdOn[i] = true; BGA_GetLayerSrc(BC, k_spdLayer[s_curSpeed - 1], &s_cmdSpr[i]); break; }
    } else {
        for (int i = 0; i < s_cmdMax; i++) if (s_cmdType[i] == 0) { s_cmdOn[i] = true; s_cmdSpr[i] = s_cmdBase[i]; }
    }
    for (int i = 0; i < s_cmdMax; i++) BGA_SetLayerSrc(BB, 3 + i * 2, &s_cmdSpr[i]);
    for (int i = 0; i < s_cmdMax; i++) if (s_cmdType[i] == 1) s_cmdOn[i] = !s_modeOff;
}

static void arrowTo(int kind, const char* scene, int state)
{
    s_arrCur[kind] = scene;
    reorganize(kind, state);
    BGA_SceneReset(BA, scene);
}

static void updateArrowEnable(void)
{
    int left = s_totalMil - s_useMil;
    if (s_arrState[AU_UL] != AR_OUT && s_curSpeed == 0 && left < SPEED_MILEAGE) arrowTo(AU_UL, k_arrOut[AU_UL], AR_OUT);
    else if (s_arrState[AU_UL] == AR_OUT && left >= SPEED_MILEAGE) arrowTo(AU_UL, k_arrIn[AU_UL], AR_IN);

    if (s_arrState[AU_UR] != AR_OUT && s_curLife == 0 && left < k_lifeMileage[0]) arrowTo(AU_UR, k_arrOut[AU_UR], AR_OUT);
    else if (s_arrState[AU_UR] == AR_OUT && left >= k_lifeMileage[0]) arrowTo(AU_UR, k_arrIn[AU_UR], AR_IN);

    if (s_arrState[AU_LL] != AR_OUT && !s_modeOff && left < MODE_MILEAGE) arrowTo(AU_LL, k_arrOut[AU_LL], AR_OUT);
    else if (s_arrState[AU_LL] == AR_OUT && left >= MODE_MILEAGE) arrowTo(AU_LL, k_arrIn[AU_LL], AR_IN);

    if (s_arrState[AU_LR] != AR_OUT && !s_lottery && left < s_lotMil) arrowTo(AU_LR, k_arrOut[AU_LR], AR_OUT);
    else if (s_arrState[AU_LR] == AR_OUT && left >= s_lotMil) arrowTo(AU_LR, k_arrIn[AU_LR], AR_IN);
}

/* UpdateUseMileage: false = a vida pedida não cabe (rollback) */
static bool updateUseMileage(int kind)
{
    switch (kind) {
    case AU_UL:
        if (s_preSpeed == 0) s_useMil += SPEED_MILEAGE;
        else if (s_curSpeed == 0) s_useMil -= SPEED_MILEAGE;
        break;
    case AU_UR:
        if (s_curLife != 0) {
            if (s_useMil + k_lifeMileage[s_curLife - 1] <= s_totalMil) s_useMil += k_lifeMileage[s_curLife - 1];
            else {
                if (s_preLife > 0) s_useMil -= k_lifeRollback[s_preLife - 1];
                setUseMileage(s_useMil);
                updateArrowEnable();
                return false;
            }
        } else if (s_preLife > 0) s_useMil -= k_lifeRollback[s_preLife - 1];
        break;
    case AU_LL: s_useMil += s_modeOff ? MODE_MILEAGE : -MODE_MILEAGE; break;
    case AU_LR: s_useMil += s_lottery ? s_lotMil : -s_lotMil; break;
    }
    if (s_useMil < 0) s_useMil = 0;
    setUseMileage(s_useMil);
    updateArrowEnable();
    return true;
}

static void updateArrowState(void)
{
    if (s_arrState[AU_UL] == AR_IN && BGA_SceneDone(BA, s_arrCur[AU_UL])) { s_arrCur[AU_UL] = k_arrClk[AU_UL]; reorganize(AU_UL, AR_CLK); }
    if (s_arrState[AU_UR] == AR_ROLLBACK) {
        if (s_rbCnt > 9) {
            s_rbCnt = 0;
            s_curLife = wrap5(s_curLife + 1);
            s_arrCur[AU_UR] = k_arrClk[AU_UR];
            BGA_SceneReset(BA, s_arrCur[AU_UR]);
            reorganize(AU_UR, AR_ROLLBACK);
            if (s_curLife == 0) s_arrState[AU_UR] = AR_CLK;
        }
        s_rbCnt++;
    } else if (s_arrState[AU_UR] == AR_IN && BGA_SceneDone(BA, s_arrCur[AU_UR])) { s_arrCur[AU_UR] = k_arrClk[AU_UR]; reorganize(AU_UR, AR_CLK); }
    if (s_arrState[AU_LL] == AR_IN && BGA_SceneDone(BA, s_arrCur[AU_LL])) { s_arrCur[AU_LL] = k_arrClk[AU_LL]; reorganize(AU_LL, AR_CLK); }
    if (s_arrState[AU_LR] == AR_IN && BGA_SceneDone(BA, s_arrCur[AU_LR])) { s_arrCur[AU_LR] = k_arrClk[AU_LR]; reorganize(AU_LR, AR_CLK); }
}

static void briefingBegin(void)
{
    stopBgm();            /* a música da região para ao entrar na missão */
    Resource_ClearBGA();
    static const char* const k_bga[4] = { "WORLD_ITEM", "WORLD_ITEM_BG", "USER_COMMAND", "WORLD_IN" };
    bool ok = true;
    for (int i = 0; i < 4; i++) if (!Resource_LoadBGAByName(k_bga[i])) { Log_Print("NX2WORLD: falha ao carregar BGA\\%s.DAT\n", k_bga[i]); ok = false; }
    BGA_Reset();
    textClear();
    s_bLaunched = false;
    if (!ok) { s_phase = PH_BRIEF_END; s_bCnt = 30; return; }
    for (int i = 0; i < 10; i++) BGA_GetLayerSrc(BB, 22 + i, &s_bTime[i]);
    briefingParseIcons(s_runCmd);
    for (int i = 0; i < s_cmdMax; i++) BGA_SetLayerSrc(BB, 3 + i * 2, &s_cmdSpr[i]);
    s_bUsb = g_nx2HasProfile[s_player] && Nx2Profile_CheckUsb(s_player);
    s_bItem = s_curM->item;
    s_bSel = !(s_bUsb && s_bItem);                    /* sem pendrive ou sem itens: já selecionado (step_se) */
    if (s_bSnd[0] < 0) { s_bSnd[0] = Audio_LoadWaveFile("3-2.WAV"); s_bSnd[1] = Audio_LoadWaveFile("A_SELECT.WAV"); s_bSnd[2] = Audio_LoadWaveFile("S_CLK.WAV"); }
    /* itens: sprites das setas (WORLD_ITEM), estado e mileage do perfil */
    s_curSpeed = s_preSpeed = s_curLife = s_preLife = s_rbCnt = 0;
    s_modeOff = s_lottery = false;
    s_useMil = 0;
    s_totalMil = g_nx2HasProfile[s_player] ? g_nx2Prof[s_player].iMileage : 0;
    if (s_totalMil < 0) s_totalMil = 0;
    if (getenv("NX2WORLD_MILEAGE")) s_totalMil = atoi(getenv("NX2WORLD_MILEAGE"));   /* teste */
    s_lotMil = s_curM->mileage;
    g_nx2WGBonus = 0;
    for (int i = 1; i < 5; i++) { BGA_GetLayerSrc(BA, 66 + i, &s_spdSpr[i]); BGA_GetLayerSrc(BA, 75 + i, &s_lifeSpr[i]); }
    BGA_GetLayerSrc(BA, 71, &s_spdSpr[0]); BGA_GetLayerSrc(BA, 71, &s_lifeSpr[0]);
    BGA_GetLayerSrc(BA, 74, &s_modeSpr[0]); BGA_GetLayerSrc(BA, 75, &s_modeSpr[1]);
    BGA_GetLayerSrc(BA, 72, &s_lotSpr[0]); BGA_GetLayerSrc(BA, 73, &s_lotSpr[1]);
    for (int i = 0; i < 10; i++) { BGA_GetLayerSrc(BA, 80 + i, &s_milA[i]); BGA_GetLayerSrc(BA, 90 + i, &s_milB[i]); }
    {
        static const char* const k_cl[4] = { "click_nw", "click_ne", "click_sw", "click_se" };
        for (int i = 0; i < 4; i++) { BGA_SceneFinish(BA, k_cl[i]); s_arrCur[i] = k_arrIn[i]; }   /* SetSceneTick(click, 29) */
    }
    for (int i = 0; i < 4; i++) reorganize(i, AR_IN);
    setTotalMileage(s_totalMil);
    setUseMileage(s_useMil);
    s_bElapsed = 0.0; s_bTimeLeft = BRIEF_TIME; s_bCnt = 0; s_bFrames = 0;
    briefingDrawTime(s_bTimeLeft);
    /* objetivo completo (DESC_*_F), fonte de 18 px: 70,180 e 24 de espaçamento */
    {
        const char* ff = (g_nx2Opt.g_Language == 3) ? "nxpt.ttf" : "nx.ttf";
        wfontFree(&s_fMed); wfontLoad(&s_fMed, ff, 18);
        int lang = g_nx2Opt.g_Language;
        const char* d = (lang == 0) ? s_curM->desc[NXM_LANG_KOR][1] : (lang == 2) ? s_curM->desc[NXM_LANG_SP][1]
                      : (lang == 3) ? s_curM->desc[NXM_LANG_CN][1] : s_curM->desc[NXM_LANG_EN][1];
        const char* ptr = d ? d : "";
        while (s_objLines < WTEXT_LINES && *ptr) {
            const char* at = strchr(ptr, '@');
            int n = at ? (int)(at - ptr) : (int)strlen(ptr);
            s_objLine[s_objLines++] = wlineMake(&s_fMed, ptr, n);
            if (!at) break;
            ptr = at + 1;
        }
    }
    s_phase = PH_BRIEF_START;
    Log_Print("NX2WORLD: briefing %x (%s) ícones %d, objetivo %d linhas, tempo %d, selecionado %d\n", s_runTrack, s_runMode, s_cmdMax, s_objLines, s_bTimeLeft, s_bSel ? 1 : 0);
}

/* "RUN": letra do modo -> índice 0..4 (N H C D M); mods do comando -> g_nxMods e extras do NX2 */
static void briefingLaunch(void)
{
    if (s_bLaunched) return;
    s_bLaunched = true;
    textClear();
    int diff = -1;
    switch (tolower((unsigned char)s_runMode[1])) { case 'n': diff = 0; break; case 'h': diff = 1; break; case 'c': diff = 2; break; case 'd': diff = 3; break; case 'm': diff = 4; break; }
    memset(&g_nxMods, 0, sizeof(g_nxMods));
    g_nxMods.skin = -1;
    static const unsigned k_flag[13] = { 0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x80, 0x100, 0x200, 0x400, 0x1000, 0x2000, 0x4000 };
    int sum = 0;
    bool sink = false, rise = false, appear = false, snake = false, hard = false, breakOn = false;
    int gaugeStart = 0, gaugeDisp = 0, gaugeLimit = 0;
    for (const char* c = s_runCmd; *c; c++) {
        char ch = (char)tolower((unsigned char)*c);
        if (ch >= '0' && ch <= '9') { sum += ch - '0'; continue; }
        if (ch == 'k') {                                              /* skin da missão: kX; o 1º k vale para o jogador (slots 2 e 3: por nota, ainda não portado) */
            if (c[1]) {
                static const char k_skinTbl[] = "xgo12mcpnshdafiw<>_r";   /* SkinParamTable (playengine.cpp:583) */
                const char* f = strchr(k_skinTbl, tolower((unsigned char)c[1]));
                int idx = f ? (int)(f - k_skinTbl) : 0;
                if (idx >= (int)strlen(k_skinTbl) - 1) idx = rand() % ((int)strlen(k_skinTbl) - 1);
                if (g_nxMods.skin < 0) g_nxMods.skin = idx;
                c++;
            }
            continue;
        }
        if (ch == 's') { g_nxMods.flags |= 0x10000; continue; }
        if (ch == 'e') { g_nxMods.flags |= 0x20000; continue; }
        if (ch == 'o') { breakOn = true; continue; }
        if (ch == '#' || ch == '%' || ch == '@') {
            int v = atoi(c + 1);
            if (ch == '#') gaugeLimit = v; else if (ch == '%') gaugeDisp = v; else gaugeStart = v;
            while (isdigit((unsigned char)c[1])) c++;
            continue;
        }
        if (ch == '(') { sink = true; continue; }
        if (ch == ')') { rise = true; continue; }
        if (ch == 'p') { appear = true; continue; }
        if (ch == 'z') { snake = true; continue; }
        if (ch == 'h') { hard = true; continue; }
        const char* f = strchr(k_cmdParam, ch);
        if (f && *f && (f - k_cmdParam) < 13) g_nxMods.flags |= k_flag[f - k_cmdParam];
    }
    g_nxMods.units = sum;
    /* efeitos dos itens (playengine.cpp:1290..1310): velocidade do jogador substitui a da missão e tira AC/DC; "modo off" tira
     * os modificadores (menos AC/DC, que só saem com a velocidade); a vida soma ao gauge */
    int userSpeed = (s_bUsb && s_bItem) ? s_curSpeed * 4 : 0;
    bool userModeOff = (s_bUsb && s_bItem) && s_modeOff;
    if (userSpeed) { g_nxMods.units = userSpeed; g_nxMods.flags &= ~(0x10000u | 0x20000u | 0x200u | 0x400u); }
    if (userModeOff) { g_nxMods.flags &= (userSpeed ? 0u : (0x200u | 0x400u)); sink = rise = appear = snake = hard = false; }
    {
        static const int k_bonus[5] = { 0, 900, 1500, 2700, 5100 };
        g_nx2WGBonus = (s_bUsb && s_bItem) ? k_bonus[s_curLife] : 0;
        g_nx2WGStart = gaugeStart; g_nx2WGDisp = gaugeDisp; g_nx2WGLimit = gaugeLimit;
    }
    g_nx2WGLottery = (s_bUsb && s_bItem) && s_lottery;   /* loteria comprada: o mileage do resultado é sorteado */
    g_nx2WorldBreak = breakOn;   /* playengine.cpp:2286: no WorldTour só quebra se bWXBreakOn */
    unsigned id = s_runTrack;
    Log_Print("NX2WORLD: RUN %x %s: modo %d, nível %d, velocidade %d, flags 0x%X, gauge @%d %%%d #%d, break %d\n", id, s_runMode, diff, s_curM->level,
              sum, g_nxMods.flags, gaugeStart, gaugeDisp, gaugeLimit, breakOn ? 1 : 0);
    Movie_Close();
    BGM_Stop();
    stopBgm();
    if (s_bUsb && s_useMil > 0) {                      /* End(): o mileage gasto sai do perfil e grava */
        g_nx2Prof[s_player].iMileage -= s_useMil;
        if (g_nx2Prof[s_player].iMileage < 0) g_nx2Prof[s_player].iMileage = 0;
        Nx2Profile_Save(s_player);
        Log_Print("NX2WORLD: itens gastaram %d de mileage (restam %d)\n", s_useMil, g_nx2Prof[s_player].iMileage);
    }
    unsigned joined = 1u << s_player;
    if (diff < 0 || !Select_StartMission((int)id, diff, s_curM->level, joined)) {
        Log_Print("NX2WORLD: missão %x não pôde ser iniciada\n", id);
        s_phase = PH_BRIEF_START;
        Game_ChangeState(STATE_GAMEOVER_ENTER);
        return;
    }
    for (int p = 0; p < 2; p++) {                    /* extras do NX2 que o Select_StartMission não conhece */
        g_game.cmdSink[p] = sink; g_game.cmdRise[p] = rise; g_game.cmdAppear[p] = appear; g_game.cmdSnake[p] = snake; g_game.cmdHard[p] = hard;
    }
}

static void briefingUpdate(float dt)
{
    s_bFrames++;
    s_dbgFrame++;
    if (s_phase == PH_BRIEF_START) {
        if (BGA_SceneDone(BB, "in_bg") && BGA_SceneDone(BA, "in_time")) {
            if (s_bUsb && s_bItem) {                         /* Start(): setas acesas se há mileage, apagadas se não */
                if (s_totalMil >= SPEED_MILEAGE) { s_arrCur[AU_UL] = k_arrClk[AU_UL]; reorganize(AU_UL, AR_CLK); }
                else { s_arrCur[AU_UL] = k_arrOut[AU_UL]; reorganize(AU_UL, AR_OUT); }
                if (s_totalMil >= 60) { s_arrCur[AU_UR] = k_arrClk[AU_UR]; reorganize(AU_UR, AR_CLK); }
                else { s_arrCur[AU_UR] = k_arrOut[AU_UR]; reorganize(AU_UR, AR_OUT); }
                if (s_totalMil >= s_lotMil) { s_arrCur[AU_LR] = k_arrClk[AU_LR]; reorganize(AU_LR, AR_CLK); }
                else { s_arrCur[AU_LR] = k_arrOut[AU_LR]; reorganize(AU_LR, AR_OUT); }
                if (s_totalMil >= MODE_MILEAGE) { s_arrCur[AU_LL] = k_arrClk[AU_LL]; reorganize(AU_LL, AR_CLK); }
                else { s_arrCur[AU_LL] = k_arrOut[AU_LL]; reorganize(AU_LL, AR_OUT); }
            }
            s_phase = PH_BRIEF_RUN; s_bElapsed = 0.0;
        }
        return;
    }
    if (s_phase == PH_BRIEF_RUN) {
        s_bElapsed += dt;
        s_bTimeLeft = BRIEF_TIME - (int)s_bElapsed;
        if (s_bUsb && s_bItem) updateArrowState();
        if (Input_IsPadHit(s_player, PAD_C) || padHit(s_player, PAD_C)) {
            if (!s_bSel) { s_bSel = true; if (s_bSnd[2] >= 0) Audio_Play(s_bSnd[2], false); }   /* S_CLK.WAV (EFF_SELECT do SFX_GLOBAL.LUA), confirmado pelo usuário. era: A_SELECT.WAV, depois WCENTER.WAV */
            else s_bTimeLeft = 0;
        }
        if (s_bUsb && s_bItem) {
            #define BSND() do { if (s_bSnd[0] >= 0) Audio_Play(s_bSnd[0], false); } while (0)
            if (padHit(s_player, PAD_UL) && s_arrState[AU_UL] != AR_OUT) {          /* velocidade: 1x..4x, depois nada */
                s_preSpeed = s_curSpeed; s_curSpeed = wrap5(s_curSpeed + 1);
                updateUseMileage(AU_UL); updateCmdSpr(); reorganize(AU_UL, AR_CLK);
                BGA_SceneReset(BA, s_arrCur[AU_UL]); BSND(); s_bSel = false;
            }
            if (padHit(s_player, PAD_UR) && s_arrState[AU_UR] != AR_OUT && s_arrState[AU_UR] != AR_ROLLBACK) {   /* vida Lv.1..4 */
                s_preLife = s_curLife; s_curLife = wrap5(s_curLife + 1);
                if (updateUseMileage(AU_UR)) reorganize(AU_UR, AR_CLK); else reorganize(AU_UR, AR_ROLLBACK);
                BGA_SceneReset(BA, s_arrCur[AU_UR]); BSND(); s_bSel = false;
            }
            if (padHit(s_player, PAD_DL) && s_arrState[AU_LL] != AR_OUT) {          /* desliga os modos */
                s_modeOff = !s_modeOff;
                updateUseMileage(AU_LL); updateCmdSpr(); reorganize(AU_LL, AR_CLK);
                BGA_SceneReset(BA, s_arrCur[AU_LL]); BSND(); s_bSel = false;
            }
            if (padHit(s_player, PAD_DR) && s_arrState[AU_LR] != AR_OUT) {          /* loteria */
                s_lottery = !s_lottery;
                updateUseMileage(AU_LR); reorganize(AU_LR, AR_CLK);
                BGA_SceneReset(BA, s_arrCur[AU_LR]); BSND(); s_bSel = false;
            }
            #undef BSND
        }
        if (s_bTimeLeft <= 0) { s_bTimeLeft = 0; s_phase = PH_BRIEF_END; s_bCnt = 0; Log_Print("NX2WORLD: briefing fim (quadro %d, %.2f s)\n", s_bFrames, s_bElapsed); }
        return;
    }
    s_bCnt++;                                         /* End: fade de 30 quadros e RUN */
    if (s_bCnt >= 30) briefingLaunch();
}

static void briefingRender(void)
{
    BGA_ScenePlay(BB, "in_bg", true);
    if (s_phase == PH_BRIEF_START && !BGA_SceneDone(BB, "in_bg")) return;
    briefingDrawTime(s_bTimeLeft);
    BGA_ScenePlay(BA, "in_time", true);
    if (s_bUsb && s_bItem) {
        BGA_ScenePlay(BA, "in_mileage", true);
        BGA_ScenePlay(BA, "in_mileagefont", true);
        for (int i = 0; i < 4; i++) BGA_ScenePlay(BA, s_arrCur[i], true);
    }
    for (int i = 0; i < s_cmdMax && i < 8; i++) {
        char nm[16];
        snprintf(nm, sizeof(nm), "%s_icon%02d", s_cmdOn[i] ? "in" : "out", i + 1);
        BGA_ScenePlay(BB, nm, true);
    }
    for (int i = 0; i < s_objLines; i++) wlineDraw(&s_objLine[i], 70, 180 + 24 * i);
    if (s_bSel && s_phase >= PH_BRIEF_RUN) BGA_ScenePlay(BB, (s_bUsb && s_bItem) ? "step_center" : "step_se", true);
    if (s_phase == PH_BRIEF_END && s_bCnt)
        Render_Rect(0.0f, 0.0f, 640.0f, 480.0f, 0, 0, 0, (uint8_t)(255.0f * s_bCnt / 30.0f > 255.0f ? 255.0f : 255.0f * s_bCnt / 30.0f));
}

/* ---- ciclo ---- */
void Nx2World_Enter(void)
{
    s_ok = false;
    NxMission_Load();
    s_player = (Title_GetJoinedMask() & 1) ? 0 : 1;
    Audio_StopAll();

    if (!s_sndInit) { for (int k = 0; k < WS_COUNT; k++) s_snd[k] = Audio_LoadWaveFile(k_wav[k]); s_sndInit = true; }

    Resource_ClearBGA();   /* limpa as texturas dos BGAs: os ladrilhos são carregados DEPOIS (antes ficavam com ids reaproveitados) */
    static const char* const k_bga[7] = { "WORLD_IN", "WORLD_OB", "M_BOX", "BOARD", "WEVENT_EFF", "W-MAX", "BARI" };
    s_ok = true;
    for (int i = 0; i < 7; i++)
        if (!Resource_LoadBGAByName(k_bga[i])) { Log_Print("NX2WORLD: falha ao carregar BGA\\%s.DAT\n", k_bga[i]); s_ok = false; }
    BGA_Reset();
    loadTiles();
    if (!s_ok || NxMission_Count() == 0) { s_ok = false; return; }

    /* camadas de origem */
    for (int i = 0; i < 10; i++) { if (i < 9) BGA_GetLayerSrc(B_W, 23 + i, &s_landSpr[i]); BGA_GetLayerSrc(B_W, 36 + i, &s_timeNum[i]); }
    for (int i = 0; i < 5; i++) BGA_GetLayerSrc(B_W, 16 + i, &s_levelSpr[i]);
    for (int i = 0; i < 3; i++) BGA_GetLayerSrc(B_W, 50 + i, &s_playerSpr[i]);
    BGA_GetLayerSrc(B_W, 13, &s_breakSpr[0]); BGA_GetLayerSrc(B_W, 12, &s_breakSpr[1]);
    BGA_GetLayerSrc(B_W, 15, &s_switchSpr[0]); BGA_GetLayerSrc(B_W, 14, &s_switchSpr[1]);
    BGA_GetLayerSrc(B_W, 54, &s_miniSpr[0]); BGA_GetLayerSrc(B_W, 55, &s_miniSpr[1]);
    BGA_GetLayerSrc(B_UI, 61, &s_pt[0]); BGA_GetLayerSrc(B_UI, 62, &s_pt[1]);
    BGA_GetLayerSrc(B_UI, 63, &s_line);
    BGA_GetLayerSrc(B_ICON, 36, &s_arrow[0]);   /* azul */
    BGA_GetLayerSrc(B_ICON, 37, &s_arrow[1]);   /* vermelha */
    for (int i = 0; i < 12; i++) BGA_GetLayerSrc(B_BARI, 5 + i, &s_lock[i]);

    s_switch = ps()->bmWMClear[999] != 0;               /* EF999 cumprida: lado "ghost" */
    if (g_nx2HasProfile[s_player]) ps()->bWMBarricadeClear[10] = true;   /* BARI_BLACK */
    setPointScenes();

    s_another = false;
    s_session = (g_game.stageCount != 3);
    if (!s_session) {
        NxMission_ClearDefaultList();
        s_worldLife = 3;
        NxMission_SetDefaultList();
        NxMission_OrganizeConnection();                 /* (no original a ligação acontece na lista corrente ao montá-la) */
        NxMission_ChangeData(s_switch ? 1 : 0);
        s_another = s_switch;
        toggleMapTexture();
        NxMission* start = NULL;
        if (g_nx2HasProfile[s_player]) {
            char b[16];
            snprintf(b, sizeof(b), "%x", (unsigned)ps()->iWMPosition);
            start = NxMission_CurByStep(b);
        }
        if (getenv("NX2WORLD_START")) { NxMission* t = NxMission_CurByStep(getenv("NX2WORLD_START")); if (t) start = t; }   /* teste: missao inicial (ex.: EF011) */
        if (!start) { short id[3] = { 0, 0, 1 }; start = NxMission_CurById(id); }
        if (start) start->show = true;
        s_curM = start;
    } else {
        NxMission_ChangeData(s_switch ? 1 : 0);
        s_another = s_switch;
        toggleMapTexture();
        /* a missão atual continua a da volta do jogo */
    }
    if (!s_curM) { Log_Print("NX2WORLD: sem missão inicial\n"); s_ok = false; return; }
    s_prevM = s_curM;
    setStartMission(s_curM);
    s_curM->state = NXM_READY;

    s_move = false; s_selected = false; s_wasMoving = false; s_bEnd = false;
    s_phase = 0; s_cnt = 0; s_timeAcc = 0.0; s_time = SELECT_TIME;
    drawTime(s_time);
    {   /* fontes: nx.ttf (nxpt.ttf em português), 22 e 14 */
        const char* ff = (g_nx2Opt.g_Language == 3) ? "nxpt.ttf" : "nx.ttf";
        if (strcmp(s_fontFile, ff)) {
            wfontFree(&s_fBig); wfontFree(&s_fSmall);
            wfontLoad(&s_fBig, ff, 22); wfontLoad(&s_fSmall, ff, 14);
            snprintf(s_fontFile, sizeof(s_fontFile), "%s", ff);
        }
    }
    setMissionInfoWnd(s_curM);
    setEnabledConnection();
    BGA_SetLayerSrc(B_W, 53, &s_miniSpr[s_switch ? 1 : 0]);
    playBgmFor(s_curM->land);
    Log_Print("NX2WORLD: Begin, missão '%s' (%s, %s), vidas %d, lista %d\n", s_curM->step, s_curM->land, s_curM->name, s_worldLife, NxMission_CurCount());
}

void Nx2World_Update(float dt)
{
    if (!s_ok) return;
    if (s_phase >= 3) { briefingUpdate(dt); return; }
    if (s_phase == 0) {
        if (BGA_SceneDone(B_W, "main start")) { s_phase = 1; }
        return;
    }
    if (s_phase == 1) {
        s_time = SELECT_TIME - (int)s_timeAcc;
        if (g_game.svcGameMode == 1) s_time = 99;       /* EVENT: tempo infinito */
        if (s_wasMoving && !s_move) {
            s_wasMoving = false;
            setMissionInfoWnd(s_curM);
            /* RANDOM (bomba E_BOMB): fase 2b */
        }
        processInputInMap(s_player);
        moveMapByPoint();
        s_timeAcc += dt;
        if (s_time <= 0) {
            /* ValidateMission: se não há vida para a missão atual, volta para a anterior (fase 2b); aqui só fecha */
            if (g_nx2HasProfile[s_player]) {
                strncpy(ps()->szLand, s_curM->land, sizeof(ps()->szLand) - 1);
                strncpy(ps()->szMission, s_curM->name, sizeof(ps()->szMission) - 1);
            }
            s_curM->state = NXM_NONE;
            startEnd();
        }
        return;
    }
    /* End: 30 quadros de fade e partida */
    s_cnt++;
    if (s_cnt >= 30) {
        char cmd[128];
        memset(cmd, 0, sizeof(cmd));
        snprintf(cmd, sizeof(cmd), "%d%s@%d%%%d#%d", s_curM->speed, s_curM->command, s_curM->gageMin, s_curM->gageMax, s_curM->gageMax);
        if (s_curM->seperate) strncat(cmd, "b", sizeof(cmd) - strlen(cmd) - 1);
        if (s_curM->nxBreak) strncat(cmd, "o", sizeof(cmd) - strlen(cmd) - 1);
        for (int i = 0; i < 3; i++) if (_stricmp(s_curM->skin[i], "nn") != 0) strncat(cmd, s_curM->skin[i], sizeof(cmd) - strlen(cmd) - 1);
        unsigned track = 0;
        sscanf(s_curM->step, "%x", &track);
        int tn = 0;
        sscanf(&s_curM->step[2], "%3d", &tn);
        if (tn >= 0 && tn < NX2_MISSION_MAX) { ps()->iWMChallenge[tn]++; if (ps()->iWMChallenge[tn] < 0) ps()->iWMChallenge[tn]--; }
        Log_Print("NX2WORLD: W_BRIEFING %x %s %s | %s | %s | %s | %s\n", track, s_curM->stepLV, cmd,
                  s_curM->conditions[0].condition, s_curM->conditions[1].condition, s_curM->conditions[2].condition, s_curM->conditions[3].condition);
        s_worldLife -= s_curM->life; if (s_worldLife < 0) s_worldLife = 0;
        if (g_nx2HasProfile[s_player]) ps()->iWMPosition = (int32_t)track;
        s_curM->state = NXM_NONE;
        snprintf(s_runMode, sizeof(s_runMode), "%s", s_curM->stepLV);
        snprintf(s_runCmd, sizeof(s_runCmd), "%s", cmd);
        s_runTrack = track;
        briefingBegin();
    }
}

void Nx2World_Render(void)
{
    if (!s_ok) return;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1.f, 1.f, 1.f, 1.f);
    if (s_phase >= 3) { briefingRender(); glColor4f(1.f, 1.f, 1.f, 1.f); return; }
    drawWorldMap();
    drawTime(s_time);
    if (s_phase == 0) {
        BGA_ScenePlay(B_W, "main start", true);
        BGA_ScenePlay(B_W, "counter start", true);
    } else if (s_phase == 1) {
        BGA_ScenePlay(B_W, "main start", true);
        drawLifeGage();
        BGA_ScenePlay(B_W, "txt start", true);
        BGA_ScenePlay(B_W, "icon start", true);
        wlineDraw(&s_nameLine, 46, 326);
        for (int i = 0; i < s_descLines; i++) wlineDraw(&s_descLine[i], 46, 358 + 14 * i);
        BGA_ScenePlay(B_W, "counter start", true);
        drawMiniMapPoint(s_posX + 320, s_posY + 160);
    } else {
        BGA_ScenePlay(B_W, "main end", true);
        BGA_ScenePlay(B_W, "icon end", true);
        BGA_ScenePlay(B_W, "counter end", true);
        if (s_cnt) Render_Rect(0.0f, 0.0f, 640.0f, 480.0f, 0, 0, 0, (uint8_t)(255.0f * s_cnt / 30.0f > 255.0f ? 255.0f : 255.0f * s_cnt / 30.0f));
    }
    glColor4f(1.f, 1.f, 1.f, 1.f);
}

void Nx2World_Leave(void)
{
    stopBgm();
    textClear();
    wfontFree(&s_fMed);
}
