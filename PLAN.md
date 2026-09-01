# TorqueBus Studio

TorqueBus Studio — Open Automotive Network & Diagnostics Workbench

---

## Revisão — pipeline visual, Lua e ISOBUS

> Esta revisão altera decisões que este documento havia congelado. O plano
> original continua abaixo, intacto; o que mudou está aqui, com o porquê.

Depois de construir o v0.1 ao v0.3, estudamos o **CANdevStudio** como
referência arquitetural e concluímos que ele resolveu bem um problema que este
plano não previa: o **pipeline CAN visual**. Blocos e fios, com fonte,
transformação e consumo explícitos na tela.

Isso é mais do que uma tela nova. É o que separa esta ferramenta de uma
reimplementação do TSMaster: um workbench fixo responde às perguntas que nós
antecipamos, um pipeline responde às que o usuário inventar.

**O que muda:**

| Decisão original | Revisão | Motivo |
|---|---|---|
| Painéis fixos ligados a fontes fixas | Grafo de dataflow tipado; os painéis são nós dele | Um caminho de dados só (regra #11 do ARCHITECTURE.md) |
| Scripting em Python (seção 28, v0.13) | **Lua 5.5.0 em tudo.** Python sai do plano | Lua embarca em ~200 KB, uma VM por ECU sai barato, é determinística e não tem GIL. E já está provada no `cansim`, com ciclo de vida de node e 20 scripts funcionando. Uma linguagem embarcada só significa um binding para manter, uma API para documentar e uma linguagem para o usuário aprender |
| Simulação de ECU como marco tardio (v0.15) | Blocos de ECU em Lua sobem junto com o canvas | O `cansim` já resolveu o contrato: `on_enable`, `on_disable`, `on_timer`, `on_message`, mais `emit` e `set_timer` |
| J1939 no v0.16 | J1939 **e ISOBUS** (ISO 11783) como alvo de primeira classe | É o domínio real de uso — maquinário agrícola — e nenhuma ferramenta aberta faz ISOBUS decentemente |

**O que não muda:** as dez regras arquiteturais originais, o `CanFrame`
independente de vendor, o `ICanBackend`, a meta de 100k frames/s sem perda, a
licença GPLv3 e a decisão de não copiar identidade visual proprietária.

**O que herdamos de onde:**

- **KDDockWidgets** dá o layout profissional tipo TSMaster.
- **QtNodes 3.x** dá o canvas de blocos e fios tipo CANdevStudio.
- **cansim** (MIT, nosso) dá o motor Lua, o contrato de node e os scripts.

Nada de código do CANdevStudio é portado: ele está em Qt5, C++17 e QtNodes 2.x,
gerações de API incompatíveis com a nossa. A dívida é intelectual, e está
reconhecida.

O detalhamento técnico — portas tipadas, fluxo em lote, contrato de nó, grafo
padrão implícito — está em `docs/ARCHITECTURE.md`, seção 3b.

---

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
 Docking             KDDockWidgets 2.x               
 Canvas de nós       QtNodes 3.x (revisão)           
 Gráficos            Qt Graphs + renderer próprio    
 CAN abstraction     Core próprio                    
 PEAK                Qt PeakCAN  PCAN-Basic 5.x     
 Kvaser              CANlib + adapter próprio        
 SocketCAN           backend próprio (Linux)         
 Scripting           Lua 5.5.0 embarcado (revisão)   
 Build               CMake + Ninja                   
 Compilador Windows  MSVC x64                        
 Configuração        JSON                                
 Dados internos      SQLite                              
 Testes              Catch2 ou GoogleTest                
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

Eu começaria desta forma

```text
torquebus-studio
│
├── .github
│   ├── workflows
│   ├── ISSUE_TEMPLATE
│   └── PULL_REQUEST_TEMPLATE.md
│
├── cmake
│
├── docs
│   ├── architecture
│   ├── development
│   ├── protocols
│   └── screenshots
│
├── resources
│   ├── icons
│   ├── themes
│   └── fonts
│
├── src
│   │
│   ├── app
│   │   ├── Main.cpp
│   │   ├── Application.cpp
│   │   └── ApplicationContext.cpp
│   │
│   ├── core
│   │   ├── can
│   │   ├── logging
│   │   ├── database
│   │   ├── diagnostics
│   │   ├── scripting
│   │   └── project
│   │
│   ├── drivers
│   │   ├── api
│   │   ├── kvaser
│   │   ├── peak
│   │   └── virtual
│   │
│   ├── services
│   │
│   ├── ui
│   │   ├── mainwindow
│   │   ├── trace
│   │   ├── transmit
│   │   ├── graph
│   │   ├── hardware
│   │   ├── project
│   │   ├── properties
│   │   └── diagnostics
│   │
│   └── plugins
│
├── tests
│   ├── unit
│   ├── integration
│   └── hardware
│
├── examples
│
├── third_party
│
├── CMakeLists.txt
├── LICENSE
├── README.md
├── CONTRIBUTING.md
└── CODE_OF_CONDUCT.md
```

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

> **Revisado a partir do v0.4.** O grafo de pipeline entra cedo, não tarde:
> ele é o caminho de dados, então quanto mais módulos forem escritos antes
> dele, mais coisa precisa ser reconectada depois. Os marcos v0.1 a v0.3 estão
> concluídos e não mudaram.
>
> | Marco | Original | Revisado |
> |---|---|---|
> | v0.4 | CAN Trace | **Pipeline graph** (executor + portas tipadas + grafo padrão implícito) |
> | v0.5 | PCAN | **CAN Trace como nó** do grafo |
> | v0.6 | Transmit | **Canvas QtNodes** — o grafo fica visível e editável |
> | v0.7 | DBC | **Blocos de ECU em Lua** — o `cansim` visual |
> | v0.8 | Graph + Statistics | DBC Decoder como nó, e daí em diante como no plano original |
> | — | J1939 no v0.16 | **J1939 + ISOBUS** promovidos, logo após DBC |
>
> A ordem completa revisada:
>
> ```text
> v0.4  Pipeline graph          executor, portas tipadas, grafo implícito
> v0.5  CAN Trace               primeiro nó consumidor de verdade
> v0.7  ECU blocks (Lua)        motor Lua + ciclo de vida do cansim
> v0.6  Canvas QtNodes          blocos e fios na tela
> v0.8  DBC Decoder             nó de transformação, porta Signals
> v0.9  PCAN + SocketCAN        mais fontes, nenhuma mudança acima do driver
> v0.10 Transmit + Graph        nós produtores e consumidores de sinais
> v0.11 J1939 + ISOBUS          porta Pgns, transporte, address claiming
> v0.12 Logger + Playback       nós de gravação e de fonte a partir de arquivo
> v0.13 Projects + Workspaces   o grafo faz parte do .tbsproj
> v0.14 ISO-TP + UDS            porta Events
> v0.15 Dashboard Designer      QML ligado a portas do grafo
> v0.16 Lua automation          runner headless, testes e relatórios
> v0.17+ expansão               LIN, XCP/CCP, A2L, ARXML, DoIP, FlexRay
> ```
>
> **v0.7 antes do v0.6, invertendo o que está escrito acima.** O motivo é
> prático, não arquitetural: trabalho de core eu compilo e meço; trabalho de Qt
> eu só consigo deduzir, e isso já custou várias rodadas de ida e volta nesta
> sessão. O motor Lua ficou pronto e medido antes de existir uma tela para
> desenhá-lo — o canvas passa a ser uma forma de editar um grafo que já roda,
> em vez de ser o único jeito de saber se ele roda.
>
> **Concluído no v0.7:** Lua 5.5.0 vendorizado e compilado com o projeto;
> `LuaRuntime` (sandbox sem `os`/`io`/`require`, erros como `Result` com
> arquivo e linha); `LuaEcuNode` com o ciclo de vida do cansim e os quatro
> bindings; `ChannelSinkNode` fechando o laço `[Lua ECU] → [CAN 1]`;
> `CanEngine::setGraphBuilder`, sem o qual o `start()` destruía os nós do
> usuário a cada partida; 20 testes; e dois scripts do cansim portados em
> `examples/scripts/`. O contrato está em `docs/development/scripting.md`.
>
> Custo medido, 600.000 frames por script, uma ECU no grafo:
>
> | Script | Por frame | Frames/s |
> |---|---|---|
> | `on_message` vazio | 83 ns | 12,0 M |
> | Lê o payload com `string.byte` | 132 ns | 7,6 M |
> | Lê, desempacota e emite um frame | 262 ns | 3,8 M |
>
> Um barramento de 500 kbit/s saturado carrega ~4.000 frames/s. Uma ECU típica
> custa ~260 ns por frame, ou seja, dá para ter dezenas delas com o orçamento
> praticamente intacto.
>
> O detalhe de cada marco original continua abaixo e ainda vale como
> especificação de conteúdo — o que mudou foi a ordem e o fato de cada painel
> passar a ser um nó em vez de um consumidor fixo.

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

# v0.13 — Python

 embedded Python;
 TorqueBus API;
 editor;
 console;
 events;
 automation.

---

# v0.14 — Dashboard Designer

Aqui entra bastante QML

```text
Gauge
Numeric display
LED
Button
Switch
Slider
Graph
Image
Text
Knob
```

Associáveis a

```text
CAN signal
system variable
Python variable
UDS result
```

---

# v0.15 — Simulation

 ECU node simulation;
 periodic messages;
 signal manipulation;
 remaining bus simulation;
 message generators;
 script-driven simulation.

---

# v0.16 — J1939

 PGN;
 SPN;
 address claiming;
 BAM;
 TP;
 DM1;
 DM2;
 network view.

Essa parte será especialmente útil para máquinas agrícolas e veículos pesados.

---

# v0.17+ — expansão

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

✓ Python

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
