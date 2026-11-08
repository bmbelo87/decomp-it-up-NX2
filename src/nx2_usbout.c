/*
 * nx2_usbout.c — tela "salvando as informações da sessão" (CUsbin em modo USB_OUTPUT) do NX2.
 *
 * Origem: nx2src ProcModules/usbin.cpp (modo USB_OUT_MODE: Begin, Run, End, ProcResult, SetLisenceOutWnd,
 * DrawLisenceWnd, DrawEndScene) e dancegrade.cpp:374/417 (acúmulo da sessão). A BGA principal é a
 * USB_REMOVE.DAT (no modo de entrada é a USB_INPUT.DAT); as demais são as mesmas (INFO_A, LISENCE_ID,
 * MESSAGE, INFO_B). As cenas RESULT_* ficam nessas BGAs.
 *
 * Fluxo (Run):
 *   1. Intro "USB_intro" da USB_REMOVE.
 *   2. Espera até 30 s (RESULT_TIME). CENTER de quem tem perfil zera o tempo. Passados 300 quadros, a cada
 *      quadro ProcResult transfere 2 de mileage, depois RW, WM, 2 de kcal e 2 de VO2 da sessão para o perfil;
 *      quando tudo foi transferido aparece a mensagem FINISH e SaveData (+ SaveRank no Arcade/Special) grava.
 *   3. Saída: fade preto de 60 quadros com as cenas OUT; vai para NAMEINPUT/GAMEOVER.
 *
 * FitnessInfo -> USB_OUTPUT: no piu (0x8062288) toda estação exceto Training passa por aqui, inclusive o
 * Special Zone (o `break` que falta no nx2src não existe no binário). A própria tela volta ao fluxo sem
 * mostrar nada quando ninguém tem perfil (Begin: Next(false)).
 *
 * Diferenças do port:
 *  - NÃO feito ainda: painel BOARD/BARI do WORLD MAX e CheckClearRate (recompensas pelo WORLD MAX);
 *    recordes por música do perfil (dancegrade.cpp:430+); o `activePlayerMask` não perde quem salvou
 *    (o original tira o jogador da máscara; aqui só decide GAMEOVER x NAMEINPUT).
 */
#include "pumpy.h"
#include "bga.h"
#include "movie.h"

#define UO_RESULT_TIME 30   /* RESULT_TIME */

enum { UA = 0, UB = 1, UC = 2, UD = 3, UE = 4 };   /* ordem de carga: USB_REMOVE, INFO_A, LISENCE_ID, MESSAGE, INFO_B */
enum { MSG_FINISH = 0, MSG_WRITE_ERR };

extern unsigned Title_GetJoinedMask(void);
GameState NxSports_NextState(void);
double Gameplay_SongTimeSec(void);
extern float g_nxKcal[2][4];
extern float g_nxVo2[2][4];

static bool     s_ok;
static int      s_phase;               /* 1 intro, 2 execução, 3 saída */
static uint32_t s_t0;
static int      s_time;
static int      s_cnt;                 /* m_Cnt */
static unsigned s_active;
static bool     s_hasUsb[2], s_resFin[2], s_saved[2], s_err[2];
static BGALayerSrc s_num[11], s_id[57], s_msg[2];
static int      s_sndMil = -1;

/* ---- acúmulo da sessão (dancegrade.cpp) ---------------------------------------------------------- */

void Nx2Sess_StageDone(int st, const float ratio[2], const int grade[2])
{
    if (st < 0 || st >= NX2_MAX_STAGE) return;
    if (st == 0) memset(g_nx2StageInfo, 0, sizeof(g_nx2StageInfo));
    float t = (float)(Gameplay_SongTimeSec() * 1000.0 / 60000.0);   /* mesmo tempo de NxSports_Store */
    for (int p = 0; p < 2; p++) {
        if (!(g_game.activePlayerMask & (1 << p))) continue;
        Nx2Sess_Start(p);
        Nx2Session* s = &g_nx2Sess[p];
        int L = g_nxLevel[p] > 0 ? g_nxLevel[p] : g_game.selectedDifficulty;
        if (L == 0) L = 1;
        if (L > 30) L = 30;
        int gain = 0;
        if (ratio[p] >= 0.75f) gain = 2 + L / 3;     /* STAGE_MILEAGE + level / LV_MILEAGE_DIVIDE; D = GradeRateTable[eGradeD] */
        s->mileage += gain;
        s->kcal += (int)g_nxKcal[p][st];
        s->vo2  += (int)g_nxVo2[p][st];
        Nx2RankUnit* u = &g_nx2StageInfo[p][st];
        u->iGameMode = g_nxDiffIdx[p];               /* rmtbl: N H C D NM */
        u->iPlayOrder = g_game.selectedSongIndex;
        u->iPlayScore = (int32_t)g_game.stats.score[p];
        u->iGrade = grade[p];
        u->iMileage = gain;
        u->fPlayTime = t;
        u->fKcal = g_nxKcal[p][st];
        u->fVO2 = g_nxVo2[p][st];
    }
}

