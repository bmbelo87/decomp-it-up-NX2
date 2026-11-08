/*
 * nx2_select.c — Arcade Station / Special Zone do NX2 (CSelect, nx2src ProcModules/select.cpp). MARCOS 1 e 2.
 *
 * Feito: carga das 8 BGAs, intro/hold, carrossel de capas (EYE_SMALL.DAT) com os discos em perspectiva (rotações Y/X dos
 * keyframes, bga.c), canais (painel/seleção/brilho), movimento com DL/DR (repetição e aceleração), contagem de tempo, sons de
 * navegação e de canal, PRÉVIA: vídeo BGA/PREVIEW/%03X.MOV dentro do disco central + AUDIO/INTRO/%03X.AUD (ENC1), iniciada com a
 * seleção parada e sem botão apertado, parada ao mover (a música-base é usada se faltar o arquivo).
 * NÃO feito ainda (marcos seguintes): painéis de dificuldade e troca de modo (UL/UR), recorde, mileage/trava, ícones de comando
 * (USER_COMMAND), CENTER para jogar, fim por tempo, texto do título/BPM, sons de modo (B12..B16, A_MODE, A_SELECT, A_START).
 *
 * BGAs (ordem de carga = índices): 0 ARCADE_SPECIAL_A, 1 _B, 2 _C, 3 _D, 4 LEVEL_A, 5 LEVEL_B, 6 LEVEL_C, 7 USER_COMMAND.
 * Fundo: BGA/ARCADE_BG.MOV (Arcade) ou SPECIAL_BG.MOV. Camadas do carrossel (BGA B): tabela L_* do select.cpp:
 *   capa (TEX) / base (SBASE) / trava (SLOCK) de cada posição: L01 1/0/2, L02 4/3/5, L03 7/6/8, L04A 10/9/11,
 *   C 25/24/27, R04A 22/21/23, R03 19/18/20, R02 16/15/17, R01 13/12/14, L04B 29/28/31, R04B 33/32/35.
 * Capas: EYE_SMALL.DAT "%03X.dds"; se faltar, a da música-base (g_nxResMap).
 * Lista: GetEnabledSongList (nx_select.c listAdd): sem censuradas/ocultas, canais 0..3 no Arcade, 4..6 no Special.
 * Entradas (select.cpp:1759): DL = música anterior, DR = próxima (segurar repete, acelera até +30 por passo a 720),
 *   UL/UR = modo (marco 3), CENTER = selecionar/jogar (marco 4).
 * Sons (SFX_SELECT.LUA): 3-2 mover, ACC mover rápido, B08..B11 canal NX/K-POP/POP/BANYA, CHGMOD canal (Special).
 */
#include "pumpy.h"
#include "bga.h"
#include "movie.h"

enum { UA = 0, UB = 1, UC = 2, UD = 3, LA = 4, LB = 5, LC = 6, UCMD = 7 };
enum { SM_IN = 0, SM_NONE, SM_LEFT, SM_RIGHT };
enum { SN_MOVE, SN_ACC, SN_CH0, SN_CH1, SN_CH2, SN_CH3, SN_CHGMOD, SN_MODE, SN_SELSTART, SN_START, SN_WRONG, SN_HIDDEN, SN_COUNT };
static const char* const k_wav[SN_COUNT] = { "3-2.WAV", "ACC.WAV", "B08.WAV", "B09.WAV", "B10.WAV", "B11.WAV", "CHGMOD.WAV", "A_MODE.WAV", "A_START.WAV", "START.WAV", "WRONG.WAV", "CMD_IN.WAV" };
static int s_snd[SN_COUNT];
static bool s_sndInit;
static void snd(int k) { if (s_snd[k] >= 0) Audio_Play(s_snd[k], false); }

/* camadas da BGA B (select.cpp L_*) */
enum { L_TEX_L01 = 1, L_TEX_L02 = 4, L_TEX_L03 = 7, L_TEX_L04A = 10, L_TEX_R01 = 13, L_TEX_R02 = 16, L_TEX_R03 = 19,
       L_TEX_R04A = 22, L_TEX_C = 25, L_TEX_L04B = 29, L_TEX_R04B = 33 };
enum { L_SBASE_L01 = 0, L_SBASE_L02 = 3, L_SBASE_L03 = 6, L_SBASE_L04A = 9, L_SBASE_R01 = 12, L_SBASE_R02 = 15, L_SBASE_R03 = 18,
       L_SBASE_R04A = 21, L_SBASE_C = 24, L_SBASE_L04B = 28, L_SBASE_R04B = 32 };
enum { L_SLOCK_L01 = 2, L_SLOCK_L02 = 5, L_SLOCK_L03 = 8, L_SLOCK_L04A = 11, L_SLOCK_R01 = 14, L_SLOCK_R02 = 17, L_SLOCK_R03 = 20,
       L_SLOCK_R04A = 23, L_SLOCK_CA = 27, L_SLOCK_L04B = 31, L_SLOCK_CC = 35, L_SLOCK_R04B = 35, L_SLOCK_CD = 39 };
#define L_SBASE_NONE 96
#define L_TEX_NONE   98
#define L_LOCK_NONE  99
#define L_PANNEL_CH01 15
#define L_PANNEL_CH02 16
#define L_LEVEL_GLOW_CH01 91
#define L_LEVEL_GLOW_CH02 92

static bool    s_ok, s_special, s_sndOk;
static int     s_phase;            /* 0 intro, 1 execução */
static int     s_state;            /* m_iCurrentScreenState */
static int     s_list[SONG_TABLE_COUNT], s_count, s_cur;
static const char* s_scene;        /* m_iScenename */
static int     s_channel, s_prevChannel;
static int     s_acc = 500;
static bool    s_arrowHold;
static uint32_t s_holdT[2][2];
static unsigned s_active;
static BGALayerSrc s_chScreen[7], s_chSel[7], s_chPanel[2][7], s_chGlow[7], s_lock[3];   /* CLOSE, OPEN, NONE */
enum { LK_CLOSE = 0, LK_OPEN, LK_NONE };
static int     s_privTile[100];    /* ladrilho privado por camada de capa (-1 = não criado) */
static BGALayerSrc s_cSrc[2];      /* camada 25 (centro): [0] normal, [1] sprite da camada 68 usado durante o movimento */
static int     s_cTile[2];         /* ladrilhos privados correspondentes */
static bool    s_ph;               /* true enquanto reorganize() monta um movimento (LEFT/RIGHT) */
static SPRTileDef s_cTileOrig;     /* ladrilho original do centro (a prévia o troca pelo do movie.spr) */
static BGALayerSrc s_movieSrc;     /* camada 48 da BGA B: sprite do vídeo (m_pMovieTex) */
static int     s_movieTex = -1;    /* textura embrulhando o quadro do vídeo */
static bool    s_previewOn;
static const char* s_arrowUp = "arrow_up_left", *s_arrowLow = "arrow_down_left";   /* m_iCurrentArrowID[UPPER/LOWER] */
/* modos e níveis (marco 3) */
#define N_MODES 5                  /* NORMAL HARD CRAZY FREESTYLE NIGHTMARE (mesma ordem de TrackEntry.level) */
static int     s_mode[2], s_userMode[2], s_lv[2], s_lvIdx[2], s_lvState[2];   /* m_iCurrentMode/UserMode/CurrentLV/LVSceneName/LVState */
static BGALayerSrc s_modeSingle[N_MODES], s_modeDual[2][N_MODES];
static char    s_lvNames[3][130][64];                      /* LEVEL_A/B/C: nomes das cenas SEM os separadores "---" */
static int     s_lvCount[3];
static BGALayerSrc s_exSrc[2][10]; /* m_pExerciseSpr: dígitos de kcal [0] e VO2 [1] (camadas 78+j+11*i da BGA C) */
static int     s_rankTex[2] = { -1, -1 };   /* CRFont: a_font_0 (512x64) / a_font_1 (256x32) */
static int     s_rankCnt;          /* m_RankFontCnt */
static float   s_rankAlpha;        /* m_fRankFontAlpha */
static bool    s_selected;         /* m_bSelected: CENTER já pressionado (o próximo inicia) */
static bool    s_end, s_endStarted;        /* m_bEnd / SetEnd já executado */
static int     s_endCnt;           /* m_Cnt */
static int     s_prevSong = -1;    /* índice (da tabela) da música cuja prévia está carregada/pedida */

extern unsigned Title_GetJoinedMask(void);
int  Texture_Wrap(int slot, unsigned glId, int w, int h);   /* texture.c */
void Texture_Unwrap(int slot);
static void validateMode(void);
static void setExerciseInfo(void);
static void cmdSessionInit(void);

/* ---- lista (GetEnabledSongList) -------------------------------------------------------------- */
static bool twoPlayers(void) { return (s_active & 3) == 3; }

