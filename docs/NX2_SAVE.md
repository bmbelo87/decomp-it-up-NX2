# NX2 — arquivos de save e perfil

Código: `src/nx2_save.c` (E/S), estruturas em `include/pumpy.h` (`Nx2Option`, `Nx2Stat`, ...),
ponte com a imagem interna de configuração em `src/eeprom.c`.

## Fatos (confirmados)

| Arquivo | Onde | Conteúdo | Tamanho | Proteção |
|---|---|---|---|---|
| `PIUNX2.INI` | máquina | `struct option`: idioma, estação padrão, opções de jogo/moeda, bookkeeping, contagem de jogos por música, censura, `usbBus`, **`strucStat` local** | 31.832 | Adler-32 de `_ident` até `_chksum`; sem cifra |
| `RANK.DAT` | máquina | 84 × `{score, nome[4]}`: top 20 do ARCADE + 64 recordes de locais do WORLD MAX | 676 | Adler-32 de `m_Scores` |
| `MRANK.DAT` | máquina | 10.000 × `{nome[4], score, tipo}` (histórico de missões) | 120.004 | Adler-32 de `m_Scores` |
| `nx2save.bin` | pendrive do jogador | `strucReview` (292, sem cifra) + `strucStat` (30.488, cifrado) | 30.780 | `Enc/Dec` + Adler-32 após o 1º `u32` |
| `nx2rank.bin` | pendrive do jogador | `strucRankInfo`: `nCnt` + anel de 128 × 4 fases | 16.392 | `Enc/Dec` + Adler-32 após o 1º `u32` |

- Caminhos no `piu` do NX2: `/mnt/%d/nx2save.bin` (`0x80f3e5f`), `/mnt/%d/nx2rank.bin` (`0x80f3e73`),
  `/SETTINGS/PIUNX2.INI` (`0x80f3bb7`).
- Cifra (`usbmanager.cpp`): `Enc: p[a] = (p[a] - ((a*1234567)>>8)) ^ p[a-1]` (a ≥ 1); `Dec` é o inverso, de trás
  para frente. Só os bits 8..15 de `a*1234567` contam, então o estouro de 32 bits não importa.
- `strucStat` usa `#pragma pack(4)`; `struct option` usa `pack(1)` e embute o `strucStat` sem preenchimento.
- Identificador do INI: `NX20` (`0x3032584E`); o da NX era `NX10`.
- **Perfil ativo**: em todo o jogo, `ps = HasUsb() ? &g_UsbManager[p].m_Stat : &g_Option.m_Stat`. Sem pendrive
  o mesmo `strucStat` fica no INI local; com pendrive, em `nx2save.bin`.

### Ciclo do perfil (`usbin.cpp`)
1. **Entrada** (`USB_IN_MODE`): a cada 16 quadros `LoadData()` do jogador até dar resultado.
   `eOk` → `SetHasUsb()`, copia `m_iRewardCnt`/`m_iWorldMaxCnt`, abre a janela da licença.
   `eNoData` → mensagem "não registrado". `eMountError`/`eBadData` → mensagem de erro de USB.
2. **Jogo**: leituras e escritas em `Nx2_Stat(p)`; mileage, desbloqueios (`bmUnlock`, bit6 música, bit7 revela),
   recordes por música/modo (`Record[song][modo]`), World Max (`bmWMClear`, `bmWMHighScore`, `iWMPosition`...).
3. **Saída** (fim do crédito): `ProcResult` atualiza e então `SaveData()`; só em ARCADE/SPECIAL também `SaveRank()`.
4. A cada quadro o jogo confere `CheckUsb()`; se o pendrive sumiu, mostra o ícone de cabo desconectado.

## Evidências
- Tamanhos exatos: `strucReview`=292 + `strucStat`=30.488 = 30.780; `strucRankInfo` = 8 + 128×4×32 = 16.392;
  `option` = 31.832; `RANK` = 4 + 84×8 = 676; `MRANK` = 4 + 10.000×12 = 120.004.
- Amostras reais: `PIUNX2.INI`, `RANK.DAT`, `MRANK.DAT` (pasta do jogo) e `nx2save0.bin`/`nx2rank0.bin`
  (`nx2src/src/PIUNX2C/src/res`, jogador `PAWPRINT`, `iWMPosition = 0xEF001`): Adler-32 válido em todos.
- Ida e volta: `Load → Save` reproduz `PIUNX2.INI`, `RANK.DAT` e `MRANK.DAT` **byte a byte**. O que o C grava em
  `nx2save.bin`/`nx2rank.bin` decodifica e valida no decodificador Python independente.
- `eeprom.c`: `Load` do INI real, mudar `svcCoin1Total` e `Save` altera exatamente 1 byte (`bk_Coin1Total`).

## Diferenças deliberadas do port
- O arcade monta o pendrive em `/mnt/0` e `/mnt/1` e amarra o perfil ao **número de série** do pendrive
  (`szSerial`, VID `0x58f`). No PC: o "pendrive" do jogador N é a pasta `NX2_USB<N>` (variável de ambiente) ou
  `<jogo>/save/usb<N>`. **O nome `save/usb<N>` é sugestão do port, não vem do original.** Pasta ausente = não inserido.