/* ---- utilitários (iguais aos de nx2_usbin.c) ----------------------------------------------------- */

static int cnvCharToValue(int ch)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789~!@#$%^&*()_-+=\\:;/?";
    if (ch == ' ') return 59;
    for (int i = 0; t[i]; i++) if (t[i] == ch) return i;
    return (ch == 0) ? 58 : 59;
}

static void sceneP(int bga, const char* fmt, int player)
{
    char n[96];
    snprintf(n, sizeof(n), fmt, player + 1);
    BGA_ScenePlay(bga, n, true);
}

static void setMessage(int p, int msg) { BGA_SetLayerSrc(UD, 21 + p, &s_msg[msg]); }

static void numLayers(int bga, int first, int last, int base, int val, bool nullIfZero)
{   /* dígitos de `last` (unidade) a `first`; o mais alto vira "num_null" quando o valor é 0 */
    for (int i = last; i >= first; i--) {
        int d = (nullIfZero && i == first && val == 0) ? 10 : val % 10;
        BGA_SetLayerSrc(bga, i + base, &s_num[d]);
        val /= 10;
    }
}

static int clampi(int v, int hi) { return v < 0 ? 0 : (v > hi ? hi : v); }

static void setEmoticon(int p)   /* SetEmoticon: camada 29 (+4 por jogador) da USB_REMOVE */
{
    int idx = g_nx2Prof[p].iCharacter;
    if (idx < 0 || idx > 127) return;
    char dat[MAX_PATH], name[16];
    snprintf(dat, sizeof(dat), "%s/BGA/USB_CHARACTER.DAT", g_game.currentDirectory);
    snprintf(name, sizeof(name), "ch_%03d.dds", idx + 1);
    uint32_t sz = 0;
    uint8_t* d = Resource_ExtractFromPack(dat, name, &sz);
    if (!d) { Log_Print("USBOUT: personagem '%s' não encontrado\n", name); return; }
    int tex = Texture_LoadFromMemory(d, sz, name);
    free(d);
    BGALayerSrc src;
    if (tex >= 0 && BGA_GetLayerSrc(UA, 29 + p * 4, &src) && src.isSPR && src.sprTileCount > 0 && src.sprTileStart >= 0)
        g_game.sprTiles[src.sprTileStart].texId = tex;
}

static void setLicenseOut(int p)   /* SetLisenceOutWnd sem o emoticon (feito uma vez no Begin) */
{
    const Nx2Stat* st = &g_nx2Prof[p];
    const Nx2Session* se = &g_nx2Sess[p];
    for (int i = 0; i < 8; i++) {
        int v = cnvCharToValue((unsigned char)st->PlayerID[i]);
        if (v == 58) v = 56;
        if (v > 56) v = 56;
        BGA_SetLayerSrc(UC, i + 1 + p * 10, &s_id[v]);
    }
    numLayers(UB, 0, 6, p * 24, clampi(st->iMileage, 9999999), false);                       /* SetMileage */
    numLayers(UB, 7, 10, p * 24, clampi(st->m_iRewardCnt * 1000 / 70, 1000), true);          /* SetRewardClear */
    numLayers(UB, 11, 14, p * 24, clampi(st->m_iWorldMaxCnt * 1000 / 333, 1000), true);      /* SetWorldMaxClear */
    for (int i = 15, v = clampi(st->ikcal, 9999999); i >= 9; i--, v /= 10) BGA_SetLayerSrc(UE, i + p * 30, &s_num[v % 10]);   /* SetSports */
    for (int i = 23, v = clampi(st->iVO2, 9999999); i >= 17; i--, v /= 10) BGA_SetLayerSrc(UE, i + p * 30, &s_num[v % 10]);
    /* SetCurrentGameData: o que a sessão ainda tem a transferir */
    numLayers(UB, 48, 52, p * 19, se->mileage, false);
    numLayers(UB, 53, 56, p * 19, clampi(se->curRW * 1000 / 70, 1000), true);
    numLayers(UB, 57, 60, p * 19, clampi(se->curWM * 1000 / 333, 1000), true);
    numLayers(UB, 61, 63, p * 19, clampi(se->kcal, 9999999), false);
    numLayers(UB, 64, 66, p * 19, clampi(se->vo2, 9999999), false);
}

