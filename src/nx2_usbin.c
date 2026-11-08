/*
 * nx2_usbin.c — tela de entrada do perfil (CUsbin, modo USB_INPUT) do NX2.
 *
 * Origem: nx2src ProcModules/usbin.cpp (nomes de cenas, camadas e regras), conferida com as BGAs reais
 * (USB_INPUT, USB_INFO_A, USB_LISENCE_ID, USB_MESSAGE, USB_INFO_B) extraídas dos .DAT. O sequenciador
 * (flowcontrol.cpp) chama USB_INPUT depois do TITLE e antes da STATION (também por entrada tardia).
 *
 * Fluxo (Run, usbin.cpp:520):
 *   1. Intro: cena "USB_intro" da USB_INPUT.
 *   2. Espera 60 s ("insert usb_hold" em loop; dígitos do tempo nas camadas 34/35 da USB_INPUT).
 *      Por jogador ativo SEM perfil: a cada 16 quadros LoadData() até dar resultado:
 *        eOk -> perfil ativo e janela da licença; eNoData -> "não registrado"; eMountError/eBadData -> erro USB.
 *      CENTER do jogador: 1º aperto desiste do USB (janela "NOT USE USB"); depois de todos prontos o tempo
 *      passa a 15 s e um CENTER seguinte encerra. Jogador inativo com crédito: CENTER entra no jogo.
 *   3. Saída: fade preto de 60 quadros com as cenas "..._OUT"; vai para a STATION.
 *
 * Diferenças do port:
 *  - O pendrive é a pasta NX2_USB<N> / save/usb<N> (ver nx2_save.c); a conferência do número de série não existe.
 *  - NÃO feito ainda: texto de posição do WORLD MAX (szLand/szMission, fonte TTF) e o ícone do perfil nas telas
 *    seguintes (CUsbstate / USB_COMMON.DAT).
 */
#include "pumpy.h"
#include "bga.h"
#include "movie.h"

#define UB_INPUT_TIME 60   /* INPUT_TIME */
#define UB_SHOW_TIME  15   /* SHOW_TIME */

enum { UA = 0, UB = 1, UC = 2, UD = 3, UE = 4 };   /* ordem de carga: USB_INPUT, INFO_A, LISENCE_ID, MESSAGE, INFO_B */
enum { MSG_CONNECT = 0, MSG_SAVE, MSG_FINISH, MSG_REG_ERR, MSG_USB_ERR, MSG_WRITE_ERR };

extern unsigned Title_GetJoinedMask(void);
extern bool Nx2Select_JoinPending(void);   /* flowcontrol: UsbInput -> MusicSelect se a Station já foi escolhida */

static bool    s_ok;
static int     s_phase;            /* 1 intro, 2 espera, 3 saída */
static uint32_t s_t0;              /* [m_pMinusTimer] */
static int     s_left;             /* m_iLeftTime */
static int     s_time;             /* m_iTime */
static int     s_cnt;              /* m_Cnt na saída */
static bool    s_selected;         /* m_bSelected */
static bool    s_endInput[2], s_error[2], s_hasUsb[2];
static unsigned s_active;          /* jogadores presentes (bit0 = P1, bit1 = P2) */
static BGALayerSrc s_time_[11], s_num[11], s_id[57], s_msg[6];
static int     s_charTex[2];

/* ---- utilitários ------------------------------------------------------------------------------ */

/* bfont.cpp: cnvTable (BLOCK_MAX 59); espaço devolve BLOCK_MAX, fim de texto 58 -> 56 */
static int cnvCharToValue(int ch)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789~!@#$%^&*()_-+=\\:;/?";
    if (ch == ' ') return 59;
    for (int i = 0; t[i]; i++) if (t[i] == ch) return i;
    return (ch == 0) ? 58 : 59;    /* CNV_END (0) = 58 */
}

static void sceneP(int bga, const char* fmt, int player, bool draw)
{
    char n[96];
    snprintf(n, sizeof(n), fmt, player + 1);
    BGA_ScenePlay(bga, n, draw);
}

