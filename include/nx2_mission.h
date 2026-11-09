/*
 * nx2_mission.h — missões do WORLD MAX (CMission / CMissionManager do nx2src, ProcModules/CMissionManager.*).
 * Dados: <jogo>/mission.txt (*NX_SCRIPT / *NX_MISSION). O piu também lê DESC_CNS_S/F (chinês simplificado), que o
 * nx2src não trata; aqui os dois entram.
 */
#ifndef NX2_MISSION_H
#define NX2_MISSION_H

#include <stdbool.h>

#define NXM_CONDITION_MAX 256
#define NXM_NAME_MAX      (32 * 3)
#define NXM_FILE_MAX      16
#define NXM_DESC_S_MAX    (256 * 3)
#define NXM_DESC_L_MAX    (512 * 3)

enum { NXM_CONN_LU = 0, NXM_CONN_LL = 1, NXM_CONN_RU = 2, NXM_CONN_RL = 3 };
enum { NXM_NONE = 0, NXM_READY = 1, NXM_SELECTED = 2 };
enum { NXM_LANG_KOR = 0, NXM_LANG_EN, NXM_LANG_SP, NXM_LANG_CN, NXM_LANG_CNS, NXM_LANGS };

typedef struct NxMission NxMission;
struct NxMission {
    short id[3];
    /* BasicData_t */
    short players, level, life, mileage;
    bool  nxBreak, item, seperate;
    short random, gageMin, gageMax;
    char  type[NXM_NAME_MAX];      /* "일반", "더미", "워프", "보스", "중간보스", "이벤트" (UTF-8 no arquivo) */
    bool  cleared;
    /* MissionParam_t */
    char  song[NXM_FILE_MAX], step[NXM_FILE_MAX], stepLV[3], reward[NXM_FILE_MAX];
    char  command[NXM_CONDITION_MAX];
    short speed;
    char  skin[3][3];
    short challenge[4];
    struct { unsigned char direction; char condition[NXM_CONDITION_MAX]; } conditions[4];
    /* MissionDesc_t (as descrições ficam em memória alocada) */
    char  name[NXM_NAME_MAX], land[NXM_NAME_MAX], city[NXM_NAME_MAX];
    char* desc[NXM_LANGS][2];      /* [idioma][0 = curta (_S), 1 = completa (_F)] */
    /* MissionCoord_t */
    int   x, y;
    /* MissionConn_t */
    struct { short connectedID[3]; bool enabled; char lock; } conn[4];
    NxMission* next[4];
    int   state;
    bool  iced, show;
    NxMission* another;
    char  scene[32];
};

bool        NxMission_Load(void);                    /* lê mission.txt (uma vez); false se não existe/inválido */
int         NxMission_Count(void);                   /* m_vMissionList */
NxMission*  NxMission_At(int i);
NxMission*  NxMission_ById(const short id[3]);
NxMission*  NxMission_ByStep(const char* step);      /* stricmp */
int         NxMission_Nx2Count(void);                /* m_vNX2MissionList: 15 pontos da NX2 ZONE (createNx2Mission) */
NxMission*  NxMission_Nx2At(int i);

/* lista corrente (m_vCurMissionList): cópias dos missões com id[1] == 0 */
void        NxMission_SetDefaultList(void);          /* setDefaultMissionList */
void        NxMission_ClearDefaultList(void);        /* clearDefaultMissionList */
int         NxMission_CurCount(void);
NxMission*  NxMission_CurAt(int i);
NxMission*  NxMission_CurById(const short id[3]);
NxMission*  NxMission_CurByStep(const char* step);
void        NxMission_OrganizeConnection(void);      /* organizeConnection (sobre a lista corrente) */
void        NxMission_ChangeData(int another);       /* changeMissionData: 0 = MISSION_NORMAL, 1 = MISSION_ANOTHER */

#endif
