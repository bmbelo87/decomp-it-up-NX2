/*
 * nx2_station.c — Station Select do NX2 (CStation, nx2src ProcModules/station.cpp).
 *
 * Diferente da Station da NX (nx_station.c, COMMON.DAT/ARRO.DAT): carrega BGA/SELECT_STATION.DAT sobre
 * BGA/BG.MOV. Estações (índice = G_STATION_*): 0 TRAINING, 1 ARCADE, 2 WORLD MAX, 3 SPECIAL ZONE.
 * Cenas da BGA conferidas no arquivo real: sstation_intro/loop/out, station_move_left/right, station_stop,
 * station_stop_loop, station_stop_click (+ versões "_2P_lock"), arrow_upper/down_left/right, time, 2player_in.
 *
 * Sons (SFX_STATION.LUA + SFX_GLOBAL.LUA, pasta WAVE):
 *   Begin   : STATION_BGM.WAV em loop (EFF_S_BGM) e B02.WAV (EFF_STATION_SELECT, som de entrada)
 *   mover   : S_MOVE.WAV   confirmar: S_CLK.WAV (EFF_S_PUSH) e a voz da estação
 *             (B04 TRAINING, B03 ARCADE, B05 WORLD MAX, B06 SPECIAL ZONE)
 *   errado  : WRONG.WAV (2 jogadores ou EVENT em TRAINING/WORLD MAX)   2º CENTER: S_LIGHT.WAV
 *   fim     : START.WAV (EFF_START) quando a cena "time" acaba        entrar: JOIN.WAV
 *   O script também cita WELCOME.WAV (EFF_WELCOME), mas o arquivo NÃO existe na instalação e o código não o usa.
 *
 * Botões: DL/UL = estação anterior (cena de seta correspondente), DR/UR = próxima, CENTER confirma.
 * Estação inicial = DEFAULT STATION do Service Menu (EEPROM +0xECA: 1 ARCADE, 2 WORLD MAX, 3 SPECIAL, outro = TRAINING).
 * Saída: com os dois jogadores TRAINING/WORLD MAX viram ARCADE (station.cpp End). Entrada tardia de jogador volta
 * ao USB_INPUT (g_FlowControl.Next(true)) para ler o perfil dele.
 */
#include "pumpy.h"
#include "bga.h"
#include "movie.h"

extern unsigned Title_GetJoinedMask(void);

enum { G_TRAINING = 0, G_ARCADE, G_WORLDMAX, G_SPECIAL };
enum { M_NONE = 0, M_CLK, M_LEFT, M_RIGHT, M_EFF };      /* SCREEN_MOVE_* */
enum { SN_BGM, SN_SELECT, SN_MOVE, SN_PUSH, SN_WRONG, SN_JOIN, SN_LIGHT, SN_START, SN_TRAIN, SN_ARCADE, SN_WORLD, SN_SPECIAL, SN_COUNT };

static const char* const k_wav[SN_COUNT] = {
    "STATION_BGM.WAV", "B02.WAV", "S_MOVE.WAV", "S_CLK.WAV", "WRONG.WAV", "JOIN.WAV", "S_LIGHT.WAV", "START.WAV",
    "B04.WAV", "B03.WAV", "B05.WAV", "B06.WAV",
};
static int s_snd[SN_COUNT];
static bool s_sndInit;
static void snd(int k, bool loop) { if (s_snd[k] >= 0) Audio_Play(s_snd[k], loop); }

#define BG 0   /* SELECT_STATION é o único BGA carregado */
#define S_FRONT1 56
#define S_FRONT2 59
#define S_TEXT1  31
#define S_TEXT2  32
#define S_EFFECT 89

static bool    s_ok;
static int     s_phase;          /* 0 intro, 1 seleção, 2 saída (fade), 3 saída para o USB_INPUT */
static int     s_state;          /* m_iCurrentState */
static int     s_sel;            /* m_iCurrentSelected */
static bool    s_selected, s_join;
static int     s_cnt2;
static unsigned s_active;
static const char* s_scene;      /* m_iScenename */
static BGALayerSrc s_scr[4], s_eff[4], s_txt[4];

static bool both(void) { return (s_active & 3) == 3; }
static bool locked(void) { return both() && (s_sel == G_TRAINING || s_sel == G_WORLDMAX); }

static void setText(void)
{
    BGA_SetLayerSrc(BG, S_TEXT1, &s_txt[s_sel]);
    BGA_SetLayerSrc(BG, S_TEXT2, &s_txt[s_sel]);
}

static void setSceneAnimation(void)
{
    switch (s_state) {
    case M_NONE: s_scene = locked() ? "station_stop_loop_2P_lock" : "station_stop_loop"; break;
    case M_LEFT: s_scene = "station_move_left"; break;
    case M_RIGHT: s_scene = "station_move_right"; break;
    case M_EFF: s_scene = locked() ? "sation_stop_2P_lock" : "station_stop"; break;   /* "sation": grafia do arquivo */
    case M_CLK: s_scene = locked() ? "station_stop_click_2p_lock" : "station_stop_click"; break;
    default: s_scene = "station_stop_loop"; return;
    }
    BGA_SceneReset(BG, s_scene);
}

