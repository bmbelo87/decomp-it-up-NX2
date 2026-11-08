/*
 * nx2_sports.c — tela de calorias e oxigenação gastas do fim do crédito (CSports do NX2).
 * Fonte: nx2src ProcModules/sports.cpp, conferido no piu (CSports::Begin 0x80a6030, Run ~0x80a5a90,
 * End 0x80a5f50, DrawBar 0x80a5680, SetKcalnVo2 0x80a5770).
 *
 * Diferenças do nx2src confirmadas no piu:
 *  - 3 estágios (1st, 2nd, bonus), não 4: os rótulos 29/30/31 do SPORTS.bga já trazem 1st/2nd/bonus; só o
 *    TRAINING troca por stage1..3 (layers 72..74). Não existe a troca 29+i <- 68+i do nx2src.
 *  - Linhas Y fixas em rodata 0x80fa8c0 (3 por tabela), em vez de 84 + 72 * i:
 *      P1 kcal 70 165 262 | P1 VO2 93 189 286 | P2 kcal 117 213 310 | P2 VO2 140 236 333
 *    barra em y + 18 (0x12) e número em y + 16 (0x10).
 *  - Escalas 3.89 (kcal) e 1.47 (VO2); número em x = valor * escala + 163.
 *
 * Telas: BGA/BG.MOV (loop), BGA/SPORTS.DAT (BGA 0), BGA/SPORTS_GRAPH.DAT (BGA 1, só as peças das barras) e
 * BGA/GRAPH_FONT.DAT (GRAPH_FO.tga: dígitos 14x14, 5 por linha). Cenas "graph_in" (Start/Run) e "graph_out" (End).
 * A versão do NX (nx_sports.c: cenas start/round start, barras verticais) fica só como referência.
 */
#include "pumpy.h"
#include "bga.h"
#include "movie.h"

extern float g_nxKcal[2][4];
extern float g_nxVo2[2][4];
int NxSports_StageCount(void);
GameState NxSports_NextState(void);

enum { SPB = 0, SPG = 1 };
#define SPORTS_TIME 20

static int   s_phase;                 /* 1 Start, 2 Run, 3 End */
static uint32_t s_t0;
static int   s_time;
static int   s_cnt2;
static int   s_stages;                /* m_CurrentStage */
static bool  s_drawing;               /* m_bGraphDrawing */
static float s_kcal[2][3], s_vo2[2][3], s_kcalG[2][3], s_vo2G[2][3];
static float s_totK[2], s_totV[2];
static BGALayerSrc s_num[10];
static BGALayerSrc s_bar[4][3];       /* 0 P1 kcal, 1 P1 VO2, 2 P2 kcal, 3 P2 VO2: base, meio, ponta */
static int   s_fontTex = -1;
static bool  s_ok;
static bool  s_train;

static const int k_yTab[4][3] = { { 70, 165, 262 }, { 93, 189, 286 }, { 117, 213, 310 }, { 140, 236, 333 } };   /* piu 0x80fa8c0 */

static void setTotals(int p)    /* SetKcalnVo2 0x80a5770 */
{
    static const int layer[2][12] = { { 44, 43, 42, 41, 40, 39, 51, 50, 49, 48, 47, 46 }, { 60, 59, 58, 57, 56, 55, 67, 66, 65, 64, 63, 62 } };
    char str[16];
    int v[2] = { (int)(s_totK[p] * 1000), (int)(s_totV[p] * 1000) };
    for (int k = 0; k < 2; k++) {
        snprintf(str, sizeof(str), "%06d", v[k]);
        for (int i = 0; i < 6; i++) {
            int d = str[i] - '0';
            if (d >= 0 && d <= 9) BGA_SetLayerSrc(SPB, layer[p][k * 6 + i], &s_num[d]);
        }
    }
}

static void tileQuad(const BGALayerSrc* src, float x0, float x1)   /* peça horizontal da barra, Y para cima a partir da base */
{
    if (!src->isSPR || src->sprTileStart < 0) return;
    SPRTileDef* t = &g_game.sprTiles[src->sprTileStart];
    if (t->texId < 0 || t->texId >= MAX_TEXTURES || !g_game.textures[t->texId].inUse) return;
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_game.textures[t->texId].id);
    float lo = (float)t->srcY, hi = (float)(t->srcY + t->srcH);
    glBegin(GL_QUADS);
    glTexCoord2f(t->u1, t->v1); glVertex2f(x0, hi);
    glTexCoord2f(t->u1, t->v2); glVertex2f(x0, lo);
    glTexCoord2f(t->u2, t->v2); glVertex2f(x1, lo);
    glTexCoord2f(t->u2, t->v1); glVertex2f(x1, hi);
    glEnd();
}

