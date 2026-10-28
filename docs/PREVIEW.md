# TorqueBus Studio Preview

This Preview is a Windows 11 x64 build of TorqueBus Studio 0.17.0. It is an
early public evaluation build; the project is still working toward its first
stable v1.0 release and has not completed validation against every physical
CAN adapter.

## Start the virtual J1939 demo

1. Extract the package to a writable folder.
2. Launch `TorqueBusStudio.exe`.
3. Choose **File → Open Project...** and open
   `examples/projects/j1939-vehicle.tbsproj` beside the executable.
4. Press **Start** (or F5). The two built-in virtual CAN channels start the
   simulated vehicle; no physical adapter is required.
5. Explore **Dashboard**, **CAN Trace**, and **J1939 Network**.

The package includes the sample project's DBC, Lua ECU scripts, and dashboard
assets with their relative folder layout intact. The J1939 function-name table
is included. The licensed manufacturer-name table is not redistributed, so
some manufacturer labels may appear as numeric addresses.

## Hardware

SAE J2534 and PEAK CAN backends are optional plugins. Real devices also require
their vendor drivers; the PEAK backend may require the vendor's PCAN-Basic DLL.
The Preview is intended to be useful with virtual CAN even when those drivers
and devices are absent.

## Build and validation

This build uses MSVC 2022 and Qt 6.12.0. The tagged release workflow runs the
automated test suite and checks the package's runtime imports. Physical adapter
validation still depends on available hardware; see the ordered checklist in
[`docs/development/validation.md`](docs/development/validation.md).

The application is licensed under GPL-3.0-or-later. See `LICENSE.txt` in the
package.
