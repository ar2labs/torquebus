# The instrument cluster

The Dashboard has one widget that is not painted by hand: an **instrument
cluster** — speed, engine speed, temperatures, tell-tales and the TinyML panel on
one screen. It is QML, added from the Dashboard's context menu like a Gauge or a
Lamp (**Add Cluster**, in Edit mode), and it ships in
`examples/projects/virtual-vehicle.tbsproj`.

`ARCHITECTURE.md` already said where QML belongs — *Qt Widgets for the workbench,
QML for dashboards* — and this is the first of it. Everything else in the
application is still Widgets, and the rest of the Dashboard is unchanged.

## The idea: roles, and a profile that feeds them

The cluster knows **roles** — a speed, an engine speed, a lamp for the parking
brake — and nothing about signals, messages or buses. A **profile** says where
each role comes from. That split is what lets the same cluster sit on the 11-bit
example, on a J1939/FMS truck or on a signal list somebody typed, without the
cluster changing.

A role with no source is not an error and is never hidden or greyed out: it shows
dashes, its lamp stays ready, and it lights up the day a source arrives. A role
whose source has no value yet — a signal that has not been seen — is the same.

```
 DashboardWidget (kind Cluster, profile "example-11bit")
        │ profile id
        ▼
 ClusterProfile ── role → DashboardBinding ──┐         core, no Qt
        ▲                                     │   src/core/dashboard/cluster/
 ClusterProfiles (registry)                   │
                                              ▼
 DashboardPanel::readBinding()  ◄──── the Dashboard's own reader (plot store, variables)
        │                                                   src/ui/dashboard/
        ▼
 ClusterDataSource ── QQmlPropertyMap "vehicle"             src/ui/dashboard/cluster/
        │
        ▼
 ClusterView.qml (module TorqueBus.Cluster, in a QQuickWidget laid over the panel)
```

A profile reuses the Dashboard's own `DashboardBinding` — a decoded CAN signal by
message and signal name, or a system variable — so the cluster reads exactly what a
Gauge bound to the same signal would, from the same place the Graph panel reads.
There is no second way of naming a value, and so no second way for the two to
disagree. Which also means: **a signal reaches the cluster only if something puts
it in the plot store** — a *Signal Plot* block, as for any Dashboard widget.

## Roles

The name of a role is its key in the `vehicle` object that `ClusterView.qml`
reads, so the list in `ClusterRole.h` and the contract at the top of the QML file
are one list. `ClusterRoleTests` reads the QML and fails if they drift.

| Numbers | Unit | | Flags (on above 0.5) |
|---|---|---|---|
| `speed` | km/h | | `ignition` (absent means *on*) |
| `rpm` | rpm | | `cruise` |
| `gear` | −1 R, 0 N, 1…n | | `lampHigh`, `lampLow`, `lampPosition` |
| `coolant` | °C | | `lampPark`, `lampBelt` |
| `oil` | kPa | | `lampEngine`, `lampOil`, `lampAbs` |
| `fuel`, `def` | % | | `lampLeft`, `lampRight`, `lampHazard` |
| `battery` | V | | `dmStop`, `dmWarn`, `dmMil`, `dmProtect` — the J1939 DM1 lamps |
| `airPressure` | bar | | |
| `ambient` | °C | | |
| `odometer`, `hours` | km, h | | |
| `aiRegime` | 0 idle … 4 anomaly | | |
| `aiAnomaly`, `aiConfidence`, `aiHealth` | % | | |

A lamp is on or off. The J1939 FMS tell-tale message also carries a *condition*
(red, yellow, information) per lamp; modelling that is a change to the role, not
to the cluster, and has not been done.

## Profiles

