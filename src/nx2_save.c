/* NX2: persistência local (PIUNX2.INI, RANK.DAT, MRANK.DAT) e perfil por jogador (nx2save.bin +
 * nx2rank.bin). Estruturas em pumpy.h. Ordem e tamanhos: nx2src (usbmanager.cpp, setupmgr.cpp,
 * rankmanager.cpp, missionrankmanager.cpp) e as strings do piu NX2: "/mnt/%d/nx2save.bin" (0x80f3e5f),
 * "/mnt/%d/nx2rank.bin" (0x80f3e73), "/SETTINGS/PIUNX2.INI" (0x80f3bb7).
 *
 * Diferenças deliberadas (o arcade monta o pendrive em /mnt/0 e /mnt/1 e amarra o perfil ao número
 * de série dele; num PC isso não existe):
 *  - "pendrive" do jogador N = pasta NX2_USB<N> (variável de ambiente) ou <jogo>/save/usb<N>. O nome
 *    da pasta padrão é sugestão do port, não vem do original. Pasta ausente = pendrive não inserido.
 *  - A conferência do szSerial não é feita (o modo HDD_PROFILES do nx2src também a pula).
 *  - Os arquivos da máquina ficam em <jogo>/save/ (onde o jogo do usuário já os guarda). */
#include "pumpy.h"

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#define _mkdir(d) mkdir((d), 0755)
#endif
#include <time.h>

/* tamanhos verificados com amostras reais (30488, 292, 12296, 31832, 676, 120004) */
typedef char nx2_chk_stat[(sizeof(Nx2Stat) == 30488) ? 1 : -1];
typedef char nx2_chk_review[(sizeof(Nx2Review) == 292) ? 1 : -1];
typedef char nx2_chk_rankinfo[(sizeof(Nx2RankInfo) == 12296) ? 1 : -1];
typedef char nx2_chk_option[(sizeof(Nx2Option) == 31832) ? 1 : -1];
typedef char nx2_chk_rank[(sizeof(Nx2Rank) == 676) ? 1 : -1];
typedef char nx2_chk_mrank[(sizeof(Nx2MRank) == 120004) ? 1 : -1];

#define NX2_IDENT 0x3032584Eu   /* "NX20" */

Nx2Option  g_nx2Opt;
Nx2Stat    g_nx2Prof[2];
Nx2RankInfo g_nx2ProfRank[2];
bool       g_nx2HasProfile[2];
char       g_nx2UsbRoot[2][260];   /* g_Option.usbBus (SETUP USB DRIVE); no PC é a raiz do drive escolhido (ex. "E:\") */
static Nx2Review s_review[2];

static uint32_t nx2Adler(const uint8_t* d, size_t n)
{
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) { a = (a + d[i]) % 65521u; b = (b + a) % 65521u; }
    return (b << 16) | a;
}

/* Enc/Dec do usbmanager.cpp: cadeia byte a byte com a constante 1234567 (só os bits 8..15 importam) */
static void nx2Enc(void* buf, size_t len)
{
    uint8_t* p = (uint8_t*)buf;
    for (size_t a = 1; a < len; a++) p[a] = (uint8_t)((p[a] - (uint8_t)((uint32_t)a * 1234567u >> 8)) ^ p[a - 1]);
}

static void nx2Dec(void* buf, size_t len)
{
    uint8_t* p = (uint8_t*)buf;
    for (size_t a = len - 1; a > 0; a--) p[a] = (uint8_t)((p[a] ^ p[a - 1]) + (uint8_t)((uint32_t)a * 1234567u >> 8));
}

static void nx2SaveDir(char* out, size_t n)
{
    snprintf(out, n, "%s/save", g_game.currentDirectory);
}

static void nx2MachinePath(char* out, size_t n, const char* name)
{
    snprintf(out, n, "%s/save/%s", g_game.currentDirectory, name);
}

static bool nx2ReadAll(const char* path, void* dst, size_t size)
{
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    size_t got = fread(dst, 1, size, f);
    fclose(f);
    return got == size;
}

