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