- A conferência do `szSerial` não é feita (o modo `HDD_PROFILES` do nx2src também a pula).
- Arquivos da máquina em `<jogo>/save/` (onde o jogo do usuário já os guarda), não em `SETTINGS/`.
- `Nx2Profile_Create` é extra: o arcade registra o perfil num servidor; aqui cria um perfil vazio.
- Créditos em andamento (`O_CREDITS`/`O_COINS`) não existem no `struct option` do NX2 e não persistem.

## Hipóteses
- `cs_CreditPerCoin1` = COIN1 SETTING e `cs_CoinPerCredit2` = COIN2 SETTING (nomes do nx2src ambíguos; no INI de
  referência ambos 0).
- `g_CreditTotal` (51 no INI de referência) é total histórico de créditos, não saldo.
- `bmUnlock`: bits 0..4 = modo desbloqueado (N, H, C, F, NM), 6 = música, 7 = revelada (comentário do nx2src).

## Tela de entrada (implementada: `src/nx2_usbin.c`, estado `STATE_NX_USBIN`)
Fluxo do original (`flowcontrol.cpp`): TITLE → **USB_INPUT** → STATION (também por entrada tardia). Assets: `USB_INPUT`,
`USB_INFO_A`, `USB_LISENCE_ID`, `USB_MESSAGE`, `USB_INFO_B` (carregados nessa ordem, índices BGA 0..4) sobre `USB_BG.MOV`;
texturas `.dds` (DXT5). Nomes de cena e camadas conferidos nas BGAs reais (extraídas com as chaves recuperadas).
1. Intro `USB_intro`; 2. espera de 60 s (`insert usb_hold`, dígitos nas camadas 34/35); por jogador sem perfil,
`Nx2Profile_Load` a cada 16 quadros: OK → licença (ID, personagem, mileage, REWARD/WORLD MAX %, play count, kcal, VO2);
`NODATA` → mensagem "não registrado"; `MOUNTERROR`/`BADDATA` → erro de USB; `NOTINSERTED` → continua esperando.
CENTER do jogador desiste do USB; com todos prontos o tempo vira 15 s e um CENTER seguinte sai; jogador ausente com
crédito entra com CENTER. 3. Fade de 60 quadros com as cenas `..._OUT` e vai para a STATION.
Teste: `PUMPY_AUTOSTATE=USBIN` (abre direto com os dois jogadores) e `NX2_USB0=<pasta>` para apontar o "pendrive" do P1.

## Station Select (implementada: `src/nx2_station.c`)
Depois da verificação do perfil abre a Station do NX2 (`CStation`, nx2src `station.cpp`): `BGA/SELECT_STATION.DAT` sobre
`BGA/BG.MOV`, 4 estações (0 TRAINING, 1 ARCADE, 2 **WORLD MAX**, 3 SPECIAL ZONE), estação inicial = DEFAULT STATION do
Service Menu. DL/UL = anterior, DR/UR = próxima, CENTER confirma (2º CENTER sai); com 2 jogadores TRAINING e WORLD MAX dão
errado (WRONG.WAV) e saem como ARCADE; EVENT também bloqueia. Entrada tardia de jogador volta ao USB_INPUT.
Sons (`SFX_STATION.LUA` + `SFX_GLOBAL.LUA`): entrada `B02.WAV` (EFF_STATION_SELECT) e `STATION_BGM.WAV` em loop;
confirmar `S_CLK.WAV` + voz da estação (`B04` TRAINING, `B03` ARCADE, `B05` WORLD MAX, `B06` SPECIAL); `S_MOVE`, `JOIN`,
`WRONG`, `S_LIGHT`, `START`. O script também cita `WELCOME.WAV` (EFF_WELCOME), que **não existe** na instalação e que o
código do nx2src não dispara. Qual arquivo contém a voz "Welcome to the NX2 World" **não foi confirmado** (hipótese: B02).

## Em aberto
- `fCensor[256]` do INI × RESTRICTION do Service Menu do port (hoje `restrict.dat`): ainda não migrado.
- Da tela de entrada faltam: o texto de posição do WORLD MAX (`szLand`/`szMission`, fonte TTF) e o ícone do perfil
  nas telas seguintes (`CUsbstate`, `USB_COMMON.DAT`). O `ProcResult` e a tela de saída (`USB_OUTPUT`, `USB_REMOVE.DAT`,
  quanto de mileage/kcal/VO2/recordes cada crédito soma e o `SaveData` do fim do crédito) não foram implementados.
- Não testado com teclado: entrada tardia, desistir do USB com CENTER e sair com CENTER (só a lógica de tempo e de
  carga do perfil foram exercitadas).
- `LoadUserOptions/SaveUserOptions` (`nx2user%d.bin`, pares `{chave, valor}`: skin `0x384`, velocidade `0x701`,
  modificadores `0x702`) só existe no modo `HDD_PROFILES` do nx2src; não consta do `piu` do arcade.
