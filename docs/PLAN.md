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

# 28. Python

Depois

```text
Python Console
```

API conceitualmente

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

**ETP fica fora da v0.16.** Acima de 1785 bytes é outro par de PGNs e outra
máquina de estados, e é raro fora de transferência de arquivo e calibração.
Ficar fora é uma decisão, não um esquecimento: o bloco **reconhece** um
`TP.CM/ETP.CM` que não trata e diz que não trata, em vez de ignorar em silêncio.

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

**A conversão do SPN é declarada, não adivinhada.** O campo foi codificado de
três formas ao longo da vida da norma, e um fabricante que usa a antiga produz
números plausíveis e errados sob a leitura nova — o pior tipo de erro, porque
o resultado parece um SPN. A leitura é uma configuração do bloco, e os quatro
bytes crus aparecem ao lado do SPN interpretado, sempre. Quem conhece a ECU
reconhece a conversão certa em um segundo olhando os bytes; ninguém reconhece
nada olhando um número já convertido errado.

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

Três lacunas deliberadas, cada uma porque a alternativa era embarcar um número
ou um rótulo que pareceria certo:

* **Os empacotamentos antigos do SPN.** Um código que declara o outro
  empacotamento não recebe SPN nenhum; os quatro bytes crus ficam no lugar.
* **ETP**, reconhecido e recusado em voz alta.
* **Tabelas de função e de fabricante.** Funções acima de 127 dependem do grupo
  industrial e do sistema veicular; a lista de fabricantes tem uns dois mil
  itens e cresce todo ano. Pertencem a um arquivo de dados corrigível sem
  recompilar. O grupo industrial, que são oito valores fixos, está nomeado.

E uma que é decisão de postura e não lacuna: **reivindicar endereço**. A tabela
observa e não tem como transmitir. Um Request for Address Claimed - que
transformaria "não visto reivindicando" em "nunca reivindicou" - é transmissão,
e transmissão aqui é opt-in explícito.

**v0.16 fechada.**

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
