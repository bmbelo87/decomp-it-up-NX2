/*
 * nx2_mission.c — CMission / CMissionManager (nx2src ProcModules/CMissionManager.cpp) e o tokenizador NXParserExt.
 * Lê <jogo>/mission.txt. A lista de textos (desc) é compartilhada entre a lista total e a corrente (nunca liberada).
 */
#include "pumpy.h"
#include "nx2_mission.h"

#include <stdlib.h>
#include <string.h>

/* ---- NXParser.cpp: NXParserExt (cópia fiel; allowLineBreaks = 0 devolve vazio na quebra de linha) ---- */
#define NXP_MAX 1024
static char s_tok[NXP_MAX];

static char* nxParserExt(char** data_p, int allowLineBreaks)
{
    int len = 0, hasNewLines = 0;
    char* data = *data_p;
    char c = 0;
    s_tok[0] = 0;
    if (!data) { *data_p = NULL; return s_tok; }
    for (;;) {
        while ((unsigned char)(c = *data) <= ' ') {      /* NXSkipWhitespace */
            if (!c) { *data_p = NULL; return s_tok; }
            if (c == '\n') hasNewLines = 1;
            data++;
        }
        if (hasNewLines && !allowLineBreaks) { *data_p = data; return s_tok; }
        c = *data;
        if (c == '/' && data[1] == '/') {
            data += 2;
            while (*data && *data != '\n') data++;
        } else if (c == '/' && data[1] == '*') {
            data += 2;
            while (*data && (*data != '*' || data[1] != '/')) data++;
            if (*data) data += 2;
        } else break;
    }
    if (c == '\"') {
        data++;
        for (;;) {
            c = *data++;
            if (c == '\"' || !c) { s_tok[len] = 0; *data_p = data; return s_tok; }
            if (len < NXP_MAX - 1) s_tok[len++] = c;
        }
    }
    do {
        if (len < NXP_MAX) { if (c != '\r') s_tok[len++] = c; }
        data++;
        c = *data;
    } while ((unsigned char)c > ' ');
    if (len == NXP_MAX) len = 0;
    s_tok[len] = 0;
    *data_p = data;
    return s_tok;
}

/* ---- listas ---- */
static NxMission** s_all;   static int s_nAll, s_capAll;
static NxMission** s_cur;   static int s_nCur, s_capCur;
static NxMission*  s_nx2[15];
static bool        s_loaded, s_loadOk;

static void vpush(NxMission*** v, int* n, int* cap, NxMission* m)
{
    if (*n >= *cap) {
        int nc = *cap ? *cap * 2 : 512;
        NxMission** nv = (NxMission**)realloc(*v, (size_t)nc * sizeof(NxMission*));
        if (!nv) return;
        *v = nv; *cap = nc;
    }
    (*v)[(*n)++] = m;
}

/* strncpy como no original: copia até n bytes (a origem é um token de até 1023 bytes), sem garantir o NUL final */
#define CPYN(dst, src, n) strncpy((dst), (src), (n))

/* CMission::CMission */
static NxMission* missionNew(int x, int y, const short id[3])
{
    NxMission* m = (NxMission*)calloc(1, sizeof(NxMission));
    if (!m) return NULL;
    m->players = 1; m->level = 1; m->life = 1;
    m->gageMin = 500; m->gageMax = 1000;
    m->item = true; m->seperate = false; m->nxBreak = true; m->mileage = 0; m->cleared = false;
    strncpy(m->type, "\xEC\x9D\xBC\xEB\xB0\x98", NXM_NAME_MAX);   /* szNormal = "일반" */
    m->speed = 2;
    for (int i = 0; i < 4; i++) {
        m->conn[i].connectedID[2] = -1;
        m->conn[i].lock = false;
        m->conn[i].enabled = true;
        m->next[i] = NULL;
        m->challenge[i] = 10;
        m->conditions[i].direction = 0;
    }
    m->x = x; m->y = y;
    m->id[0] = id[0]; m->id[1] = id[1]; m->id[2] = id[2];
    m->state = NXM_NONE;
    return m;
}

