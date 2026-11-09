/*
 * nx2_wgrade.c — resultado da missão do WORLD MAX (CWorldGrade, nx2src ProcModules/worldgrade.cpp + mission.cpp CalcExpr).
 *
 * BGA/WORLD_MAX_GRADE.DAT (cenas grade_in, rank_S..rank_F, misson_success / misson_fail), fonte dos números
 * BGA/WGRADE_FONT.DAT (GRADE_FONT_G/R/Y, 8 x 2 glifos de 32 x 32: 0..9, 10 = ponto, 11 = %), fundo BGA/WMAXGRADE.MOV.
 * Tempos do SCRIPT/UI/WORLDGRADE.LUA: números a partir do quadro 90 (intervalo 3), nota em 270, som da nota em 275, fade em 550.
 * A condição de clear é a lista de até 4 expressões da missão: basta uma verdadeira (CalcExpr != 0).
 *
 * NÃO feito ainda: eventos de chefe (castelo), recompensa escondida e barricada (BOARD/BARI), e os contadores por banco
 * (Perfect1, Miss1...): sem banco no chart do port, valem como o total.
 */
#include "pumpy.h"
#include "bga.h"
#include "movie.h"
#include "nx2_mission.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

int  Nx2World_Player(void);
const NxMission* Nx2World_CurMission(void);
extern bool g_nx2WGLottery;

/* WORLDGRADE.LUA */
#define WG_SCORE_START   90
#define WG_SCORE_INTERVAL 3
#define WG_GRADE_START   270
#define WG_GRADE_SOUND   275
#define WG_FADE_START    550
static const int k_scoreY[7] = { 370, 315, 260, 205, 150, 95, 40 };   /* SCORE_Y_1 = 370, HEIGHT = 55 */

enum { C_G = 0, C_R, C_Y };
enum { SN_TICK, SN_ANNOUNCE, SN_RANK0, SN_RANKB0 = SN_RANK0 + 6, SN_COUNT = SN_RANKB0 + 6 };
static const char* const k_wav[SN_COUNT] = {
    "TICK.WAV", "RANK.WAV",
    "RANK_S.WAV", "RANK_A.WAV", "RANK_B.WAV", "RANK_C.WAV", "RANK_D.WAV", "RANK_F.WAV",
    "9-S.WAV", "9-A.WAV", "9-B.WAV", "9-C.WAV", "9-D.WAV", "9-F.WAV",
};
static int  s_snd[SN_COUNT];
static bool s_sndInit;
static void sfx(int k) { if (s_snd[k] >= 0) Audio_Play(s_snd[k], false); }

static int   s_p, s_grade, s_cnt, s_cnt2, s_prevCnt;
static bool  s_clear, s_end, s_init;
static int   s_font[3];
static int   s_nd[14];
static int   s_gaugeVal;
static int   s_rewardMileage;

/* ---------------------------------------------------------------------------
 * CalcExpr (mission.cpp): infixa com pilhas; variáveis sem caixa, sufixo numérico = banco (3 = total)
 * ------------------------------------------------------------------------- */
static int s_vs[8], s_vd, s_os[8], s_od;
static int s_cp, s_cRank;

static const char* const k_vars[29] = {
    "rank", "S", "A", "B", "C", "D", "F", "perfect", "great", "good", "bad", "miss", "totalstep", "hidden", "allhidden",
    "maxcombo", "misscombo", "score", "item", "allitem", "heart", "allheart", "mine", "allmine", "potion", "allpotion",
    "velocity", "allvelocity", "life",
};