static void buildList(void)
{
    s_count = 0;
    for (int i = 0; i < SONG_TABLE_COUNT; i++) {
        const TrackEntry* e = &g_songTable[i];
        if (g_nx2SongExtra[i].censor || g_nx2SongExtra[i].hiddenPlay) continue;
        bool sp = (e->channel & 4) != 0;
        if (s_special ? !sp : sp) continue;
        bool ok = e->level[0] >= 0 || e->level[1] >= 0 || e->level[2] >= 0;
        if (!ok && !twoPlayers()) ok = e->level[3] >= 0 || e->level[4] >= 0;
        if (ok) s_list[s_count++] = i;
    }
}

static int idx(int off)     /* GetIndex: posição relativa circular na lista */
{
    int i = (s_cur + off) % s_count;
    if (i < 0) i += s_count;
    return i;
}

/* ---- capas (EYE_SMALL.DAT) -------------------------------------------------------------------- */
#define COVER_CACHE 48
static struct { int id; int tex; uint32_t used; } s_cover[COVER_CACHE];
static uint32_t s_coverClock;

static int loadCoverFile(int id)
{
    char dat[MAX_PATH], name[32];
    snprintf(dat, sizeof(dat), "%s/BGA/EYE_SMALL.DAT", g_game.currentDirectory);
    snprintf(name, sizeof(name), "%03X.dds", (unsigned)id);
    uint32_t sz = 0;
    uint8_t* d = Resource_ExtractFromPack(dat, name, &sz);
    if (!d) return -1;
    int tex = Texture_LoadFromMemory(d, sz, "cover.dds");
    free(d);
    return tex;
}

static int coverTex(int songId)
{
    for (int i = 0; i < COVER_CACHE; i++)
        if (s_cover[i].tex >= 0 && s_cover[i].id == songId) { s_cover[i].used = ++s_coverClock; return s_cover[i].tex; }
    int tex = loadCoverFile(songId);
    if (tex < 0) {                                        /* GetParentTrack: música-base (mapa recurso->chart) */
        int base = Song_BaseId(songId);
        if (base >= 0) tex = loadCoverFile(base);
    }
    int slot = 0;
    for (int i = 0; i < COVER_CACHE; i++) {
        if (s_cover[i].tex < 0 && s_cover[i].id == 0) { slot = i; break; }
        if (s_cover[i].used < s_cover[slot].used) slot = i;
    }
    if (s_cover[slot].tex >= 0) Texture_Unload(s_cover[slot].tex);
    s_cover[slot].id = songId; s_cover[slot].tex = tex; s_cover[slot].used = ++s_coverClock;
    return tex;
}

/* uma cópia privada do ladrilho SPR por camada de capa, para cada uma ter a sua textura (CSprite::Pic[0].TexID) */
static void makePrivate(int slot)
{
    BGALayerSrc src;
    s_privTile[slot] = -1;
    if (!BGA_GetLayerSrc(UB, slot, &src) || !src.isSPR || src.sprTileCount < 1 || src.sprTileStart < 0) return;
    if (g_game.sprTileCount >= MAX_SPR_TILES) return;
    int n = g_game.sprTileCount++;
    g_game.sprTiles[n] = g_game.sprTiles[src.sprTileStart];
    s_privTile[slot] = n;
    src.sprTileStart = n; src.sprTileCount = 1;
    BGA_SetLayerSrc(UB, slot, &src);
}

static void setCover(int slot, int songIdx)
{
    int tile = (slot == L_TEX_C) ? s_cTile[s_ph ? 1 : 0] : s_privTile[slot];
    if (tile < 0) return;
    int tex = coverTex(g_songTable[songIdx].id);
    if (tex >= 0) g_game.sprTiles[tile].texId = tex;
}

/* ---- SetScreen / ReorganizeScene --------------------------------------------------------------- */
static void setScreen(int texL, int baseL, int lockL, int listIdx)
{
    const TrackEntry* e = &g_songTable[s_list[listIdx]];
    if (texL != L_TEX_NONE && baseL != L_SBASE_NONE) {
        setCover(texL, s_list[listIdx]);
        BGA_SetLayerSrc(UB, baseL, &s_chScreen[e->channel % 7]);
    }
    BGA_SetLayerSrc(UB, lockL, &s_lock[LK_NONE]);          /* sem músicas travadas neste marco (bEnable_play = 1) */
}

static void reorganize(int mode)
{
    if (s_count <= 0) return;
    s_ph = (mode == SM_LEFT || mode == SM_RIGHT);
    if (s_cTile[0] >= 0) g_game.sprTiles[s_cTile[0]] = s_cTileOrig;     /* sai do ladrilho do vídeo (se estava) */
    if (s_cTile[s_ph ? 1 : 0] >= 0) BGA_SetLayerSrc(UB, L_TEX_C, &s_cSrc[s_ph ? 1 : 0]);   /* SetLayerSprite(L_TEX_C, camada 68) */
    switch (mode) {
    case SM_IN: case SM_NONE:
        setScreen(L_TEX_L02,  L_SBASE_L02,  L_SLOCK_L02,  idx(-3));
        setScreen(L_TEX_L03,  L_SBASE_L03,  L_SLOCK_L03,  idx(-2));
        setScreen(L_TEX_L04A, L_SBASE_L04A, L_SLOCK_L04A, idx(-1));
        setScreen(L_TEX_NONE, L_SBASE_NONE, L_SLOCK_L04B, idx(-1));
        setScreen(L_TEX_C,    L_SBASE_C,    L_SLOCK_CA,   idx(0));
        setScreen(L_TEX_R04A, L_SBASE_R04A, L_SLOCK_R04A, idx(1));
        setScreen(L_TEX_NONE, L_SBASE_NONE, L_SLOCK_R04B, idx(1));
        setScreen(L_TEX_R03,  L_SBASE_R03,  L_SLOCK_R03,  idx(2));
        setScreen(L_TEX_R02,  L_SBASE_R02,  L_SLOCK_R02,  idx(3));
        break;
    case SM_RIGHT:
        setScreen(L_TEX_L02,  L_SBASE_L02,  L_SLOCK_L02,  idx(-4));
        setScreen(L_TEX_L03,  L_SBASE_L03,  L_SLOCK_L03,  idx(-3));
        setScreen(L_TEX_L04A, L_SBASE_L04A, L_SLOCK_L04A, idx(-2));
        setScreen(L_TEX_NONE, L_SBASE_NONE, L_SLOCK_L04B, idx(-2));
        setScreen(L_TEX_C,    L_SBASE_C,    L_SLOCK_CA,   idx(-1));
        setScreen(L_TEX_NONE, L_SBASE_NONE, L_LOCK_NONE,  idx(-1));
        setScreen(L_TEX_R04A, L_SBASE_R04A, L_SLOCK_R04A, idx(0));
        setScreen(L_TEX_R04B, L_SBASE_R04B, L_SLOCK_R04B, idx(0));
        setScreen(L_TEX_R03,  L_SBASE_R03,  L_SLOCK_R03,  idx(1));
        setScreen(L_TEX_R02,  L_SBASE_R02,  L_SLOCK_R02,  idx(2));
        setScreen(L_TEX_R01,  L_SBASE_R01,  L_SLOCK_R01,  idx(3));
        break;
    case SM_LEFT:
        setScreen(L_TEX_L01,  L_SBASE_L01,  L_SLOCK_L01,  idx(-3));
        setScreen(L_TEX_L02,  L_SBASE_L02,  L_SLOCK_L02,  idx(-2));
        setScreen(L_TEX_L03,  L_SBASE_L03,  L_SLOCK_L03,  idx(-1));
        setScreen(L_TEX_L04A, L_SBASE_L04A, L_SLOCK_L04A, idx(0));
        setScreen(L_TEX_L04B, L_SBASE_L04B, L_SLOCK_L04B, idx(0));
        setScreen(L_TEX_C,    L_SBASE_C,    L_SLOCK_CA,   idx(1));
        setScreen(L_TEX_NONE, L_SBASE_NONE, L_LOCK_NONE,  idx(1));
        setScreen(L_TEX_R04A, L_SBASE_R04A, L_SLOCK_R04A, idx(2));
        setScreen(L_TEX_NONE, L_SBASE_NONE, L_SLOCK_R04B, idx(2));
        setScreen(L_TEX_R03,  L_SBASE_R03,  L_SLOCK_R03,  idx(3));
        setScreen(L_TEX_R02,  L_SBASE_R02,  L_SLOCK_R02,  idx(4));
        break;
    }
}

static void setSceneAnimation(void)
{
    switch (s_state) {
    case SM_LEFT:
        if (s_acc >= 740) { s_scene = "song_move_left (high speed)"; return; }
        s_scene = "song_move_left"; BGA_SceneReset(UB, s_scene); break;
    case SM_RIGHT:
        if (s_acc >= 740) { s_scene = "song_move_right (high speed)"; return; }
        s_scene = "song_move_right"; BGA_SceneReset(UB, s_scene); break;
    default:
        s_scene = "song_hold"; BGA_SceneReset(UB, s_scene); break;
    }
}