static void setDesc(NxMission* m, int lang, int full, const char* s)
{
    free(m->desc[lang][full]);
    size_t n = strlen(s);
    char* d = (char*)malloc(n + 1);
    if (d) memcpy(d, s, n + 1);
    m->desc[lang][full] = d;
}

static int connDir(const char* t)
{
    if (!_stricmp(t, "UPPER_LEFT"))  return NXM_CONN_LU;
    if (!_stricmp(t, "UPPER_RIGHT")) return NXM_CONN_RU;
    if (!_stricmp(t, "LOWER_LEFT"))  return NXM_CONN_LL;
    if (!_stricmp(t, "LOWER_RIGHT")) return NXM_CONN_RL;
    return -1;
}

/* CMissionManager::parseMissionScript */
static bool parseMission(char** p, NxMission* m)
{
    char* t = nxParserExt(p, 1);
    if (_stricmp(t, "{")) return false;
    for (;;) {
        t = nxParserExt(p, 1);
        if (!*t && !*p) return false;                     /* fim do arquivo sem '}' (o original ficaria em laço) */
        if (!_stricmp(t, "ID")) {
            for (int i = 0; i < 3; i++) { t = nxParserExt(p, 0); m->id[i] = (short)atoi(t); }
        }
        else if (!_stricmp(t, "PLAYERS"))   { m->players = (short)atoi(nxParserExt(p, 0)); }
        else if (!_stricmp(t, "LEVEL"))     { m->level = (short)atoi(nxParserExt(p, 0)); }
        else if (!_stricmp(t, "LIFE"))      { m->life = (short)atoi(nxParserExt(p, 0)); }
        else if (!_stricmp(t, "MILEAGE"))   { m->mileage = (short)atoi(nxParserExt(p, 0)); }
        else if (!_stricmp(t, "TYPE"))      { CPYN(m->type, nxParserExt(p, 0), NXM_NAME_MAX); }
        else if (!_stricmp(t, "BREAK"))     { m->nxBreak = atoi(nxParserExt(p, 0)) != 0; }
        else if (!_stricmp(t, "ITEM"))      { m->item = atoi(nxParserExt(p, 0)) != 0; }
        else if (!_stricmp(t, "SEPERATE"))  { m->seperate = atoi(nxParserExt(p, 0)) != 0; }
        else if (!_stricmp(t, "RANDOM"))    { m->random = (short)atoi(nxParserExt(p, 0)); }
        else if (!_stricmp(t, "GAGE_MIN"))  { m->gageMin = (short)atoi(nxParserExt(p, 0)); }
        else if (!_stricmp(t, "GAGE_MAX"))  { m->gageMax = (short)atoi(nxParserExt(p, 0)); }
        else if (!strcmp(t, "LAND"))        { CPYN(m->land, nxParserExt(p, 0), NXM_NAME_MAX); }
        else if (!_stricmp(t, "NAME"))      { CPYN(m->name, nxParserExt(p, 0), NXM_NAME_MAX); }
        else if (!_stricmp(t, "REWARD"))    { CPYN(m->reward, nxParserExt(p, 0), NXM_FILE_MAX); }
        else if (!_stricmp(t, "SONG"))      { CPYN(m->song, nxParserExt(p, 0), NXM_FILE_MAX); }
        else if (!_stricmp(t, "STEP"))      { CPYN(m->step, nxParserExt(p, 0), NXM_FILE_MAX); }
        else if (!_stricmp(t, "STEP_LV"))   { CPYN(m->stepLV, nxParserExt(p, 0), 3); }
        else if (!_stricmp(t, "SPEED"))     { m->speed = (short)atoi(nxParserExt(p, 0)); }
        else if (!_stricmp(t, "SKIN"))      { for (int i = 0; i < 3; i++) CPYN(m->skin[i], nxParserExt(p, 0), 3); }
        else if (!_stricmp(t, "CHALLENGE")) { for (int i = 0; i < 4; i++) m->challenge[i] = (short)atoi(nxParserExt(p, 0)); }
        else if (!_stricmp(t, "CONDITION_1")) { CPYN(m->conditions[0].condition, nxParserExt(p, 0), NXM_CONDITION_MAX); }
        else if (!_stricmp(t, "DIRECTION_1")) { m->conditions[0].direction = (unsigned char)atoi(nxParserExt(p, 0)); }
        else if (!_stricmp(t, "CONDITION_2")) { CPYN(m->conditions[1].condition, nxParserExt(p, 0), NXM_CONDITION_MAX); }
        else if (!_stricmp(t, "DIRECTION_2")) { m->conditions[1].direction = (unsigned char)atoi(nxParserExt(p, 0)); }
        else if (!_stricmp(t, "CONDITION_3")) { CPYN(m->conditions[2].condition, nxParserExt(p, 0), NXM_CONDITION_MAX); }
        else if (!_stricmp(t, "DIRECTION_3")) { m->conditions[2].direction = (unsigned char)atoi(nxParserExt(p, 0)); }
        else if (!_stricmp(t, "CONDITION_4")) { CPYN(m->conditions[3].condition, nxParserExt(p, 0), NXM_CONDITION_MAX); }
        else if (!_stricmp(t, "DIRECTION_4")) { m->conditions[3].direction = (unsigned char)atoi(nxParserExt(p, 0)); }
        else if (!_stricmp(t, "COMMAND"))   { CPYN(m->command, nxParserExt(p, 0), NXM_CONDITION_MAX); }
        else if (!_stricmp(t, "DESC_KOR_S")) { setDesc(m, NXM_LANG_KOR, 0, nxParserExt(p, 0)); }
        else if (!_stricmp(t, "DESC_KOR_F")) { setDesc(m, NXM_LANG_KOR, 1, nxParserExt(p, 0)); }
        else if (!_stricmp(t, "DESC_EN_S"))  { setDesc(m, NXM_LANG_EN, 0, nxParserExt(p, 0)); }
        else if (!_stricmp(t, "DESC_EN_F"))  { setDesc(m, NXM_LANG_EN, 1, nxParserExt(p, 0)); }
        else if (!_stricmp(t, "DESC_SP_S"))  { setDesc(m, NXM_LANG_SP, 0, nxParserExt(p, 0)); }
        else if (!_stricmp(t, "DESC_SP_F"))  { setDesc(m, NXM_LANG_SP, 1, nxParserExt(p, 0)); }
        else if (!_stricmp(t, "DESC_CN_S"))  { setDesc(m, NXM_LANG_CN, 0, nxParserExt(p, 0)); }
        else if (!_stricmp(t, "DESC_CN_F"))  { setDesc(m, NXM_LANG_CN, 1, nxParserExt(p, 0)); }
        /* piu (0x80f769f): DESC_CNS_S / DESC_CNS_F; o nx2src não os trata */
        else if (!_stricmp(t, "DESC_CNS_S")) { setDesc(m, NXM_LANG_CNS, 0, nxParserExt(p, 0)); }
        else if (!_stricmp(t, "DESC_CNS_F")) { setDesc(m, NXM_LANG_CNS, 1, nxParserExt(p, 0)); }
        else if (!_stricmp(t, "COORD")) {
            m->x = atoi(nxParserExt(p, 0));
            m->y = atoi(nxParserExt(p, 0));
        }
        else if (connDir(t) >= 0) {
            int d = connDir(t);
            for (int i = 0; i < 3; i++) {
                m->conn[d].connectedID[i] = (short)atoi(nxParserExt(p, 0));
                m->conn[d].enabled = false;
            }
            m->conn[d].lock = (char)atoi(nxParserExt(p, 0));
        }
        else if (!_stricmp(t, "}")) break;
    }
    return true;
}

