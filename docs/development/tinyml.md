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