`ClusterProfiles` is a registry in the same shape as `CanBackendRegistry` and
`NodeCatalog` (rule #10): a new profile is a new entry, and the cluster, the
Dashboard and the project file do not change. The built-in ones are registered by
the registry's constructor rather than behind a call that has to be made first —
they probe nothing, and a project opened before anybody called it would otherwise
be refused for naming a profile this build has.

The only built-in profile today is `example-11bit`, the vehicle of
`virtual-vehicle.tbsproj` as `examples/databases/vehicle.dbc` and `ecu.dbc`
describe it:

| Role | Source |
|---|---|
| `speed` | `VehicleSpeed.SpeedKmh` |
| `coolant` | `EngineTemp.EngTemp` |
| `rpm` | `EngineSpeed.RPM` |
| `aiRegime`, `aiAnomaly`, `aiConfidence`, `aiHealth` | `TinyML_Telemetry.RegimeClass`, `.AnomalyScore`, `.Confidence`, `.ThermalHealth` |

Everything else has no source in that profile, and shows dashes.

**Adding a profile**

1. Add a function that returns a `ClusterProfile` to `ClusterProfiles.cpp`, listing
   only the roles the vehicle actually reports, and register it in the
   constructor. The `id` is written into project files: once released, it never
   changes.
2. Add a test next to `TheExampleProfileReadsSignalsTheExampleDatabasesHave`
   that opens the database the profile names and checks every message and signal
   it lists. The profile is names typed by hand, and a renamed signal would
   otherwise become a needle that never moves, with nothing in the Output panel to
   say why.
3. Nothing else. The Widget panel lists the registered profiles by itself.

A project that names a profile this build does not have is **refused** when it is
opened, like one that names a widget kind it does not have: drawing another
profile's numbers on a cluster would be a lie about what the file contains.

## What the example shows today

Opening `virtual-vehicle.tbsproj` and pressing Start, the AI panel follows the
TinyML virtual ECU — regime, risk, confidence and thermal health, and the lamp test
at start-up. **Speed, engine speed and temperature show dashes.** The example emits
`0x101` and `0x102`, but nothing in its pipeline decodes them into the plot store
(only the TinyML telemetry goes through a *Signal Plot*), and no ECU in it emits
`EngineSpeed`. Giving the cluster those is a change to the example's pipeline, not
to the cluster; the profile is ready for them.

## The QML

```
src/ui/dashboard/cluster/qml/
  ClusterView.qml          the root: the `vehicle` contract, the lamp test, the layout
  ClusterFrame.qml         the housing
  common/                  ClusterTheme (colours and fonts), CText, RadialGlow
  instruments/             RadialGauge, SegmentRing, BarGauge, AiCore
  telltales/               Lamp, TelltaleIcon, TurnArrow
  preview/                 ClusterPreview and FakeVehicle: not part of the module
```

It is the module `TorqueBus.Cluster`, built with `qt_add_qml_module` into a static
library, so it is compiled ahead of time and travels inside the executable.
Whoever loads it links `torquebus_clusterplugin` and names it once, in a source of
the executable: `Q_IMPORT_QML_PLUGIN(TorqueBus_ClusterPlugin)` (`app/Main.cpp`; the
UI tests do it in `ClusterViewTests.cpp`). Nothing finds a static plugin by itself.

A few conventions that are not optional:

- **Every file that uses a type from another directory of the module starts with
  `import TorqueBus.Cluster`.** The compiled module does not need it, but the
  preview tool reads the files from the source tree, where a file sees only its own
  directory otherwise, and the reload would stop at *"Lamp is not a type"*.
- **Colours and fonts come from `ClusterTheme`, which mirrors the application's
  `Theme`** (`ClusterThemeSync` writes it, initially and on every theme change).
  A colour typed into a `.qml` file is one that stops following the application the
  day the theme changes. A role the cluster needs and `Theme` lacks is added to
  `Theme` — the six lamp and instrument roles were — not invented here.
- **Icons are the application's own set**: `resources/icons/cluster-*.svg`, white,
  served tinted through `image://torquebus-icons/<name>/<RRGGBB>` by
  `IconImageProvider`, which shares its painting with `ThemeManager`.
- **A role is a finite number or no data, and nothing in between.** `ClusterView`
  turns anything else into `NaN` (`num()`), and `NaN` is what draws dashes. Keep
  `NaN` out of anything that is *animated* or reaches an item's geometry: the
  scene graph rounds geometry to integers, which Qt asserts against in a debug
  build. `RadialGauge` animated its value from and to `NaN` once, and that is how
  this was found.
- **Strings in the QML go through `qsTr()`**, like the C++ goes through `tr()`.

### Working on it without the rest of the application

```
cmake --build --preset windows-msvc-vs --target torquebus_cluster_preview
build\windows-msvc-vs\bin\Debug\torquebus_cluster_preview.exe
```

Leave it open: it watches the cluster's `.qml` files and **reloads when one is
saved**. It is the real cluster — the real `Theme` through the same function the
Dashboard uses, the real icons behind the real provider — over a pretend vehicle
(`FakeVehicle.qml`) standing where the bus will. `--light` shows the light theme,
`--scene=hazard|anomaly|nodata|stop` jumps to a state, and `--shot=<file.png>`
saves a picture and exits. A change to an icon is the one thing it does not pick
up: icons are compiled in, and that needs a build.

`cmake --build --preset windows-msvc-vs --target torquebus_cluster_qmllint` runs
Qt's linter over the module.

## How it sits on the Dashboard

`ClusterHost` lays one `QQuickWidget` over the panel for each Cluster widget, at
the widget's rectangle, and the panel keeps doing everything else on the widget
beneath it.

- The view is **transparent to the mouse**, so selecting, dragging, resizing and the
  context menu work as for any other widget, and has a **transparent background**,
  so what surrounds the cluster's frame is the panel's canvas.
- It is stacked **above** the panel's own painting (`Qt::WA_AlwaysStackOnTop`),
  which a `QQuickWidget` cannot be talked out of. A widget that overlaps a cluster
  is drawn *under* it.
- In **Edit** mode the view is hidden — it would cover the selection outline and the
  resize handle the panel draws — and the panel paints a **picture of the cluster as
  it last was** instead, fitted to the widget. A cluster that has never been on
  screen - added in Edit mode, or in a project opened in it - is shown for the
  instant the picture takes, so it is pictured as it first draws itself: with the
  lamp test running and every lamp lit. If the platform cannot take the picture, the
  panel draws the name of the cluster's profile in its place.
- One QML engine and one `ClusterTheme` serve every cluster on the panel.
- The host is a **member** of the panel, not a child, and deletes its views itself:
  Qt destroys a widget's members before its children, so a view left to the panel's
  destructor would outlive the engine it runs in (`ARCHITECTURE.md`, *Qt teardown
  order*).
- A signal that stops arriving **keeps its last value**, as it does on every other
  widget: the plot store does not age samples.

## Tests

| Test | What it is for |
|---|---|
| `ClusterRoleTests`, `ClusterProfilesTests` (unit) | the roles are the QML's keys; a profile names signals that exist in the databases |
| `DashboardTests`, `ProjectFileTests` (unit) | the widget kind, its `profile`, the refusals, the round trip |
| `ClusterViewTests` (ui) | the module loads; no data, every role and every lamp draw without a single warning from Qt, on both themes |
| `ClusterThemeSyncTests`, `IconImageProviderTests` (ui) | the theme mirror and the icon set |
| `ClusterHostTests`, `ClusterDataSourceTests`, `DashboardPanelClusterTests` (ui) | a value followed from the store through the profile to the QML property; edit/run; removal; teardown |

The UI tests run on the `offscreen` platform, where Qt Quick falls back to its
**software renderer** — which is stricter about `NaN` than the one on a screen is.

## Packaging

Qt's QML modules are plugins the engine loads by name, and nothing imports them, so
`windeployqt` has to be told which the application uses: `--qmldir
src/ui/dashboard/cluster/qml` in `ci.yml`, `release.yml` and
`tools/check-before-push.ps1`. `tools/check-package.ps1` requires the three modules
the cluster imports, because the check of imports cannot see them missing.
