# TorqueBus Studio

TorqueBus Studio — Open Automotive Network & Diagnostics Workbench

O nome remete imediatamente a automotivo + barramentos, mas não nos limita a CAN no futuro podemos incorporar LIN, J1939, UDS, DoIP, XCP, Automotive Ethernet etc. Não há registro de produto automotivo de software relevante usando exatamente o nome “TorqueBus Studio”.

A proposta seria posicioná-lo como uma alternativa comunitária a ferramentas como TSMaster, CANalyzerCANoe e PCAN-Explorer.

 TorqueBus Studio será uma plataforma open source para análise, simulação, diagnóstico e automação de redes automotivas.

Uma decisão arquitetural importante: reproduzir a organização, fluxo de trabalho, densidade de informação e ergonomia do TSMaster, mas não copiar logotipo, ícones, recursos gráficos ou identidade visual proprietária pixel a pixel. Fazemos uma interface funcionalmente equivalente, moderna e com identidade própria.

A TOSUN descreve o TSMaster justamente como uma plataforma que integra monitoramento, transmissão, loggingplayback, gráficos, bancos de dados, scripting, diagnóstico e simulação, além de abstrair diversos fabricantes de hardware. Esse será nosso norte arquitetural. ([TOSUN Technology][1])

---

# 1. Stack oficial do TorqueBus Studio

Definição inicial da stack:

 Componente          Escolha                             
 ------------------  ----------------------------------- 
 Linguagem           C++23                           
 Qt                  Qt 6.11.2 CommunityOpen Source 
 UI desktop          Qt Widgets                      
 UI modernaHMI      Qt Quick  QML                  
 Docking             KDDockWidgets                   
 Gráficos            Qt Graphs + renderer próprio    
 CAN abstraction     Core próprio                    
 PEAK                Qt PeakCAN  PCAN-Basic 5.x     
 Kvaser              CANlib + adapter próprio        
 Build               CMake + Ninja                   
 Compilador Windows  MSVC x64                        
 Configuração        JSON                                
 Dados internos      SQLite                              
 Testes              GoogleTest                          
 CICD               GitHub Actions                      
 Plataforma inicial  Windows 11 x64                  
 Licença             GPL-3.0                         

Hoje, 29 de agosto de 2026, a versão corrigida mais recente da série que escolhemos é Qt 6.11.2, lançada em 18 de agosto de 2026 com cerca de 400 correções e melhorias sobre 6.11.1. Portanto o projeto adota diretamente a versão 6.11.2 em vez da 6.11.0. ([Qt][2])

---

# 2. Licença GPLv3

A licença adotada para o projeto é

GNU GPL v3.0

Isso resolve de maneira muito elegante uma questão importante.

O Qt possui diversos módulos LGPL, mas Qt Graphs é GPLv3 para usuários open source. Além disso, o KDDockWidgets é GPL2GPL3 ou comercial. ([Documentação Qt][3])

Como nossa intenção já é

 software aberto para a comunidade,

não vejo vantagem em lutar contra isso.

Então

```text
TorqueBus Studio
Copyright © TorqueBus contributors

License
GNU General Public License v3.0
```

O projeto inteiro poderá ser

 estudado;
 modificado;
 redistribuído;
 compilado;
 forkado;
 melhorado pela comunidade.

E podemos usar tranquilamente os componentes GPL do ecossistema Qt.

---

# 3. Primeiros hardwares oficiais

Na versão inicial

```text
TorqueBus Studio
        │
        ├── Kvaser
        │     ├── Kvaser Virtual CAN 0
        │     ├── Kvaser Virtual CAN 1
        │     └── futuros Kvaser físicos
        │
        └── PEAK-System
              ├── PCAN-USB
              └── PCAN-USB FD
```

## Kvaser

Usaremos diretamente

```text
Kvaser CANlib
```

e criaremos

```cpp
KvaserCanBackend
```

O CANlib permite enumerar os canais instalados com `canGetNumberOfChannels()` e consultar seus detalhes com `canGetChannelData()`. A API também permite identificar especificamente interfaces virtuais e abrir virtual channels com `canOPEN_ACCEPT_VIRTUAL`. ([Kvaser][4])

Um detalhe excelente para nosso desenvolvimento a instalação padrão da Kvaser no Windows instala dois dispositivos virtuais, justamente adequados para desenvolvimento e teste sem hardware. ([Kvaser][5])

Isso casa perfeitamente com

```text
Kvaser Virtual CAN 0
Kvaser Virtual CAN 1
```

como nosso primeiro ambiente de desenvolvimento.

---

# 4. PEAK-System

Aqui temos uma vantagem enorme.

O Qt 6.11 já possui

```text
Qt SerialBus
   └── peakcan
```

e o plugin encapsula diretamente o PCAN-Basic. ([Documentação Qt][6])

Portanto inicialmente podemos implementar

```cpp
PeakCanBackend
```

envolvendo

```cpp
QCanBus
QCanBusDevice
QCanBusDeviceInfo
```

O PCAN-Basic atualmente está na versão 5.1.0, publicada em julho de 2026, e suporta CAN CC, CAN FD e CAN XL. ([peak-system.com][7])

O PCAN-USB e seus drivers atuais têm suporte a Windows 11. ([peak-system.com][8])

---

# 5. Não acoplaremos o Core ao Qt CAN

Essa decisão será fundamental.

Evita-se acoplar

```text
Application
    ↓
QCanBusDevice
```

diretamente.

Faremos

```text
                       TorqueBus Core
                              │
                     ICanDriverBackend
                              │
          ┌───────────────────┴────────────────────┐
          │                                        │
 KvaserCanBackend                         PeakCanBackend
          │                                        │
     Kvaser CANlib                    Qt PeakCAN  PCAN-Basic
```

Assim futuramente entram

```text
VectorCanBackend
SocketCanBackend
IxxatCanBackend
TosunCanBackend
J2534Backend
ZlgCanBackend
SlcanBackend
UsbCanBackend
```

sem alterar absolutamente nada no restante do software.

---

# 6. Interface principal

A experiência será deliberadamente próxima ao TSMasterCANoe.

Layout planejado:

```text
┌─────────────────────────────────────────────────────────────────────────┐
│ TorqueBus Studio                                   project.tbs         │
├─────────────────────────────────────────────────────────────────────────┤
│ File  Home  Hardware  Analysis  Simulation  Diagnostics  Tools  Help  │
├─────────────────────────────────────────────────────────────────────────┤
│ ▶ Start │ ■ Stop │ ● Record │ ▶ Replay │ CAN │ DBC │ UDS │ Graph     │
├───────────────┬───────────────────────────────────────┬─────────────────┤
│ Project       │ CAN Trace                             │ Properties      │
│ Explorer      │                                       │                 │
│               │ Time      Ch Dir ID     DLC Data      │ Channel         │
│ ▼ Hardware    │ 1.00123   1  Rx  18FF.. 8   ...      │ Kvaser Virtual │
│   CAN 1       │ 1.00145   1  Tx  100    8   ...      │                 │
│   CAN 2       │ 1.00241   1  Rx  101    8   ...      │ Bitrate         │
│               │                                       │ 500 kbits      │
│ ▼ Databases   │                                       │                 │
│   vehicle.dbc │                                       │ Status Online  │
├───────────────┴───────────────────────────────────────┴─────────────────┤
│ Trace │ Transmit │ Graph │ Statistics │ Diagnostics │ Python │ Log    │
├─────────────────────────────────────────────────────────────────────────┤
│ CAN1 ● Online    Frames 1,283,331    Load 42.3%        Recording ● │
└─────────────────────────────────────────────────────────────────────────┘
```

Isso representa o mesmo paradigma de ferramenta profissional de análise automotiva, mas com nossa identidade visual.

---

# 7. Docking profissional

Todos estes componentes serão painéis

```text
CAN Trace
CAN Transmit
Project Explorer
Hardware Manager
DBC Explorer
Properties
Statistics
Graph
Diagnostics
Logging
Replay
Console
Script Editor
System Variables
```

e poderão ser

 redimensionados;
 arrastados;
 destacados;
 transformados em tabs;
 movidos para outro monitor;
 fechados;
 reabertos;
 salvos como workspace.

Exemplo

```text
Workspace
CAN Development

Workspace
Diagnostics

Workspace
Vehicle Testing

Workspace
Logging

Workspace
Simulation
```

KDDockWidgets foi desenvolvido justamente para aplicações Qt complexas desse tipo. ([GitHub][9])