/* CMissionManager::parseScript */
static bool parseScript(char* p)
{
    for (;;) {
        char* t = nxParserExt(&p, 1);
        if (!t || !t[0]) break;
        if (!_stricmp(t, "*NX_SCRIPT")) {
            t = nxParserExt(&p, 1);
            if (!_stricmp(t, "{")) {
                for (;;) {
                    t = nxParserExt(&p, 1);
                    if (!t || !t[0]) break;
                    if (!_stricmp(t, "}")) break;
                    if (!_stricmp(t, "*NX_MISSION")) {
                        short id[3] = { 0, 0, 0 };
                        NxMission* m = missionNew(0, 0, id);
                        if (!m || !parseMission(&p, m)) { free(m); return false; }
                        vpush(&s_all, &s_nAll, &s_capAll, m);
                    }
                }
            }
        }
    }
    return true;
}

/* createNx2Mission: 15 pontos da NX2 ZONE (coordenadas e ids fixos do nx2src) */
static void createNx2(void)
{
    static const int cx[15] = { 447, 447, 535, 638, 638, 709, 709, 771, 836, 836, 936, 936, 1000, 1060, 1060 };
    static const int cy[15] = { 330, 442, 389, 330, 442, 330, 442, 389, 330, 442, 330, 442, 392, 330, 442 };
    for (int i = 0; i < 15; i++) {
        short id[3] = { 0, 0, (short)(1000 + i) };
        NxMission* m = missionNew(cx[i], cy[i], id);
        if (!m) continue;
        strcpy(m->type, "\xEC\x9D\xBC\xEB\xB0\x98");   /* szNormal */
        strcpy(m->land, "NX2 ZONE");
        s_nx2[i] = m;
    }
}