static int condVar(char* tok)
{
    size_t n = strlen(tok);
    int bank = 3;
    if (n && isdigit((unsigned char)tok[n - 1])) { bank = tok[n - 1] - '0'; tok[n - 1] = 0; }
    int i;
    for (i = 0; i < 29; i++) if (!_stricmp(tok, k_vars[i])) break;
    if (i >= 29) { Log_Print("NX2WGRADE: variavel desconhecida '%s'\n", tok); return 0; }
    if (bank < 3 && i >= 7 && i <= 12) {          /* perfect..miss e totalstep por banco (m_Stat[bank]) */
        if (i == 12) return g_nxBankTotal[s_cp][bank];
        return g_nxBankJudge[s_cp][bank][i - 7];
    }
    if (bank < 3 && (i == 15 || i == 16)) return i == 15 ? g_nxBankMaxCombo[s_cp][bank] : g_nxBankMaxMissCombo[s_cp][bank];
    const NxMisStats* m = &g_nxMis[s_cp];
    const GameplayStats* s = &g_game.stats;
    switch (i) {
    case 0: return s_cRank;
    case 1: return 5; case 2: return 4; case 3: return 3; case 4: return 2; case 5: return 1; case 6: return 0;
    case 7: return (int)s->perfectCount[s_cp];
    case 8: return (int)s->greatCount[s_cp];
    case 9: return (int)s->goodCount[s_cp];
    case 10: return (int)s->badCount[s_cp];
    case 11: return (int)s->missCount[s_cp];
    case 12: return (int)(s->perfectCount[s_cp] + s->greatCount[s_cp] + s->goodCount[s_cp] + s->badCount[s_cp] + s->missCount[s_cp]);
    case 13: return m->hidden;
    case 14: return m->allHidden;
    case 15: return (int)s->maxCombo[s_cp];
    case 16: return (int)s->missCombo[s_cp];
    case 17: return (int)s->score[s_cp];
    case 18: return m->item;
    case 19: return m->allItem;
    case 20: return m->heart;
    case 21: return m->allHeart;
    case 22: return m->mine;
    case 23: return m->allMine;
    case 24: return m->potion;
    case 25: return m->allPotion;
    case 26: return m->velocity;
    case 27: return m->allVelocity;
    case 28: return (int)s->life[s_cp];
    }
    return 0;
}

static const struct { const char* op; int prec; } k_op[13] = {
    { "*", 0 }, { "/", 0 }, { "%", 0 }, { "+", 1 }, { "-", 1 }, { "<", 2 }, { ">", 2 }, { "<=", 2 }, { ">=", 2 },
    { "==", 3 }, { "!=", 3 }, { "&&", 4 }, { "||", 5 },
};

static bool calc(void)
{
    if (s_vd < 2 || !s_od) return false;
    int a = s_vs[s_vd - 2], b = s_vs[s_vd - 1];
    s_vd--;
    switch (s_os[--s_od]) {
    case 0: a *= b; break;
    case 1: a = b ? a / b : 0; break;
    case 2: a = b ? a % b : 0; break;
    case 3: a += b; break;
    case 4: a -= b; break;
    case 5: a = a < b; break;
    case 6: a = a > b; break;
    case 7: a = a <= b; break;
    case 8: a = a >= b; break;
    case 9: a = a == b; break;
    case 10: a = a != b; break;
    case 11: a = a && b; break;
    case 12: a = a || b; break;
    default: return false;
    }
    s_vs[s_vd - 1] = a;
    return true;
}

static void nextTok(const char** str, char* tok)
{
    int a = 0;
    while (isspace((unsigned char)**str)) ++*str;
    if (**str) {
        if (isdigit((unsigned char)**str)) { while (isdigit((unsigned char)**str) && a < 15) tok[a++] = *(*str)++; }
        else if (isalpha((unsigned char)**str)) { while (isalnum((unsigned char)**str) && a < 15) tok[a++] = *(*str)++; }
        else if (**str == '(' || **str == ')') tok[a++] = *(*str)++;
        else { while (**str && !isalnum((unsigned char)**str) && **str != '(' && **str != ')' && !isspace((unsigned char)**str) && a < 15) tok[a++] = *(*str)++; }
    }
    tok[a] = 0;
}