static bool nx2WriteAll(const char* path, const void* src, size_t size)
{
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    size_t put = fwrite(src, 1, size, f);
    fclose(f);
    return put == size;
}

/* ---- PIUNX2.INI --------------------------------------------------------------------------- */

/* Extra do port: o original apaga um INI inválido (padrão e grava). Aqui o arquivo existente é copiado
 * para "<arquivo>.bad" (só se ainda não houver um) antes de ser sobrescrito pelo padrão. */
static void nx2KeepBad(const char* path)
{
    FILE* in = fopen(path, "rb");
    if (!in) return;                             /* ausente: nada a guardar */
    char bad[MAX_PATH + 8];
    snprintf(bad, sizeof(bad), "%s.bad", path);
    FILE* t = fopen(bad, "rb");
    if (t) { fclose(t); fclose(in); return; }    /* já existe uma cópia anterior: não a substitui */
    FILE* out = fopen(bad, "wb");
    if (out) {
        uint8_t buf[4096]; size_t n;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
        fclose(out);
        Log_Print("NX2SAVE: cópia do arquivo inválido em '%s'\n", bad);
    }
    fclose(in);
}

static uint32_t optChecksum(const Nx2Option* o)
{
    return nx2Adler((const uint8_t*)&o->_ident, (size_t)((const uint8_t*)&o->_chksum - (const uint8_t*)&o->_ident));
}

bool Nx2Option_Load(void)
{
    char path[MAX_PATH];
    nx2MachinePath(path, sizeof(path), "PIUNX2.INI");
    memset(&g_nx2Opt, 0, sizeof(g_nx2Opt));
    if (!nx2ReadAll(path, &g_nx2Opt, sizeof(g_nx2Opt))) {
        memset(&g_nx2Opt, 0, sizeof(g_nx2Opt));
        nx2KeepBad(path);                      /* só age se o arquivo existir (tamanho errado) */
        return false;
    }
    if (g_nx2Opt._ident != NX2_IDENT) {
        Log_Print("NX2SAVE: '%s' com identificador errado (0x%08X)\n", path, g_nx2Opt._ident);
        memset(&g_nx2Opt, 0, sizeof(g_nx2Opt)); nx2KeepBad(path); return false;
    }
    if (g_nx2Opt._chksum != optChecksum(&g_nx2Opt)) {
        Log_Print("NX2SAVE: '%s' com checksum inválido\n", path);
        memset(&g_nx2Opt, 0, sizeof(g_nx2Opt)); nx2KeepBad(path); return false;
    }
    return true;
}

bool Nx2Option_Save(void)
{
    char dir[MAX_PATH], path[MAX_PATH];
    nx2SaveDir(dir, sizeof(dir));
    _mkdir(dir);
    nx2MachinePath(path, sizeof(path), "PIUNX2.INI");
    g_nx2Opt._ident = NX2_IDENT;
    g_nx2Opt._chksum = optChecksum(&g_nx2Opt);
    if (!nx2WriteAll(path, &g_nx2Opt, sizeof(g_nx2Opt))) { Log_Print("NX2SAVE: não gravou '%s'\n", path); return false; }
    return true;
}

/* ---- RANK.DAT / MRANK.DAT ----------------------------------------------------------------- */

bool Nx2Rank_Load(Nx2Rank* r)
{
    char path[MAX_PATH];
    nx2MachinePath(path, sizeof(path), "RANK.DAT");
    memset(r, 0, sizeof(*r));
    if (!nx2ReadAll(path, r, sizeof(*r))) { memset(r, 0, sizeof(*r)); return false; }
    if ((uint32_t)r->_adler32 != nx2Adler((const uint8_t*)r->m_Scores, sizeof(r->m_Scores))) { memset(r, 0, sizeof(*r)); return false; }
    return true;
}