/* ---- Begin ---------------------------------------------------------------------------------------- */

static GameState nextAfter(bool allSaved)
{
    /* UsbOutput (flowcontrol.cpp:224 / piu 0x80623e2): só o Arcade com estágio cumprido vai ao NAMEINPUT, e
     * Next(true) (todos salvaram) vai direto ao GAMEOVER */
    if (allSaved || g_game.nxGameMode != 0) return STATE_GAMEOVER_ENTER;
    return NxSports_NextState();
}

static void leave(bool allSaved)
{
    Movie_Close();
    Resource_ClearBGA();
    Game_ChangeState(nextAfter(allSaved));
}

static bool begin(void)
{
    s_active = Title_GetJoinedMask() & 3;
    if (!s_active) s_active = g_game.activePlayerMask & 3;
    for (int p = 0; p < 2; p++) {
        s_resFin[p] = s_saved[p] = s_err[p] = false;
        if (g_nx2HasProfile[p] && !Nx2Profile_CheckUsb(p)) {   /* pendrive retirado no meio da sessão: SetHasUsb(false) */
            Log_Print("USBOUT: P%d pendrive retirado, perfil descartado\n", p + 1);
            Nx2Profile_Unload(p);
        }
    }
    if (!g_nx2HasProfile[0] && !g_nx2HasProfile[1]) {          /* ninguém com perfil: Next(false) sem mostrar a tela */
        Log_Print("USBOUT: sem perfil, segue direto\n");
        return false;
    }
    for (int p = 0; p < 2; p++) {
        s_hasUsb[p] = (s_active & (1 << p)) && g_nx2HasProfile[p];
        if (s_hasUsb[p]) {
            Nx2Sess_Start(p);                                   /* sessão sem passar pelo USBIN (depuração) */
            if (++g_nx2Prof[p].iPlayCount == 0) --g_nx2Prof[p].iPlayCount;
        }
    }

    Movie_Close();
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/BGA/USB_BG.MOV", g_game.currentDirectory);
    Movie_Open(path, true);

    Resource_ClearBGA();
    static const char* const k_bga[5] = { "USB_REMOVE", "USB_INFO_A", "USB_LISENCE_ID", "USB_MESSAGE", "USB_INFO_B" };
    s_ok = true;
    for (int i = 0; i < 5; i++)
        if (!Resource_LoadBGAByName(k_bga[i])) { Log_Print("USBOUT: falha ao carregar BGA/%s.DAT\n", k_bga[i]); s_ok = false; }
    BGA_Reset();

    if (s_ok) {
        for (int i = 0; i < 11; i++) s_ok &= BGA_GetLayerSrc(UB, 87 + i, &s_num[i]);
        for (int i = 0; i < 57; i++) s_ok &= BGA_GetLayerSrc(UC, 40 + i, &s_id[i]);
        int lang = g_game.svcLangOption & 3;                    /* 0 KR, 1 EN, 2 ES, 3 PT */
        int base = (lang == 0) ? 60 : (lang == 2) ? 80 : (lang == 1) ? 70 : 90;
        s_ok &= BGA_GetLayerSrc(UD, base + 6, &s_msg[MSG_FINISH]);
        s_ok &= BGA_GetLayerSrc(UD, base + 7, &s_msg[MSG_WRITE_ERR]);
        if (!s_ok) Log_Print("USBOUT: alguma camada de origem não existe\n");

        /* idioma nas camadas 28 e 32 (licença) da USB_REMOVE */
        static const int k_lic1[4] = { 75, 76, 77, 78 }, k_lic2[4] = { 81, 82, 83, 84 };
        BGALayerSrc a;
        if (BGA_GetLayerSrc(UA, k_lic1[lang], &a)) BGA_SetLayerSrc(UA, 28, &a);
        if (BGA_GetLayerSrc(UA, k_lic2[lang], &a)) BGA_SetLayerSrc(UA, 32, &a);

        for (int p = 0; p < 2; p++) {
            if (!s_hasUsb[p]) continue;
            setEmoticon(p);
            setLicenseOut(p);
        }
    }
    BGA_SceneReset(UA, "USB_intro");
    BGA_SceneReset(UA, "insert usb_hold");
    BGA_SceneReset(UA, "insert usb_out");
    s_phase = 1;
    s_cnt = 0;
    s_t0 = timeGetTime();
    Log_Print("USBOUT: Begin, jogadores 0x%X, perfis P1=%d P2=%d, sessão P1 mil %d kcal %d vo2 %d | P2 mil %d kcal %d vo2 %d\n",
              s_active, s_hasUsb[0], s_hasUsb[1],
              g_nx2Sess[0].mileage, g_nx2Sess[0].kcal, g_nx2Sess[0].vo2,
              g_nx2Sess[1].mileage, g_nx2Sess[1].kcal, g_nx2Sess[1].vo2);
    return true;
}