static int calcExpr(int player, int rank, const char* expr)
{
    char tok[16];
    if (!expr) return 1;
    s_cp = player; s_cRank = rank; s_od = s_vd = 0;
    for (;;) {
        nextTok(&expr, tok);
        if (!tok[0]) {
            while (s_od) if (!calc()) break;
            return s_vd ? s_vs[0] : 0;
        } else if (isdigit((unsigned char)tok[0])) { if (s_vd < 8) s_vs[s_vd++] = atoi(tok); }
        else if (isalpha((unsigned char)tok[0])) { if (s_vd < 8) s_vs[s_vd++] = condVar(tok); }
        else if (!strcmp(tok, "(")) { if (s_od < 8) s_os[s_od++] = -1; }
        else if (!strcmp(tok, ")")) {
            while (s_od && s_os[s_od - 1] != -1) if (!calc()) break;
            if (s_od) --s_od;
        } else {
            int a;
            for (a = 0; a < 13; a++) if (!strcmp(tok, k_op[a].op)) break;
            if (a >= 13) { Log_Print("NX2WGRADE: operador desconhecido '%s'\n", tok); continue; }
            while (s_od) {
                if (s_os[s_od - 1] < 0) break;
                if (k_op[a].prec < k_op[s_os[s_od - 1]].prec) break;
                calc();
            }
            if (s_od < 8) s_os[s_od++] = a;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Contadores animados (CWorldMaxFont::ScoreDraw3 / ScoreDraw2)
 * ------------------------------------------------------------------------- */
static int pow10i(int i) { int v = 1; while (i-- > 0) v *= 10; return v; }

static void glyph(int color, float x, float yUp, int n)
{
    if (s_font[color] < 0 || n < 0 || n > 15) return;
    float u = (float)((n % 8) * 32), v = (float)((n / 8) * 32);
    Texture_DrawUV(s_font[color], x, 480.0f - yUp - 32.0f, 32.f, 32.f, u, v, u + 32.f, v + 32.f, 1.f, 1.f, 1.f, 1.f);
}

/* ScoreDraw3: um dígito por vez rola de 0 até o valor, 10 quadros por dígito */
static void scoreDraw3(int x, int y, int num, int w, int cnt, int color)
{
    if (cnt < 0) return;
    int phase = 0, cnt2 = 0;
    x -= 22 * (w - 1);
    for (int i = w - 1; i >= 0; i--) {
        int number = num / pow10i(i);
        cnt2 += 10;
        if ((w - 1) - i < phase) glyph(color, (float)x, (float)y, number % 10);
        else if ((w - 1) - i == phase) {
            if (cnt < cnt2) glyph(color, (float)x, (float)y, (10 - (cnt2 - cnt)) % 10);
            if (cnt >= cnt2) { glyph(color, (float)x, (float)y, number % 10); phase++; }
        } else break;
        x += 22;
    }
}

/* ScoreDraw2 (vida em décimos de %): w + 1 dígitos, ponto antes do último e % no fim */
static void scoreDraw2(int x, int y, int num, int w, int cnt, int color)
{
    if (cnt < 0) return;
    int phase = 0, cnt2 = 0;
    w++;
    x -= 22 * (w - 1);
    for (int i = w - 1; i >= 0; i--) {
        int number = num / pow10i(i);
        cnt2 += 10;
        if (i == 0) { glyph(color, (float)x, (float)y, 10); x += 22; }
        if ((w - 1) - i < phase) glyph(color, (float)x, (float)y, number % 10);
        else if ((w - 1) - i == phase) {
            if (cnt < cnt2) glyph(color, (float)x, (float)y, (10 - (cnt2 - cnt)) % 10);
            if (cnt >= cnt2) { glyph(color, (float)x, (float)y, number % 10); phase++; }
        } else break;
        x += 22;
        if (phase > w - 1) { x += 10; glyph(color, (float)x, (float)y, 11); }
    }
}

/* ---------------------------------------------------------------------------
 * Ciclo
 * ------------------------------------------------------------------------- */
static const NxMission* mission(void) { return Nx2World_CurMission(); }

void Nx2WG_Enter(int player, int grade)
{
    s_p = player;
    s_grade = (grade < 0 || grade > 5) ? 5 : grade;       /* 0 S .. 5 F */
    s_cnt = s_cnt2 = s_prevCnt = 0;
    s_end = false;
    s_init = false;
    s_rewardMileage = 0;
    const NxMission* m = mission();
    int rank = 5 - s_grade;

    /* condições: o RUN passa as 4 como argumentos; as vazias somem e as demais sobem (playengine.cpp:935..1054) */
    char cc[4][NXM_CONDITION_MAX];
    snprintf(cc[0], sizeof(cc[0]), "1");
    for (int b = 1; b < 4; b++) snprintf(cc[b], sizeof(cc[b]), "0");
    if (m) {
        int j = 0;
        for (int a = 0; a < 4; a++)
            if (m->conditions[a].condition[0] && j < 4) snprintf(cc[j++], sizeof(cc[0]), "%s", m->conditions[a].condition);
    }
    s_clear = false;
    Nx2Stat* ps = Nx2_Stat(player);
    int tracknum = 0;
    if (m) sscanf(&m->step[2], "%3d", &tracknum);
    for (int a = 0; a < 4; a++) {
        bool chk = calcExpr(player, rank, cc[a]) != 0;
        Log_Print("NX2WGRADE: condicao %d \"%s\" -> %d\n", a, cc[a], chk ? 1 : 0);
        if (chk) {
            s_clear = true;
            if (m && tracknum >= 0 && tracknum < NX2_MISSION_MAX) ps->bmWMClear[tracknum] |= (char)m->conditions[a].direction;
        }
    }
    if (s_clear) g_nx2Sess[player].curWM++;                /* m_iCurWMClearCnt */

    /* mileage: D ou melhor; a loteria sorteia 0,0..2,5 (x o mileage da missão + nível / 3) */
    if (m && rank >= 1) {
        float lot = (float)(rand() % 26) / 10.0f;
        int base = m->mileage + m->level / 3;
        s_rewardMileage = g_nx2WGLottery ? (int)(lot * (float)base) : base;
        g_nx2Sess[player].mileage += s_rewardMileage;
        g_nx2Opt.g_MileageTotal += s_rewardMileage;
        if (g_nx2Opt.g_MileageTotal < 0) g_nx2Opt.g_MileageTotal = 0x7FFFFFFF;
    }
    Log_Print("NX2WGRADE: missao %s nota %c -> %s, mileage +%d%s\n", m ? m->step : "?", "SABCDF"[s_grade], s_clear ? "SUCCESS" : "FAILED",
              s_rewardMileage, g_nx2WGLottery ? " (loteria)" : "");

    for (int k = 0; k < 14; k++) s_nd[k] = 4;
    s_nd[12] = 3; s_nd[13] = 9;
    {
        float disp = 1000.0f;
        if (g_nx2WGDisp) disp = (float)g_nx2WGDisp;
        disp += (float)g_nx2WGBonus;
        s_gaugeVal = (int)(1000.0f * (float)g_game.stats.life[player] / disp);
        if (s_gaugeVal < 0) s_gaugeVal = 0;
        if (s_gaugeVal > 999) s_gaugeVal = 999;            /* 3 dígitos: 100,0% não cabe no formato original */
    }
}

static void lazyInit(void)
{
    if (s_init) return;
    s_init = true;
    char path[MAX_PATH];
    for (int i = 0; i < 3; i++) s_font[i] = -1;
    snprintf(path, sizeof(path), "%s/BGA/WGRADE_FONT.DAT", g_game.currentDirectory);
    if (RES_Open(path)) {
        s_font[C_G] = loadTextureFromRES("GRADE_FONT_G.tga");
        s_font[C_R] = loadTextureFromRES("GRADE_FONT_R.tga");
        s_font[C_Y] = loadTextureFromRES("GRADE_FONT_Y.tga");
        RES_Close();
    }
    Movie_Close();
    snprintf(path, sizeof(path), "%s/BGA/WMAXGRADE.MOV", g_game.currentDirectory);
    Movie_Open(path, true);
    if (!s_sndInit) {
        for (int k = 0; k < SN_COUNT; k++) s_snd[k] = Audio_LoadWaveFile(k_wav[k]);
        s_sndInit = true;
    }
    BGA_SceneReset(0, "grade_in");
    static const char* const k_rk[6] = { "rank_S", "rank_A", "rank_B", "rank_C", "rank_D", "rank_F" };
    for (int i = 0; i < 6; i++) BGA_SceneReset(0, k_rk[i]);
    BGA_SceneReset(0, "misson_success");
    BGA_SceneReset(0, "misson_fail");
}

/* true quando terminou (fade de 60 quadros depois do quadro 550) */
bool Nx2WG_Update(float dt)
{
    if (Movie_IsOpen()) Movie_Update(dt);
    if (!s_end && s_cnt >= WG_FADE_START) { s_end = true; s_cnt2 = 0; }
    if (s_cnt >= WG_SCORE_START && (s_cnt - s_prevCnt) > 4 &&
        s_cnt <= WG_SCORE_START + WG_SCORE_INTERVAL * 13 + 10 * s_nd[13]) { s_prevCnt = s_cnt; sfx(SN_TICK); }
    if (s_cnt == WG_GRADE_SOUND) sfx(SN_ANNOUNCE);
    if (s_cnt == WG_GRADE_SOUND + 5) { sfx(SN_RANKB0 + s_grade); sfx(SN_RANK0 + s_grade); }
    bool done = s_end && s_cnt2 > 60;
    s_cnt++;
    if (s_end) s_cnt2++;
    return done;
}

extern int g_testShotNow;
void Nx2WG_Render(void)
{
    lazyInit();
    if (getenv("NX2WG_SHOT") && (s_cnt == 200 || s_cnt == 330 || s_cnt == 460)) g_testShotNow = 1;   /* teste */
    if (Movie_IsOpen()) Movie_Render();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1.f, 1.f, 1.f, 1.f);
    BGA_ScenePlay(0, "grade_in", true);
    const GameplayStats* s = &g_game.stats;
    int p = s_p;
    if (s_cnt < WG_FADE_START) {
        int cnt = s_cnt;
        const NxMission* mm = mission();
        bool sep = mm && mm->seperate;                       /* missao com bancos separados: 3 colunas (G, R, Y) */
        int judgeCnt = sep ? 3 : 1;
        for (int b = 0; b < judgeCnt; b++) {
            int judge[5];
            if (sep) for (int a = 0; a < 5; a++) judge[a] = g_nxBankJudge[p][b][a];
            else { judge[0] = (int)s->perfectCount[p]; judge[1] = (int)s->greatCount[p]; judge[2] = (int)s->goodCount[p]; judge[3] = (int)s->badCount[p]; judge[4] = (int)s->missCount[p]; }
            int xb = 100 * (sep ? b : 1);
            for (int a = 0; a < 5; a++)
                if (cnt >= WG_SCORE_START + WG_SCORE_INTERVAL * b * a)
                    scoreDraw3((230 - 22 * (7 - s_nd[a])) / 2 + xb, k_scoreY[a], judge[a], s_nd[a], cnt - (WG_SCORE_START + WG_SCORE_INTERVAL * a), b);
            if (cnt >= WG_SCORE_START + WG_SCORE_INTERVAL * 5 * b)
                scoreDraw3((230 - 22 * (7 - s_nd[5])) / 2 + xb, k_scoreY[5], sep ? g_nxBankMaxCombo[p][b] : (int)s->maxCombo[p], s_nd[5], cnt - (WG_SCORE_START + WG_SCORE_INTERVAL * 5), b);
            if (cnt >= WG_SCORE_START + WG_SCORE_INTERVAL * 6 * b)
                scoreDraw3((235 - 22 * (7 - s_nd[6])) / 2 + xb, k_scoreY[6], sep ? g_nxBankMaxMissCombo[p][b] : (int)s->missCombo[p], s_nd[6], cnt - (WG_SCORE_START + WG_SCORE_INTERVAL * 5), b);
        }
        const NxMisStats* m = &g_nxMis[p];
        const int right[5] = { m->heart, m->potion, m->mine, m->velocity, m->item };
        for (int k = 0; k < 5; k++)
            if (cnt >= WG_SCORE_START + WG_SCORE_INTERVAL * (7 + k))
                scoreDraw3((1050 - 22 * (7 - s_nd[7 + k])) / 2, k_scoreY[k], right[k], s_nd[7 + k], cnt - (WG_SCORE_START + WG_SCORE_INTERVAL * (6 + k)), C_R);
        if (cnt >= WG_SCORE_START + WG_SCORE_INTERVAL * 12)
            scoreDraw2((1050 - 22 * (7 - s_nd[12])) / 2, k_scoreY[5], s_gaugeVal, s_nd[12], cnt - (WG_SCORE_START + WG_SCORE_INTERVAL * 11), C_R);
        if (cnt >= WG_SCORE_START + WG_SCORE_INTERVAL * 13)
            scoreDraw3((1050 - 22 * (7 - s_nd[13])) / 2, k_scoreY[6], (int)s->score[p], s_nd[13], cnt - (WG_SCORE_START + WG_SCORE_INTERVAL * 12), C_R);
        if (cnt >= WG_GRADE_START) {
            static const char* const k_rk[6] = { "rank_S", "rank_A", "rank_B", "rank_C", "rank_D", "rank_F" };
            BGA_ScenePlay(0, k_rk[s_grade], true);
        }
        if (cnt >= WG_GRADE_START + 110) BGA_ScenePlay(0, s_clear ? "misson_success" : "misson_fail", true);
    }
    if (s_end && s_cnt2) Render_Rect(0.0f, 0.0f, 640.0f, 480.0f, 0, 0, 0, (uint8_t)(255.0f * (float)s_cnt2 / 60.0f > 255.0f ? 255.0f : 255.0f * (float)s_cnt2 / 60.0f));
    glColor4f(1.f, 1.f, 1.f, 1.f);
}

/* g_StateManager.m_bCleared / Next(!m_bClear): sem vidas, o crédito acaba (salvar e GameOver); com vidas volta ao mapa */
GameState Nx2WG_Next(void)
{
    if (!s_clear) {
        g_game.stats.score[s_p] = 0;                          /* worldgrade.cpp: falhou, o score zera */
    }
    if (Nx2World_LivesLeft() <= 0) return STATE_NX_USBOUT;
    return s_clear ? STATE_STAGE_TRANSITION : STATE_SELECT;
}
