/*
 * nx_credit.c — faixa de crédito da NX (CCreditManager, piu 0x804e324.. = NX2
 * creditmanager.cpp), recursos do BGA/CREDIT.DAT.
 *
 * Draw (todo quadro, menos no SETUP; NX2 main.cpp:604):
 *   GAME MODE = EVENT            -> cena "EVENT"
 *   moedas por crédito = 0       -> cena "freeplay"
 *   senão, crédito < 10          -> cena "creidit _0"  (crédito, resto de moedas, moedas por crédito)
 *          crédito 10..99        -> cena "creidit _00" (dois dígitos de crédito)
 * DrawDemoPlay (durante a demo; NX2 playengine.cpp:260): "DEMOPLAY_1P" e "DEMOPLAY_2P".
 *
 * As cenas do credit.bga são estáticas (os keyframes de cada slot têm a mesma
 * posição), então os sprites são desenhados direto, sem o sistema de BGA: ele é
 * limpo inteiro a cada troca de tela (Resource_ClearBGA) e esta faixa fica por cima
 * de todas. As texturas também são apagadas na limpeza; g_texGeneration avisa
 * quando é preciso recarregar.
 *
 * Posições (credit.bga do NX2, BGA3: x,y = centro da extensão do SPR, Y para baixo):
 *   slot 0  credit01.spr (322,463)   slot 1..3  dígitos (334|354|374, 463)
 *   slot 4  credit02.spr (322,463)   slot 5..8  dígitos (327|337|357|378, 463)
 *   slot 9  freeplay.spr (322,463)   slot 10    event.spr (320,460)
 *   slot 11 demoplay.spr (128,460) escala 0.8   slot 12 demoplay.spr (512,460) escala 0.8
 * Cor (keyframes +0x2C..+0x38, r g b a): tudo (1,1,1) menos demoplay (1,1,0.75). Sem
 *   animação: os dois keyframes de cada slot são iguais, e o NX2 só chama DrawScene.
 * Recortes: os "T tex x y w h u1 v1 u2 v2" dos .spr do CREDIT.DAT.
 */
#include "pumpy.h"

extern unsigned g_texGeneration;   /* texture.c: +1 a cada Texture_Shutdown */
extern bool g_demoPlay;

static int      s_texText = -1;    /* CTEXTS.TGA   */
static int      s_texCred = -1;    /* credit01.tga */
static unsigned s_gen = 0xFFFFFFFFu;
static bool     s_failed;

typedef struct { int x, y, w, h, u1, v1, u2, v2; } CrTile;

/* credit01.spr e credit02.spr: 3 tiles cada (desenhados do último para o primeiro) */
/* NX2: credit01.spr / credit02.spr têm 5 tiles ("CREDIT[S]", "(", "/", ")" e a barra), extensão 150x20 */
static const CrTile k_cred01[5] = { { 5, 0, 80, 20, 1, 1, 81, 21 }, { 87, 0, 20, 20, 82, 1, 102, 21 }, { 106, 0, 20, 20, 103, 1, 123, 21 },
                                    { 125, 0, 20, 20, 124, 1, 144, 21 }, { 0, 0, 150, 20, 1, 43, 151, 63 } };
static const CrTile k_cred02[5] = { { 0, 0, 80, 20, 1, 1, 81, 21 }, { 90, 0, 20, 20, 82, 1, 102, 21 }, { 109, 0, 20, 20, 103, 1, 123, 21 },
                                    { 130, 0, 20, 20, 124, 1, 144, 21 }, { 0, 0, 150, 20, 1, 43, 151, 63 } };
/* cr0..cr9.spr: 20x20 em credit01.tga, 5 por linha, passo 21, a partir de (151,1) */
static const CrTile k_free = { 0, 0, 120, 26, 1, 1, 121, 27 };    /* freeplay.spr (CTEXTS.tga) */
static const CrTile k_event = { 0, 0, 120, 26, 1, 73, 121, 99 };  /* event.spr    */
static const CrTile k_demo = { 0, 0, 240, 44, 1, 28, 241, 72 };   /* demoplay.spr */