static void drawBar(int g, float y, float size)   /* CGraph::DrawBar 0x80a5680: Translate(140, 480 - y) */
{
    if (size < 10.0f) size = 10.0f;
    if (size > 440.0f) size = 440.0f;
    glPushMatrix();
    glTranslatef(140.0f, 480.0f - y, 0.0f);
    SPRTileDef* t0 = &g_game.sprTiles[s_bar[g][0].sprTileStart];
    tileQuad(&s_bar[g][0], (float)t0->srcX, (float)(t0->srcX + t0->srcW));                       /* DrawPic(0) */
    tileQuad(&s_bar[g][1], 9.0f, size);                                                          /* DrawPicXX(0, 9, size) */
    tileQuad(&s_bar[g][2], size, size + (float)g_game.sprTiles[s_bar[g][2].sprTileStart].srcW);  /* DrawPicAt(0, size, 0) */
    glPopMatrix();
}

static void scoreDraw(float x, float y, int num, int w)   /* CSportsFont::ScoreDraw: 14x14, x -= 8 por dígito, y = base (Y para baixo) */
{
    if (s_fontTex < 0) return;
    for (int i = 0; i < w; i++) {
        int n = num % 10;
        float u = (float)((n % 5) * 14), v = (float)((n / 5) * 14);
        Texture_DrawUV(s_fontTex, x, y - 14.0f, 14.0f, 14.0f, u, v, u + 14.0f, v + 14.0f, 1.f, 1.f, 1.f, 1.f);
        num /= 10;
        x -= 8.0f;
    }
}

static void enter(void)
{
    memset(s_kcal, 0, sizeof(s_kcal)); memset(s_vo2, 0, sizeof(s_vo2));
    memset(s_kcalG, 0, sizeof(s_kcalG)); memset(s_vo2G, 0, sizeof(s_vo2G));
    s_totK[0] = s_totK[1] = s_totV[0] = s_totV[1] = 0.0f;
    s_drawing = true;
    s_cnt2 = 0;
    s_stages = NxSports_StageCount(); if (s_stages > 2) s_stages = 2;
    s_train = (g_game.nxGameMode == 3);

    BGM_Stop();
    Movie_Close();
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/BGA/BG.MOV", g_game.currentDirectory);
    Movie_Open(path, true);
    Resource_ClearBGA();
    s_ok = Resource_LoadBGAByName("SPORTS") && Resource_LoadBGAByName("SPORTS_GRAPH");
    BGA_Reset();
    s_fontTex = -1;
    snprintf(path, sizeof(path), "%s/BGA/GRAPH_FONT.DAT", g_game.currentDirectory);
    if (RES_Open(path)) { s_fontTex = loadTextureFromRES("GRAPH_FO.tga"); RES_Close(); }
    if (!s_ok) { Log_Print("NX2SPORTS: falha ao carregar SPORTS.DAT / SPORTS_GRAPH.DAT\n"); return; }
    if (s_fontTex < 0) Log_Print("NX2SPORTS: falha ao carregar GRAPH_FO.tga do GRAPH_FONT.DAT\n");

    int lang = g_game.svcLangOption & 3;
    static const int k_title[4] = { 77, 77, 78, 80 }, k_kcal[4] = { 81, 82, 83, 82 }, k_vo2[4] = { 85, 86, 87, 86 };
    BGALayerSrc a;
    if (BGA_GetLayerSrc(SPB, k_vo2[lang], &a))  BGA_SetLayerSrc(SPB, 35, &a);
    if (BGA_GetLayerSrc(SPB, k_kcal[lang], &a)) BGA_SetLayerSrc(SPB, 34, &a);
    if (BGA_GetLayerSrc(SPB, k_title[lang], &a)) BGA_SetLayerSrc(SPB, 26, &a);
    if (s_train)   /* TRAINING: stage1..3 no lugar de 1st/2nd/bonus */
        for (int i = 0; i < 3; i++) if (BGA_GetLayerSrc(SPB, 72 + i, &a)) BGA_SetLayerSrc(SPB, 29 + i, &a);
    for (int i = 0; i < 10; i++) BGA_GetLayerSrc(SPB, 90 + i, &s_num[i]);
    static const int k_g[4][3] = { { 1, 2, 3 }, { 5, 6, 7 }, { 9, 10, 11 }, { 13, 14, 15 } };
    for (int g = 0; g < 4; g++) for (int i = 0; i < 3; i++) s_ok &= BGA_GetLayerSrc(SPG, k_g[g][i], &s_bar[g][i]);

    for (int p = 0; p < 2; p++) {
        if (!(g_game.activePlayerMask & (1 << p))) continue;
        for (int i = 0; i <= s_stages; i++) {
            s_kcal[p][i] = g_nxKcal[p][i]; s_vo2[p][i] = g_nxVo2[p][i];
            s_totK[p] += s_kcal[p][i]; s_totV[p] += s_vo2[p][i];
        }
        setTotals(p);
    }
    BGA_SceneReset(SPB, "graph_in");
    BGA_SceneReset(SPB, "graph_out");
    s_phase = 1;
    s_t0 = timeGetTime();
    Log_Print("NX2SPORTS: Begin, estágios 0..%d, kcal P1 %.3f P2 %.3f, VO2 P1 %.3f P2 %.3f\n", s_stages, s_totK[0], s_totK[1], s_totV[0], s_totV[1]);
}