/* SetBoardCH: painel/seleção/brilho do canal da música atual e anúncio ao mudar de canal */
static void setBoardCH(bool force)
{
    int player = twoPlayers() ? 1 : 0;
    const TrackEntry* e = &g_songTable[s_list[s_cur]];
    s_prevChannel = s_channel;
    s_channel = e->channel % 7;
    if (s_prevChannel != s_channel || force) {
        BGA_SetLayerSrc(UA, L_PANNEL_CH01, &s_chPanel[player][s_channel]);
        BGA_SetLayerSrc(UA, L_PANNEL_CH02, &s_chPanel[player][s_channel]);
        static const int k_ssel[4] = { 26, 30, 34, 38 };
        for (int i = 0; i < 4; i++) BGA_SetLayerSrc(UB, k_ssel[i], &s_chSel[s_channel]);
        BGA_SetLayerSrc(UD, L_LEVEL_GLOW_CH01, &s_chGlow[s_channel]);
        BGA_SetLayerSrc(UD, L_LEVEL_GLOW_CH02, &s_chGlow[s_channel]);
        BGA_SceneReset(UB, s_special ? "special_channel_change_b" : "arcade_channel_change_b");
    }
    if (s_prevChannel != s_channel) {
        if (s_channel <= 3) snd(SN_CH0 + s_channel);
        if (s_special) snd(SN_CHGMOD);
    }
}

/* ---- Begin ------------------------------------------------------------------------------------ */
/* volta da Select depois de um estágio: o cursor (e as dificuldades) ficam na música jogada, como nas outras Pumps */
static int  s_lastId = -1, s_lastMode[2];
static bool s_lastSpecial;

void Nx2Select_Enter(void)
{
    s_special = (g_game.nxGameMode == 1);
    s_active = Title_GetJoinedMask() & 3;
    if (!s_active) s_active = 1;
    if (getenv("PUMPY_JOIN")) s_active = atoi(getenv("PUMPY_JOIN")) & 3;   /* teste: força os jogadores da Select */
    Title_StopMusic();
    BGM_Stop();
    Movie_Select(1); Movie_Close(); Movie_Select(0);
    Movie_Close();
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/BGA/%s.MOV", g_game.currentDirectory, s_special ? "SPECIAL_BG" : "ARCADE_BG");
    Movie_Open(path, true);

    Resource_ClearBGA();
    static const char* const k_bga[8] = { "ARCADE_SPECIAL_A", "ARCADE_SPECIAL_B", "ARCADE_SPECIAL_C", "ARCADE_SPECIAL_D",
                                          "LEVEL_A", "LEVEL_B", "LEVEL_C", "USER_COMMAND" };
    s_ok = true;
    for (int i = 0; i < 8; i++)
        if (!Resource_LoadBGAByName(k_bga[i])) { Log_Print("NX2SELECT: falha ao carregar BGA\\%s.DAT\n", k_bga[i]); s_ok = false; }
    BGA_Reset();

    if (!s_sndInit) {
        for (int k = 0; k < SN_COUNT; k++) s_snd[k] = Audio_LoadWaveFile(k_wav[k]);
        s_sndInit = true;
    }
    {   /* CRFont("BGA/H_FONT_USB.DAT" ou "BGA/H_FONT_NOUSB.DAT") */
        char dp[MAX_PATH];
        snprintf(dp, sizeof(dp), "%s/BGA/%s.DAT", g_game.currentDirectory,
                 (g_nx2HasProfile[0] || g_nx2HasProfile[1]) ? "H_FONT_USB" : "H_FONT_NOUSB");
        if (!RES_Open(dp)) Log_Print("NX2SELECT: falha ao abrir %s\n", dp);
        else {
            s_rankTex[0] = loadTextureFromRES("a_font_0.tga");
            s_rankTex[1] = loadTextureFromRES("a_font_1.tga");
            RES_Close();
        }
    }
    s_rankCnt = 0; s_rankAlpha = 0.f;
    memset(s_cover, 0, sizeof(s_cover));
    for (int i = 0; i < COVER_CACHE; i++) s_cover[i].tex = -1;
    for (int i = 0; i < 100; i++) s_privTile[i] = -1;

    buildList();
    if (s_count <= 0) { Log_Print("NX2SELECT: lista vazia\n"); s_ok = false; }
    if (s_ok) {
        for (int i = 0; i < 7; i++) {
            s_ok &= BGA_GetLayerSrc(UB, 41 + i, &s_chScreen[i]);
            s_ok &= BGA_GetLayerSrc(UB, 49 + i, &s_chSel[i]);
            s_ok &= BGA_GetLayerSrc(UD, 93 + i, &s_chGlow[i]);
            for (int j = 0; j < 2; j++) s_ok &= BGA_GetLayerSrc(UA, 17 + j * 7 + i, &s_chPanel[j][i]);
        }
        s_ok &= BGA_GetLayerSrc(UB, 57, &s_lock[LK_CLOSE]);
        s_ok &= BGA_GetLayerSrc(UB, 56, &s_lock[LK_OPEN]);
        s_ok &= BGA_GetLayerSrc(UB, 58, &s_lock[LK_NONE]);
        if (!s_ok) Log_Print("NX2SELECT: alguma camada de origem não existe\n");
        static const int k_tex[] = { L_TEX_L01, L_TEX_L02, L_TEX_L03, L_TEX_L04A, L_TEX_R01, L_TEX_R02, L_TEX_R03,
                                     L_TEX_R04A, L_TEX_C, L_TEX_L04B, L_TEX_R04B };
        for (size_t i = 0; i < sizeof(k_tex) / sizeof(k_tex[0]); i++) makePrivate(k_tex[i]);
        /* centro: ladrilho normal (já privado) e uma cópia privada do sprite da camada 68 */
        s_cTile[0] = s_privTile[L_TEX_C]; s_cTile[1] = -1;
        if (s_cTile[0] >= 0) s_cTileOrig = g_game.sprTiles[s_cTile[0]];
        s_ok &= BGA_GetLayerSrc(UB, 48, &s_movieSrc);
        for (int i = 0; i < N_MODES; i++) {
            s_ok &= BGA_GetLayerSrc(UC, i + 1, &s_modeSingle[i]);
            for (int j = 0; j < 2; j++) s_ok &= BGA_GetLayerSrc(UC, (N_MODES + 2) + j * N_MODES + j + i, &s_modeDual[j][i]);
        }
        for (int k = 0; k < 3; k++) {                              /* cenas de nível sem os separadores "---" */
            s_lvCount[k] = 0;
            for (int i = 0; i < BGA_SceneCount(LA + k); i++) {
                const char* nm = BGA_SceneNameAt(LA + k, i);
                if (nm && nm[0] && nm[0] != '-' && s_lvCount[k] < 130) snprintf(s_lvNames[k][s_lvCount[k]++], 64, "%s", nm);
            }
        }
        for (int i = 0; i < 2; i++)
            for (int j = 0; j < 10; j++) BGA_GetLayerSrc(UC, 78 + j + 11 * i, &s_exSrc[i][j]);
        BGA_GetLayerSrc(UB, L_TEX_C, &s_cSrc[0]);
        BGALayerSrc s68;
        if (BGA_GetLayerSrc(UB, 68, &s68) && s68.isSPR && s68.sprTileCount >= 1 && s68.sprTileStart >= 0 && g_game.sprTileCount < MAX_SPR_TILES) {
            int n = g_game.sprTileCount++;
            g_game.sprTiles[n] = g_game.sprTiles[s68.sprTileStart];
            s68.sprTileStart = n; s68.sprTileCount = 1;
            s_cSrc[1] = s68; s_cTile[1] = n;
        }

        /* SetSceneTick(total): cenas de seta e de troca de canal começam no último quadro */
        static const char* const k_done[] = { "arrow_up_left", "arrow_up_right", "arrow_down_left", "arrow_down_right",
                                              "arrow_up_left(effacts)", "arrow_up_right(effacts)" };
        for (size_t i = 0; i < sizeof(k_done) / sizeof(k_done[0]); i++) BGA_SceneFinish(UA, k_done[i]);
        BGA_SceneFinish(UB, "arcade_channel_change_b");
        BGA_SceneFinish(UB, "special_channel_change_b");
    }

    /* música inicial: 0xE13 (Arcade sem perfil) / 0xE01 (com perfil) / a primeira (Special) */
    int ss = s_special ? 0 : ((g_nx2HasProfile[0] || g_nx2HasProfile[1]) ? 0xE01 : 0xE13);
    bool keepLast = (g_game.stageCount != 3 && g_game.svcGameMode != 1 && s_lastId >= 0 && s_lastSpecial == (bool)s_special);
    if (keepLast) ss = s_lastId;   /* 2º/3º estágio: última música jogada */
    s_cur = 0;
    for (int i = 0; i < s_count; i++) if ((int)g_songTable[s_list[i]].id == ss) { s_cur = i; break; }
    s_selected = s_end = s_endStarted = false; s_endCnt = 0;
    cmdSessionInit();
    Render_SetGlobalColor(0, 0, 0, 0);
    s_state = SM_IN; s_phase = 0; s_acc = 500; s_arrowHold = false; s_ph = false;
    s_previewOn = false; s_prevSong = -1; s_movieTex = -1;
    s_arrowUp = "arrow_up_left"; s_arrowLow = "arrow_down_left";
    s_holdT[0][0] = s_holdT[0][1] = s_holdT[1][0] = s_holdT[1][1] = 0;
    if (s_ok) {
        s_channel = g_songTable[s_list[s_cur]].channel % 7;
        s_prevChannel = s_channel;
        reorganize(SM_IN);
        setBoardCH(true);
        s_mode[0] = s_mode[1] = s_userMode[0] = s_userMode[1] = 0;      /* MODE_NORMAL */
        if (keepLast) { s_mode[0] = s_userMode[0] = s_lastMode[0]; s_mode[1] = s_userMode[1] = s_lastMode[1]; }   /* validateMode ajusta se não existir */
        if (getenv("PUMPY_P2MODE")) { s_mode[1] = s_userMode[1] = atoi(getenv("PUMPY_P2MODE")); }   /* teste: dificuldade do P2 */
        validateMode();
        setExerciseInfo();
        BGA_SceneReset(UA, s_special ? "special_inter_a" : "arcade_intro_a");
        BGA_SceneReset(UB, s_special ? "special_intro_b" : "arcade_intro_b");
        BGA_SceneReset(UC, "base_intro_c");
        s_scene = "song_hold";
        BGA_SceneReset(UB, s_scene);
        BGA_SceneSetOffset(UA, "time_99", 90 * 39);        /* SetSceneTick(90 * 39) */
    }
    Log_Print("NX2SELECT: %s, %d músicas, inicial %s (índice %d), jogadores 0x%X\n",
              s_special ? "SPECIAL ZONE" : "ARCADE STATION", s_count, s_ok ? Song_IdStr((int)g_songTable[s_list[s_cur]].id) : "?", s_cur, s_active);
}

