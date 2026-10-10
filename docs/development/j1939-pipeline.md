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

---

## 2. Pipeline Dataflow: Simulation to Dashboard

The J1939 simulation runs directly within the **TorqueBus Visual Pipeline**:

```
+----------------------------------------------------------------------------+
|                            PIPELINE DATAFLOW                               |
|                                                                            |
|  [Simulated ECUs (Lua)]                                                    |
|  ├── EngineECU.lua         ──────\                                         |
|  ├── AftertreatmentECU.lua ───────+──> [Virtual CAN 1] ──> [J1939 Decoder]  |
|  ├── BodyController.lua    ───────|                             │          |
|  ├── BrakeSystem.lua       ───────/                             ▼          |
|  └── SwitchPanel.lua                                  [Dashboard Cluster]  |
|                                                       ├── Digital Speed    |
|  [TinyML Virtual ECU]      ──────────> [Virtual CAN 2] ├── Analog Tacho     |
|                                                       ├── Fuel & Temp      |
|                                                       └── Warning Lamps    |
+----------------------------------------------------------------------------+
```

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