bool Nx2Rank_Save(const Nx2Rank* r)
{
    char dir[MAX_PATH], path[MAX_PATH];
    Nx2Rank tmp = *r;
    nx2SaveDir(dir, sizeof(dir)); _mkdir(dir);
    nx2MachinePath(path, sizeof(path), "RANK.DAT");
    tmp._adler32 = (int32_t)nx2Adler((const uint8_t*)tmp.m_Scores, sizeof(tmp.m_Scores));
    return nx2WriteAll(path, &tmp, sizeof(tmp));
}

bool Nx2MRank_Load(Nx2MRank* r)
{
    char path[MAX_PATH];
    nx2MachinePath(path, sizeof(path), "MRANK.DAT");
    memset(r, 0, sizeof(*r));
    if (!nx2ReadAll(path, r, sizeof(*r))) { memset(r, 0, sizeof(*r)); return false; }
    if ((uint32_t)r->_adler32 != nx2Adler((const uint8_t*)r->m_Scores, sizeof(r->m_Scores))) { memset(r, 0, sizeof(*r)); return false; }
    return true;
}

bool Nx2MRank_Save(const Nx2MRank* r)
{
    char dir[MAX_PATH], path[MAX_PATH];
    nx2SaveDir(dir, sizeof(dir)); _mkdir(dir);
    nx2MachinePath(path, sizeof(path), "MRANK.DAT");
    /* 120 KB: cópia só do cabeçalho de checksum, sem duplicar a tabela na pilha */
    Nx2MRank* tmp = (Nx2MRank*)malloc(sizeof(*tmp));
    if (!tmp) return false;
    memcpy(tmp, r, sizeof(*tmp));
    tmp->_adler32 = (int32_t)nx2Adler((const uint8_t*)tmp->m_Scores, sizeof(tmp->m_Scores));
    bool ok = nx2WriteAll(path, tmp, sizeof(*tmp));
    free(tmp);
    return ok;
}

/* ---- perfil do jogador (nx2save.bin / nx2rank.bin) ------------------------------------------ */