/* ---- modos e níveis (ValidateMode / SetCurrentMode / SetCurrentLV / DrawLV, select.cpp) ------------- */
static bool validMode(int m)       /* valid(m): nível existe, modo habilitado (EnableMode) e, com 2P, só até CRAZY */
{
    const TrackEntry* e = &g_songTable[s_list[s_cur]];
    return e->level[m] >= 0 && e->lock[m] != 0 && (!twoPlayers() || m <= 2);
}

static int mmax(void) { return twoPlayers() ? 3 : N_MODES; }

static int nextMode(int p, int dir)       /* GetNextMode / GetPrevMode */
{
    int q = twoPlayers() ? p : 0, n = mmax();
    for (int b = 1; b < n; b++) {
        int m = (s_mode[q] + n + dir * b) % n;
        if (validMode(m)) return m;
    }
    return s_mode[q];
}

/* id (1-based, sem separadores) de uma cena de nível -> nome */
static const char* lvName(int bgaK, int id) { return (id >= 1 && id <= s_lvCount[bgaK]) ? s_lvNames[bgaK][id - 1] : NULL; }

static int lvBga(int lv) { return lv <= 8 ? 0 : (lv < 17 ? 1 : 2); }

static const char* lvScene(int p, int state)      /* m_iLVSingle / m_iLV1p / m_iLV2p [state][m_iLVSceneName] */
{
    int i = s_lvIdx[p], count = i % 16, step = i / 16, k = lvBga(s_lv[p]);
    int base = !twoPlayers() ? 1 : (p == 0 ? (step == 2 ? 19 : 33) : (step == 2 ? 37 : 65));
    return lvName(k, base + count * 2 + state);
}

static void setCurrentLV(int p)
{
    int q = twoPlayers() ? p : 0;
    const TrackEntry* e = &g_songTable[s_list[s_cur]];
    int lv = e->level[s_mode[q]];
    if (lv < 1) lv = 1;
    s_lv[q] = lv;
    s_lvIdx[q] = lv < 17 ? 2 * (lv - 1) : (lv < 25 ? lv + 16 - 1 : 40);
    s_lvState[q] = 0;
    const char* n = lvScene(q, 0);
    if (n) BGA_SceneReset(LA + lvBga(lv), n);
}

static void setCurrentMode(int p, int mode)
{
    if (twoPlayers()) {
        s_mode[p] = mode;
        BGA_SetLayerSrc(UC, 6 + p * 6, &s_modeDual[p][mode]);        /* L_MODE_1P / L_MODE_2P */
        BGA_SceneReset(UC, p == 0 ? "degree of difficulty_dual_1p_in" : "degree of difficulty_dual_2p_in");
    } else {
        s_mode[0] = mode;
        BGA_SceneReset(UC, "degree of difficulty_single_in");
        BGA_SetLayerSrc(UC, 0, &s_modeSingle[mode]);                 /* L_MODE_SINGLE */
    }
    setCurrentLV(p);
}

static void validateMode(void)
{
    int c = twoPlayers() ? 2 : 1, n = mmax();
    for (int a = 0; a < c; a++) {
        if (s_mode[a] != s_userMode[a] && validMode(s_userMode[a])) s_mode[a] = s_userMode[a];
        else if (!validMode(s_mode[a])) {
            s_mode[a] = 0;
            for (int b = 0; b < n; b++) if (validMode(b)) { s_mode[a] = b; break; }
        }
        setCurrentMode(a, s_mode[a]);
    }
}

/* GetExpectedKcal / SetExerciseInfo (select.cpp): kcal e VO2 esperados da música no modo atual -> dígitos 66..71 (+6 por jogador) */
static void setExerciseInfo(void)
{
    const TrackEntry* e = &g_songTable[s_list[s_cur]];
    int players = twoPlayers() ? 2 : 1;
    for (int i = 0; i < players; i++) {
        float cont = (s_mode[i] == 2) ? 0.931f : 0.f;                        /* MODE_CRAZY */
        int level = e->level[s_mode[i]];
        if (level == 0) level = 1;
        if (level > 30) level = 30;
        int kcal = (int)(((int)e->length / 60.f) * (7.414f + (0.475f * level) + cont));
        int vo2  = (int)(((int)e->length / 60.f) * (19.829f + (1.335f * level) + cont));
        BGA_SetLayerSrc(UC, 68 + i * 6, &s_exSrc[0][kcal % 10]); kcal /= 10;
        BGA_SetLayerSrc(UC, 67 + i * 6, &s_exSrc[0][kcal % 10]); kcal /= 10;
        BGA_SetLayerSrc(UC, 66 + i * 6, &s_exSrc[0][kcal % 10]);
        BGA_SetLayerSrc(UC, 71 + i * 6, &s_exSrc[1][vo2 % 10]); vo2 /= 10;
        BGA_SetLayerSrc(UC, 70 + i * 6, &s_exSrc[1][vo2 % 10]); vo2 /= 10;
        BGA_SetLayerSrc(UC, 69 + i * 6, &s_exSrc[1][vo2 % 10]);
    }
}

/* CRFont::Printf (wfont.cpp): centralizado por strlen(fmt) (não do texto), avanço de 10 px, célula 22x22; y = base em Y para cima.
 * cnvTable (bfont.cpp): A-Z 0-9 ~ ! @ # $ % ^ & * ( ) _ ; wfont só tem 46 células (2 linhas de 23). */