/* ReorganizeScene (station.cpp:446): frente/direita/trás/esquerda = base, base+1, base+2, base+3.
 * NONE/CLK/EFF: base = atual; RIGHT: base = atual+1; LEFT: base = atual-1 (tabelas extraídas do código). */
static void reorganize(int mode)
{
    int base = s_sel;
    if (mode == M_RIGHT) base = (s_sel + 1) & 3;
    else if (mode == M_LEFT) base = (s_sel + 3) & 3;
    static const int k_front[2] = { 56, 59 }, k_right[4] = { 46, 48, 53, 58 }, k_back[3] = { 44, 47, 54 }, k_left[4] = { 43, 49, 55, 57 };
    for (int i = 0; i < 2; i++) BGA_SetLayerSrc(BG, k_front[i], &s_scr[base]);
    for (int i = 0; i < 4; i++) BGA_SetLayerSrc(BG, k_right[i], &s_scr[(base + 1) & 3]);
    for (int i = 0; i < 3; i++) BGA_SetLayerSrc(BG, k_back[i], &s_scr[(base + 2) & 3]);
    for (int i = 0; i < 4; i++) BGA_SetLayerSrc(BG, k_left[i], &s_scr[(base + 3) & 3]);
    BGA_SetLayerSrc(BG, S_EFFECT, &s_eff[s_sel]);
}

void Nx2Station_Enter(void)
{
    s_active = Title_GetJoinedMask() & 3;
    if (!s_active) s_active = 1;
    Title_StopMusic();
    Movie_Select(1); Movie_Close(); Movie_Select(0);
    Movie_Close();
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/BGA/BG.MOV", g_game.currentDirectory);
    Movie_Open(path, true);

    Resource_ClearBGA();
    s_ok = Resource_LoadBGAByName("SELECT_STATION");
    if (!s_ok) Log_Print("NX2STATION: falha ao carregar BGA\\SELECT_STATION.DAT\n");
    BGA_Reset();

    if (!s_sndInit) {
        for (int k = 0; k < SN_COUNT; k++) s_snd[k] = Audio_LoadWaveFile(k_wav[k]);
        s_sndInit = true;
    }
    if (s_ok) {
        static const int k_scr[4] = { 61, 63, 64, 66 }, k_eff[4] = { 91, 92, 93, 94 }, k_txt[4] = { 34, 35, 36, 37 };
        for (int i = 0; i < 4; i++) {
            s_ok &= BGA_GetLayerSrc(BG, k_scr[i], &s_scr[i]);
            s_ok &= BGA_GetLayerSrc(BG, k_eff[i], &s_eff[i]);
            s_ok &= BGA_GetLayerSrc(BG, k_txt[i], &s_txt[i]);
        }
        if (!s_ok) Log_Print("NX2STATION: alguma camada de origem não existe\n");
        /* SetSceneTick(total): setas e "station_stop" começam no último quadro (sem desenhar) */
        static const char* const k_done[] = { "arrow_upper_left", "arrow_upper_right", "arrow_down_left", "arrow_down_right", "station_stop" };
        for (size_t i = 0; i < sizeof(k_done) / sizeof(k_done[0]); i++) BGA_SceneFinish(BG, k_done[i]);
    }
    uint32_t def = Eeprom_Get32(0xECA);                 /* g_Option.g_DefaultStation */
    s_sel = (def == G_ARCADE || def == G_WORLDMAX || def == G_SPECIAL) ? (int)def : G_TRAINING;
    s_state = M_NONE; s_selected = false; s_join = false; s_cnt2 = 0; s_phase = 0;
    if (s_ok) {
        setText();
        reorganize(M_NONE);
        BGA_SceneReset(BG, "sstation_intro");
        BGA_SceneReset(BG, "time");
    }
    snd(SN_BGM, true);
    snd(SN_SELECT, false);
    Log_Print("NX2STATION: estação inicial %d, jogadores 0x%X\n", s_sel, s_active);
}

static void leave(void)
{
    if (both() && (s_sel == G_TRAINING || s_sel == G_WORLDMAX)) s_sel = G_ARCADE;
    static const int k_mode[4] = { 3, 0, 2, 1 };       /* estação -> modo de jogo (mesma tabela da NX) */
    Movie_Close();
    if (s_snd[SN_BGM] >= 0) Audio_Stop(s_snd[SN_BGM]);
    Title_SetJoinedMask(s_active);
    Menu_ResetState();
    g_game.nxGameMode = k_mode[s_sel];
    Game_ChangeState(STATE_SELECT);
}