/* ---- ProcResult: transfere a sessão ao perfil ------------------------------------------------------- */

static void milSound(void)
{
    if (s_sndMil < 0) s_sndMil = Audio_LoadWaveFile("MILEAGE.WAV");   /* EFF_MILEAGE (SFX_GLOBAL.LUA) */
    if (s_sndMil >= 0) Audio_Play(s_sndMil, false);
}

static void procResult(int p)
{
    Nx2Session* se = &g_nx2Sess[p];
    Nx2Stat* st = &g_nx2Prof[p];
    int val;
    if (se->mileage > 0) {
        se->mileage -= 2;
        if (se->mileage > 0) val = 2; else { val = 2 + se->mileage; se->mileage = 0; }
        st->iMileage += val; if (st->iMileage < val) st->iMileage = 0x7FFFFFFF;
        milSound();
    } else if (se->curRW > 0) {
        se->curRW--; st->m_iRewardCnt++;
        milSound();
    } else if (se->curWM > 0) {
        se->curWM--; st->m_iWorldMaxCnt++;
        milSound();
    } else if (se->kcal > 0) {
        se->kcal -= 2;
        if (se->kcal > 0) val = 2; else { val = 2 + se->kcal; se->kcal = 0; }
        st->ikcal += val; if (st->ikcal < val) st->ikcal = 0x7FFFFFFF;
        milSound();
    } else if (se->vo2 > 0) {
        se->vo2 -= 2;
        if (se->vo2 > 0) val = 2; else { val = 2 + se->vo2; se->vo2 = 0; }
        st->iVO2 += val; if (st->iVO2 < val) st->iVO2 = 0x7FFFFFFF;
        milSound();
    } else if (!s_resFin[p]) {
        s_resFin[p] = true;
        setMessage(p, MSG_FINISH);   /* CheckClearRate (recompensas pelo WORLD MAX): não portado */
    }
}

static void saveIfReady(int p)
{
    if (!(s_active & (1 << p)) || !s_resFin[p] || !g_nx2HasProfile[p] || !Nx2Profile_CheckUsb(p)) return;
    if (s_saved[p]) return;
    int r = Nx2Profile_Save(p);
    if (g_game.nxGameMode == 0 || g_game.nxGameMode == 1)      /* Arcade ou Special: SaveRank; o resultado dele é o que vale */
        r = Nx2Profile_SaveRank(p, g_nx2StageInfo[p]);
    s_saved[p] = (r == NX2_USB_OK);
    Log_Print("USBOUT: P%d salvo, resultado %d\n", p + 1, r);
    if (r == NX2_USB_CANNOTWRITE) {
        setMessage(p, MSG_WRITE_ERR);
        s_saved[p] = s_err[p] = true;
    }
}

