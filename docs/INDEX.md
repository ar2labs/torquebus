# TorqueBus Documentation Index

Welcome to the **TorqueBus Studio** documentation hub. This index organizes all architectural contracts, development guides, protocol specifications, and testing references.

---

## 🏛️ Architecture & Core Principles

* **[Architecture Guide](ARCHITECTURE.md)**: The definitive contract. Threading model, bounded lock-free queues, zero-allocation hot paths, dataflow pipeline compilation, and plugin decoupling.
* **[Engineering Roadmap & Plan](PLAN.md)**: Architectural milestones from initial shell to v0.17 plugin decoupling and future v1.0 hardware validation.

---

## 🚀 Getting Started & Build System

* **[Getting Started](development/getting-started.md)**: Step-by-step setup for Windows 11, MSVC 2022, Qt 6.11.2, CMake, Ninja, and troubleshooting build environments.
* **[Validation Checklist](development/validation.md)**: Ordered acceptance test pass for verifying new builds, packaging integrity, and plugin loading.

---

## 🧪 Testing & Quality Assurance

* **[Google Test & QA Framework](development/testing-framework.md)**: Comprehensive guide to the **1364 automated tests**, test hierarchy (`unit`, `ui`, `integration`, `hardware`), CTest automation, XML/JUnit reports, and GitHub Actions CI.
* **[Pipeline Test Sequences](development/testing.md)**: Creating automated test sequence blocks in the pipeline canvas using Lua coroutines to assert bus timings and silence.

---

## 🚛 Protocols & Vehicle Simulation

* **[The Pipeline Canvas & the CAN Bus](development/pipeline-canvas.md)**: The canvas drawn as a network - a bus as two wires, every module one drop off it, chips instead of cards - how a drop is wired, how 11-bit and 29-bit frames are routed, and how it is built and drawn.
* **[Robustness](development/robustness.md)**: What happens when something is wrong - a script that loops, a block that throws, a damaged database or project, a frame that cannot exist - how far each failure travels, what is not covered, and how it is tested and fuzzed.
* **[SAE J1939 & Pipeline Simulation](development/j1939-pipeline.md)**: Heavy-duty vehicle networking, 29-bit CAN identifiers, PGNs (EEC1, EEC2, ET1, CCVS, DM1), simulated truck ECUs, and real-time dashboard dataflow.
* **[DTC Diagnostics Guide (OBD-II, UDS & J1939)](development/dtc-diagnostics.md)**: `L D D D D` code structure (`P/B/C/U`), generic vs. OEM codes, OBD-II ↔ SAE J1939 (`SPN/FMI`) cross-reference, ISO 14229 status bits, and practical diagnostic workflows.
* **[The Dashboard](development/dashboard.md)**: Building a dashboard from the loaded `.dbc` files - drag a signal out of the DBC Explorer, search and completion, the widget each signal suits, pictures (PNG, JPEG, SVG), alarm limits, silent signals, trends and message tables, the J1939 fault list, and what the project file holds.
* **[Instrument Cluster Dashboard](development/cluster.md)**: Hardware-accelerated Qt Quick / QML gauge cluster, SVG dials, telltale lamps (ISO 2575), and scenario fault injection.
* **[DBC Database Engine](development/databases.md)**: CAN database parsing, bit numbering, signal multiplexing, Intel/Motorola endianness, and signal conversion.
* **[ECU Scripting in Lua](development/scripting.md)**: Writing simulated ECUs in Lua, sandboxing, memory boundaries, and sub-microsecond frame dispatching.

---

## 🤖 Advanced Features & Hardware

* **[TinyML Anomaly Detection](development/tinyml.md)**: Machine learning inference on the CAN bus, Virtual ML ECUs, and automated bus anomaly classification.
* **Hardware Plugins**: Modular CAN drivers for **PEAK-System** (`pcanbasic`) and **Kvaser** (`canlib`), isolated from core binaries.

---

## 📋 Project Policies & Contribution

* **[Contributing Guidelines](../CONTRIBUTING.md)**: Code style, clang-format 21.1.0 gate, pull request process, and architectural review requirements.
* **[Changelog](../CHANGELOG.md)**: Detailed historical log of all releases, fixes, and architectural decisions.