bool NxMission_Load(void)
{
    if (s_loaded) return s_loadOk;
    s_loaded = true;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/mission.txt", g_game.currentDirectory);
    FILE* f = fopen(path, "rb");
    if (!f) { Log_Print("NXMISSION: '%s' nao existe\n", path); return false; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(buf); return false; }
    fclose(f);
    buf[sz] = 0;
    bool ok = parseScript(buf);
    free(buf);
    createNx2();
    int a0 = 0, a1 = 0;
    for (int i = 0; i < s_nAll; i++) { if (s_all[i]->id[1] == 0) a0++; else a1++; }
    Log_Print("NXMISSION: %d missoes (%d com id[1]=0, %d com id[1]!=0), ok=%d\n", s_nAll, a0, a1, ok ? 1 : 0);
    s_loadOk = ok && s_nAll > 0;
    return s_loadOk;
}

int NxMission_Count(void) { return s_nAll; }
NxMission* NxMission_At(int i) { return (i >= 0 && i < s_nAll) ? s_all[i] : NULL; }
int NxMission_Nx2Count(void) { return 15; }
NxMission* NxMission_Nx2At(int i) { return (i >= 0 && i < 15) ? s_nx2[i] : NULL; }

NxMission* NxMission_ById(const short id[3])
{
    for (int i = 0; i < s_nAll; i++) if (!memcmp(s_all[i]->id, id, sizeof(short) * 3)) return s_all[i];
    return NULL;
}
NxMission* NxMission_ByStep(const char* step)
{
    for (int i = 0; i < s_nAll; i++) if (!_stricmp(s_all[i]->step, step)) return s_all[i];
    return NULL;
}

void NxMission_SetDefaultList(void)
{
    s_nCur = 0;   /* m_vCurMissionList.clear(); as cópias anteriores saem em ClearDefaultList */
    for (int i = 0; i < s_nAll; i++) {
        if (s_all[i]->id[1] != 0) continue;
        NxMission* c = (NxMission*)malloc(sizeof(NxMission));
        if (!c) continue;
        memcpy(c, s_all[i], sizeof(NxMission));
        vpush(&s_cur, &s_nCur, &s_capCur, c);
    }
    Log_Print("NXMISSION: setDefaultMissionList %d\n", s_nCur);
}

void NxMission_ClearDefaultList(void)
{
    for (int i = 0; i < s_nCur; i++) free(s_cur[i]);
    s_nCur = 0;
}

int NxMission_CurCount(void) { return s_nCur; }
NxMission* NxMission_CurAt(int i) { return (i >= 0 && i < s_nCur) ? s_cur[i] : NULL; }
NxMission* NxMission_CurById(const short id[3])
{
    for (int i = 0; i < s_nCur; i++) if (!memcmp(s_cur[i]->id, id, sizeof(short) * 3)) return s_cur[i];
    return NULL;
}
NxMission* NxMission_CurByStep(const char* step)
{
    for (int i = 0; i < s_nCur; i++) if (!_stricmp(s_cur[i]->step, step)) return s_cur[i];
    return NULL;
}