static void setDigits(int bga, int slot, const BGALayerSrc* pool, int val, int count, int firstNullIfZero)
{
    for (int i = count - 1; i >= 0; i--) {
        int d = val % 10;
        if (firstNullIfZero && i == 0 && val == 0) d = 10;    /* "num_null" no dígito mais alto */
        BGA_SetLayerSrc(bga, slot + i, &pool[d]);
        val /= 10;
    }
}

static void setTime(void)       /* SetTime */
{
    s_left = (s_endInput[0] && s_endInput[1]) ? UB_SHOW_TIME : UB_INPUT_TIME;
    int t = s_left;
    BGA_SetLayerSrc(UA, 35, &s_time_[t % 10]); t /= 10;
    BGA_SetLayerSrc(UA, 34, &s_time_[t % 10]);
    s_t0 = timeGetTime();
}

static void drawTime(int t)     /* DrawTime */
{
    if (t < 0) t = 0;
    if (t > 99) t = 99;
    BGA_SetLayerSrc(UA, 35, &s_time_[t % 10]); t /= 10;
    BGA_SetLayerSrc(UA, 34, &s_time_[t % 10]);
}

static void setMessage(int p, int msg)   /* SetMessageWnd: camada 21 + jogador da USB_MESSAGE */
{
    BGA_SetLayerSrc(UD, 21 + p, &s_msg[msg]);
}

/* personagem do perfil (ch_NNN.dds em USB_CHARACTER.DAT) no tile da camada 41 (+4 por jogador) */
static void setEmoticon(int p)
{
    int idx = g_nx2Prof[p].iCharacter;
    if (idx < 0 || idx > 127) return;
    char dat[MAX_PATH], name[16];
    snprintf(dat, sizeof(dat), "%s/BGA/USB_CHARACTER.DAT", g_game.currentDirectory);
    snprintf(name, sizeof(name), "ch_%03d.dds", idx + 1);
    uint32_t sz = 0;
    uint8_t* d = Resource_ExtractFromPack(dat, name, &sz);
    if (!d) { Log_Print("USBIN: personagem '%s' não encontrado\n", name); return; }
    int tex = Texture_LoadFromMemory(d, sz, name);
    free(d);
    BGALayerSrc src;
    if (tex >= 0 && BGA_GetLayerSrc(UA, 41 + p * 4, &src) && src.isSPR && src.sprTileCount > 0 && src.sprTileStart >= 0) {
        g_game.sprTiles[src.sprTileStart].texId = tex;
        s_charTex[p] = tex;
    }
}

static void setLicense(int p)   /* SetLisenceInWnd */
{
    const Nx2Stat* s = &g_nx2Prof[p];
    setEmoticon(p);
    /* SetId: 8 letras nas camadas 1..8 (P1) / 11..18 (P2) da USB_LISENCE_ID */
    for (int i = 0; i < 8; i++) {
        int v = cnvCharToValue((unsigned char)s->PlayerID[i]);
        if (v == 58) v = 56;
        if (v > 56) v = 56;
        BGA_SetLayerSrc(UC, i + 1 + p * 10, &s_id[v]);
    }
    int val = s->iMileage;                               /* SetMileage: 7 dígitos, camadas 0..6 (+24 por jogador) */
    if (val > 9999999) val = 9999999; if (val < 0) val = 0;
    setDigits(UB, p * 24, s_num, val, 7, 0);
    val = s->m_iRewardCnt * 1000 / 70;                   /* SetRewardClear: camadas 7..10 */
    if (val > 1000) val = 1000; if (val < 0) val = 0;
    for (int i = 10; i >= 7; i--) {
        BGA_SetLayerSrc(UB, i + p * 24, &s_num[(i == 7 && val == 0) ? 10 : val % 10]);
        val /= 10;
    }
    val = s->m_iWorldMaxCnt * 1000 / 333;                /* SetWorldMaxClear: camadas 11..14 */
    if (val > 1000) val = 1000; if (val < 0) val = 0;
    for (int i = 14; i >= 11; i--) {
        BGA_SetLayerSrc(UB, i + p * 24, &s_num[(i == 11 && val == 0) ? 10 : val % 10]);
        val /= 10;
    }
    val = s->iPlayCount;                                 /* SetPlayCount: INFO_B camadas 1..7 (+30) */
    if (val > 9999999) val = 9999999; if (val < 0) val = 0;
    for (int i = 7; i >= 1; i--) { BGA_SetLayerSrc(UE, i + p * 30, &s_num[val % 10]); val /= 10; }
    val = s->ikcal;                                      /* SetSports: kcal 9..15, VO2 17..23 */
    if (val > 9999999) val = 9999999; if (val < 0) val = 0;
    for (int i = 15; i >= 9; i--) { BGA_SetLayerSrc(UE, i + p * 30, &s_num[val % 10]); val /= 10; }
    val = s->iVO2;
    if (val > 9999999) val = 9999999; if (val < 0) val = 0;
    for (int i = 23; i >= 17; i--) { BGA_SetLayerSrc(UE, i + p * 30, &s_num[val % 10]); val /= 10; }
}