static bool allDone(void)
{
    for (int p = 0; p < 2; p++) {
        bool need = (s_active & (1 << p)) && g_nx2HasProfile[p] && Nx2Profile_CheckUsb(p);
        if (need && !(s_resFin[p] && s_saved[p])) return false;
    }
    return true;
}

/* ---- Update / Render -------------------------------------------------------------------------------- */

void NxUsbOut_Update(float dt)
{
    if (g_game.stateFrame == 1) {
        if (!begin()) { Game_ChangeState(nextAfter(false)); return; }
    }
    Movie_Update(dt);
    if (!s_ok) { Game_ChangeState(nextAfter(false)); return; }

    if (s_phase == 1) {
        if (BGA_SceneDone(UA, "USB_intro")) { s_t0 = timeGetTime(); s_phase = 2; }
        return;
    }
    if (s_phase == 3) {
        if (++s_cnt >= 60) {
            bool allSaved = true;                              /* quem salvou sai da máscara; sobrou alguém? */
            for (int p = 0; p < 2; p++)
                if ((s_active & (1 << p)) && !s_saved[p]) allSaved = false;
            leave(allSaved);
        }
        return;
    }

    /* phase 2 (Run) */
    s_time = UO_RESULT_TIME - (int)((timeGetTime() - s_t0) / 1000);
    for (int p = 0; p < 2; p++)                                /* ProcessInput (modo OUT): CENTER de quem tem perfil */
        if (g_nx2HasProfile[p] && Input_IsPadHit(p, PAD_C)) s_time = 0;

    if (s_time <= 0) {
        if (s_cnt < 300) s_cnt = 300;
        if (allDone()) { s_cnt = 0; s_phase = 3; return; }
    }
    if (s_cnt > 300) {
        for (int p = 0; p < 2; p++) {
            if (!(s_active & (1 << p))) continue;
            if (!g_nx2HasProfile[p]) continue;
            procResult(p);
            setLicenseOut(p);
        }
        for (int p = 0; p < 2; p++) saveIfReady(p);
    }
    s_cnt++;
}

void NxUsbOut_Render(void)
{
    Movie_Render();
    if (!s_ok) return;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1, 1, 1, 1);

    if (s_phase == 1) { BGA_ScenePlay(UA, "USB_intro", true); return; }

    if (s_phase == 3) {                                       /* End: fade sob as cenas OUT (DrawEndScene) */
        int a = s_cnt * 255 / 60; if (a > 255) a = 255;
        Render_Rect(0.0f, 0.0f, 640.0f, 480.0f, 0, 0, 0, (uint8_t)a);
        glColor4f(1, 1, 1, 1);
        BGA_ScenePlay(UA, "insert usb_out", true);
        for (int p = 0; p < 2; p++) {
            if (!(s_active & (1 << p))) continue;
            if (s_hasUsb[p]) {
                sceneP(UA, "lisence_in_%dP_OUT", p);
                sceneP(UB, "RESULT_LISENCE_INFO_%dP_OUT", p);
                sceneP(UC, "RESULT_ LISENCE_ID_%dP_OUT", p);
                sceneP(UE, "RESULT_INFO_b_%dP_OUT", p);
            } else {
                sceneP(UA, "lisence_in_%dP_OUT_(NOT USE USB)", p);
            }
        }
        return;
    }

    BGA_ScenePlay(UA, "insert usb_hold", true);
    for (int p = 0; p < 2; p++) {                             /* DrawLisenceWnd (modo OUT) */
        if (!(s_active & (1 << p))) continue;
        if (s_hasUsb[p]) {
            sceneP(UA, "lisence_in_%dP_IN", p);
            sceneP(UB, "RESULT_LISENCE_INFO_%dP_IN", p);
            sceneP(UC, "RESULT_ LISENCE_ID_%dP_IN", p);
            sceneP(UE, "RESULT_INFO_b_%dP_IN", p);
        } else {
            sceneP(UA, "lisence_in_%dP_IN_(NOT USE USB)", p);
        }
    }
    for (int p = 0; p < 2; p++)                               /* DrawMessageWnd depois de gravar */
        if (s_saved[p] && s_hasUsb[p] && (s_active & (1 << p))) {
            sceneP(UD, "RESULT_MESSAGE_WINDOW_%dP", p);
            sceneP(UD, "RESULT_text_%dp", p);
        }
}