static void leave(void)
{
    Movie_Close();
    Resource_ClearBGA();
    Game_ChangeState(g_game.nxGameMode == 0 ? NxSports_NextState() : STATE_GAMEOVER_ENTER);
}

void Nx2Sports_Update(float dt)
{
    if (g_game.stateFrame == 1) enter();
    Movie_Update(dt);
    if (!s_ok) { leave(); return; }
    if (s_phase == 1) {                                          /* Start: graph_in */
        if (BGA_SceneDone(SPB, "graph_in")) s_phase = 2;
        return;
    }
    if (s_phase == 2) {                                          /* Run */
        s_time = SPORTS_TIME - (int)((timeGetTime() - s_t0) / 1000);
        for (int p = 0; p < 2; p++) {
            if (!(g_game.activePlayerMask & (1 << p))) continue;
            for (int i = 0; i <= s_stages; i++) {
                if (s_kcal[p][i] != 0.0f) {
                    s_drawing = true;
                    s_kcalG[p][i] += 1; s_vo2G[p][i] += 1;
                    if (s_kcalG[p][i] >= s_kcal[p][i]) { s_kcalG[p][i] = s_kcal[p][i]; s_drawing = false; }
                    if (s_vo2G[p][i] >= s_vo2[p][i])   { s_vo2G[p][i] = s_vo2[p][i];   s_drawing = false; }
                } else s_drawing = false;
            }
        }
        if (s_time < 17) {
            for (int p = 0; p < 2; p++)
                if ((g_game.activePlayerMask & (1 << p)) && Input_IsPadHit(p, PAD_C)) s_time = 0;
        }
        if (s_time <= 0 && !s_drawing) { s_phase = 3; s_cnt2 = 0; }
        return;
    }
    if (s_cnt2 >= 40) { leave(); return; }                       /* End: fade de 40 quadros */
    s_cnt2++;
}

void Nx2Sports_Render(void)
{
    Movie_Render();
    if (!s_ok) return;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1, 1, 1, 1);
    if (s_phase == 3) {
        BGA_ScenePlay(SPB, "graph_out", true);
        if (s_cnt2) {
            float a = s_cnt2 / 40.0f * 255.0f;
            Render_Rect(0.0f, 0.0f, 640.0f, 480.0f, 0, 0, 0, (uint8_t)(a > 255.0f ? 255.0f : a));
            glColor4f(1, 1, 1, 1);
        }
        return;
    }
    BGA_ScenePlay(SPB, "graph_in", true);
    if (s_phase < 2) return;
    glColor4f(1, 1, 1, 1);
    for (int p = 0; p < 2; p++) {
        if (!(g_game.activePlayerMask & (1 << p))) continue;
        for (int i = 0; i <= s_stages; i++) {
            if (s_kcal[p][i] == 0.0f) continue;
            float yk = (float)k_yTab[p * 2][i], yv = (float)k_yTab[p * 2 + 1][i];
            drawBar(p * 2, yk + 18.0f, s_kcalG[p][i] * 3.89f);
            scoreDraw(s_kcalG[p][i] * 3.89f + 163.0f, yk + 16.0f, (int)s_kcalG[p][i], 3);
            drawBar(p * 2 + 1, yv + 18.0f, s_vo2G[p][i] * 1.47f);
            scoreDraw(s_vo2G[p][i] * 1.47f + 163.0f, yv + 16.0f, (int)s_vo2G[p][i], 3);
        }
    }
}
