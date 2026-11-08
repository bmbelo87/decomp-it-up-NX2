/*
 * nx2_usbstate.c — apelido do perfil USB no canto de baixo das telas (CUsbstate::DrawPlayerState,
 * nx2src usbstate.cpp). 1P à esquerda, 2P à direita, por cima de qualquer tela que chamar NxUsbState_Draw.
 *
 * Original: m_pBGA->DrawScene(USB_ID_xP) e m_pIDFont->Printf(62 + 516 * Player, 7, nick) com a CUsbFont
 * (BGA/USB_FONT.DAT, usb_font_0.tga: células 22x22, 23 por linha, mesma tabela do wfont).
 *
 * HIPÓTESE: o arquivo BGA/USB_NICKNAME.DAT (cenas "1 player" / "2 player", mesmos quadros 0..60 / 65..125 da
 * USB_ID_1P/2P do USB_COMMON.DAT) é o que o jogo usa para a barra do apelido (indicação do usuário); o nome não
 * aparece no piu como string.
 *
 * O BGA do port é limpo a cada troca de tela (Resource_ClearBGA); como a faixa de crédito, esta camada se
 * recarrega sozinha (g_texGeneration) e entra como o último BGA da tela.
 */
#include "pumpy.h"
#include "bga.h"

extern unsigned g_texGeneration;
extern unsigned Title_GetJoinedMask(void);

static unsigned s_gen = 0xFFFFFFFFu;
static int      s_bga = -1;
static int      s_fontTex = -1;
static bool     s_failed;
static bool     s_disc[2];         /* E_USB_DISCONNECTED: pendrive retirado no meio da sessão (main.cpp:589) */
static bool     s_animDone[2];     /* a barra entra animada uma vez por perfil; nas telas seguintes já aparece aberta */

static const char* k_scene[2] = { "1 player", "2 player" };
static const char* k_uncable[2] = { "1_PLAYER_(UNCABLE_USB)", "2_PLAYER_(UNCABLE_USB)" };   /* = UNCABLE_USB_1P/2P do USB_COMMON.DAT */

static bool load(void)
{
    if (s_gen == g_texGeneration) return !s_failed && s_bga >= 0;
    s_gen = g_texGeneration;
    s_bga = -1; s_fontTex = -1; s_failed = false;

    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/BGA/USB_FONT.DAT", g_game.currentDirectory);
    if (RES_Open(path)) {
        s_fontTex = loadTextureFromRES("usb_font_0.tga");
        RES_Close();
    }
    if (s_fontTex < 0) Log_Print("NX2USBSTATE: falha ao carregar usb_font_0.tga do USB_FONT.DAT\n");
    int prevActive = BGA_GetActivePic();   /* o BGA da tela (gameplay, Dance Grade) continua sendo o ativo */
    bool okLoad = Resource_LoadBGAByName("USB_NICKNAME");
    BGA_SetActivePic(prevActive);
    if (!okLoad) { Log_Print("NX2USBSTATE: falha ao carregar BGA/USB_NICKNAME.DAT\n"); s_failed = true; return false; }
    s_bga = g_game.bgaPicCount - 1;
    for (int p = 0; p < 2; p++)
        if (s_animDone[p]) BGA_SceneFinish(s_bga, k_scene[p]);
    return true;
}

/* CUsbFont::Printf: centralizado por strlen(fmt) (= o próprio apelido), avanço de 10 px, célula 22x22 */
static void printNick(float x, float y, const char* text)
{
    static const char k_cnv[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789~!@#$%^&*()_";
    if (s_fontTex < 0) return;
    float px = x - (float)((10 * (int)strlen(text)) / 2) - 8.f;
    for (const char* c = text; *c; c++, px += 10.f) {
        const char* f = strchr(k_cnv, *c);
        int val = f ? (int)(f - k_cnv) : -1;
        if (val < 0 || val >= 45) continue;                    /* ' ' e desconhecidos não desenham; 45+ é da 2ª textura */
        float u = (float)((val % 23) * 22), v = (float)((val / 23) * 22);
        Texture_DrawUV(s_fontTex, px, 480.f - (y + 22.f), 22.f, 22.f, u, v, u + 22.f, v + 22.f, 1.f, 1.f, 1.f, 1.f);
    }
}

void NxUsbState_Draw(void)
{
    /* telas que mostram o perfil (CUsbstate::Draw); WORLD MAX e TRAINING do NX2 ainda não foram portados */
    if (g_game.state != STATE_STATION && g_game.state != STATE_SELECT &&
        g_game.state != STATE_DANCE_GRADE_ENTER && g_game.state != STATE_DANCE_GRADE_DISPLAY &&
        g_game.state != STATE_GAMEPLAY && g_game.state != STATE_NX_SPORTS) return;
    unsigned mask = Title_GetJoinedMask() & 3;
    bool any = false;
    for (int p = 0; p < 2; p++) {
        if (!g_nx2HasProfile[p]) { s_animDone[p] = false; s_disc[p] = false; }
        else if (mask & (1u << p)) {
            any = true;
            /* main.cpp:589: com a sessão em andamento, CheckUsb() falso -> E_USB_DISCONNECTED (fica assim até o fim do crédito) */
            if (!s_disc[p] && Nx2Profile_CheckUsb(p) == false) {
                Log_Print("NX2USBSTATE: P%d pendrive retirado\n", p + 1);
                s_disc[p] = true;
            }
        }
    }
    if (!any || Loading_IsActive() || !load()) return;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1.f, 1.f, 1.f, 1.f);
    for (int p = 0; p < 2; p++) {
        if (!g_nx2HasProfile[p] || !(mask & (1u << p))) continue;
        if (s_disc[p]) {                          /* DrawPlayerState: DISCONNECTED -> cena UNCABLE no lugar do apelido */
            BGA_ScenePlay(s_bga, k_uncable[p], true);
            continue;
        }
        BGA_ScenePlay(s_bga, k_scene[p], true);
        if (BGA_SceneDone(s_bga, k_scene[p])) s_animDone[p] = true;
        char nick[NX2_ID_MAX + 1];
        memset(nick, 0, sizeof(nick));
        memcpy(nick, g_nx2Prof[p].PlayerID, NX2_ID_MAX);
        printNick(62.f + 516.f * (float)p, 7.f, nick);
    }
    glColor4f(1.f, 1.f, 1.f, 1.f);
}