/* ---- Begin ------------------------------------------------------------------------------------ */

void NxUsbIn_Enter(void)
{
    s_active = Title_GetJoinedMask() & 3;
    if (!s_active) s_active = 1;
    for (int p = 0; p < 2; p++) {
        if (!(s_active & (1 << p))) Nx2Profile_Unload(p);     /* quem não entrou não tem perfil */
        s_hasUsb[p] = g_nx2HasProfile[p];
        s_endInput[p] = true; s_error[p] = false; s_charTex[p] = -1;
    }
    s_selected = false;

    Movie_Close();
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/BGA/USB_BG.MOV", g_game.currentDirectory);
    Movie_Open(path, true);

    Resource_ClearBGA();
    static const char* const k_bga[5] = { "USB_INPUT", "USB_INFO_A", "USB_LISENCE_ID", "USB_MESSAGE", "USB_INFO_B" };
    s_ok = true;
    for (int i = 0; i < 5; i++)
        if (!Resource_LoadBGAByName(k_bga[i])) { Log_Print("USBIN: falha ao carregar BGA\\%s.DAT\n", k_bga[i]); s_ok = false; }
    BGA_Reset();

    if (s_ok) {
        for (int i = 0; i < 11; i++) {
            s_ok &= BGA_GetLayerSrc(UA, 87 + i, &s_time_[i]);
            s_ok &= BGA_GetLayerSrc(UB, 87 + i, &s_num[i]);
        }
        for (int i = 0; i < 57; i++) s_ok &= BGA_GetLayerSrc(UC, 40 + i, &s_id[i]);
        /* mensagens por idioma (0 KR, 1 EN, 2 ES, 3 PT): base 60/70/80/90 */
        int lang = g_game.svcLangOption & 3;
        int base = (lang == 0) ? 60 : (lang == 2) ? 80 : (lang == 1) ? 70 : 90;
        static const int k_off[6] = { 0, 5, 6, 2, 3, 7 };   /* CONNECT SAVE FINISH REG_ERR USB_ERR WRITE_ERR */
        for (int i = 0; i < 6; i++) s_ok &= BGA_GetLayerSrc(UD, base + k_off[i], &s_msg[i]);
        if (!s_ok) Log_Print("USBIN: alguma camada de origem não existe\n");

        /* idioma nas camadas 24/25 ("skip"), 40 e 44 (licença) da USB_INPUT */
        static const int k_skip[4] = { 67, 64, 65, 66 }, k_lic1[4] = { 75, 76, 77, 78 }, k_lic2[4] = { 81, 82, 83, 84 };
        BGALayerSrc a;
        if (BGA_GetLayerSrc(UA, k_skip[lang], &a)) { BGA_SetLayerSrc(UA, 24, &a); BGA_SetLayerSrc(UA, 25, &a); }
        if (BGA_GetLayerSrc(UA, k_lic1[lang], &a)) BGA_SetLayerSrc(UA, 40, &a);
        if (BGA_GetLayerSrc(UA, k_lic2[lang], &a)) BGA_SetLayerSrc(UA, 44, &a);

        for (int p = 0; p < 2; p++) {
            if (!(s_active & (1 << p))) continue;
            if (!s_hasUsb[p]) s_endInput[p] = false;
            else setLicense(p);
        }
        setMessage(0, MSG_CONNECT);
        setMessage(1, MSG_CONNECT);
    }
    BGA_SceneReset(UA, "USB_intro");
    BGA_SceneReset(UA, "insert usb_hold");
    BGA_SceneReset(UA, "insert usb_out");
    setTime();
    s_phase = 1;
    s_cnt = 0;
    Log_Print("USBIN: Begin, jogadores 0x%X, perfis ativos P1=%d P2=%d, idioma %d\n", s_active, s_hasUsb[0], s_hasUsb[1], g_game.svcLangOption);
}