---

# 8. Identidade visual

O TorqueBus conta com dois temas oficiais

### TorqueBus Dark

Inspirado em

```text
Visual Studio
TSMaster
CANoe
Qt Creator
```

com

```text
background       #1E1F22
panels           #25262A
toolbar          #2B2D31
border           #3A3D42

accent           azulciano
Rx               verde suave
Tx               azul
Warning          amarelo
Error            vermelho
```

E

### TorqueBus Light

Interface profissional clara para laboratório.

Nada de aparência Qt antiga.

Widgets receberão um `QProxyStyle`stylesheet próprio e ícones SVG.

---

# 9. Arquitetura geral

Arquitetura base congelada antes da implementação da UI

```text
                       TorqueBus Studio
                              │
                  ┌───────────▼───────────┐
                  │     Application       │
                  │ Qt Widgets + QML      │
                  └───────────┬───────────┘
                              │
                  ┌───────────▼───────────┐
                  │ Application Services  │
                  └───────────┬───────────┘
                              │
 ┌────────────────────────────┼───────────────────────────┐
 │                            │                           │
 ▼                            ▼                           ▼
CAN Engine               Database Engine            Diagnostic Engine
 │                            │                           │
 │                       DBC  ARXML                  ISO-TP  UDS
 │
 ▼
CAN Hardware Manager
 │
 ├── Kvaser backend
 ├── PEAK backend
 ├── Virtual backend
 └── future backends
```

E separadamente

```text
                   Event  Signal System
                           │
          ┌────────────────┼────────────────┐
          ▼                ▼                ▼
       Logging          Graphics        Scripting
```

---

# 10. Estrutura do repositório

Isto era uma intenção, escrita antes da primeira linha de código, e virou a
estrutura real com três desvios que vale nomear em vez de deixar o leitor
descobrir sozinho. Esta é a árvore **de hoje**:

```text
torquebus-studio
│
├── .github
│   ├── workflows              (ci.yml, release.yml)
│   ├── ISSUE_TEMPLATE
│   └── PULL_REQUEST_TEMPLATE.md
│
├── cmake
├── data                       (tabela de nomes J1939 que vai junto)
│
├── docs
│   ├── development            (getting-started, testing, validation, …)
│   ├── ARCHITECTURE.md
│   └── PLAN.md
│
├── resources
│   ├── icons
│   └── themes
│
├── src
│   ├── app                    (Main.cpp, ApplicationContext.cpp)
│   │
│   ├── core                   (sem Qt, sem vendor)
│   │   ├── can       database    isotp      j1939
│   │   ├── log       pipeline    plot       scripting
│   │   ├── dashboard simulation  testing    trace
│   │   ├── transmit  diagnostics
│   │   └── pipeline/nodes
│   │
│   ├── drivers
│   │   ├── api                (ICanBackend)
│   │   └── virtual            (o único embutido)
│   │
│   ├── plugins
│   │   ├── host               (ABI e carregador)
│   │   ├── driver-kvaser
│   │   └── driver-peak
│   │
│   ├── services
│   │
│   └── ui
│       ├── mainwindow  canvas     trace      transmit
│       ├── graph       hardware   project    properties
│       ├── diagnostics dashboard  database   engine
│       ├── j1939       output     playback   plugins
│       ├── preferences scripting  statistics testing
│       ├── theme       common
│
├── tests
│   ├── unit                   integration    hardware
│
├── examples
│   ├── databases              projects       scripts
│
├── third_party                (Lua 5.5.0, compilado do fonte)
│
├── tools                      (prompt de build, j1939-names.py, check-package.ps1)
│
├── CMakeLists.txt   CMakePresets.json
├── LICENSE          README.md
├── CONTRIBUTING.md  CODE_OF_CONDUCT.md
```

Os três desvios:

* **`core/logging` e `core/project` nunca existiram.** O primeiro virou
  `core/log`; o segundo virou `services`, porque projeto e workspace são estado
  do aplicativo e não domínio do barramento.
* **`drivers/kvaser` e `drivers/peak` saíram de `drivers`** na v0.17 e viraram
  plugins. `drivers` guarda a interface e o barramento virtual — o que todo
  build tem.
* **`docs/architecture`, `docs/protocols`, `docs/screenshots` e
  `resources/fonts` foram reservados e nunca preenchidos.** Ficaram anos como
  diretórios vazios com um `.gitkeep` dentro, prometendo uma organização que a
  documentação não seguiu: arquitetura é um arquivo, e protocolo é assunto de
  `docs/development`. Removidos.

---

# 11. Nosso CAN Frame

Não usaria `QCanBusFrame` como estrutura universal do programa.

Criaria nosso próprio modelo

```cpp
namespace torquebus {

enum class CanDirection  stduint8_t {
    Rx,
    Tx
};

enum class CanFrameFormat  stduint8_t {
    Standard,
    Extended
};

struct CanFrame final {
    stduint64_t timestampNs{};

    stduint32_t identifier{};

    stduint8_t channel{};
    stduint8_t dlc{};
    stduint8_t length{};

    CanDirection direction{};
    CanFrameFormat format{};

    bool fd{};
    bool brs{};
    bool esi{};
    bool rtr{};
    bool error{};

    stdarraystduint8_t, 64 data{};
};

}
```

Isso nos deixa independentes de

```text
Qt
Kvaser
PEAK
Vector
Linux
Windows
```

---

# 12. API de driver

Algo nessa direção

```cpp
class ICanBackend
{
public
    virtual ~ICanBackend() = default;

    virtual stdstring_view name() const noexcept = 0;

    virtual stdvectorCanDeviceInfo enumerate() = 0;

    virtual Result open(
        const CanChannelConfig& config) = 0;

    virtual void close() = 0;

    virtual Result transmit(
        const CanFrame& frame) = 0;

    virtual CanBusStatus status() const = 0;

    virtual void setFrameHandler(
        FrameHandler handler) = 0;
};
```

Kvaser

```cpp
class KvaserCanBackend final  public ICanBackend
{
};
```

PEAK

```cpp
class PeakCanBackend final  public ICanBackend
{
};
```

Essa interface será uma das peças mais importantes de todo o TorqueBus.

---

# 13. Hardware Manager

Teremos uma janela parecida com

```text
Hardware Configuration

Application Channel        Hardware

CAN 1                      Kvaser Virtual CAN 0
CAN 2                      Kvaser Virtual CAN 1
CAN 3                      PCAN-USB Channel 1

------------------------------------------------

Protocol                    CAN
Bitrate                     500 kbits
Sample Point                Auto

Listen Only                 [ ]
Silent Mode                 [ ]

Acceptance Filter           Off

Status                      Ready

                   [ Apply ] [ OK ]
```

O usuário não trabalhará diretamente com hardware channel 4.

Ele trabalhará com

```text
CAN 1
CAN 2
CAN 3
```

E o projeto guardará o mapeamento.

---

# 14. Device capability model

Desde o começo o driver deverá informar

```cpp
struct CanCapabilities
{
    bool canClassic;
    bool canFd;
    bool canFdBrs;

    bool listenOnly;
    bool hardwareTimestamp;

    bool errorFrames;
    bool hardwareFilters;

    stduint32_t maxChannels;
};
```

Isso será essencial porque

```text
PCAN-USB
```

não tem necessariamente as mesmas capacidades de

```text
PCAN-USB FD
```

e não devemos espalhar

```cpp
if (driver == PCAN)
```

pelo código.

---

# 15. CAN Trace

Será nosso primeiro módulo realmente importante.

Colunas

```text
Time
Delta
Channel
Direction
ID
Name
Type
DLC
Data
Cycle
Count
Flags
```

Exemplo

```text
0.124582   +0.010   CAN1 Rx  18FF50E5 EngineData  EXT 8  FF 42...
0.125143   +0.001   CAN1 Rx  0CF00400 EngineSpeed EXT 8  00 FF...
```

Dois modos

### Chronological

```text
frame
frame
frame
frame
frame
```

### Fixed  Unique ID

```text
0x100       count 10023
0x101       count 8921
0x18FF...   count 14832
```

O próprio TSMaster oferece os conceitos de scrollingfixed display, cycle, DBC parsing, filtros por canalID e destaque de bytes alterados; usaremos esses requisitos como referência funcional. ([TOSUN Technology][10])

---

# 16. Performance do Trace

Isso aqui precisa ser projetado corretamente desde o primeiro commit.

