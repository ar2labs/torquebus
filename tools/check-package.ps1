# SPDX-License-Identifier: GPL-3.0-or-later
#
# Looks at a packaged TorqueBus Studio and says whether it is one.
#
# This exists because of how the packaging broke: v0.17 moved the Kvaser and
# PEAK backends out of the executable and into plugins, which was the point -
# and the install rules were never told. The build was green, the tests passed,
# the zip was produced, and the only symptom was a hardware list with nothing
# in it on a machine with the hardware attached. Absence is the failure mode of
# packaging, and absence does not announce itself.
#
# So: run this against the directory that is about to become the zip. It fails
# loudly, with the missing path named, rather than leaving it to be discovered.
#
#   pwsh tools/check-package.ps1 -Path dist
#
# The negative checks matter as much as the positive ones. A package that
# contains a third-party SDK's headers is bloated; one that contains the J1939
# manufacturer table is redistributing a licensed database.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Path
)

$ErrorActionPreference = "Stop"
$problems = @()

function Require-Path {
    param([string] $Relative, [string] $Because)

    $full = Join-Path $Path $Relative
    if (Test-Path -LiteralPath $full) {
        Write-Host ("  ok       {0}" -f $Relative)
    } else {
        $script:problems += ("missing: {0} - {1}" -f $Relative, $Because)
    }
}

function Refuse-Path {
    param([string] $Relative, [string] $Because)

    $full = Join-Path $Path $Relative
    if (Test-Path -LiteralPath $full) {
        $script:problems += ("must not be packaged: {0} - {1}" -f $Relative, $Because)
    } else {
        Write-Host ("  absent   {0}" -f $Relative)
    }
}

if (-not (Test-Path -LiteralPath $Path)) {
    Write-Error "no such directory: $Path"
    exit 1
}

Write-Host "Checking package at $Path"
Write-Host ""

# --- What has to be there --------------------------------------------------

Require-Path "TorqueBusStudio.exe" "the application"

Require-Path "plugins/torquebus-driver-kvaser.dll" `
    "the Kvaser backend; without it a package has no Kvaser support at all"
Require-Path "plugins/torquebus-driver-peak.dll" `
    "the PEAK backend; without it a package has no PCAN support at all"

Require-Path "data/j1939-names-functions.csv" `
    "J1939 function names; without it the network panel shows bare numbers"

# --- Does the executable have what it imports? ------------------------------
#
# Asking by name was not enough, and the way it failed is worth keeping. This
# used to require "Qt6Core.dll", which a package can contain while still being
# unrunnable: a Debug build imports Qt6Cored.dll, and `windeployqt --release`
# deploys the release set beside it. Qt6Core.dll is present, the check passes,
# and the application stops at startup with "Qt6Guid.dll was not found".
#
# So the question is not whether a file with a familiar name is there. It is
# whether every DLL the binary actually imports can be found in the package -
# which is what the loader will ask, and the only question worth answering.

$imports = @()
$dumpbin = Get-Command "dumpbin.exe" -ErrorAction SilentlyContinue

if ($dumpbin) {
    $exe = Join-Path $Path "TorqueBusStudio.exe"

    if (Test-Path -LiteralPath $exe) {
        $imports = @(& $dumpbin.Source /DEPENDENTS $exe 2>$null |
            Select-String -Pattern '^\s+(\S+\.dll)$' |
            ForEach-Object { $_.Matches[0].Groups[1].Value })
    }
}

if ($imports.Count -eq 0) {
    # No dumpbin means no MSVC environment. Worth saying rather than silently
    # checking less than the message implies.
    Write-Host "  skipped  import check (dumpbin not on PATH)" -ForegroundColor DarkGray

    Require-Path "Qt6Core.dll"    "windeployqt did not run, or ran against the wrong executable"
    Require-Path "Qt6Widgets.dll" "windeployqt did not run, or ran against the wrong executable"
} else {
    # System DLLs come from Windows, not from us.
    $ours = $imports | Where-Object {
        $_ -notmatch '^(api-ms-|KERNEL32|USER32|SHELL32|ADVAPI32|ole32|OLEAUT32|WS2_32|dwmapi|d3d|dxgi|dxguid|VCRUNTIME|MSVCP|ucrtbase|WINMM|IMM32|NETAPI32|VERSION|CRYPT32|bcrypt|SETUPAPI|USERENV|MPR|UxTheme|COMDLG32|GDI32)'
    }

    foreach ($dll in $ours) {
        Require-Path $dll "TorqueBusStudio.exe imports it; without it the application does not start"
    }
}
Require-Path "platforms/qwindows.dll" `
    "the Qt platform plugin; without it the application exits at startup"

Require-Path "LICENSE.txt" "GPLv3 requires the licence text to travel with the binary"
Require-Path "CHANGELOG.md" `
    "what changed, for somebody holding a build and wondering which one it is"

# --- What must not be ------------------------------------------------------

Refuse-Path "data/j1939-names.csv" `
    "the J1939 manufacturer table is built from licensed data and is not ours to redistribute"

Refuse-Path "include" `
    "third-party headers; a package for end users is not an SDK"
Refuse-Path "lib/cmake" `
    "third-party CMake config; same"

# --- The point of the whole plugin exercise --------------------------------
#
# The vendor SDKs must not be reachable from the executable. This is the claim
# PLAN.md section 31 makes, and it is worth re-checking on the artifact rather
# than on a build tree, because the artifact is what people download.

Refuse-Path "canlib32.dll" `
    "the Kvaser SDK is proprietary and is never bundled - it comes from the driver install"
Refuse-Path "PCANBasic.dll" `
    "the PEAK SDK is proprietary and is never bundled - it comes from the driver install"

Write-Host ""

if ($problems.Count -gt 0) {
    Write-Host ("{0} problem(s):" -f $problems.Count) -ForegroundColor Red
    foreach ($problem in $problems) {
        Write-Host ("  {0}" -f $problem) -ForegroundColor Red
    }
    exit 1
}

Write-Host "Package looks complete." -ForegroundColor Green
exit 0