/* jogador que entra com a tela já aberta: as cenas dele recomeçam do primeiro quadro */
static void resetPlayerScenes(int p)
{
    static const struct { int bga; const char* fmt; } k[] = {
        { UA, "lisence_in_%dP_IN" }, { UA, "lisence_in_%dP_OUT" },
        { UA, "lisence_in_%dP_IN_(NOT USE USB)" }, { UA, "lisence_in_%dP_OUT_(NOT USE USB)" },
        { UB, "LISENCE_INFO_%dP_IN" }, { UB, "LISENCE_INFO_%dP_OUT" },
        { UC, "LISENCE_ID_%dP_IN" }, { UC, "LISENCE_ID_%dP_OUT" },
        { UD, "MESSAGE_WINDOW_%dP" }, { UD, "text_%dp" },
        { UE, "INFO_b_%dP_IN" }, { UE, "INFO_b_%dP_OUT" },
    };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        char n[96];
        snprintf(n, sizeof(n), k[i].fmt, p + 1);
        BGA_SceneReset(k[i].bga, n);
    }
}

/* ---- ProcessInput ----------------------------------------------------------------------------- */

static void processInput(int p, bool* leaveNow)
{
    if (!(s_active & (1 << p))) {                         /* jogador ausente: CENTER com crédito entra */
        if (Input_IsPadHit(p, PAD_C) && Coin_HasCredit()) {
            Coin_ConsumeCredit();
            Audio_Play(g_waveSoundIds[SND_2_1], false);
            Nx2Profile_Unload(p);
            s_hasUsb[p] = false; s_error[p] = false;
            s_endInput[p] = false;
            s_selected = false;
            s_active |= (1u << p);
            Title_SetJoinedMask(Title_GetJoinedMask() | (1u << p));
            setTime();
            setMessage(p, MSG_CONNECT);
            resetPlayerScenes(p);
        }
        return;
    }
    if (Input_IsPadHit(p, PAD_C)) {
        if (!s_endInput[p]) { s_endInput[p] = true; return; }   /* desiste do USB */
        if (s_selected) *leaveNow = true;                        /* m_iTime = 0 */
    }
}

/* ---- Update ----------------------------------------------------------------------------------- */