static void rankPrintf(float x, float y, const char* fmt, const char* text, float alpha)
{
    static const char k_cnv[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789~!@#$%^&*()_";
    float px = x - (float)((10 * (int)strlen(fmt)) / 2) - 8.f;
    for (const char* c = text; *c; c++, px += 10.f) {
        const char* f = *c ? strchr(k_cnv, *c) : NULL;
        int val = f ? (int)(f - k_cnv) : -1;
        if (val < 0 || val >= 46 || s_rankTex[0] < 0) continue;              /* ' ' e desconhecidos: _CnvCharToValue = BLOCK_MAX */
        float u = (float)((val % 23) * 22), v = (float)((val / 23) * 22);
        Texture_DrawUV(s_rankTex[0], px, 480.f - (y + 22.f), 22.f, 22.f, u, v, u + 22.f, v + 22.f, 1.f, 1.f, 1.f, alpha);
    }
}

/* DrawScore (select.cpp): painéis de recorde (BGA D) e kcal/VO2 (BGA C) + apelido e pontuação do melhor registro */
static void drawScore(void)
{
    int si = s_list[s_cur];
    if (!g_songTable[si].avail) return;                                       /* bEnable_play */
    if (twoPlayers()) {
        BGA_ScenePlay(UD, "dual_high_score_in", true);
        BGA_ScenePlay(UC, "dual_1p_kcal-vo2(all)", true);
        BGA_ScenePlay(UC, "dual_2p_kcal-vo2(all)", true);
    } else {
        BGA_ScenePlay(UD, "single_high_score_in", true);
        BGA_ScenePlay(UC, "single_kcal-vo2(all)", true);
    }
    if (s_rankCnt >= 20) {
        s_rankAlpha = (float)(s_rankCnt - 20) / 20;
        if (s_rankAlpha > 1) s_rankAlpha = 1;
    }
    int mode = s_mode[0];
    int highscore = g_nx2Opt.m_Stat.Record[si][mode].Score;
    char nick[NX2_ID_MAX + 1];
    memset(nick, 0, sizeof(nick));
    memcpy(nick, g_nx2Opt.m_Stat.Record[si][mode].Name, NX2_ID_MAX);
    for (int a = 0; a < 2; a++) {
        if ((s_active & (1u << a)) && g_nx2HasProfile[a]) {
            const Nx2Stat* st = Nx2_Stat(a);
            if (st->Record[si][mode].Score >= highscore) {
                if (st->Record[si][mode].Score > highscore || !nick[0]) memcpy(nick, st->Record[si][mode].Name, NX2_ID_MAX);
                highscore = st->Record[si][mode].Score;
            }
        }
    }
    rankPrintf(320.f, 480.f - 315.f, nick, nick, s_rankAlpha);
    if (highscore > 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", highscore);
        rankPrintf(320.f, 480.f - 333.f, "%d", buf, s_rankAlpha);
    }
    s_rankCnt++;
}

static void drawLV(void)
{
    int c = twoPlayers() ? 2 : 1;
    for (int p = 0; p < c; p++) {
        int k = lvBga(s_lv[p]);
        const char* in = lvScene(p, 0);
        if (s_lvState[p] == 0 && in && BGA_SceneDone(LA + k, in)) s_lvState[p] = 1;      /* LV_IN -> LV_LOOP */
        const char* n = lvScene(p, s_lvState[p]);
        if (n) BGA_ScenePlay(LA + k, n, true);
    }
}

/* ---- prévia (LoadIntro, select.cpp:644): vídeo no disco central + áudio de intro ------------------ */
static bool findFile(int id, const char* fmt, char* out, size_t sz)
{
    for (int g = 0; id != -1 && g < 8; g++) {              /* música, depois a base (GetParentTrack) */
        snprintf(out, sz, fmt, g_game.currentDirectory, (unsigned)id);
        FILE* f = fopen(out, "rb");
        if (f) { fclose(f); return true; }
        id = Song_BaseId(id);
    }
    return false;
}

static void previewStop(void)
{
    if (!s_previewOn) return;
    s_previewOn = false;
    Movie_Select(1); Movie_Close(); Movie_Select(0);
    BGM_Stop();
    if (s_movieTex >= 0) { Texture_Unwrap(s_movieTex); s_movieTex = -1; }
}

static void previewStart(void)
{
    int id = (int)g_songTable[s_list[s_cur]].id;
    char mov[MAX_PATH], aud[MAX_PATH];
    if (!findFile(id, "%s/BGA/PREVIEW/%03X.MOV", mov, sizeof(mov))) {
        snprintf(mov, sizeof(mov), "%s/BGA/PREVIEW/001.MOV", g_game.currentDirectory);
        FILE* pf = fopen(mov, "rb");
        if (pf) fclose(pf); else snprintf(mov, sizeof(mov), "%s/BGA/001.MOV", g_game.currentDirectory);
    }
    Movie_Select(1);
    Movie_Open(mov, true);
    Movie_Select(0);
    if (findFile(id, "%s/AUDIO/INTRO/%03X.AUD", aud, sizeof(aud)) && BGM_LoadAUDDirect(aud)) BGM_Play(false);
    s_previewOn = true;
    s_prevSong = s_list[s_cur];
}

static bool anyPadDown(void)                              /* isButtonPressed */
{
    static const PadButton b[5] = { PAD_UL, PAD_UR, PAD_C, PAD_DL, PAD_DR };
    for (int p = 0; p < 2; p++)
        if (s_active & (1u << p))
            for (int k = 0; k < 5; k++) if (Input_IsPadDown(p, b[k])) return true;
    return false;
}

/* o quadro do vídeo vira o disco central (movie.spr foi feito p/ 256x256 com o vídeo 256x192 no topo) */
static void previewUpdate(float dt)
{
    Movie_Select(1);
    Movie_Update(dt);
    unsigned gl = Movie_GLTexture();
    Movie_Select(0);
    if (!gl || s_cTile[0] < 0 || s_movieSrc.sprTileCount < 1 || s_movieSrc.sprTileStart < 0) return;
    s_movieTex = Texture_Wrap(s_movieTex, gl, 256, 192);
    if (s_movieTex < 0) return;
    SPRTileDef* t = &g_game.sprTiles[s_cTile[0]];
    *t = g_game.sprTiles[s_movieSrc.sprTileStart];
    t->texId = s_movieTex;
    t->v1 *= 256.0f / 192.0f;
    t->v2 *= 256.0f / 192.0f;
}

/* ---- entrada ---------------------------------------------------------------------------------- */
/* extra de teste: PUMPY_SELKEY="DL:300,UR:420" dispara o botão (UL UR DL DR) do P1 no quadro indicado do estado */
static bool dbgKey(PadButton b)
{
    static const char* const nm[5] = { "UL", "UR", "C ", "DL", "DR" };
    const char* e = getenv("PUMPY_SELKEY");
    while (e && *e) {
        char name[3] = { e[0], e[1], 0 };
        int fr = atoi(e + 3);
        if ((int)b >= 0 && (int)b <= 4 && !strcmp(name, nm[b]) && g_game.stateFrame == fr) return true;
        e = strchr(e, ',');
        if (e) e++;
    }
    return false;
}

static bool s_wasRepeat; static uint32_t s_repeatNow;
static bool repeatHit(int p, int k, PadButton b)          /* BN_x PRESS ou HOLD com ElapsedTime > 800 */
{
    uint32_t now = timeGetTime();
    s_wasRepeat = false; s_repeatNow = now;
    if (Input_IsPadHit(p, b) || (p == 0 && dbgKey(b))) { s_holdT[p][k] = now; return true; }
    if (!Input_IsPadDown(p, b) || now - s_holdT[p][k] <= 800) return false;
    s_wasRepeat = true;
    return true;
}

static void move(int p, int k, PadButton b, int dir)
{
    if (!repeatHit(p, k, b)) return;
    bool held = s_wasRepeat || (Input_IsPadDown(p, b) && !Input_IsPadHit(p, b));
    previewStop();
    s_prevSong = -1;
    s_rankCnt = 0; s_rankAlpha = 0.f;
    s_selected = false;
    s_state = dir < 0 ? SM_LEFT : SM_RIGHT;
    s_arrowLow = dir < 0 ? "arrow_down_left" : "arrow_down_right";
    BGA_SceneReset(UA, s_arrowLow);
    if (dir < 0) s_cur = s_cur ? s_cur - 1 : s_count - 1; else s_cur = (s_cur < s_count - 1) ? s_cur + 1 : 0;
    reorganize(s_state);
    setSceneAnimation();
    if (held) {
        s_arrowHold = true;
        if (s_acc < 720) s_acc += 30;
        if (s_acc >= 740) s_arrowLow = dir < 0 ? "arrow_down_left(high-speed)" : "arrow_down_right(high-speed)";   /* SCENE_ARROW_LL/LR_HOLD */
    }
    snd(s_acc >= 650 ? SN_ACC : SN_MOVE);
    setBoardCH(false);
    if (s_channel != s_prevChannel) s_acc = 500;
    s_holdT[p][k] = s_repeatNow - (uint32_t)s_acc;        /* SetElapsedTime(BN, m_iAcc): próxima repetição em (800 - acc) ms */
}

static void changeMode(int p, PadButton b, int dir)       /* BN_7 = anterior, BN_9 = próximo */
{
    if (!Input_IsPadHit(p, b) && !(p == 0 && dbgKey(b))) return;
    int i = twoPlayers() ? p : 0;
    s_arrowUp = dir < 0 ? "arrow_up_left" : "arrow_up_right";
    BGA_SceneReset(UA, s_arrowUp);
    BGA_SceneReset(UA, dir < 0 ? "arrow_up_left(effacts)" : "arrow_up_right(effacts)");
    s_rankCnt = 0; s_rankAlpha = 0.f;
    s_selected = false;
    int m = nextMode(p, dir);
    s_userMode[i] = m;
    setCurrentMode(p, m);
    setExerciseInfo();
    snd(SN_MODE);                                          /* EFF_MODE */
}

/* ---- códigos de comando (g_Cmd_1p/2p + Select_ProcessCommand + CCommandIcon, select.cpp / commandIcon.cpp) ----------
 * Fila de 16 botões por jogador (cmdqueue.cpp): compara o final da fila com cada código na ordem da tabela; o 1º que casa limpa a fila.
 * Teclas: UL=7 UR=9 DL=1 DR=3 C=5. Não portados: skins, SNAKE, HARD JUDGE, APPEAR, DROP, SINK/RISE, +0.5X (sem campo no jogo). */
enum { CM_SINK, CM_RISE, CM_UA, CM_NX, CM_SNAKE, CM_GRADEREV, CM_RV, CM_RS, CM_XMODE, CM_EW, CM_FREEDOM, CM_ACCEL, CM_DECEL, CM_MIRROR, CM_FLASH, CM_DROP, CM_APPEAR, CM_HARDJ,
       CM_SK_OLD, CM_SK_CANON, CM_SK_CARD, CM_SK_HWATU, CM_SK_HORSE, CM_SK_EZ, CM_SK_MELODY, CM_SK_SLIME, CM_SK_NXCEL, CM_SK_SHEEP, CM_SK_DOG, CM_SK_GIRL, CM_SK_RND,
       CM_SPEEDUP, CM_SPEEDDOWN, CM_VANISH, CM_CANCEL, CM_COUNT };
static const char* const k_cmdKeys[CM_COUNT] = {
    "771133995", "117799335", "15151595", "1753917539", "9753197531", "79513315975", "797979795", "797913135", "191937375", "319739175", "719337915", "113377995",
    "331199775", "319731975", "173193795", "719371933", "157359795", "71593717931",
    /* CmdList1P6 (skins) + 1P_13 (random skin) */
    "333137915", "791513399", "333137917", "333137919", "111777915", "791513391", "791513393", "791513397", "777111371", "777111375", "999333195", "333999735", "7779991113335",
    "79795", "97975", "79135", "131313" };
/* LAYER_* da BGA USER_COMMAND (commandLayer.h) */
enum { LY_2X = 15, LY_3X, LY_4X, LY_8X, LY_AC, LY_DC, LY_EW, LY_RV, LY_V = 24, LY_NS, LY_FD, LY_FL, LY_EXCEED = 38, LY_NX, LY_MIRROR, LY_UA,
       LY_RS = 43, LY_RG, LY_1X = 47, LY_HG = 63, LY_EZ = 23, LY_NXCEL = 28, LY_OLD = 29, LY_HWATOO, LY_CANON, LY_CARD, LY_SHEEP, LY_HORSE, LY_DOG, LY_GIRL, LY_NOTE = 57, LY_SLIME = 60, LY_RNDSKIN = 75, LY_DROP = 69, LY_SNAKE, LY_APPEAR, LY_SINK, LY_RISE };
static char    s_cq[2][16];
static int     s_cspd[2] = { 1, 1 };                       /* 1,2,3,4,8 */
static int     s_cspec[2];                                 /* 0, 1 = RV, 2 = EW */
static bool    s_cflag[2][12];                             /* 0 accel 1 decel 2 mirror 3 randomstep 4 flash 5 freedom 6 gradeRev 7 xmode(global) 8 snake 9 sink 10 rise 11 hardjudge */
static int     s_cvan[2];                                  /* 0, 1 = vanish, 2 = nonstep (V|A), 3 = appear */
/* skin por jogador (m_CurrentSkin[0]): -1 = padrão do estilo; ordem = CM_SK_OLD..CM_SK_GIRL (player.cpp SkinTable / select.cpp:3493) */
static int     s_cskin[2] = { -1, -1 };
static bool    s_crnd[2];                                   /* GAMEMODE_RANDOMSKIN */
static const int k_skId[12] = { 2, 6, 7, 1, 10, 3, 5, 4, 8, 9, 11, 12 };
static const int k_skLy[12] = { LY_OLD, LY_CANON, LY_CARD, LY_HWATOO, LY_HORSE, LY_EZ, LY_NOTE, LY_SLIME, LY_NXCEL, LY_SHEEP, LY_DOG, LY_GIRL };
static bool    s_cua, s_cnx, s_cdrop;                               /* globais (os dois jogadores) */
typedef struct { int max; BGALayerSrc spr[5]; bool has[5]; int count, cur; uint32_t t0; } CmdIcon;
static CmdIcon s_ico[5][2];
static const int k_icoMax[5] = { 2, 3, 1, 5, 4 };

static void iconClear(CmdIcon* c) { c->count = 0; c->cur = -1; for (int i = 0; i < 5; i++) c->has[i] = false; }
static void iconSet(int g, int p, int layer, int idx)       /* CCommandIcon::SetSprite(layer ou -1, slot) */
{
    CmdIcon* c = &s_ico[g][p];
    if (layer < 0) { if (c->has[idx]) { c->has[idx] = false; c->count--; } return; }
    if (idx >= c->max) return;
    if (!c->has[idx]) { c->has[idx] = true; c->cur = idx; c->count++; }
    if (BGA_GetLayerSrc(UCMD, layer, &c->spr[idx])) { char nm[40]; snprintf(nm, sizeof(nm), "user command %dp_%c", p + 1, 'a' + g); BGA_SceneReset(UCMD, nm); }
    c->t0 = timeGetTime();
}
static void iconDraw(int p)
{
    for (int g = 0; g < 5; g++) {
        CmdIcon* c = &s_ico[g][p];
        if (c->cur < 0 || !c->has[c->cur]) {                     /* o ícone atual saiu: passa para outro que exista */
            c->cur = -1;
            for (int i = 0; i < 5; i++) if (c->has[i]) { c->cur = i; break; }
            if (c->cur < 0) continue;
        }
        if (c->count > 1 && timeGetTime() - c->t0 > 3000) {       /* DELAYTIME */
            do c->cur = (c->cur + 1) % c->max; while (!c->has[c->cur]);
            c->t0 = timeGetTime();
        }
        char nm[40];
        snprintf(nm, sizeof(nm), "user command %dp_%c", p + 1, 'a' + g);
        BGA_SetLayerSrc(UCMD, 1 + p * 5 + g, &c->spr[c->cur]);
        BGA_ScenePlay(UCMD, nm, true);
    }
}
static void iconInit(void)
{
    for (int g = 0; g < 5; g++) for (int p = 0; p < 2; p++) { s_ico[g][p].max = k_icoMax[g]; iconClear(&s_ico[g][p]); }
}
static int speedLy(int v) { return v == 2 ? LY_2X : v == 3 ? LY_3X : v == 4 ? LY_4X : v == 8 ? LY_8X : -1; }
static void speedIcon(int p) { iconSet(0, p, s_cspd[p] > 1 ? speedLy(s_cspd[p]) : -1, 0); }

static void processCommand(int p, int cm)
{
    switch (cm) {
    case CM_CANCEL:
        s_cspd[p] = 1; s_cspec[p] = 0; memset(s_cflag[p], 0, sizeof(s_cflag[p])); s_cvan[p] = 0; s_cskin[p] = -1; s_crnd[p] = false;
        s_cua = s_cnx = s_cdrop = false; s_cflag[0][7] = s_cflag[1][7] = false;
        for (int g = 0; g < 5; g++) iconClear(&s_ico[g][p]);
        for (int q = 0; q < 2; q++) { iconSet(3, q, -1, 0); iconSet(3, q, -1, 2); iconSet(3, q, -1, 3); iconSet(3, q, -1, 4); iconSet(4, q, -1, 0); iconSet(4, q, -1, 2); }
        break;
    case CM_SPEEDUP:
        if (s_cspec[p]) { s_cspec[p] = 0; s_cspd[p] = 1; }
        else s_cspd[p] = s_cspd[p] >= 8 ? 1 : s_cspd[p] >= 4 ? 8 : s_cspd[p] + 1;
        iconSet(0, p, -1, 0); speedIcon(p);
        break;
    case CM_SPEEDDOWN:
        if (s_cspec[p]) { s_cspec[p] = 0; s_cspd[p] = 1; }
        else s_cspd[p] = s_cspd[p] >= 8 ? 4 : s_cspd[p] > 1 ? s_cspd[p] - 1 : 8;
        iconSet(0, p, -1, 0); speedIcon(p);
        break;
    case CM_RV: case CM_EW: {
        int k = (cm == CM_RV) ? 1 : 2;
        s_cspd[p] = 1; s_cspec[p] = (s_cspec[p] == k) ? 0 : k;
        iconSet(0, p, s_cspec[p] ? (k == 1 ? LY_RV : LY_EW) : -1, 0); iconSet(0, p, -1, 1);
        break; }
    case CM_ACCEL: case CM_DECEL: {
        int a = (cm == CM_ACCEL) ? 0 : 1; bool on = !s_cflag[p][a];
        s_cflag[p][0] = s_cflag[p][1] = false; s_cflag[p][a] = on;
        iconSet(0, p, on ? (a ? LY_DC : LY_AC) : -1, 1);
        break; }
    case CM_VANISH:                                         /* c = bits V|A: 0 -> V, V -> V|A (NONSTEP), senão limpa */
        s_cvan[p] = (s_cvan[p] == 0) ? 1 : (s_cvan[p] == 1) ? 2 : 0;
        iconSet(1, p, s_cvan[p] == 1 ? LY_V : s_cvan[p] == 2 ? LY_NS : -1, 0);
        break;
    case CM_APPEAR:                                         /* limpa V|A; só liga se estava vazio (senão apenas limpa) */
        s_cvan[p] = (s_cvan[p] == 0) ? 3 : 0;
        iconSet(1, p, s_cvan[p] == 3 ? LY_APPEAR : -1, 0);
        break;
    case CM_DROP:
        s_cdrop = !s_cdrop;
        for (int q = 0; q < 2; q++) iconSet(3, q, s_cdrop ? LY_DROP : -1, 3);
        break;
    case CM_SNAKE: s_cflag[p][8] = !s_cflag[p][8]; iconSet(3, p, s_cflag[p][8] ? LY_SNAKE : -1, 4); break;
    case CM_SINK: case CM_RISE: {
        int a = (cm == CM_SINK) ? 9 : 10; bool on = !s_cflag[p][a];
        s_cflag[p][9] = s_cflag[p][10] = false; s_cflag[p][a] = on;
        iconSet(4, p, on ? (a == 9 ? LY_SINK : LY_RISE) : -1, 2);
        break; }
    case CM_FLASH: s_cflag[p][4] = !s_cflag[p][4]; iconSet(1, p, s_cflag[p][4] ? LY_FL : -1, 1); break;
    case CM_FREEDOM: s_cflag[p][5] = !s_cflag[p][5]; iconSet(1, p, s_cflag[p][5] ? LY_FD : -1, 2); break;
    case CM_MIRROR: case CM_RS: {
        int a = (cm == CM_MIRROR) ? 2 : 3; bool on = !s_cflag[p][a];
        s_cflag[p][2] = s_cflag[p][3] = false; s_cflag[p][a] = on;
        iconSet(3, p, on ? (a == 2 ? LY_MIRROR : LY_RS) : -1, 1);
        break; }
    case CM_HARDJ: s_cflag[p][11] = !s_cflag[p][11]; iconSet(4, p, s_cflag[p][11] ? LY_HG : -1, 3); break;
    case CM_GRADEREV: s_cflag[p][6] = !s_cflag[p][6]; iconSet(4, p, s_cflag[p][6] ? LY_RG : -1, 1); break;
    case CM_XMODE:
        s_cflag[0][7] = s_cflag[1][7] = !s_cflag[p][7];
        for (int q = 0; q < 2; q++) iconSet(3, q, s_cflag[0][7] ? LY_EXCEED : -1, 0);
        break;
    case CM_SK_OLD: case CM_SK_CANON: case CM_SK_CARD: case CM_SK_HWATU: case CM_SK_HORSE: case CM_SK_EZ:
    case CM_SK_MELODY: case CM_SK_SLIME: case CM_SK_NXCEL: case CM_SK_SHEEP: case CM_SK_DOG: case CM_SK_GIRL: {
        int k = cm - CM_SK_OLD;
        s_cskin[p] = k_skId[k]; s_crnd[p] = false;          /* m_CurrentSkin[0] = n; limpa RANDOMSKIN */
        iconSet(2, p, k_skLy[k], 0);
        break; }
    case CM_SK_RND:
        s_cskin[p] = 0; s_crnd[p] = !s_crnd[p];            /* m_CurrentSkin[0] = 0; ^= RANDOMSKIN */
        iconSet(2, p, s_crnd[p] ? LY_RNDSKIN : -1, 0);
        break;
    case CM_UA:
        s_cua = !s_cua;
        for (int q = 0; q < 2; q++) iconSet(3, q, s_cua ? LY_UA : -1, 2);
        break;
    case CM_NX:
        s_cnx = !s_cnx;
        for (int q = 0; q < 2; q++) iconSet(4, q, s_cnx ? LY_NX : -1, 0);
        break;
    }
    snd(SN_HIDDEN);                                         /* EFF_HIDDEN_SELECTED */
}

static void cmdSessionInit(void)
{
    iconInit();
    memset(s_cq, 0, sizeof(s_cq));
    if (g_game.stageCount == 3 || g_game.svcGameMode == 1) {      /* códigos valem pela sessão: zera no 1º estágio */
        for (int p = 0; p < 2; p++) { s_cspd[p] = 1; s_cspec[p] = 0; memset(s_cflag[p], 0, sizeof(s_cflag[p])); s_cvan[p] = 0; s_cskin[p] = -1; s_crnd[p] = false; }
        s_cua = s_cnx = s_cdrop = false;
    }
    else {                                                        /* estágios seguintes: refaz os ícones dos códigos ativos */
        for (int p = 0; p < 2; p++) {
            speedIcon(p);
            if (s_cspec[p]) iconSet(0, p, s_cspec[p] == 1 ? LY_RV : LY_EW, 0);
            if (s_cflag[p][0]) iconSet(0, p, LY_AC, 1);
            if (s_cflag[p][1]) iconSet(0, p, LY_DC, 1);
            if (s_cvan[p]) iconSet(1, p, s_cvan[p] == 1 ? LY_V : s_cvan[p] == 2 ? LY_NS : LY_APPEAR, 0);
            if (s_cdrop) iconSet(3, p, LY_DROP, 3);
            if (s_cflag[p][8]) iconSet(3, p, LY_SNAKE, 4);
            if (s_cflag[p][9]) iconSet(4, p, LY_SINK, 2);
            if (s_cflag[p][10]) iconSet(4, p, LY_RISE, 2);
            if (s_cflag[p][4]) iconSet(1, p, LY_FL, 1);
            if (s_cflag[p][5]) iconSet(1, p, LY_FD, 2);
            if (s_cflag[p][2]) iconSet(3, p, LY_MIRROR, 1);
            if (s_cflag[p][3]) iconSet(3, p, LY_RS, 1);
            if (s_cflag[p][7]) iconSet(3, p, LY_EXCEED, 0);
            if (s_cua) iconSet(3, p, LY_UA, 2);
            if (s_crnd[p]) iconSet(2, p, LY_RNDSKIN, 0);
            else for (int k = 0; k < 12; k++) if (s_cskin[p] == k_skId[k]) iconSet(2, p, k_skLy[k], 0);
            if (s_cflag[p][6]) iconSet(4, p, LY_RG, 1);
            if (s_cflag[p][11]) iconSet(4, p, LY_HG, 3);
            if (s_cnx) iconSet(4, p, LY_NX, 0);
        }
    }
}

/* AddButtonInput + CheckCommand: true se este quadro fechou um código (o CENTER dele não seleciona a música) */
static bool commandInput(int p)
{
    static const PadButton k_pad[5] = { PAD_UL, PAD_UR, PAD_DL, PAD_DR, PAD_C };
    static const char k_ch[5] = { '7', '9', '1', '3', '5' };
    bool changed = false;
    for (int k = 0; k < 5; k++)
        if (Input_IsPadHit(p, k_pad[k]) || (p == 0 && dbgKey(k_pad[k]))) { memmove(s_cq[p], s_cq[p] + 1, 15); s_cq[p][15] = k_ch[k]; changed = true; }
    if (!changed) return false;
    for (int i = 0; i < CM_COUNT; i++) {
        int n = (int)strlen(k_cmdKeys[i]);
        if (memcmp(k_cmdKeys[i], s_cq[p] + 16 - n, n) == 0) {
            memset(s_cq[p], 0, sizeof(s_cq[p]));
            s_selected = false;
            processCommand(p, i);
            return true;
        }
    }
    return false;
}

/* aplica os códigos ao jogo (Select_StartSong zera os campos de velocidade; chamar antes dele e passar speed/rv) */
static void commandsToGame(int speed[2], bool rv[2])
{
    g_game.cmdDrop = s_cdrop;
    for (int p = 0; p < 2; p++) {
        /* NX2 playengine.cpp:1253: RANDOMSKIN sorteia rand() % (strlen(SkinParamTable) - 1) = 0..18 */
        Skin_SetIndexP(p, s_crnd[p] ? rand() % 19 : (s_cskin[p] >= 0 ? s_cskin[p] : Skin_Default()));
        speed[p] = s_cspd[p]; rv[p] = (s_cspec[p] == 1);
        g_game.cmdEarthworm[p] = (s_cspec[p] == 2);
        g_game.cmdAccel[p] = s_cflag[p][0]; g_game.cmdDecel[p] = s_cflag[p][1];
        g_game.cmdMirror[p] = s_cflag[p][2]; g_game.cmdRandomStep[p] = s_cflag[p][3];
        g_game.cmdFlash[p] = s_cflag[p][4]; g_game.cmdFreedom[p] = s_cflag[p][5];
        g_game.cmdHard[p] = s_cflag[p][11]; g_game.cmdGradeRev[p] = s_cflag[p][6]; g_game.cmdXMode[p] = s_cflag[p][7];
        g_game.cmdVanish[p] = (s_cvan[p] == 1); g_game.cmdNonStep[p] = (s_cvan[p] == 2); g_game.cmdAppear[p] = (s_cvan[p] == 3);
        g_game.cmdSnake[p] = s_cflag[p][8]; g_game.cmdSink[p] = s_cflag[p][9]; g_game.cmdRise[p] = s_cflag[p][10];
        g_game.cmdUnderAttack[p] = s_cua; g_game.cmdNXMode[p] = s_cnx;
    }
}

/* CENTER (ProcessInput BN_5): 1º toque seleciona (A_START), 2º inicia (START); música bloqueada = WRONG.
 * Não portado ainda: mileage/travas (LOCK_PROCESS_*) e entrada tardia. */
static void centerPress(int p)
{
    (void)p;
    const TrackEntry* e = &g_songTable[s_list[s_cur]];
    if (s_selected) { snd(SN_START); s_end = true; return; }
    if (!e->avail) { snd(SN_WRONG); return; }
    s_selected = true;
    if (s_state != SM_NONE) { s_state = SM_NONE; validateMode(); reorganize(s_state); setSceneAnimation(); }
    BGA_SceneReset(UB, "song_click");
    snd(SN_SELSTART);
}

/* SetEnd + End (select.cpp): cenas de saída, fade para preto em 60 quadros e RUN <id> <modos> */
static void launch(void)
{
    if (s_special && !g_songTable[s_list[s_cur]].avail) {          /* GetRandomMusic */
        int n = 0;
        for (int i = 0; i < s_count; i++) n += g_songTable[s_list[i]].avail ? 1 : 0;
        if (n) { do s_cur = rand() % s_count; while (!g_songTable[s_list[s_cur]].avail); validateMode(); }
    }
    const TrackEntry* e = &g_songTable[s_list[s_cur]];
    previewStop();
    Movie_Close();
    BGM_Stop();
    for (int p = 0; p < 2; p++) {
        int d = twoPlayers() ? s_mode[p] : s_mode[0];
        g_nxDiffIdx[p] = d;
        g_nxLevel[p] = (d >= 0 && d <= 4) ? e->level[d] : 0;
    }
    static const char* const k_arg[5] = { "-n", "-h", "-c", "-d", "-m" };
    Log_Print("NX2SELECT: RUN %X %s %s" "%c", (unsigned)e->id, k_arg[s_mode[0]], twoPlayers() ? k_arg[s_mode[1]] : "", 10);
    int speed[2] = { 1, 1 };
    bool rv[2] = { false, false };
    s_lastId = (int)e->id; s_lastSpecial = s_special; s_lastMode[0] = s_mode[0]; s_lastMode[1] = s_mode[1];
    if (g_nx2Opt.g_PlayCountPerTrack[s_list[s_cur]] < 0x7FFFFFFF) g_nx2Opt.g_PlayCountPerTrack[s_list[s_cur]]++;
    Render_SetGlobalColor(0, 0, 0, 0);
    commandsToGame(speed, rv);
    if (!Select_StartSong((int)e->id, s_mode[0], s_active, speed, rv)) {
        Log_Print("NX2SELECT: Select_StartSong falhou" "%c", 10);
        s_end = s_endStarted = false; s_phase = 1; s_endCnt = 0;
    }
}

void Nx2Select_Update(float dt)
{
    Movie_Update(dt);
    if (!s_ok) return;
    if (s_phase == 0) {                                   /* Start */
        if (BGA_SceneDone(UB, s_special ? "special_intro_b" : "arcade_intro_b")) {
            s_state = SM_NONE; reorganize(s_state); s_phase = 1;
        }
        return;
    }
    if (g_game.svcGameMode == 1) BGA_SceneReset(UA, "time_99");   /* EVENT: sem contagem */
    if (s_phase == 2) {                                   /* End */
        if (s_previewOn && s_state == SM_NONE) previewUpdate(dt);
        Render_SetGlobalColor(0, 0, 0, s_endCnt / 60.0f);
        if (s_endCnt >= 60) launch();
        s_endCnt++;
        return;
    }

    if ((s_state == SM_LEFT || s_state == SM_RIGHT) && BGA_SceneDone(UB, s_scene) && s_acc < 730) {
        s_state = SM_NONE; reorganize(s_state); setSceneAnimation();
        validateMode();
        setExerciseInfo();
        BGA_SceneReset(UD, "level_glow_channel_color");
        if (twoPlayers()) {
            BGA_SceneReset(UC, "dual_1p_kcal-vo2(all)");
            BGA_SceneReset(UC, "dual_2p_kcal-vo2(all)");
            BGA_SceneReset(UD, "dual_high_score_in");
        } else {
            BGA_SceneReset(UC, "single_kcal-vo2(all)");
            BGA_SceneReset(UD, "single_high_score_in");
        }
    }
    for (int p = 0; p < 2; p++) {
        if (!(s_active & (1u << p))) continue;
        move(p, 0, PAD_DL, -1);
        move(p, 1, PAD_DR, +1);
        changeMode(p, PAD_UL, -1);
        changeMode(p, PAD_UR, +1);
    }
    /* solta DL/DR: a aceleração volta a 500 e a seta deixa o modo rápido */
    bool anyHeld = false;
    for (int p = 0; p < 2; p++)
        if ((s_active & (1u << p)) && (Input_IsPadDown(p, PAD_DL) || Input_IsPadDown(p, PAD_DR))) anyHeld = true;
    if (!anyHeld) {
        s_acc = 500;
        if (s_arrowHold) {
            s_arrowHold = false;
            s_arrowLow = "arrow_down_left";
            BGA_SceneFinish(UA, s_arrowLow);
        }
    }
    setBoardCH(false);
    bool cmdHit[2] = { false, false };
    for (int p = 0; p < 2; p++) if (s_active & (1u << p)) cmdHit[p] = commandInput(p);
    for (int p = 0; p < 2; p++)
        if ((s_active & (1u << p)) && !cmdHit[p] && (Input_IsPadHit(p, PAD_C) || (p == 0 && dbgKey(PAD_C)))) centerPress(p);
    if (BGA_SceneDone(UA, "time_99")) s_end = true;
    if (s_end && !s_endStarted) {                         /* SetEnd */
        s_endStarted = true; s_phase = 2; s_endCnt = 0;
        s_state = SM_NONE;
        if (s_cSrc[1].isSPR) BGA_SetLayerSrc(UB, L_TEX_C, &s_cSrc[1]);
        reorganize(s_state); setSceneAnimation(); setBoardCH(true); validateMode(); setExerciseInfo();
        BGA_SceneReset(UA, s_special ? "special_out_a" : "arcade_out_a");
        BGA_SceneReset(UB, s_special ? "special_base_out_b" : "arcade_base_out_b");
        BGA_SceneReset(UC, "base_out_c");
        return;
    }

    if (s_state == SM_NONE && !anyPadDown() && !s_previewOn && s_prevSong != s_list[s_cur]) previewStart();
    if (s_previewOn && s_state == SM_NONE) previewUpdate(dt);
}

/* ---- Render ----------------------------------------------------------------------------------- */
void Nx2Select_Render(void)
{
    Movie_Render();
    if (!s_ok) return;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1, 1, 1, 1);
    if (s_phase == 0) {                                   /* Start: intro das três BGAs */
        BGA_ScenePlay(UA, s_special ? "special_inter_a" : "arcade_intro_a", true);
        BGA_ScenePlay(UB, s_special ? "special_intro_b" : "arcade_intro_b", true);
        BGA_ScenePlay(UC, "base_intro_c", true);
        return;
    }
    if (s_phase == 2) {                                   /* End: só as cenas de saída (o fade vem do overlay global) */
        BGA_ScenePlay(UA, s_special ? "special_out_a" : "arcade_out_a", true);
        BGA_ScenePlay(UB, s_special ? "special_base_out_b" : "arcade_base_out_b", true);
        BGA_ScenePlay(UC, "base_out_c", true);
        return;
    }
    BGA_ScenePlay(UA, s_arrowUp, true);                    /* só a corrente: a outra desenharia sua camada estática por cima */
    BGA_ScenePlay(UA, s_arrowLow, true);
    BGA_ScenePlay(UA, s_special ? "special_hold_a" : "arcade_hold_a", true);
    BGA_ScenePlay(UA, "arrow_up_left(effacts)", true);
    BGA_ScenePlay(UA, "arrow_up_right(effacts)", true);
    BGA_ScenePlay(UA, "time_99", true);
    BGA_ScenePlay(UB, s_scene, true);
    if (s_state == SM_NONE) drawScore();
    if (s_selected) BGA_ScenePlay(UB, "song_click", true);
    BGA_ScenePlay(UB, s_special ? "special_hold_b" : "arcade_hold_b", true);
    if (s_state == SM_NONE) {
        drawLV();
        BGA_ScenePlay(UD, "level_glow_channel_color", true);
    }
    BGA_ScenePlay(UC, "base_hold_c", true);
    BGA_ScenePlay(UC, twoPlayers() ? "degree of difficulty_dual_1p_in" : "degree of difficulty_single_in", true);
    if (twoPlayers()) BGA_ScenePlay(UC, "degree of difficulty_dual_2p_in", true);
    BGA_ScenePlay(UB, s_special ? "special_channel_change_b" : "arcade_channel_change_b", true);
    for (int p = 0; p < 2; p++) if (s_active & (1u << p)) iconDraw(p);   /* Select_DrawIcon */
}