static bool profRoot(int player, char* out, size_t n)
{
    char env[16];
    snprintf(env, sizeof(env), "NX2_USB%d", player);
    const char* e = getenv(env);
    if (e && *e) snprintf(out, n, "%s", e);
    else if (player >= 0 && player < 2 && g_nx2UsbRoot[player][0]) snprintf(out, n, "%s", g_nx2UsbRoot[player]);   /* USB DRIVE do service */
    else snprintf(out, n, "%s/save/usb%d", g_game.currentDirectory, player);
    char probe[MAX_PATH + 16];
    snprintf(probe, sizeof(probe), "%s/.", out);
#ifdef _WIN32
    DWORD a = GetFileAttributesA(out);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st; return stat(out, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static void profFile(int player, const char* name, char* out, size_t n)
{
    char root[MAX_PATH];
    profRoot(player, root, sizeof(root));
    snprintf(out, n, "%s/%s", root, name);
}

void Nx2Profile_Unload(int player)
{
    if (player < 0 || player > 1) return;
    memset(&g_nx2Prof[player], 0, sizeof(Nx2Stat));
    memset(&g_nx2ProfRank[player], 0, sizeof(Nx2RankInfo));
    memset(&s_review[player], 0, sizeof(Nx2Review));
    g_nx2HasProfile[player] = false;
}

/* CUsbManager::LoadData: nx2save.bin (Dec + Adler) e depois nx2rank.bin. Arquivo ausente = NODATA;
 * rank ausente/inválido com o save bom = BADDATA (como no original). */
int Nx2Profile_Load(int player)
{
    if (player < 0 || player > 1) return NX2_USB_NOCONFIG;
    char root[MAX_PATH], path[MAX_PATH];
    if (!profRoot(player, root, sizeof(root))) { static char warned[2]; if (!warned[player]) { warned[player] = 1; Log_Print("NX2SAVE: P%d pendrive nao inserido (raiz '%s' nao existe)\n", player + 1, root); } return NX2_USB_NOTINSERTED; }

    profFile(player, "nx2save.bin", path, sizeof(path));
    FILE* f = fopen(path, "rb");
    if (!f) { Log_Print("NX2SAVE: P%d sem dados: '%s' nao existe\n", player + 1, path); return NX2_USB_NODATA; }
    Nx2Review rev; Nx2Stat* st = (Nx2Stat*)malloc(sizeof(Nx2Stat));
    if (!st) { fclose(f); return NX2_USB_BADDATA; }
    memset(&rev, 0, sizeof(rev)); memset(st, 0, sizeof(*st));
    size_t r1 = fread(&rev, 1, sizeof(rev), f);
    size_t r2 = fread(st, 1, sizeof(*st), f);
    fclose(f);
    if (r1 != sizeof(rev) || r2 != sizeof(*st)) { Log_Print("NX2SAVE: P%d nx2save.bin com tamanho errado (%u + %u, esperado %u + %u)\n", player + 1, (unsigned)r1, (unsigned)r2, (unsigned)sizeof(rev), (unsigned)sizeof(*st)); free(st); return NX2_USB_BADDATA; }
    nx2Dec(st, sizeof(*st));
    if (st->CheckSum != nx2Adler((const uint8_t*)st + 4, sizeof(*st) - 4)) { Log_Print("NX2SAVE: P%d nx2save.bin com checksum invalido\n", player + 1); free(st); return NX2_USB_BADDATA; }

    Nx2RankInfo* rk = (Nx2RankInfo*)malloc(sizeof(Nx2RankInfo));
    if (!rk) { free(st); return NX2_USB_BADDATA; }
    memset(rk, 0, sizeof(*rk));
    profFile(player, "nx2rank.bin", path, sizeof(path));
    bool okRank = false;
    f = fopen(path, "rb");
    if (f) {
        okRank = fread(rk, 1, sizeof(*rk), f) == sizeof(*rk);
        fclose(f);
        if (okRank) {
            nx2Dec(rk, sizeof(*rk));
            okRank = rk->CheckSum == nx2Adler((const uint8_t*)rk + 4, sizeof(*rk) - 4);
        }
    }
    if (!okRank) { Log_Print("NX2SAVE: P%d nx2rank.bin ausente ou invalido ('%s')\n", player + 1, path); free(st); free(rk); return NX2_USB_BADDATA; }

    s_review[player] = rev;
    g_nx2Prof[player] = *st;
    g_nx2ProfRank[player] = *rk;
    g_nx2HasProfile[player] = true;
    Log_Print("NX2SAVE: P%d perfil carregado de '%s'\n", player + 1, root);
    free(st); free(rk);
    return NX2_USB_OK;
}

static void nowTime(Nx2Time* t)
{
    time_t now = time(NULL);
    struct tm* l = localtime(&now);
    if (!l) return;
    t->year = (int16_t)(l->tm_year + 1900); t->month = (int8_t)(l->tm_mon + 1); t->day = (int8_t)l->tm_mday;
    t->hour = (int8_t)l->tm_hour; t->min = (int8_t)l->tm_min; t->ms = 0;
}

/* CUsbManager::SaveData */
int Nx2Profile_Save(int player)
{
    if (player < 0 || player > 1 || !g_nx2HasProfile[player]) return NX2_USB_NOTINSERTED;
    Nx2Stat* s = &g_nx2Prof[player];
    if (!s->PlayerID[0]) return NX2_USB_BADDATA;
    char root[MAX_PATH], path[MAX_PATH];
    if (!profRoot(player, root, sizeof(root))) return NX2_USB_NOTINSERTED;

    Nx2Review* r = &s_review[player];
    memcpy(r->PlayerID, s->PlayerID, NX2_ID_MAX);
    r->iMileage = s->iMileage; r->m_iRewardCnt = s->m_iRewardCnt; r->m_iWorldMaxCnt = s->m_iWorldMaxCnt;
    r->iPlayCount = s->iPlayCount;
    memcpy(r->szLand, s->szLand, sizeof(r->szLand)); memcpy(r->szMission, s->szMission, sizeof(r->szMission));
    r->ikcal = s->ikcal; r->iVO2 = s->iVO2;

    nowTime(&s->Time);
    s->CheckSum = nx2Adler((const uint8_t*)s + 4, sizeof(*s) - 4);

    Nx2Stat* enc = (Nx2Stat*)malloc(sizeof(*enc));
    if (!enc) return NX2_USB_CANNOTWRITE;
    memcpy(enc, s, sizeof(*enc));
    nx2Enc(enc, sizeof(*enc));
    profFile(player, "nx2save.bin", path, sizeof(path));
    FILE* f = fopen(path, "wb");
    bool ok = f && fwrite(r, 1, sizeof(*r), f) == sizeof(*r) && fwrite(enc, 1, sizeof(*enc), f) == sizeof(*enc);
    if (f) fclose(f);
    free(enc);
    return ok ? NX2_USB_OK : NX2_USB_CANNOTWRITE;
}

/* CUsbManager::SaveRank: guarda as 4 fases do crédito na posição nCnt do anel de 128 */
int Nx2Profile_SaveRank(int player, const Nx2RankUnit info[NX2_MAX_STAGE])
{
    if (player < 0 || player > 1 || !g_nx2HasProfile[player]) return NX2_USB_NOTINSERTED;
    char root[MAX_PATH], path[MAX_PATH];
    if (!profRoot(player, root, sizeof(root))) return NX2_USB_NOTINSERTED;
    Nx2RankInfo* rk = &g_nx2ProfRank[player];
    rk->nCnt %= 128;
    uint32_t no = rk->nCnt;
    for (int i = 0; i < NX2_RANK_STAGE; i++) rk->tRI[no][i] = info[i];
    rk->nCnt = (rk->nCnt + 1) % 128;
    rk->CheckSum = nx2Adler((const uint8_t*)rk + 4, sizeof(*rk) - 4);

    Nx2RankInfo* enc = (Nx2RankInfo*)malloc(sizeof(*enc));
    if (!enc) return NX2_USB_CANNOTWRITE;
    memcpy(enc, rk, sizeof(*enc));
    nx2Enc(enc, sizeof(*enc));
    profFile(player, "nx2rank.bin", path, sizeof(path));
    bool ok = nx2WriteAll(path, enc, sizeof(*enc));
    free(enc);
    return ok ? NX2_USB_OK : NX2_USB_CANNOTWRITE;
}

/* Extra do port: o arcade registra o perfil num servidor (eNoData = "não registrado"); aqui cria um
 * perfil vazio com o nome dado (até 8 letras no original: o PlayerID tem 12 bytes). */
bool Nx2Profile_Create(int player, const char* id)
{
    if (player < 0 || player > 1 || !id || !id[0]) return false;
    char root[MAX_PATH];
    if (!profRoot(player, root, sizeof(root))) {   /* pasta ausente: cria (pendrive "inserido") */
        _mkdir(root);
        if (!profRoot(player, root, sizeof(root))) return false;
    }
    Nx2Profile_Unload(player);
    Nx2Stat* s = &g_nx2Prof[player];
    snprintf(s->PlayerID, NX2_ID_MAX, "%.*s", NX2_ID_MAX - 1, id);
    s->iWMPosition = 0xEF001;   /* posição inicial do WORLD MAX na amostra nx2save0.bin */
    g_nx2HasProfile[player] = true;
    int r = Nx2Profile_Save(player);
    Nx2RankUnit z[NX2_MAX_STAGE]; memset(z, 0, sizeof(z));
    g_nx2ProfRank[player].nCnt = 127;   /* SaveRank grava em nCnt e avança: o 1º registro vira 0 */
    memset(&g_nx2ProfRank[player].tRI, 0, sizeof(g_nx2ProfRank[player].tRI));
    int r2 = Nx2Profile_SaveRank(player, z);
    g_nx2ProfRank[player].nCnt = 0;
    return r == NX2_USB_OK && r2 == NX2_USB_OK;
}

Nx2Stat* Nx2_Stat(int player)
{
    return (player >= 0 && player <= 1 && g_nx2HasProfile[player]) ? &g_nx2Prof[player] : &g_nx2Opt.m_Stat;
}
