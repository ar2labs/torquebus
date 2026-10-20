# TinyML Virtual ECU (`tinyml.ecu`) & Animated Canvas Simulation

TorqueBus Studio includes a first-class **TinyML Virtual ECU** pipeline block (`tinyml.ecu`) that executes embedded neural network inference over live CAN traffic in Software-in-the-Loop (SIL) or Hardware-in-the-Loop (HIL) topologies—requiring **no physical microcontroller or CAN adapter** when paired with the built-in `VirtualCanBackend`.

---

## 1. Embedded Architecture (`src/core/tinyml/`)

Physical automotive ECUs running TinyML (such as ARM Cortex-M4/M7, NXP S32K, or Infineon AURIX TriCore) operate under strict deterministic memory and latency constraints. `torquebus_core` mirrors the TensorFlow Lite for Microcontrollers (TFLite Micro) execution model:

1. **Static Tensor Arena (`TinyMlArena`)**:
   - All intermediate activation buffers and Softmax scratch tensors are carved out of a single pre-allocated, 16-byte-aligned contiguous byte buffer (`TinyMlArena`, default `4096 B`) during `prepare()`.
   - At runtime (`TinyMlEcuNode::process()`), the bump allocator resets in $O(1)$ (`arena.resetPass()`) and performs **zero heap allocations** (complying with Architecture Rule #12).
   - The exact SRAM high-water mark (`Arena bytes used`) is reported to the Statistics panel and rendered live on the Canvas neural network card.
2. **Symmetric `int8` Quantized Multi-Layer Perceptron (`TinyMlModel`)**:
   - Weights are stored as signed 8-bit integers (`int8_t`, $[-128, 127]$) with a per-layer floating-point dequantization scale (`weightScale`), reducing ROM/Flash weight footprint by $4\times$ compared to `float32`.
   - Default topology (`automotive_powertrain_int8_v1`):
     $$\text{6 Inputs} \xrightarrow{\text{int8 Dense + ReLU}} 10 \xrightarrow{\text{int8 Dense + Tanh}} 8 \xrightarrow{\text{int8 Dense + Softmax/Sigmoid}} 7\text{ Outputs}$$

---

## 2. Inputs, Outputs, and CAN Frame Encoding

### Input Features (Extracted from CAN Bus Traffic)
| Index | Feature | Source |
|---|---|---|
| `0` | Vehicle Speed ($v$, $\text{km/h}$) | `speedCanId` (default `0x101` / `257`, 16-bit LE, factor `0.1`) |
| `1` | Longitudinal Acceleration ($\frac{\Delta v}{\Delta t}$, $\text{km/h/s}$) | EWMA derivative of `speedCanId` frame timestamps |
| `2` | Engine Coolant Temp ($T$, $^\circ\text{C}$) | `tempCanId` (default `0x102` / `258`, 8-bit, offset `-40`) |
| `3` | Thermal Delta ($T - T_{\text{nominal}}(v)$, $^\circ\text{C}$) | Excess temperature above nominal speed-load curve |
| `4` | Timing Jitter Ratio ($|\Delta t - \overline{\Delta t}| / \overline{\Delta t}$) | Inter-frame arrival timing deviation |
| `5` | Bus / Fault Stress Factor | Out-of-profile payload or interactive fault injection |

### Dual Output Ports on `tinyml.ecu`
- **Port 0 (`frames`, `PortType::Frames`)**: Emits an 8-byte CAN telemetry frame on `outputCanId` (default `0x105` / `261`, defined as `TinyML_Telemetry` in `examples/databases/vehicle.dbc`):
  - **Byte 0 (`RegimeClass`)**: `0` = `Idle`, `1` = `Cruise`, `2` = `HighLoad`, `3` = `ThermalStress`, `4` = `Anomaly`
  - **Byte 1 (`Confidence`)**: Classification confidence (`0..100 %`)
  - **Bytes 2..3 (`AnomalyScore`)**: 16-bit unsigned LE, factor `0.1 %` (`0.0 .. 100.0 %`)
  - **Bytes 4..5 (`ThermalHealth`)**: Virtual sensor estimate, 16-bit unsigned LE, factor `0.1 %` (`0.0 .. 100.0 %`)
  - **Bytes 6..7 (`InferenceTimeUs`)**: Neural network execution latency in microseconds (`uint16` LE)
- **Port 1 (`signals`, `PortType::Signals`)**: Publishes five `DecodedSignal` streams (`RegimeClass`, `Confidence`, `AnomalyScore`, `ThermalHealth`, `InferenceTimeUs`) directly to any connected `Signal Plot` (`signal.plot`) block.

### J1939 mode

With the `j1939` parameter set - or an `outputCanId` that does not fit 11 bits - the block reads a J1939 bus
instead: it matches by **PGN**, so the engine may answer from any address.

| PGN | Message | Used for |
|---|---|---|
| `0xFEF1` | CCVS1 | wheel-based vehicle speed (SPN 84, bytes 2-3, 1/256 km/h) |
| `0xFEEE` | ET1 | engine coolant temperature (SPN 110, byte 1, offset -40) |
| `0xF003` | EEC2 | engine load (SPN 92); above 100 % the engine is asked for more than it has, which is stress |
| `0xFEF5` | AMB | ambient temperature (SPN 171); a hot day lets the coolant run hotter before it means anything |

A value that is *error* or *not available* in one of those is not a reading: it adds **bus stress**
(0.4 each), and every valid speed frame takes 15 % of it off, so a sensor that comes back stops counting.

**The coolant is calibrated.** The built-in model was set against a vehicle whose coolant idles at 70 °C and a
heavy-duty diesel's thermostat holds 88. Fed that as it was, the model called every healthy truck a thermal
emergency from the first frame - 100 % anomaly, 0 % health - and, with the predictive trouble code below,
lit a red stop lamp for it. A J1939 coolant temperature is moved down by 18 °C on its way in, which puts a
healthy engine where the model's healthy vehicle is and leaves every degree above that meaning what it meant.

An `outputCanId` that is not a CAN identifier - more than 29 bits, which includes the `0x80000000` a
.dbc adds to mark an identifier extended - is refused before Start, with the block named. It used to be
used as it was, and every telemetry frame was a failed transmit that nothing reported.

**Output** is a Proprietary B frame, PGN `0xFF00` (`0x18FF0080` from address `0x80` unless `outputCanId` says
otherwise), described by `TinyML_Proprietary` in `examples/databases/j1939.dbc`:

| Byte | Signal | |
|---|---|---|
| 1 | `ThermalHealth` | 0.4 %/bit |
| 2 | `AnomalyScore` | 0.4 %/bit |
| 3 | `Confidence` | 0.4 %/bit |
| 4 | `RiskLevel` (bits 1-3), `ModelStatus` (4-5), `RegimeClass` (6-8) | the regime is what the vehicle is doing; the risk is how worried the model is about it |
| 5 | `MessageCounter` | 0 to 250: above that a byte means error on a J1939 bus |
| 6 | `InputAge` | 0.1 s/bit |
| 7-8 | `InferenceTimeUs` | held to 64255 |

**It claims its address.** Like any J1939 ECU, the block claims the address it speaks from before it says
anything else: the first time it hears the bus in a measurement it sends an Address Claimed (PGN `0xEE00`,
`0x18EEFF80` from address `0x80`) under a NAME of its own - on-highway, function 33, *System Monitor*, with the
address as its serial number - and only then the telemetry and the predictive DM1, from that address. That is
what lets the J1939 Network panel show it as *claimed*, and the Dashboard's fault list call a code of its own
`SA 128 · System Monitor` and not only `SA 128`. It claims again at every Start (the ECU is switched on
again), takes its own claim coming back off the bus for the bus's and does not analyse it, and a block that
is not on a J1939 bus - the 11-bit example vehicle - has no address to claim.

**It keeps the rules of a claim (J1939-81).** A Request for Address Claimed - to every ECU, or to this one -
is answered with its claim, which is how a tool that joins a running bus learns who is on it (the J1939
Network panel's **Request claims** button sends one). Another ECU claiming the same address with a **lower**
NAME takes it, and what this block does about it depends on whether it can move:

* **It cannot, by default.** It does what such an ECU does - sends *cannot claim* (an Address Claimed from the
  null address, 254, with its NAME), and puts nothing more on the bus for the rest of the measurement,
  answering a request with *cannot claim* again. The model goes on running and its signals and variables
  still reach the Dashboard; the block's *J1939 address lost* statistic says 1, and the J1939 Network panel
  lists its NAME as an ECU with no address.
* **It can, with `j1939ArbitraryAddress`.** It moves to the lowest address from 128 to 247 that no other ECU has
  been heard to claim in this measurement, claims it under the **same NAME**, and says everything from there
  on from there: the telemetry, the predictive DM1, the answer to a request. The *J1939 address* statistic
  says where it is. An ECU that can move has the top bit of its NAME set, so it loses to every ECU that
  cannot - whoever can move, moves - and when the address it moved to is lost as well it moves again; when
  every address of the range is somebody's it gives up like one that cannot move.

One claiming its address with a **higher** NAME is told again that the address is taken. A new Start is a new
switch-on: the block claims the address it was given (`outputCanId`), and knows nothing yet of who is on the bus.

**What it claims to be** is its NAME, and three things in it are the block's settings: `j1939Function` (default
33, *System Monitor*, read against the on-highway industry group), `j1939Identity` (the serial number, by
default the address it was given, so that two of these blocks at two addresses are two NAMEs and not a contest)
and `j1939ArbitraryAddress` (off). The manufacturer is not a setting: it is 0, because a simulated ECU is
nobody's product.

**One rule of J1939-81 is left out, on purpose.** An ECU waits a pseudo-random time, up to 153 ms, before it
answers a request or says it cannot claim. The delay is there to spread out the answers of ECUs that power up
at the same instant on a shared wire. Here the answers are queued by the engine and arbitrated by the bus
model, in an order that does not vary, so a delay would add nothing but a measurement that cannot be repeated.

**Predictive DM1.** When the anomaly has been 80 % or more for five seconds - or the thermal health falls
under 30 %, at once - the block sends a DM1 from its own address: SPN 110, FMI 15 with the amber lamp, or
FMI 16 with the red lamp as well when it is the health that is failing. It is sent when it starts and then
**once a second**, not once per inference; it is taken back when the anomaly has been under 60 % for ten
seconds, with one DM1 of every lamp dark to say so.

---

## 3. System Variables & Interactive Anomaly Injection

When a measurement runs, `TinyMlEcuNode` publishes its outputs lock-free to `SystemVariables` and monitors `tinyml.inject_fault`:

| System Variable | Direction | Description |
|---|---|---|
| `tinyml.anomaly_score` | Output (`0..100`) | Continuous anomaly score percentage |
| `tinyml.confidence` | Output (`0..100`) | Winning regime confidence percentage |
| `tinyml.regime` | Output (`0..4`) | Classified operating regime index |
| `tinyml.thermal_health` | Output (`0..100`) | Predicted powertrain thermal health index |
| `tinyml.inference_us` | Output ($\mu\text{s}$) | Last forward-pass execution duration |
| `tinyml.inferences` | Output | Total inferences executed during the run |
| `tinyml.anomalies` | Output | Total anomalous passes detected |
| `tinyml.inject_fault` | Input (`0.0` or `1.0`) | Injects simulated thermal runaway and bus jitter |

---

## 4. Custom `.tbusml` Model Files

You can train custom quantized models in Python / Google Colab / TensorFlow and load them into any `tinyml.ecu` block via the **`Model File (.tbusml)`** (`modelPath`) parameter. See [`examples/models/powertrain_int8.tbusml`](../../examples/models/powertrain_int8.tbusml) for a complete reference model.

---

## 5. Qt6 Canvas Visual Simulation & Animations

The Pipeline Canvas (`CanvasPanel`) renders real-time visual telemetry at 60 FPS during a measurement:
- **Animated Wire Packets**: Glowing particles travel along the cubic Bézier wires between blocks, colour-coded by port type (`Frames` in cyan/accent, `Signals` in emerald green, `Events` in amber).
- **Live Node Halos & Badges**: Every active block displays a pulsing border halo and a floating telemetry pill above the block.
- **Live Neural Network & Tensor Arena Card**: Beneath every `tinyml.ecu` block, the Canvas draws the 4-column neural network topology with travelling synapse activation waves, regime badge, Anomaly Score meter, Thermal Health meter, and static tensor arena usage.
- **Canvas HUD Controls**: Use **TinyML Demo Setup** to build a complete Virtual Vehicle + TinyML pipeline in one click, **Animate Flow** to toggle animations, and **Inject TinyML Anomaly** to trigger real-time anomaly detection on screen.
- **Canvas View Controls**: A column of small icon buttons down the left edge of the canvas moves the view. **Pan** is a tool that stays on: while it is down, dragging anywhere (over a block as well) moves the canvas and touches no block. **Zoom In** and **Zoom Out** step like the mouse wheel, between 10 % and 200 %. **Fit to Window** zooms and centres so that every block - and the card under a TinyML block - is in view, without going past 100 %. The view keeps its zoom and position when you switch to another tab and back; it is fitted by itself only the first time the canvas is shown and when a project is opened or the demo is set up.