Nunca

```text
CAN frame
   ↓
QObject
   ↓
QML object
   ↓
linha gráfica
```

para cada frame.

Faremos

```text
             hardware
                │
                ▼
        driver receiver
                │
                ▼
          Frame Queue
                │
                ▼
           CAN Engine
                     
                      
      filter   logger    decoder
        │
        ▼
   Trace Store
        │
        ▼
 QAbstractTableModel
        │
        ▼
     QTableView
```

A UI não recebe um signal Qt para cada frame.

Ela recebe batches

```text
100 frames
500 frames
1000 frames
```

ou atualizações temporizadas.

Isso será decisivo com CAN FD.

---

# 17. Meta de desempenho

Metas verificadas por testes desde o início:

```text
1.000.000+ frames armazenados
sem travar a interface

100k+ framess internos
sem perda na pipeline

scroll suave

filter instantâneo

logging independente da UI
```

O Trace deve poder ficar fechado e o logging continuar.

Esse detalhe separa um software profissional de um CAN viewer.

---

# 18. CAN Transmit

Outro painel fundamental

```text
CAN Transmit
────────────────────────────────────────────────────────

Enable  Ch    ID    Type  DLC  Data                 Cycle
  ☑      CAN1   100    STD     8    01 02 03 04...        100 ms
  ☑      CAN1   200    STD     8    FF FF 00 00...        500 ms
  ☐      CAN2   18FF.. EXT     8    01 00...              manual
```

Modos

```text
Manual
Periodic
One Shot
Burst
Sequence
```

Posteriormente

```text
Signal generator
Ramp
Sine
Square
Random
Script
```

---

# 19. DBC

Será um componente central do produto.

```text
vehicle.dbc
│
├── Nodes
│   ├── ECU
│   ├── TCU
│   └── Dashboard
│
├── Messages
│   ├── EngineData
│   │    ├── EngineSpeed
│   │    ├── Torque
│   │    └── CoolantTemp
│   │
│   └── VehicleData
│        └── VehicleSpeed
```

O Qt já possui `QCanDbcFileParser` e classes de descriçãodecodificação CAN. ([Documentação Qt][11])

Mas colocaremos isso atrás de uma interface nossa

```text
IDatabaseParser
```

para posteriormente suportar

```text
DBC
ARXML
LDF
FIBEX
A2L
```

---

# 20. Signal Engine

A partir do DBC

```text
CAN Frame
    ↓
DBC Decoder
    ↓
Signal Engine
    ↓
EngineSpeed = 3245 rpm
VehicleSpeed = 123.4 kmh
CoolantTemp = 91 °C
```

Esses signals poderão alimentar simultaneamente

```text
Trace
Graphs
Dashboard
Python
Logging
Diagnostics
Simulation
Statistics
```

Esse modelo deve estar no Core, nunca na interface.

---

# 21. Graph

Painel

```text
Graph
────────────────────────────────

7000 ┤            RPM
6000 ┤        ╭────────
5000 ┤      ╭─╯
4000 ┤ ─────╯
3000 ┤
     └─────────────────────────
        0    5    10   15 sec
```

Recursos

 zoom;
 pan;
 múltiplos signals;
 múltiplos eixos Y;
 cursores;
 markers;
 freeze;
 measurement;
 minmax;
 delta;
 export;
 liveoffline.

O Qt Graphs é adequado para a primeira implementação e está disponível em Qt 6.11, lembrando que no modelo open source é GPL. ([Documentação Qt][3])

Depois podemos substituir o renderer sem afetar o restante do software.

---

# 22. Logging

Arquitetura

```text
CAN Engine
    │
    └────────────→ Logging Engine
                        │
                    background
                       thread
                        │
                 buffered writer
```

Nunca

```text
UI → save file
```

Primeiros formatos

```text
.tblog      TorqueBus native binary
.asc
.csv
```

Depois

```text
BLF
MDF4
MAT
```

O formato nativo será otimizado para gravação rápida e playback.

---

# 23. Playback

Teremos

```text
Playback
──────────────────────────────

File vehicle_test.tblog

◀   ▶   ❚❚   ■   ▶▶

Speed
0.1x
0.5x
1x
2x
5x
10x
Maximum

000124.320  002041.120
```

Com

```text
offline analysis
```

e

```text
online replay
```

onde os frames podem ser retransmitidos para um CAN físico.

---

# 24. Statistics

Desde as primeiras versões

```text
CAN Statistics

Bus Load               42.7 %
Peak Load              67.9 %

Rx frames               1,342,238
Tx frames                  42,118

Framess                 8,291

Error frames                 12

Bus status                 OK
```

A TSMaster oferece carga de barramento, pico, taxa de frames, contadores e estado do controller; adotaremos funcionalidade equivalente. ([TOSUN Technology][10])

---

# 25. Projects

Extensão

```text
.tbsproj
```

Exemplo

```text
EngineTest
│
├── EngineTest.tbsproj
│
├── databases
│   └── vehicle.dbc
│
├── scripts
│
├── panels
│
├── diagnostics
│
├── logs
│
└── workspace
```

O projeto lembrará

 hardware;
 canais;
 baud rates;
 DBCs;
 filters;
 gráficos;
 mensagens transmitidas;
 janelas;
 posições;
 workspaces;
 scripts;
 diagnóstico.

Ao abrir

```text
EngineTest.tbsproj
```

o ambiente volta exatamente ao estado anterior.

---

# 26. UDS

Depois que CAN + DBC estiverem maduros, entra

```text
ISO 15765-2
ISO-TP
        ↓
ISO 14229
UDS
```

Arquitetura

```text
CAN
 ↓
ISO-TP
 ↓
UDS Transport
 ↓
UDS Client
 ↓
Diagnostic Session
```

Suporte

```text
0x10 Diagnostic Session Control
0x11 ECU Reset
0x14 Clear Diagnostic Information
0x19 Read DTC
0x22 Read Data By Identifier
0x27 Security Access
0x2E Write Data By Identifier
0x31 Routine Control
0x34 Request Download
0x36 Transfer Data
0x37 Transfer Exit
0x3E Tester Present
```

A primeira meta do módulo será diagnóstico legítimo e desenvolvimentoteste de ECUs.

---

# 27. Diagnostic Console

Algo como

```text
UDS Console

ECU
Request ID       0x7E0
Response ID      0x7E8

──────────────────────────────

10 03  →  Extended Session
22 F1 90 → Read VIN

TX  02 10 03 ...
RX  06 50 03 ...

Response time 12.4 ms
```

Posteriormente teremos um

```text
Diagnostic Service Editor
```

---

# 28. Scripting — Lua 5.5

> **Revisado.** Este plano congelou Python. A revisão o substitui inteiramente
> por **Lua 5.5.0**: blocos de ECU, nó de script, automação de teste e
> relatórios. Uma linguagem embarcada só significa um binding para manter, uma
> API para documentar e uma linguagem para o usuário aprender — e o `cansim` já
> provou o desenho.

**Versão congelada: Lua 5.5.0**, compilada do fonte junto com o projeto. Não
usamos a `liblua.a` pré-compilada de nenhuma máquina: o CI precisa reproduzir o
build em qualquer lugar, e uma biblioteca binária num caminho local é
exatamente o tipo de dependência que funciona só na máquina de quem escreveu.

O contrato vem direto do `cansim`, que já roda com 20 ECUs:

```lua
-- Um bloco de ECU no canvas é um script com este ciclo de vida.
function on_enable()          -- inicialização
function on_disable()         -- limpeza
function on_timer()           -- execução periódica
function on_message(frame)    -- tratamento de mensagem recebida
```

com a API já existente: `emit`, `set_timer`, `enable_node`, `disable_node`,
`get_time_us`, `log_message`, `set_bitrate`, `get_can_status`.

Cada bloco no canvas é um desses scripts, com portas visíveis e configuração
por formulário. Montar uma rede CAN de ECUs simuladas deixa de ser editar um
JSON e passa a ser desenhar.

## Compatibilidade: três scripts precisam de migração

Auditando os 20 scripts do `cansim` contra o Lua 5.5, três usam funções que
**não existem mais**:

| Script | Chamada | Removida em | Substituta |
|---|---|---|---|
| `ecu_motor.lua`, `ecu_lift.lua`, `ecu_vehicle.lua` | `math.frexp` | 5.4 | `string.pack("<f", x)` |
| `ecu_motor.lua` | `math.ldexp` | 5.4 | idem |
| `ecu_motor.lua` | `unpack` global | 5.2 | `table.unpack` |