static bool crLoad(void) {
    if (s_gen == g_texGeneration && !s_failed) return s_texText >= 0 && s_texCred >= 0;
    if (s_gen == g_texGeneration && s_failed) return false;
    s_gen = g_texGeneration;
    s_texText = s_texCred = -1;
    s_failed = false;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/BGA/CREDIT.DAT", g_game.currentDirectory);
    if (!RES_Open(path)) { s_failed = true; return false; }
    s_texText = loadTextureFromRES("ctexts.tga");
    s_texCred = loadTextureFromRES("credit01.tga");
    RES_Close();
    if (s_texText < 0 || s_texCred < 0) { s_failed = true; return false; }
    return true;
}

/* tile de um SPR cujo centro da extensão (extW x extH) está em (cx, cy), com escala */
static void crTile(int tex, const CrTile* t, float cx, float cy, float extW, float extH, float sc, float blue) {
    float ox = cx - extW * 0.5f * sc, oy = cy - extH * 0.5f * sc;
    /* Texture_DrawUV: x,y = canto de cima-esquerda (Y para baixo), UV em pixels, V=0 no topo */
    Texture_DrawUV(tex, ox + (float)t->x * sc, oy + (float)t->y * sc, (float)t->w * sc, (float)t->h * sc,
                   (float)t->u1, (float)t->v1, (float)t->u2, (float)t->v2, 1.0f, 1.0f, blue, 1.0f);
}

static void crDigit(int d, float cx, float cy) {
    if (d < 0) d = 0;
    if (d > 9) d = 9;
    int u = 151 + 21 * (d % 5), v = 1 + 21 * (d / 5);
    CrTile t = { 0, 0, 20, 20, u, v, u + 20, v + 20 };
    crTile(s_texCred, &t, cx, cy, 20.0f, 20.0f, 1.0f, 1.0f);
}

void NxCredit_Draw(void) {
    if (g_game.state == STATE_SERVICE_MENU) return;
    /* pedido do usuário: a capa TITLE (STATE_SONG_TITLE/_OUT) cobre a tela inteira,
     * sem CREDIT / FREE PLAY / EVENT por cima */
    if (Loading_IsActive()) return;
    if (!crLoad()) return;

    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
    glOrtho(0, 640, 0, 480, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    /* sem teste de profundidade: o gameplay pode deixar ligado e as setas ficavam por
     * cima do DEMO PLAY (no NX2 o IconDraw vem depois das setas, playengine.cpp:2208) */
    glPushAttrib(GL_ENABLE_BIT | GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (g_demoPlay && g_game.state == STATE_GAMEPLAY) {   /* DrawDemoPlay */
        crTile(s_texText, &k_demo, 128.0f, 460.0f, 240.0f, 44.0f, 0.8f, 0.75f);
        crTile(s_texText, &k_demo, 512.0f, 460.0f, 240.0f, 44.0f, 0.8f, 0.75f);
    }

    if (g_game.svcGameMode == 1) {                      /* EVENT */
        crTile(s_texText, &k_event, 320.0f, 460.0f, 120.0f, 26.0f, 1.0f, 1.0f);
    } else if (g_game.svcCoin1 == 0) {                  /* FREE PLAY */
        crTile(s_texText, &k_free, 322.0f, 463.0f, 120.0f, 26.0f, 1.0f, 1.0f);
    } else {
        int credit = g_game.svcCoinTotal / g_game.svcCoin1;
        int tail = g_game.svcCoinTotal % g_game.svcCoin1;
        if (credit > 99) credit = 99;
        const CrTile* base = (credit < 10) ? k_cred01 : k_cred02;
        for (int t = 4; t >= 0; t--) crTile(s_texCred, &base[t], 322.0f, 463.0f, 150.0f, 20.0f, 1.0f, 1.0f);
        if (credit < 10) {                              /* "creidit _0" */
            crDigit(credit, 334.0f, 463.0f);
            crDigit(tail, 354.0f, 463.0f);
            crDigit(g_game.svcCoin1, 374.0f, 463.0f);
        } else {                                        /* "creidit _00" */
            crDigit(credit / 10, 327.0f, 463.0f);
            crDigit(credit % 10, 337.0f, 463.0f);
            crDigit(tail, 357.0f, 463.0f);
            crDigit(g_game.svcCoin1, 378.0f, 463.0f);
        }
    }

    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glPopAttrib();
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW); glPopMatrix();
}
