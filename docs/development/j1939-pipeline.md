# SAE J1939 Protocol & Instrument Cluster Pipeline

TorqueBus includes native support for **SAE J1939 / ISOBUS heavy-duty vehicle networks**, combined with a visual dataflow pipeline and a hardware-accelerated **QML Instrument Cluster Dashboard**.

---

## 1. SAE J1939 Architecture in TorqueBus

SAE J1939 uses 29-bit extended CAN identifiers divided into priority, Parameter Group Number (PGN), and Source Address (SA):

```
Bits: 28..26  25   24   23..16    15..8      7..0
      [Priority][R] [DP] [PF/PDU1] [PS/PDU2] [Source Address]
                          <------ PGN ------>
```

### Supported Parameter Group Numbers (PGNs)

| PGN | Hex | Name | Cycle | Key Signals |
|---|---|---|---|---|
| **61444** | `0xF004` | **EEC1** (Electronic Engine Controller 1) | 10 ms | Engine Speed (RPM), Engine Demand Torque, Driver's Demand Torque |
| **61443** | `0xF003` | **EEC2** (Electronic Engine Controller 2) | 50 ms | Accelerator Pedal Position, Engine Percent Load at Current Speed |
| **65262** | `0xFEEE` | **ET1** (Engine Temperature 1) | 1000 ms | Engine Coolant Temperature, Engine Oil Temperature, Fuel Temperature |
| **65265** | `0xFEF1` | **CCVS1** (Cruise Control / Vehicle Speed) | 100 ms | Wheel-Based Vehicle Speed, Cruise Control Active, Brake Switch |
| **65269** | `0xFEF5` | **AMB** (Ambient Conditions) | 1000 ms | Ambient Air Temperature, Barometric Pressure |
| **65266** | `0xFEF2` | **LFE1** (Fuel Economy) | 100 ms | Fuel Rate (L/h), Instantaneous Fuel Economy (km/L) |
| **65248** | `0xFEE0` | **VD** (Vehicle Distance) | 100 ms | Total Vehicle Distance (Odometer), Trip Distance |
| **65226** | `0xFECA` | **DM1** (Active Diagnostic Trouble Codes) | 1000 ms | SPN (Suspect Parameter Number), FMI (Failure Mode Identifier), Lamp Status |

All codecs are located in `src/core/j1939/` and operate with zero dynamic allocations on hot data paths.

### Standards conformance notes

The decoder and the multi-packet transport follow these rules, each covered by a regression test:

* **Special values (SAE J1939-71 §5.1.4):** the top of every signal's range is reserved. For 8/16/24/32-bit fields `0xFB..` is reserved, `0xFE..` is *error* and `0xFF..` is *not available*; for 3-bit fields `6` is error and `7` is not available; for 64-bit fields anything `>= 0xFE00000000000000` is special. Such raw values decode to `NaN` rather than to a plausible number. Unsigned 64-bit signals are scaled without passing through a signed integer.
* **Transport (SAE J1939-21 §5.10, ISO 11783-3 §5.11):** `TP.DT`/`ETP.DT` sequence numbers are `1..255` (`0` abandons the session); `BAM` must be broadcast and `RTS` point-to-point; `ETP.CM_DPO` windows may not exceed the announced packet count; an `Abort` or `CTS` sent by the *responder* (addresses reversed) is matched to its session, and a `CTS` refreshes the `T3` handshake timeout. Sessions are keyed by source, destination **and CAN channel**.
* **DM1 broadcasts:** a steady 1 Hz `DM1` with unchanged faults does not rebuild the J1939 Network panel; expanded ECU rows survive rebuilds and ECUs with active faults expand automatically.
* **What the bus is reporting now:** the block works out, on every pass, the trouble codes of every `DM1` that is still being repeated - one not repeated for three seconds of bus time is an ECU that has gone quiet, and takes its codes and its lamp with it - and hands the list over with the rest of its view of the bus. It is empty after Stop and after the bus has been silent for three seconds, while the J1939 Network panel keeps what each ECU last said and labels it *last reported* - a healthy ECU's *none* included, which is why the block also hands over which ECUs are still repeating a DM1: an ECU with nothing wrong has no code for the list to name. Every ECU of the example - the six Lua ones and the TinyML block - also sends an Address Claimed when it starts, so the panel shows them *claimed* and the fault list names them. The Dashboard's **Faults** widget lists it (see [the Dashboard](dashboard.md#the-fault-list)).
* **More than one bus:** an address is a seat on one bus, so everything a J1939 block learns from the traffic - who claimed what, what each ECU's `DM1` says, the transfers in flight, what the bus is reporting, how long it has been silent - it keeps for each CAN channel the frames arrive on. A block that reads two channels (an input takes one wire, so a block gets two buses from a recording of two buses replayed into it, or from a **Frame Merge** block that joins a CAN Bus on each channel - see below) reads two buses: the ECU at address 0 on CAN 1 and the one at 0 on CAN 2 are two ECUs, a transfer is finished only by the packets of its own bus, and each bus is aged by its own clock - one that falls silent while the other talks is silent. Every J1939 block of a graph hands its views over to the same place, under the channel they are of, and a publication replaces the view of that bus and of no other - so with two buses the J1939 Network panel lists each bus's ECUs under a heading of its own (*CAN 1*, *CAN 2*), the fault list says the bus on each row (`CAN 2 · SA 0 · Body Control`), and the lamps of the cluster are lit while any bus asks for them. Two blocks on one channel read one bus. With one bus nothing changes on screen.
* **Joining the wires of several buses (the Frame Merge block):** an input of the graph takes exactly one wire, because merging two streams needs a policy; the **Frame Merge** block (`can.merge`, under *Transforms*) is that policy for frames. It has four inputs and one output: drop it on one CAN Bus after another (each drop is wired to the next free input), draw its output to the J1939 block, and the block reads every bus - the ECUs listed under *CAN 1*, *CAN 2*. Frames come out in the order of their timestamps (`CanFrame.h` says those of one measurement share an epoch - see the end of this entry), a tie goes to the lower input, the order of one wire is never changed, and every frame keeps the channel it was received on and everything else it says. A pass carries at most one batch, which is all anything downstream is sized for; what does not fit waits for the next pass (up to what one pass of every wire at its fullest leaves), and beyond that the latest frames are dropped. Both are counted - *Frames held over* and *Frames dropped* in Statistics, next to *Frames passed*. What it cannot do is order what has not arrived: a frame that reaches the block a pass after one it happened before is a pass late - a pass is a few milliseconds of bus. And it trusts the channels' timestamps to be on one clock: the virtual backend's are (one process-wide clock); the Kvaser and PEAK backends stamp with their own (the adapter's ticks, or the driver's hardware time, and the machine's clock for what it transmits itself), and those have not been checked against each other - across two different adapters the order of the merged stream is only as good as their clocks, while each bus's own order is kept whatever the clocks say, and the J1939 block, which keeps its state for each bus on its own, does not depend on the order *between* buses.
* **Asking a bus, or an ECU on it, for something (SAE J1939-21, -81):** an ECU claims its address once, at power-up, so a measurement started on a machine that was already running sees no claims and lists every ECU as *not seen claiming*; and most of what an ECU knows - its previously active faults (DM2), what it and its software are called, the vehicle's number - it sends only when asked. The J1939 Network panel asks, and only when asked: **Request claims** sends a Request for Address Claimed (PGN 59904 asking for PGN 60928, to the global address, from the null address 254) on every channel; **Request PGN...** opens a dialog for any other group - from the list of the ones worth asking for by name, or typed as `65227` or `0xFECB`; of every ECU or of one; on every channel or on one - and the menu of a right click on an ECU's row (*Ask the ECU at 33*) or on a bus's heading (*Ask every ECU on CAN 2*) has the same groups and *Another group...*. All of them are available while a measurement runs. The J1939 block reads a bus and does not transmit on it, so nothing goes out unless a button or a menu entry is pressed, and 254 is not listed as an ECU. Every request goes out from the null address, 254, because the tool has no address of its own - and J1939-81 gives the null address to the Request for Address Claimed and the *cannot claim* message, so a conformant ECU on a real bus may ignore a request for anything else that comes from it. The simulated ECUs answer; whether the ones on a real bus do has not been tried (see [validation](validation.md)), and the remedy - a tool that claims an address of its own, and so becomes a node of the bus that defends its claim and answers a contest - is not built. The dialog says in words why a group cannot be asked for - not a number, more than the 18 bits of a PGN, or one for a single ECU whose last byte (its address on the wire, which is not part of the group) is not zero - and asks before DM3 and DM11, which tell an ECU to clear its fault codes.
* **What an ECU does with a request (SAE J1939-21):** an ECU that has sent the group answers by sending the latest copy of it again - the group as it sent it, whether it was asked of everyone or of this ECU by its own address. The Lua ECUs answer for any group they have sent - the latest copy for each source address, up to 256 groups, with nothing in the script; the example wires the bus to all six so that they can hear the request - and the TinyML ECU for its claim, its `DM1` and its telemetry. Asked of everyone, an ECU with nothing to say says nothing: most ECUs have nothing to say to most requests. Asked by its own address for a group it has not sent, it sends the refusal J1939-21 has an ECU send - an Acknowledgement (PGN 59392, control byte 1, *negative*) naming the group and the address of the one who asked, to that address, or to everyone when the request came from the null address, where nobody is to be told. The panel keeps the last answer an ECU gave on its row (*last answer: PGN 65227 (0xFECB) not supported*). A Lua script that wants to answer in its own way defines `on_request` (see [Scripting](scripting.md#answering-requests)); one that returns `false` for a request has that request left alone by the node.
* **The pause before an answer (SAE J1939-81, RTxD):** ECUs asked at one instant do not answer at one instant. An answer to a request to everyone for the claims of the bus, and a *cannot claim*, wait a time that comes from the ECU's NAME - the top byte of the NAME multiplied by a constant that spreads NAMEs one serial number apart, times 0.6 ms: 0 to 153 ms. The standard calls the delay pseudo-random and leaves where it comes from to the ECU; here it comes from the NAME so that two ECUs differ and one measurement is the same as the next, which a simulation that is to be repeated needs. A Lua ECU waits it on the machine's clock; the TinyML ECU, which has no clock of its own, waits it in bus time, and its answer goes out with the first frame of the bus that is as late as the answer is due (so an ECU on a bus with no traffic at all has no one to wake it). What is not an answer is not delayed: the claim at power-up, a defending re-claim and a move to another address.
* **Address claiming of the TinyML ECU (SAE J1939-81):** it answers a Request for Address Claimed with its claim; another ECU claiming its address with a lower NAME takes it, and the block either sends *cannot claim* from the null address and falls silent for the rest of the measurement (the default) or, when it is arbitrary-address capable, moves to the lowest free address from 128 to 247 under the same NAME; one with a higher NAME is told again that the address is taken. Its function and serial number are settings (see [TinyML](tinyml.md)).

---


## 2. Pipeline Dataflow: Simulation to Dashboard

The J1939 simulation runs directly within the **TorqueBus Visual Pipeline**:

```
   EngineECU  Aftertreatment  BodyController  BrakeSystem  SwitchPanel  ClusterCore  TinyML ECU
   (Lua)      (Lua)           (Lua)           (Lua)        (Lua)        (Lua)        (int8 model)
      |           |               |               |            |            |            |
 CAN 1 =====================================================================================  CAN_H
 (virtual)  ===================================================================================  CAN_L
                   |                |
             [J1939 Decoder]   [Frame Filter: PGN 65265]
                   |
             [Signal Plot]  ------>  [Dashboard Cluster]
                                      |-- Digital Speed
                                      |-- Analog Tacho
                                      |-- Fuel & Temp
                                      '-- Warning Lamps
```

Every ECU is one drop off the **CAN Bus** block - the two wires - and the J1939 decoder and the filter are two
more. There is no CAN Channel and no CAN Transmit block per ECU: a wire to the bus *is* the transmit, and a wire
from it *is* the channel (see [The Pipeline Canvas & the CAN Bus](pipeline-canvas.md)).

### The Shipped Example Project
A complete, turnkey J1939 truck simulation is provided in:
`examples/projects/j1939-vehicle.tbsproj`

To run it:
1. Open **TorqueBus Studio**.
2. Select **File → Open Project...** and choose `j1939-vehicle.tbsproj`.
3. Press **Start** on the toolbar.
4. Switch to the **Dashboard** panel to watch the digital speedometer, tachometer, temperature gauges, and telltale lamps react in real time. The **Active faults** list under the trends shows the `DM1` trouble codes of the whole bus: set `ecu_engine` to `low_oil_pressure` and a red row, `SPN 100 / FMI 1`, appears under the lit stop lamp.

---

## 3. Simulated Vehicle ECUs (Lua)

The vehicle simulation is driven by modular Lua scripts in `examples/scripts/`:

* **`EngineECU.lua`**: Simulates the diesel powertrain, cycling vehicle speed (0 to 110 km/h) and engine RPM (650 to 2400 RPM) across gears with realistic throttle response.
* **`AftertreatmentECU.lua`**: Emits DPF soot loading, SCR catalyst temperatures, and DEF (AdBlue) tank level.
* **`BodyController.lua`**: Emits exterior lighting, door status, and turn signals.
* **`BrakeSystem.lua`**: Emits ABS status, retarder torque request, and air brake reservoir pressures.
* **`SwitchPanel.lua`**: Simulates driver controls (cruise control switches, hazard lights).
* **`ClusterCore.lua`**: Orchestrates cluster telltales and converts vehicle signals into dashboard telemetry.

### Scenario Injection

Each ECU supports test scenarios via script parameters to validate error handling and safety telltales:

```lua
-- In EngineECU.lua:
if scenario == "overheating" then
    coolant_temp = 115.0 -- Triggers red engine warning lamp on cluster
elseif scenario == "low_oil_pressure" then
    oil_pressure_kpa = 80.0 -- Below 120 kPa triggers critical warning
end
```

---

## 4. Instrument Cluster Implementation (QML)

The Instrument Cluster (`src/ui/dashboard/cluster/`) is rendered using **Qt Quick / QML** with hardware acceleration:

* **Tachometer & Speedometer**: Smooth SVG needle sweeps with configurable redlines and easing animations.
* **Central Information Display (CID)**: Digital readout of gear selection, odometer, instant fuel economy, and trip meter.
* **Secondary Gauges**: Engine coolant temperature, engine oil pressure, DEF level, and fuel tank capacity.
* **Automotive Telltale Icons**: ISO 2575 compliant indicators (High Beam, Check Engine, ABS Fault, Air Pressure Low, Oil Pressure Warning, Stop Engine).

All QML bindings are tested by the automated test suite in `tests/ui/DashboardClusterTests.cpp`.