void NxUsbIn_Update(float dt)
{
    if (g_game.stateFrame == 1) NxUsbIn_Enter();
    Movie_Update(dt);
    if (!s_ok) { Game_ChangeState(Nx2Select_JoinPending() ? STATE_SELECT : STATE_STATION); return; }

    if (s_phase == 1) {                                   /* Start: intro */
        if (BGA_SceneDone(UA, "USB_intro")) { s_t0 = timeGetTime(); s_phase = 2; }
        return;
    }
    if (s_phase == 3) {                                   /* End: fade e saída */
        if (++s_cnt >= 60) {
            for (int p = 0; p < 2; p++) {
                if (!(s_active & (1 << p)) || !s_hasUsb[p]) { if (!s_hasUsb[p]) Nx2Profile_Unload(p); }
            }
            Movie_Close();
            Resource_ClearBGA();
            Game_ChangeState(Nx2Select_JoinPending() ? STATE_SELECT : STATE_STATION);
        }
        return;
    }

    /* phase 2 (Run) */
    s_time = s_left - (int)((timeGetTime() - s_t0) / 1000);
    bool leaveNow = false;
    for (int p = 0; p < 2; p++) {
        if (!(s_active & (1 << p)) || s_endInput[p]) continue;
        if ((g_game.stateFrame & 0xF) != 0) continue;     /* a cada 16 quadros */
        int r = Nx2Profile_Load(p);
        if (r == NX2_USB_OK) {
            s_hasUsb[p] = true;
            setLicense(p);
            s_endInput[p] = true;
        } else if (r == NX2_USB_MOUNTERROR || r == NX2_USB_BADDATA) {
            s_error[p] = true; s_hasUsb[p] = false;
            setMessage(p, MSG_USB_ERR);
            s_endInput[p] = true;
        } else if (r == NX2_USB_NODATA) {
            s_error[p] = true; s_hasUsb[p] = false;
            setMessage(p, MSG_REG_ERR);
            s_endInput[p] = true;
        }
        /* NOTINSERTED / NOCONFIG: continua esperando o pendrive (o original também) */
    }
    for (int p = 0; p < 2; p++) processInput(p, &leaveNow);
    if (leaveNow) s_time = 0;

    if (s_time <= 0) {
        if (s_selected) {
            Audio_Play(g_waveSoundIds[SND_START], false);   /* Effect("EFF_START") */
            for (int p = 0; p < 2; p++) if (s_active & (1 << p)) Nx2Sess_Start(p);   /* g_Player[].m_iMileage = CREDIT_MILEAGE (select.cpp:2215) */
            s_cnt = 0;
            s_phase = 3;
            return;
        }
        s_endInput[0] = s_endInput[1] = true;             /* tempo esgotado: segue sem USB */
    }
    if (s_endInput[0] && s_endInput[1] && !s_selected) {
        setTime();                                        /* SHOW_TIME */
        s_selected = true;
    }
}

/* ---- Render ----------------------------------------------------------------------------------- */

static void drawLicense(int p)   /* DrawLisenceWnd (modo entrada) */
{
    if (!(s_active & (1 << p))) return;
    if (s_hasUsb[p]) {
        sceneP(UA, "lisence_in_%dP_IN", p, true);
        sceneP(UB, "LISENCE_INFO_%dP_IN", p, true);
        sceneP(UC, "LISENCE_ID_%dP_IN", p, true);
        sceneP(UE, "INFO_b_%dP_IN", p, true);
    } else {
        sceneP(UA, "lisence_in_%dP_IN_(NOT USE USB)", p, true);
    }
}

void NxUsbIn_Render(void)
{
    Movie_Render();
    if (!s_ok) return;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1, 1, 1, 1);

    if (s_phase == 1) { BGA_ScenePlay(UA, "USB_intro", true); return; }

    if (s_phase == 3) {                                   /* End: fade sob as cenas OUT (DrawEndScene) */
        int a = s_cnt * 255 / 60; if (a > 255) a = 255;
        Render_Rect(0.0f, 0.0f, 640.0f, 480.0f, 0, 0, 0, (uint8_t)a);
        glColor4f(1, 1, 1, 1);
        BGA_ScenePlay(UA, "insert usb_out", true);
        for (int p = 0; p < 2; p++) {
            if (!(s_active & (1 << p))) continue;
            if (s_hasUsb[p]) {
                sceneP(UA, "lisence_in_%dP_OUT", p, true);
                sceneP(UB, "LISENCE_INFO_%dP_OUT", p, true);
                sceneP(UC, "LISENCE_ID_%dP_OUT", p, true);
                sceneP(UE, "INFO_b_%dP_OUT", p, true);
            } else {
                sceneP(UA, "lisence_in_%dP_OUT_(NOT USE USB)", p, true);
            }
        }
        return;
    }

    BGA_ScenePlay(UA, "insert usb_hold", true);
    for (int p = 0; p < 2; p++) if (s_endInput[p]) drawLicense(p);
    for (int p = 0; p < 2; p++)
        if ((s_active & (1 << p)) && (!s_endInput[p] || s_error[p])) {
            sceneP(UD, "MESSAGE_WINDOW_%dP", p, true);
            sceneP(UD, "text_%dp", p, true);
        }
    drawTime(s_time);
}