static void processInput(int p)
{
    if (!(s_active & (1u << p))) {                        /* jogador ausente: CENTER com crédito entra */
        if (Input_IsPadHit(p, PAD_C) && Coin_HasCredit()) {
            Coin_ConsumeCredit();
            snd(SN_JOIN, false);
            s_selected = false;
            s_join = true;
            s_active |= (1u << p);
            Title_SetJoinedMask(Title_GetJoinedMask() | (1u << p));
            BGA_SceneReset(BG, "2player_in");
            reorganize(s_state);
            setSceneAnimation();
        }
        return;
    }
    int dir = 0; const char* arrow = NULL;
    if (Input_IsPadHit(p, PAD_DL)) { dir = -1; arrow = "arrow_down_left"; }
    else if (Input_IsPadHit(p, PAD_UL)) { dir = -1; arrow = "arrow_upper_left"; }
    else if (Input_IsPadHit(p, PAD_DR)) { dir = +1; arrow = "arrow_down_right"; }
    else if (Input_IsPadHit(p, PAD_UR)) { dir = +1; arrow = "arrow_upper_right"; }
    if (dir) {
        Log_Print("NX2STATION: P%d %s -> estação %d (quadro %d)\n", p + 1, arrow, (s_sel + dir + 4) & 3, g_game.stateFrame);
        Log_Flush();
        snd(SN_MOVE, false);
        s_selected = false;
        s_sel = (s_sel + dir + 4) & 3;
        if (s_state == M_LEFT || s_state == M_RIGHT) setText();
        s_state = dir < 0 ? M_RIGHT : M_LEFT;             /* BN_1/BN_7: RIGHT; BN_3/BN_9: LEFT */
        reorganize(s_state);
        BGA_SceneReset(BG, arrow);
        setSceneAnimation();
    }
    if (Input_IsPadHit(p, PAD_C)) {
        if (!s_selected) {
            if ((s_sel == G_WORLDMAX || s_sel == G_TRAINING) && (both() || g_game.svcGameMode == 1)) {
                snd(SN_WRONG, false);
            } else {
                snd(SN_PUSH, false);
                s_selected = true;
            }
            s_state = M_CLK;
            setText();
            setSceneAnimation();
            reorganize(s_state);
            static const int k_voice[4] = { SN_TRAIN, SN_ARCADE, SN_WORLD, SN_SPECIAL };
            snd(k_voice[s_sel], false);
        } else {
            snd(SN_LIGHT, false);
            s_cnt2 = 0;
            s_phase = 2;                                   /* PS_END */
        }
    }
}

void Nx2Station_Update(float dt)
{
    Movie_Update(dt);
    if (!s_ok) { leave(); return; }

    if (s_phase == 0) {                                   /* Start: intro */
        if (BGA_SceneDone(BG, "sstation_intro")) { s_phase = 1; s_state = M_NONE; setSceneAnimation(); }
        return;
    }
    if (s_phase == 2) {                                   /* End: fade de 60 quadros */
        if (++s_cnt2 >= 60) leave();
        return;
    }
    if (s_phase == 3) return;

    if (g_game.svcGameMode == 1) BGA_SceneReset(BG, "time");   /* EVENT: sem contagem */
    if ((s_state == M_LEFT || s_state == M_RIGHT) && BGA_SceneDone(BG, s_scene)) {
        s_state = M_EFF; setText(); reorganize(s_state); setSceneAnimation();
    } else if (s_state == M_EFF && BGA_SceneDone(BG, s_scene)) {
        s_state = M_NONE; setText(); reorganize(s_state); setSceneAnimation();
    } else if (s_state == M_CLK && both() && (s_sel == G_TRAINING || s_sel == G_WORLDMAX) && BGA_SceneDone(BG, s_scene)) {
        s_state = M_NONE; setText(); reorganize(s_state); setSceneAnimation();
    }
    if (BGA_SceneDone(BG, "time")) {                      /* acabou o tempo */
        snd(SN_START, false);
        s_cnt2 = 0;
        s_phase = 2;
        return;
    }
    processInput(0);
    processInput(1);
    if (s_join && BGA_SceneDone(BG, "2player_in")) {      /* Next(true): volta ao USB_INPUT para ler o perfil */
        Movie_Close();
        if (s_snd[SN_BGM] >= 0) Audio_Stop(s_snd[SN_BGM]);
        Title_SetJoinedMask(s_active);
        s_phase = 3;
        Game_ChangeState(STATE_NX_USBIN);
    }
}

void Nx2Station_Render(void)
{
    Movie_Render();
    if (!s_ok) return;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1, 1, 1, 1);
    if (s_phase == 0) { BGA_ScenePlay(BG, "sstation_intro", true); return; }
    if (s_phase == 2) {                                   /* End: fade sob a cena de saída */
        int a = s_cnt2 * 255 / 60; if (a > 255) a = 255;
        Render_Rect(0.0f, 0.0f, 640.0f, 480.0f, 0, 0, 0, (uint8_t)a);
        glColor4f(1, 1, 1, 1);
        BGA_ScenePlay(BG, "sstation_out", true);
        return;
    }
    BGA_ScenePlay(BG, "sstation_loop", true);
    BGA_ScenePlay(BG, "time", true);
    BGA_ScenePlay(BG, "arrow_upper_left", true);
    BGA_ScenePlay(BG, "arrow_upper_right", true);
    BGA_ScenePlay(BG, "arrow_down_left", true);
    BGA_ScenePlay(BG, "arrow_down_right", true);
    BGA_ScenePlay(BG, s_scene, true);
    if (s_join) BGA_ScenePlay(BG, "2player_in", true);
}
