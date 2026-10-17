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
4. Switch to the **Dashboard** panel to watch the digital speedometer, tachometer, temperature gauges, and telltale lamps react in real time.

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