Elas só reaparecem com `LUA_COMPAT_5_3` / `LUA_COMPAT_5_1`, e o Makefile do
`cansim` não define nenhum dos dois — ou seja, esses três scripts falham com
*"attempt to call a nil value"* no momento em que `float_to_bytes` roda.

**Decisão: Lua 5.5 limpo, sem flags de compatibilidade, e os três scripts
migram.** O motivo é que os scripts mais novos do próprio `cansim` — `node1`,
`node3`, `signal_generator` — já usam `string.pack("<f", x)`, que faz em uma
linha o que o `float_to_bytes` faz em vinte. A migração segue uma direção que
você já tomou na prática; carregar flags de compatibilidade para sempre daria
uma superfície de script com formato de 5.3 num mundo 5.5, e todo autor de
script futuro herdaria isso.

## Automação de teste, também em Lua

```text
Lua Console
```

Para automação de teste, processamento offline e relatórios. API conceitualmente

```python
can1 = torquebus.channel(CAN1)

can1.send(
    id=0x123,
    data=[0x01, 0x02, 0x03]
)

rpm = signals[EngineSpeed]

print(rpm.value)
```

E

```python
@can.on_frame(0x123)
def received(frame)
    print(frame)
```

Isso permitirá

 testes automatizados;
 geração de mensagens;
 ECU simulation;
 diagnóstico;
 processamento de signals;
 reports.

---

# 29. Plugins

É importante fazermos isso cedo.

```text
plugins
    driver-kvaser
    driver-peak
    protocol-j1939
    protocol-uds
    database-dbc
    scripting-python
```

No futuro um terceiro poderá criar

```text
driver-acme-usbcan.dll
```

e instalar em

```text
TorqueBusplugins
```

sem modificar nosso código.

---

# 30. Roadmap oficial

Divisão das etapas de desenvolvimento:

## v0.1 — Foundation

Objetivo aplicação abre e possui aparência profissional.

Implementar

 CMake;
 Qt 6.11.2;
 C++23;
 GPLv3;
 estrutura modular;
 MainWindow;
 darklight themes;
 docking;
 menus;
 toolbar;
 status bar;
 Project Explorer;
 Properties;
 Output;
 persistência de layout.

---

## v0.2 — CAN Core

Implementar

 `CanFrame`;
 `ICanBackend`;
 CAN Engine;
 channels;
 threaded RXTX;
 queues;
 timestamp;
 filtering;
 counters;
 bus state;
 virtual internal backend para unit tests.

---

## v0.3 — Kvaser

Implementar

```text
Kvaser CANlib
```

com

 autodetect CANlib;
 device enumeration;
 Virtual CAN;
 Physical CAN;
 openclose;
 bitrate;
 receive;
 transmit;
 timestamps;
 status;
 errors.

Primeiro teste oficial

```text
Kvaser Virtual 0
        ↓
TorqueBus
        ↓
Kvaser Virtual 1
```

O CANlib documenta especificamente canais virtuais e sua enumeração, então isso é uma ótima base para nossa primeira integração. ([Kvaser][12])

---

## v0.4 — CAN Trace

Implementar

 scrolling mode;
 fixed mode;
 filters;
 RxTx;
 colors;
 timestamps;
 delta;
 cycle time;
 count;
 copy;
 export;
 search;
 pause;
 clear.

Aqui já teremos uma ferramenta realmente utilizável.

---

# v0.5 — PCAN

Implementar

```text
PCAN-USB
PCAN-USB FD
```

usando

```text
Qt PeakCAN
        ↓
PCAN-Basic
```

Qt 6.11 já fornece essa integração e exige PCAN-Basicdriver instalado no Windows. ([Documentação Qt][6])

---

# v0.6 — Transmit

 manual frames;
 periodic frames;
 channel;
 STDEXT;
 CAN FD;
 editable payload;
 presets;
 sequences.

---

# v0.7 — DBC

 import DBC;
 database tree;
 messages;
 signals;
 nodes;
 decode;
 encode;
 units;
 scaling;
 value tables;
 multiplexing;
 drag & drop de signal.

---

# v0.8 — Graph + Statistics

 signal plotting;
 cursors;
 zoom;
 multi-axis;
 bus load;
 frequency;
 errors;
 counters.

---

# v0.9 — Logger + Playback

 `.tblog`;
 ASC;
 CSV;
 large-file recording;
 replay;
 offline analysis;
 timeline.

---

# v0.10 — Workspace  Project

Transformamos tudo em ferramenta de engenharia completa

```text
.tbsproj
workspace
recent projects
session restore
project explorer
hardware configuration
```

---

# v0.11 — ISO-TP

 segmentation;
 flow control;
 timing;
 addressing;
 CANCAN FD.

---

# v0.12 — UDS

 UDS client;
 service editor;
 DTC;
 DID;
 session;
 diagnostic console.

---

# v0.13 — Lua, todo o potencial

Python foi retirado do plano. A ferramenta já embarca Lua 5.5, já tem vinte
ECUs escritas em Lua vindas do cansim, e uma segunda linguagem embarcada seria
duas APIs a manter, duas sandboxes a auditar e duas metades da documentação
sempre desatualizadas. O investimento é em Lua.

O que já foi feito:

- **ECU simulada que fala UDS** — `uds_did`, `uds_dtc`, `uds_session`,
  `on_uds_request` (três respostas: bytes, nada, ou `false` = silêncio),
  `on_security_seed`.
- **Temporização rica** — `every(ms, fn)` com vários temporizadores, mensagens
  cíclicas declarativas (`cyclic`), `stop_cyclic`, e o prelúdio `tb` com
  geradores de sinal, contador e CRC-8/E2E.
- **Ler o barramento de dentro do script** — `bus_last`, `bus_stats`.
- **Injeção de falha** — `fault(id, {...})`: congelar a mensagem, DLC errado,
  truncar, inverter bits (um CRC deliberadamente errado).
- **Recarga a quente** — editar um script sem parar a medição. `ScriptLibrary`
  (o executor nunca espera pelo editor), `LuaEcuNode::reload`, e o painel
  **Script** com número de linha, coloração e a linha do erro marcada. A regra:
  *um script que falha ao carregar deixa o que está rodando em paz.*

- **Sequências de teste** — o bloco **Test Sequence**: `test(name, fn)`,
  `expect`/`expect_frame`/`expect_silence`, `assert_*`, `send`, `wait`, e um
  relatório de aprovado/reprovado/erro por caso. Cada caso roda numa corrotina
  Lua, então a sequência é escrita na ordem em que as coisas acontecem sem que o
  executor jamais bloqueie. Três resultados, não dois: uma falha é sobre a rede,
  um erro é sobre o teste, e confundir os dois manda a pessoa errada para a
  bancada.

- **Painel Test** — o resumo primeiro (que é a resposta inteira), uma linha por
  caso, e cada linha abre nas verificações que fez. Exporta a execução em
  Markdown, porque um veredito que não sai da janela é um veredito sobre o qual
  ninguém mais pode agir. O resultado continua na tela depois do Stop, que é
  quando ele é lido de verdade.

**v0.13 fechada.**

---

# v0.14 — Dashboard Designer

Widgets: Gauge, Numeric, Lamp, Button, Switch, Slider, Knob, Label.

Fora da lista original, e de propósito: **Graph** é o painel Graph, que já
existe e faz isso melhor; **Image** é uma referência a arquivo que o projeto
teria que carregar e resolver, e isso é um problema à parte.

Associáveis a **sinal CAN** (leitura) e **variável de sistema** (leitura e
escrita). A "Python variable" do plano original sai junto com o Python; o que
ela nomeava — um valor que não é sinal nem resultado de diagnóstico, que
pertence à simulação e não ao fio — é a variável de sistema. Resultado UDS e
estatística de barramento ficam para depois.

Escrever um sinal CAN direto **não** é uma das ligações: um widget que
escrevesse um sinal teria que ter mensagem, tempo de ciclo e canal — que é uma
entrada de lista de transmissão, e já existe uma. Um slider escreve uma
variável; um script ou uma entrada de transmissão decide o que aquilo significa
no fio. Um mecanismo, um lugar para olhar quando o valor não chega.

Feito:

- **SystemVariables** — valores nomeados compartilhados entre dashboard,
  script e barramento. Um `std::atomic<double>` por variável num array fixo:
  ler e escrever não pega lock nenhum, então um script pode tocar uma variável
  dentro do `on_message`. Os valores sobrevivem ao Start e ao Stop — um slider
  que se reseta a cada medição é o comportamento que ninguém quer e todo mundo
  já encontrou.
- **`var_get` / `var_set`** nos scripts Lua.
- **DashboardDescription** — o dashboard como dado, com `validate()` que recusa
  o que faria um painel abrir e não mostrar nada.

- **Serialização no .tbsproj** — formato 3. Um projeto escrito antes disso abre
  com um dashboard vazio; um widget de um tipo que este build não tem **recusa
  o arquivo**, porque desenhar outra coisa no lugar seria mentir sobre o que o
  arquivo contém — e a próxima gravação escreveria a mentira de volta.
- **Painel Dashboard** — dois modos, e o modo é o projeto inteiro. Em **Run** os
  ponteiros andam e um slider sob a mão escreve a variável que uma ECU simulada
  está lendo. Em **Edit** os widgets são arrastados, redimensionados pelo canto,
  adicionados pelo menu de contexto e apagados — e os controles não respondem,
  porque arrastar um slider para o lugar não pode mandar para o barramento os
  valores que ele varre no caminho. Tudo desenhado à mão: não existe QGauge, e a
  alternativa é um QDial vestindo uma folha de estilo que briga com ele.
- **Painel Widget** — as configurações do widget selecionado, irmão do painel
  Block e no mesmo lugar da tela. Um controle ligado a um sinal CAN é recusado
  com a saída escrita por extenso.

**v0.14 fechada.**

---

# v0.15 — Simulation

Metade desta lista já chegou na v0.13, e vale dizer qual metade em vez de
riscar em silêncio:

- *ECU node simulation* — o bloco Lua ECU.
- *periodic messages* — `cyclic()` no Lua e a lista de transmissão periódica.
- *script-driven simulation* — é o que a v0.13 inteira é.
- *message generators* — o prelúdio `tb`: ramp, sine, square, drift, steps,
  counter. Continuam em Lua de propósito: são aritmética sobre o relógio da
  medição, e um editor de geradores seria uma segunda forma de dizer a mesma
  coisa.

O que faltava de verdade:

- **Simulação de barramento restante** — o bloco **Rest Bus**. Uma ECU numa
  bancada está cercada de silêncio: ela espera a mensagem do motor, o status da
  ignição, a velocidade — e sem isso fica em estado de falha, ou simplesmente
  parada, enquanto alguém se pergunta se a fiação está errada. O bloco manda
  tudo o que o resto da rede mandaria.

  A forma é a que o fluxo de trabalho pede: uma lista dos nós **que não** são
  simulados. É a pergunta que a pessoa tem de verdade ("tudo menos o que está
  na minha mesa"), e fazer ao contrário é a mesma resposta digitada quarenta
  vezes e redigitada toda vez que o banco de dados cresce um nó.

  **Uma mensagem sem tempo de ciclo não é enviada.** Um banco que não declara
  `GenMsgCycleTime` não está dizendo "a cada 100 ms"; muita mensagem é por
  evento, e inventar um período põe no barramento tráfego que a rede real não
  carrega — o que é pior que tráfego faltando, porque parece certo.

- **Manipulação de sinal** — sinais nomeados em `signals` seguem uma **variável
  de sistema**, e essa é a história interativa inteira: um slider do dashboard
  ligado a `throttle_pedal` dirige o rest bus, e `var_set("throttle_pedal", 40)`
  num script Lua também. Um mecanismo, já construído, já na tela.

**v0.15 fechada.**

---

# v0.16 — J1939

Máquina agrícola, caminhão, ônibus, barco, gerador. A camada física é a mesma
CAN de 29 bits que já está no fio; o que muda é que o identificador deixa de ser
um número que um banco de dados traduz e passa a ser **estrutura** — quem
mandou, para quem, e qual mensagem. Uma ferramenta que mostra `0x18FEE500` está
tecnicamente certa e praticamente inútil.

--- O que já existe e não será reescrito ------------------------------------

**A decodificação de sinal.** Um SPN é um sinal: bit inicial, comprimento, ordem
de bytes, fator, offset. `CanSignal` já faz isso, o `DbcParser` já lê bancos com
identificador estendido (bit 31 do número na linha `BO_`), e bancos J1939 em
`.dbc` são o formato que todo mundo realmente troca. Não haverá um segundo
decodificador, nem um segundo formato de banco.

O que falta não é decodificar: é **casar**. Hoje uma mensagem é encontrada pelo
identificador inteiro. Em J1939 o identificador carrega o endereço de quem
transmitiu, então a mesma mensagem vinda de duas ECUs são dois identificadores —
e procurar por identificador não acha nenhuma das duas. O casamento passa a ser
por **PGN**, com o endereço de origem como informação, não como parte da chave.

--- O identificador, desmontado ---------------------------------------------

Três bits de prioridade, EDP, DP, PF, PS, SA. A regra que decide tudo:

* **PF < 240 (PDU1)** — específica de destino. `PS` é o endereço do destinatário
  e **não faz parte do PGN**.
* **PF >= 240 (PDU2)** — difusão. `PS` é extensão de grupo e **faz parte do PGN**.

Zerar `PS` sempre, ou nunca, são os dois jeitos de errar isto, e os dois
produzem o mesmo sintoma: um PGN que não existe em banco nenhum, e uma mensagem
que a ferramenta jura não conhecer. Fica em `core/j1939/J1939Id.h` como função
pura sobre um `CanFrame` — sem estado, sem alocação, testável sozinha. É a peça
que todo o resto usa, então é a peça que tem de estar certa primeiro.

--- Transporte: TP, BAM, e o que não será feito ------------------------------

Acima de oito bytes, J1939 tem dois mecanismos, e **nenhum dos dois é ISO-TP**.
A tentação de reaproveitar `IsoTpConnection` é real e está errada: outro
cabeçalho, outra máquina de estados, outra numeração, outro handshake. Um
transporte que é "quase" outro é a forma mais cara de compartilhar código.

* **TP** ponto a ponto — RTS/CTS (PGN 60416) e os pacotes de dados (60160).
* **BAM** difusão — anunciada e despejada, sem handshake, com no mínimo 50 ms
  entre pacotes. Ninguém confirma nada, e ninguém pode pedir de novo.

**Um BAM com pacote faltando é abandonado e relatado, nunca remendado.** Sete
bytes ausentes preenchidos com zero produzem uma mensagem que remonta, decodifica
e mente — e uma pressão de óleo zero lida de um buraco é indistinguível de uma
pressão de óleo zero medida. Perder a mensagem é recuperável; acreditar nela não.

Uma segunda sessão da mesma origem abandona a primeira, que é o que a norma diz e
também o único comportamento que não vaza buffer numa bancada onde alguém está
resetando uma ECU repetidamente.

**ETP entrou - e não é J1939-21.** Esta foi a descoberta mais útil da leitura da
norma, e não estava no roteiro.

A J1939-21 **MAY2022** não tem transporte estendido. A seção 5.10 termina em
TP.DT, e a faixa de bytes de controle diz por extenso que **20 a 31 são
reservados para atribuição pela SAE** - exatamente a faixa que o ETP usa. Até
alguém abrir o documento, a ausência lê-se como lacuna deste projeto em vez de
outro documento.

O que é SAE são os dois **números**: o Digital Annex registra 50944 como ETP.DT
e 51200 como ETP.CM, e os dois foram conferidos lá. O **comportamento** por trás
deles - bytes de controle, janela de deslocamento, tamanho de quatro bytes -
pertence à **ISO 11783-3**, a camada de enlace do ISOBUS.

Isso não é detalhe de papelada. ISOBUS é agrícola, que é exatamente a frota para
a qual este marco existe, e saber que o ETP chega de lá diz a quem está
depurando um trator qual norma abrir.

A diferença estrutural entre os dois transportes é uma só: o número de sequência
de um pacote tem um byte e só conta até 255, então o ETP move uma **janela de
deslocamento** (DPO) ao longo da mensagem e os números contam dentro dela.

Isto começou fora do escopo, e a razão escrita aqui era a raridade. A razão real
era a necessidade de validar os formatos diretamente contra a especificação oficial,
evitando constantes definidas sem verificação normativa. As duas constantes de PGN foram
escritas de memória mesmo assim e **estavam ambas erradas** - e o efeito não foi
um aviso faltando, foi pior: com as PGNs erradas o reassemblador não reconhecia
quadro de ETP nenhum, então todos caíam na decodificação comum. Um ETP.DT
decodificado como mensagem são sete bytes de carga alheia sob um número de
sequência, que é exatamente a falha contra a qual este bloco foi projetado,
escondida atrás de uma funcionalidade que parecia pronta.

O limite: o protocolo permite 117.440.505 bytes, que é número que todo fuzzer
tenta e nenhuma ECU de bancada quer dizer. O teto aqui é o mesmo que o
IsoTpConnection põe na mesma pergunta - uma imagem de firmware - e um anúncio
acima dele é recusado com o tamanho por extenso, em vez de virar uma alocação
escolhida por outra pessoa.

Uma janela que pula para a frente é recusada como buraco, pela mesma regra do
resto do arquivo: é o remetente pulando um trecho que acredita entregue, e
aceitar deixaria uma lacuna que remonta.

--- Address claiming, e por que a ferramenta fica calada --------------------

`0xEE00`, com o NAME de 64 bits: capaz de endereço arbitrário, grupo industrial,
sistema veicular e instância, função e instância, instância de ECU, código de
fabricante, número de identidade. NAME menor ganha a disputa.

**Por padrão o TorqueBus observa e não reivindica.** Uma ferramenta que reivindica
um endereço ao ser ligada pode derrubar do barramento uma ECU real que estava
usando aquele endereço — numa bancada isso é uma tarde perdida, num veículo é
pior. Reivindicar é uma opção explícita, com o NAME digitado por quem sabe o que
está fazendo, e nunca o comportamento de quem só abriu o programa para olhar.

Observando, dá para responder às perguntas que realmente se faz: quem está no
barramento, quem disputou endereço com quem, e quem está transmitindo de um
endereço que nunca reivindicou — este último é o caso interessante, e é
justamente o que uma tabela só de reivindicações esconderia.

--- DM1 e DM2 ---------------------------------------------------------------

Byte de lâmpadas — MIL, parada vermelha, alerta âmbar, proteção — e depois DTCs
de quatro bytes: SPN, FMI, CM, OC.

Duas recusas:

**Um DM1 sem falha ativa manda um DTC zerado.** Isso é a norma, e lê-se como uma
falha de SPN 0 e FMI 0 em qualquer ferramenta que não trate o caso. Aparece como
"sem falhas ativas", que é o que a mensagem significa.

**A conversão do SPN é declarada, não adivinhada.** A J1939-73 seção 5.7.1.14
define **quatro** empacotamentos, e o bit de conversão num código diz só a que
metade da história o remetente pertence: zero é a versão 4, inequívoca; um é
versão 1, 2 **ou** 3, e o fio não diz qual.

Essa é a dificuldade inteira, e não é do tipo que código melhor resolve. Os
mesmos três bytes sob as três leituras dão três números diferentes, e todos
parecem um SPN.

Então: quem conhece o barramento **declara** qual empacotamento antigo as ECUs
dele usam, e só então um código com o bit ligado é montado. Um código com o bit
desligado é versão 4 sob qualquer configuração, porque não é ambíguo. E sem nada
declarado, número nenhum é produzido.

Os quatro bytes crus aparecem ao lado do SPN interpretado, sempre. Quem conhece
a ECU reconhece a conversão certa em um segundo olhando os bytes; ninguém
reconhece nada olhando um número já convertido errado.

Mais de um DTC ativo não cabe em oito bytes, então DM1 real chega por BAM. DM1
depende do transporte, e o transporte é o que tem de estar pronto antes.

--- A forma na tela ---------------------------------------------------------

Um bloco, não quatro. **J1939** em *Transforms*: recebe frames, remonta TP e BAM,
casa por PGN contra o banco carregado, e emite sinais decodificados como o
decodificador DBC já emite — o painel Graph, o dashboard e os scripts continuam
recebendo o que sempre receberam. Endereçamento e diagnóstico são saídas
adicionais do mesmo bloco, porque são leituras do mesmo tráfego: separá-los em
blocos distintos seria pedir para desenhar três vezes o mesmo fio.

Um painel **J1939 Network**: uma linha por endereço, o NAME decodificado em
campos legíveis, fabricante, visto pela primeira e pela última vez, e as disputas
de endereço. É a resposta para "o que está nesse barramento", que é a primeira
pergunta de quem conecta numa máquina que não montou.

--- Testes ------------------------------------------------------------------

`[j1939]` — a desmontagem do identificador, com PDU1 e PDU2 nos dois lados da
fronteira em 240; um BAM com pacote faltando, que tem de ser abandonado e não
remontado; duas sessões concorrentes da mesma origem; um DM1 zerado lido como
"sem falhas"; um SPN sob as três convenções, provando que a escolhida é a que
foi pedida.

Versão: **0.15.0 -> 0.16.0**.

--- O que ficou de fora, e continua de fora ---------------------------------

Uma coisa deliberadamente fora do repositório, e não é uma lacuna de
funcionalidade:

* **Os nomes de fabricante.** As **funções** passaram a vir junto: são
  derivadas do AgIsoStack++, que é MIT e portanto nosso para repassar com o
  aviso de licença anexado. `data/j1939-names-functions.csv` cobre a faixa
  independente de grupo inteira mais as específicas cuja origem nomeia o grupo
  sem ambiguidade, e o arquivo diz no topo quantas ficaram de fora e por quê.

  Os fabricantes não vêm. Aquele registro é o Digital Annex da SAE, produto
  licenciado, e a cópia pública no isobus.net não declara licença nenhuma -
  embarcar 1672 linhas num repositório GPL seria redistribuir banco de dados
  alheio por suposição. `tools/j1939-names.py` monta essa metade na máquina de
  quem quiser, da planilha licenciada ou do registro público, e o `.gitignore`
  impede o resultado de voltar.

  A razão técnica original continua valendo e é independente
  dessa: a lista de fabricantes cresce todo ano, e compilada estaria errada no
  mês seguinte ao lançamento. É por isso que o arquivo do usuário é carregado
  **por cima** do embarcado, e não ao lado: uma tabela mais nova tem de poder
  corrigir a nossa.

E uma questão de procedência, que vale escrever porque muda o que confiar:

* O TP clássico inteiro foi conferido contra a **J1939-21 MAY2022** lida
  diretamente - PGNs, os cinco bytes de controle, o formato de cada mensagem, a
  Tabela 6 e o papel de quem aborta.
* Os quatro empacotamentos do SPN vêm da **J1939-73 AUG2022** seção 5.7.1.14. A
  versão 1 está fixada nos testes pelo exemplo trabalhado da própria norma: o
  DM22 que limpa "SPN 1208, FMI 3" carrega `00 97 03`, e ler aquilo com o bit
  mais significativo primeiro dá 1208 exatamente.
* As duas PGNs de ETP foram conferidas contra o **Digital Annex**.
* O **comportamento** do ETP veio da **ISO 11783-3:2018** seção 5.11, lida
  diretamente. Tudo conferiu - PGNs, os cinco bytes de controle, o campo de
  tamanho de quatro bytes com a faixa 1786 a 117.440.505, o deslocamento de 24
  bits nos bytes 3 a 5 do DPO, e a aritmética que a 5.11.5.5 escreve como
  "número de sequência real = sequência do ETP.DT + deslocamento do
  ETP.CM_DPO".

  Duas coisas a leitura **mudou** em vez de confirmar, e valem mais que as
  dezenas que confirmou:

  - **O ETP tem tabela própria de motivos de abort** (Tabela 9). Nove valores
    significam o mesmo nas duas e aí divergem: o 9 é "tamanho maior que 1785
    bytes" no TP e "pacote de deslocamento inesperado" no ETP, e de 10 a 15 só
    existem no ETP. O código usava a tabela do TP para os dois - o que dá uma
    frase gramatical, plausível, e sobre outro defeito.
  - **Um abort de ETP não carrega papel de quem abortou**: os bytes 3 a 5 são
    reservados pela ISO. Reportar `Unspecified` ali era o certo, e agora é o
    certo com a norma atrás em vez de por cautela.
* Os **tempos** do transporte vieram da J1939-21 direto: Tr 200, Th 500,
  T1 750, T2 1250, T3 1250, T4 1050 ms. Ler a alínea (a) da seção 5.10.2 -
  "um intervalo maior que T1 **após o recebimento do último pacote**" - mostrou
  um defeito: o código aplicava T1 também entre o anúncio e o primeiro pacote,
  onde não há pacote nenhum de onde contar, e relatava timeout numa
  transferência legal que estava esperando um handshake entre duas outras ECUs.
  Agora uma sessão negociada tem T3 até o primeiro pacote e T1 depois; uma
  difusão, que não tem handshake, tem T1 desde o começo.

E uma que é decisão de postura e não lacuna: **reivindicar endereço**. A tabela
observa e não tem como transmitir. Um Request for Address Claimed - que
transformaria "não visto reivindicando" em "nunca reivindicou" - é transmissão,
e transmissão aqui é opt-in explícito.

**v0.16 fechada.**

---

# v0.17 — Plugins

O último item da seção 35, e o que a seção 29 mandava fazer cedo. As duas
costuras já existem e dizem isso no próprio cabeçalho: `CanBackendRegistry`
promete que "o carregador de plugins registrará backends de fora da árvore
através desta mesma chamada", e `NodeCatalog::registerType` diz que um plugin
pode substituir um tipo embutido. Falta o carregador.

E, junto com ele, o que a seção 31 realmente pede: **os dois backends
proprietários saem do binário.** Hoje `torquebus_drivers` linka CANlib e
Qt6::SerialBus diretamente; enquanto isso for verdade, a distribuição GPL carrega
uma dependência de SDK proprietário na própria imagem. Como plugin, o que toca
CANlib é *carregado* e não linkado, e quem não tem o driver instalado
simplesmente não tem aquele plugin.

--- O que atravessa a fronteira, e por que isso é a decisão toda -------------

`ICanBackend` usa `std::function`, `std::span`, `std::string_view` e `Result`.
Isso é **ABI C++**, não C, e ABI C++ não é estável entre compiladores nem entre
versões de biblioteca padrão. Há dois caminhos, e o barato é o errado:

* Reescrever a interface em C puro — estável em qualquer lugar, e transforma
  cada chamada numa tradução à mão, para sempre, do lado de dentro e do lado de
  fora.
* Manter o C++ e **recusar carregar** qualquer plugin que não tenha sido
  construído com o mesmo compilador, o mesmo Qt e a mesma versão de ABI.

O segundo, porque a seção 1 já congelou MSVC x64 e este é um aplicativo de
desktop de plataforma única, não uma biblioteca. A ABI C++ custa uma regra
declarada; a ABI C custaria uma camada de tradução em toda chamada de toda
funcionalidade futura.

A regra fica no formato de uma **chave de build** que o plugin exporta e o
carregador compara byte a byte: versão da ABI do TorqueBus, identificação do
compilador, versão do Qt. Diferiu, o plugin não é carregado, e **o motivo aparece
escrito com o nome do arquivo**. Um plugin que some em silêncio é um backend que
não aparece na lista, e a pessoa vai conferir o cabo.

Só o ponto de entrada é `extern "C"` — um símbolo com nome fixo que devolve o
descritor. C++ decora nomes de forma diferente entre compiladores, e um
carregador que não acha o símbolo não consegue nem dizer por quê.

--- O que um plugin pode fazer ----------------------------------------------

Registrar **backends** e **tipos de bloco**, que são exatamente as duas costuras
que já existem. Nada mais, por enquanto: painéis são Qt e widgets, e uma ABI de
widget é uma promessa muito maior que uma de nó.

Um plugin recebe o registro e o catálogo e chama as mesmas funções que o código
embutido chama. Não há uma segunda API para quem vem de fora — se houvesse, ela
seria a que apodrece, porque ninguém de dentro a usa.

--- O que o carregador recusa fazer -----------------------------------------

**Não carrega de qualquer lugar.** Só do diretório `plugins` ao lado do
executável. Carregar DLL do diretório de trabalho ou do PATH é o jeito clássico
de transformar "abrir um projeto" em "executar o que estava na pasta".

**Um plugin que falha não derruba o aplicativo, e não some.** Falha ao abrir,
símbolo ausente, chave de build diferente, exceção durante o registro: cada um
vira uma linha no painel Output com o nome do arquivo e o motivo. Carregar
plugins é o tipo de coisa cujo erro normalmente aparece como ausência, e
ausência não se diagnostica.

**Um plugin não substitui um embutido por acidente.** Registrar por cima é
permitido de propósito — é como se depura um backend — mas é anunciado, não
silencioso.

--- O que fica embutido -----------------------------------------------------

O backend **virtual**. Um aplicativo com zero plugins tem de abrir, montar um
grafo e rodar uma medição contra o barramento virtual - senão a primeira
experiência de quem baixa depende de ter o SDK certo instalado.

Os blocos também ficam embutidos. A seção 29 lista protocol-j1939 e
protocol-uds como plugins, e isso fica para quando houver um segundo consumidor
da ABI de bloco que prove que ela está certa. Uma ABI publicada sem nenhum
usuário de fora é uma suposição com número de versão.

--- Kvaser e PEAK como plugins ----------------------------------------------

Cada um vira uma DLL construída só quando o SDK está presente — que é a mesma
condição que o CMake já testa hoje. O que muda é que a ausência deixa de ser um
`#if` dentro do binário e passa a ser um arquivo que não existe.

O PEAK linka `Qt6::SerialBus`, então aquele plugin carrega Qt. Isso não é um
problema: o aplicativo já carrega Qt, e a chave de build garante que é o mesmo.

E a tela que a seção 31 pediu: uma lista dos plugins encontrados, dos que foram
recusados e por quê, e para os backends proprietários a diferença entre "não
instalado" e "instalado e falhou ao carregar" — que são dois problemas
diferentes com duas soluções diferentes.

--- Testes ------------------------------------------------------------------

`[plugins]` — a chave de build recusada com um motivo legível; um símbolo de
entrada ausente relatado por nome em vez de ignorado; um plugin que levanta
exceção durante o registro não levando o resto junto; um caminho fora do
diretório de plugins recusado; e a prova que importa de verdade: **o aplicativo
sem plugin nenhum ainda abre, monta um grafo e roda contra o barramento
virtual.**

Versão: **0.16.0 -> 0.17.0**.

--- O que isto prova, e o que não prova -------------------------------------

Que o SDK saiu da imagem é verificável e foi verificado: `dumpbin /DEPENDENTS`
sobre o executável não encontra canlib32 nem Qt SerialBus, e sobre os dois
plugins encontra cada um no seu. Essa era a afirmação da seção 31 e ela agora é
um fato mensurável em vez de uma intenção.

Que a costura de plugin funciona é provado pelos testes do carregador, que
carregam bibliotecas de verdade - inclusive erradas de propósito - e depois
encontram no catálogo o tipo que uma delas registrou.

O que **não** está provado aqui é o caminho com hardware de verdade: os dois
plugins de vendor foram construídos e carregados, mas abrir um canal Kvaser
exige uma Kvaser. Os testes de etiqueta `hardware` continuam sendo a rede para
isso, e continuam fora da execução padrão.

**v0.17 fechada.**

---

# v0.18+ — expansão

Depois

```text
LIN
LDF

ARXML

XCP
CCP
A2L

DoIP
Automotive Ethernet

FlexRay

Vector
IXXAT
TOSUN
J2534
SocketCAN

Linux

Simulink integration
```

---

# 31. Estratégia para DLLs proprietárias

Outra decisão importante:

não colocaremos SDKs proprietários indiscriminadamente dentro do GitHub.

Para Kvaser

```text
TorqueBus plugin
        ↓
CANlib instalado
        ↓
Kvaser driver
```

A Kvaser permite distribuição de determinados componentes, mas manda consultar `EULA.pdf` e `REDISTRIBUTABLES.txt` do SDK para determinar exatamente os arquivos redistribuíveis. ([Kvaser][13])

Portanto inicialmente o instalador dirá

```text
Kvaser CANlib
✓ Installed
```

ou

```text
Kvaser CANlib
✗ Not installed

Install the Kvaser driverCANlib to use this backend.
```

Igualmente

```text
PCAN-Basic
✓ Installed
```

Dessa maneira nossa distribuição GPL fica limpa.

A própria PEAK informa que PCAN-Basic consiste no driver + interface DLL e fornece os headersDLLs para desenvolvimento. ([peak-system.com][14])

---

# 32. Testes sem hardware

Teremos três níveis.

### Unit tests

```text
CanFrame
filters
DBC
signals
ISO-TP
UDS
logging
```

### Virtual integration

```text
VirtualBackend
Kvaser Virtual
```

### Physical integration

```text
PCAN-USB
Kvaser hardware
```

Isso vai permitir executar grande parte do CI sem nenhum adaptador.

---

# 33. CI no GitHub

Cada Pull Request deverá executar

```text
Configure
    ↓
Build Debug
    ↓
Build Release
    ↓
Unit Tests
    ↓
Integration Tests
    ↓
Static Analysis
```

Ferramentas

```text
clang-format
clang-tidy
CTest
AddressSanitizer (onde suportado)
```

E releases

```text
tag v0.5.0
       ↓
GitHub Actions
       ↓
build Windows x64
       ↓
package
       ↓
TorqueBus-Studio-v0.5.0-win64.exe
       ↓
GitHub Release
```

## O pacote também é verificado, e por um motivo concreto

A v0.17 tirou Kvaser e PEAK de dentro do executável, que era o objetivo da
seção 31 — e **ninguém avisou as regras de instalação**. Durante três marcos o
`install()` copiava só o executável. O build ficava verde, os 631 testes
passavam, o zip era produzido, e o único sintoma seria uma lista de interfaces
vazia numa máquina com a interface ligada. Ausência é o modo de falha de
empacotamento, e ausência não se anuncia.

Havia um segundo defeito no mesmo lugar, em sentido oposto: KDDockWidgets e
QtNodes entram por `FetchContent`, então as regras de instalação **deles** são
nossas para executar. Um `cmake --install` comum escrevia 139 cabeçalhos, duas
bibliotecas de importação e quatro arquivos de configuração CMake de SDK alheio
dentro do que deveria ser um aplicativo para usuário final.

As duas coisas se resolvem juntas: tudo que é nosso vai no componente
`torquebus`, e o empacotamento pede esse componente pelo nome. Não é
`EXCLUDE_FROM_ALL` — aquilo exclui alvos do build, não regras de instalação de
uma instalação.

E `tools/check-package.ps1` olha o diretório antes de ele virar zip:

```text
tem de estar             não pode estar
─────────────────────    ─────────────────────────────
TorqueBusStudio.exe      data/j1939-names.csv  (licenciado)
plugins/*-kvaser.dll     include/              (SDK alheio)
plugins/*-peak.dll       lib/cmake/            (idem)
data/…-functions.csv     canlib32.dll          (proprietário)
Qt6Core, Qt6Widgets      PCANBasic.dll         (proprietário)
platforms/qwindows.dll
LICENSE.txt
```

As recusas importam tanto quanto as exigências. A do meio é de licença e não de
tamanho: `install(DIRECTORY data/)` levaria junto a tabela de fabricantes que o
próprio `tools/j1939-names.py` manda escrever num diretório chamado `data` — o
`.gitignore` a mantém fora do repositório, e só isto a mantém fora de um
release. E a LICENSE passou a vir das regras de instalação em vez de uma linha
do workflow: cumprir a GPL é propriedade do que entregamos, não uma etapa que
alguém lembra de manter.

---

# 34. Regras arquiteturais congeladas

Regras definidas no `ARCHITECTURE.md`

1. Nenhuma UI fala diretamente com hardware.
2. Nenhum driver conhece a UI.
3. Core não depende de widgets.
4. CanFrame é independente dos vendors.
5. Um frame recebido não gera um QObject.
6. UI nunca bloqueia esperando hardware.
7. Logging nunca depende da UI.
8. Protocols nunca dependem de hardware específico.
9. Hardware é descoberto por capabilities.
10. Tudo que pode crescer deve possuir APIplugin boundary.

Isso evita que daqui a dois anos tenhamos um monólito impossível de manter.

---

# 35. Nosso objetivo de v1.0

Critérios para o marco TorqueBus Studio 1.0:

```text
✓ Windows 11
✓ Qt 6.11
✓ Kvaser
✓ PEAK

✓ CAN
✓ CAN FD

✓ CAN Trace
✓ CAN Transmit
✓ Filters
✓ Statistics

✓ DBC
✓ Signals

✓ Graph
✓ Logging
✓ Playback

✓ ISO-TP
✓ UDS

✓ Projects
✓ Workspaces

✓ Lua            (Python saiu na v0.13: duas linguagens embarcadas seriam
                  duas APIs a manter e duas sandboxes a auditar)

✓ Dashboards

✓ Plugin system
```

Nesse ponto já não estamos falando de um visualizador CAN.

Estamos falando de uma plataforma automotiva séria.

---

# 36. Depois do 1.0

O roadmap natural seria

```text
TorqueBus Studio 1.x
       │
       ├── CAN  CAN FD
       ├── UDS
       ├── J1939
       ├── Python
       └── hardware ecosystem

TorqueBus Studio 2.x
       │
       ├── LIN
       ├── XCP  CCP
       ├── A2L
       ├── ARXML
       └── simulation

TorqueBus Studio 3.x
       │
       ├── Automotive Ethernet
       ├── DoIP
       ├── SOMEIP
       ├── advanced HIL
       └── distributed testing
```

---

# 37. Primeira tela do projeto

O ponto de partida prioriza a bancada visual antes dos protocolos:

```text
┌─────────────────────────────────────────────────────────────────┐
│ TorqueBus Studio                                                │
├─────────────────────────────────────────────────────────────────┤
│ File Home Hardware Analysis Simulation Diagnostics Tools Help  │
├─────────────────────────────────────────────────────────────────┤
│ ▶ Start   ■ Stop     ● Record    CAN1 ▼       500 kbits       │
├─────────────┬────────────────────────────────────┬──────────────┤
│ Project     │ CAN Trace                          │ Properties   │
│             │                                    │              │
│ Hardware    │ Time Ch Dir ID DLC Data            │ CAN1         │
│ ├ CAN1      │                                    │ Kvaser       │
│ └ CAN2      │                                    │ Virtual 0    │
│             │                                    │              │
│ Databases   │                                    │ 500 kbits   │
│             │                                    │              │
├─────────────┴────────────────────────────────────┴──────────────┤
│ Trace │ Transmit │ Graph │ Statistics │ Diagnostics │ Console │
├─────────────────────────────────────────────────────────────────┤
│ ● CAN1 Ready       ● CAN2 Ready       Frames 0                │
└─────────────────────────────────────────────────────────────────┘
```

Com darklight mode e docking funcionando.

Depois conectaríamos o Kvaser Virtual 0 e 1.

Isso nos dá a sequência perfeita

```text
UI
 ↓
architecture
 ↓
driver API
 ↓
Kvaser Virtual
 ↓
CAN Trace
 ↓
CAN Transmit
 ↓
PCAN
 ↓
DBC
 ↓
restante
```

Essa ordem garante evolução incremental da arquitetura sem necessidade de retrabalho estrutural.

[1]: https://tosunai.us/tsmaster "TSMaster - Advanced Automotive Diagnostics Software | TOSUN Technology"
[2]: https://www.qt.io/blog/qt-6.11.2-released "Qt 6.11.2 Released"
[3]: https://doc.qt.io/qt-6/licensing.html "Qt Licensing | Qt 6.11"
[4]: https://kvaser.com/canlib-webhelp/canlib_8h.htm "canlib.h File Reference"
[5]: https://kvaser.com/canlib-webhelp/section_install_windows.htm "Installing on Windows"
[6]: https://doc.qt.io/qt-6/qtserialbus-peakcan-overview.html "Using PeakCAN Plugin | Qt Serial Bus"
[7]: https://www.peak-system.com/PCAN-Basic.239.0.html "PCAN-Basic | PEAK-System"
[8]: https://www.peak-system.com/PCAN-USB.199.0.html "PCAN-USB | PEAK-System"
[9]: https://github.com/KDAB/KDDockWidgets "KDAB's Dock Widget Framework for Qt"
[10]: https://tosunai.us/tsmaster "TSMaster Analysis Functions | TOSUN Technology"
[11]: https://doc.qt.io/qt-6/qtserialbus-module.html "Qt Serial Bus C++ Classes"
[12]: https://kvaser.com/canlib-webhelp/page_user_guide_device_and_channel.htm "Devices and Channels"
[13]: https://kvaser.com/canlib-webhelp/page_user_guide_build_compiling_linking_windows.htm "Compiling and Linking on Windows"
[14]: https://www.peak-system.com/PCAN-Basic.239.0.html "PCAN-Basic | PEAK-System"