/* CMission::setChildConn / setParentConn (sem o parâmetro clear, que só o editor usa) */
static void setParent(NxMission* m, int dir, NxMission* parent)
{
    m->next[dir] = parent;
    memcpy(m->conn[dir].connectedID, parent->id, sizeof(short) * 3);
}
static void setChild(NxMission* m, int dir, NxMission* child)
{
    static const int opp[4] = { NXM_CONN_RL, NXM_CONN_RU, NXM_CONN_LL, NXM_CONN_LU };   /* LU->RL, LL->RU, RU->LL, RL->LU */
    m->next[dir] = child;
    memcpy(m->conn[dir].connectedID, child->id, sizeof(short) * 3);
    setParent(child, opp[dir], m);
}

void NxMission_OrganizeConnection(void)
{
    for (int i = 0; i < s_nCur; i++)
        for (int d = 0; d < 4; d++) {
            NxMission* c = NxMission_CurById(s_cur[i]->conn[d].connectedID);
            if (c) setChild(s_cur[i], d, c);
        }
}

/* copia BasicData + Param + Desc + scene (memcpy dos três blocos no original) */
static void copyData(NxMission* dst, const NxMission* src)
{
    dst->players = src->players; dst->level = src->level; dst->life = src->life; dst->mileage = src->mileage;
    dst->nxBreak = src->nxBreak; dst->item = src->item; dst->seperate = src->seperate;
    dst->random = src->random; dst->gageMin = src->gageMin; dst->gageMax = src->gageMax;
    memcpy(dst->type, src->type, sizeof(dst->type)); dst->cleared = src->cleared;
    memcpy(dst->song, src->song, sizeof(dst->song)); memcpy(dst->step, src->step, sizeof(dst->step));
    memcpy(dst->stepLV, src->stepLV, sizeof(dst->stepLV)); memcpy(dst->reward, src->reward, sizeof(dst->reward));
    memcpy(dst->command, src->command, sizeof(dst->command));
    dst->speed = src->speed; memcpy(dst->skin, src->skin, sizeof(dst->skin));
    memcpy(dst->challenge, src->challenge, sizeof(dst->challenge)); memcpy(dst->conditions, src->conditions, sizeof(dst->conditions));
    memcpy(dst->name, src->name, sizeof(dst->name)); memcpy(dst->land, src->land, sizeof(dst->land));
    memcpy(dst->city, src->city, sizeof(dst->city)); memcpy(dst->desc, src->desc, sizeof(dst->desc));
    strcpy(dst->scene, src->scene);
}

/* changeMissionData: AnotherTable = EF050..EF085 (normal) <-> EF100..EF135 (another) */
void NxMission_ChangeData(int another)
{
    for (int i = 0; i < s_nCur; i++) {
        for (int j = 0; j < 36; j++) {
            char nStep[8], aStep[8];
            snprintf(nStep, sizeof(nStep), "EF%03d", 50 + j);
            snprintf(aStep, sizeof(aStep), "EF%03d", 100 + j);
            if (!_stricmp(s_cur[i]->step, another ? nStep : aStep)) {
                NxMission* dst = NxMission_CurByStep(another ? nStep : aStep);
                NxMission* src = NxMission_ByStep(another ? aStep : nStep);
                if (dst && src) copyData(dst, src);
            }
        }
    }
}

/* WORLD MAX: o chart EFnnn não tem áudio/capa próprios; usa a música do campo SONG (hex) da missão */
int NxMission_SongIdForStepId(int stepId)
{
    char step[16];
    snprintf(step, sizeof(step), "%X", (unsigned)stepId);
    NxMission_Load();
    NxMission* m = NxMission_ByStep(step);
    if (!m || !m->song[0]) return -1;
    char* end = NULL;
    long v = strtol(m->song, &end, 16);
    return (end && end != m->song) ? (int)v : -1;
}
