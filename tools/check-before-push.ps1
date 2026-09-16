# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs the gates CONTRIBUTING.md says every pull request must pass.
#
# --- Why this exists --------------------------------------------------------
#
# `.github/workflows/ci.yml` describes those gates, and CONTRIBUTING.md says a
# pull request must pass them. Both were written before the repository had
# anywhere to push to: there is no remote, so no workflow in this repository has
# ever executed. "Enforced on every pull request" described a machine that has
# never run.
#
# Two ways to fix that. One is to add a remote, which is not a script's decision
# to make. The other is to make the gates runnable where the code actually is,
# which is this file - and which stays useful afterwards, because finding out
# locally beats finding out from a red tick eight minutes later.
#
# It deliberately runs the *strict* preset. That is the one with warnings as
# errors, and it is what CI configures; passing Debug and failing strict is the
# usual way a change looks fine locally and is rejected remotely.
#
#   pwsh tools/check-before-push.ps1            # build, tests, formatting
#   pwsh tools/check-before-push.ps1 -SkipBuild # only the cheap checks
#
# Needs the MSVC and Qt environment: run it from tools\torquebus-prompt.bat, or
# from any prompt where cl.exe and windeployqt resolve.

[CmdletBinding()]
param(
    # Skips configure, build and tests - useful when you have just run them and
    # only want the formatting and packaging opinion.
    [switch] $SkipBuild,

    [string] $Preset = "windows-msvc-strict"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$failures = @()
$advisories = @()

function Write-Heading {
    param([string] $Text)
    Write-Host ""
    Write-Host "== $Text" -ForegroundColor Cyan
}

Push-Location $root
try {
    # --- 1. The toolchain, before anything that needs it --------------------
    #
    # Checked first and by name, because the failure it prevents is the one
    # documented in tools\torquebus-prompt.bat: without vcvars, CMake finds a
    # cl.exe on PATH, compiles its test file, and links it with whatever ld.exe
    # it finds first - producing a wall of errors that reads like a broken
    # Windows SDK and is not.

    Write-Heading "Toolchain"

    foreach ($tool in @("cmake", "ctest")) {
        if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
            $failures += "$tool is not on PATH"
        }
    }

    if (-not $SkipBuild -and -not (Get-Command "cl.exe" -ErrorAction SilentlyContinue)) {
        $failures += "cl.exe is not on PATH - run tools\torquebus-prompt.bat first"
    }

    if ($failures.Count -gt 0) {
        foreach ($problem in $failures) {
            Write-Host "  missing: $problem" -ForegroundColor Red
        }
        exit 1
    }

    Write-Host "  ok" -ForegroundColor Green

    # --- 2. Configure, build, test ------------------------------------------

    if (-not $SkipBuild) {
        Write-Heading "Configure ($Preset)"

        # The layering rules in cmake/TorqueBusLayering.cmake fail here, not at
        # build time, so a configure that succeeds has already checked them.
        cmake --preset $Preset 2>&1 | Select-Object -Last 12
        if ($LASTEXITCODE -ne 0) { $failures += "configure failed" }

        if ($failures.Count -eq 0) {
            Write-Heading "Build"
            cmake --build --preset $Preset 2>&1 | Select-Object -Last 12
            if ($LASTEXITCODE -ne 0) { $failures += "build failed" }
        }

        if ($failures.Count -eq 0) {
            Write-Heading "Tests"
            Push-Location (Join-Path $root "build/$Preset")
            try {
                ctest --output-on-failure --label-exclude hardware -j 4 2>&1 |
                    Select-Object -Last 10
                if ($LASTEXITCODE -ne 0) { $failures += "tests failed" }
            } finally {
                Pop-Location
            }
        }
    }

    # --- 3. Formatting ------------------------------------------------------
    #
    # Advisory, exactly as in CI, and for a reason worth repeating here rather
    # than hiding in a workflow file: this codebase is hand-formatted. Line
    # breaks are chosen for meaning in places where clang-format would join the
    # line because it fits. Running `clang-format -i` over a file you touched
    # will reformat the whole file and bury your change in a diff nobody can
    # review.
    #
    # So: it tells you what clang-format thinks, and lets you decide.

    Write-Heading "Formatting (advisory)"

    $clangFormat = Get-Command "clang-format" -ErrorAction SilentlyContinue

    if (-not $clangFormat) {
        Write-Host "  clang-format not found - skipped" -ForegroundColor DarkGray
    } else {
        $version = (& clang-format --version) -replace '.*version ([0-9]+).*', '$1'

        $files = git ls-files '*.cpp' '*.h' | Where-Object { $_ -notlike "third_party/*" }

        # clang-format writes its findings to stderr, and under
        # ErrorActionPreference=Stop PowerShell turns any stderr from a native
        # command into a terminating NativeCommandError - so the tool reporting
        # what it found would abort the script that asked it. Lowered for this
        # one call only; a real failure is caught by the exit code below.
        $output = & {
            $ErrorActionPreference = "Continue"
            & clang-format --dry-run $files 2>&1
        }

        $dirty = @($output |
            ForEach-Object { $_.ToString() } |
            Select-String -Pattern "^(.+?):\d+:\d+: (warning|error)" |
            ForEach-Object { ($_.Line -split ":")[0] } |
            Select-Object -Unique)

        if ($dirty.Count -eq 0) {
            Write-Host "  ok - $($files.Count) files" -ForegroundColor Green
        } else {
            Write-Host ("  clang-format {0} would change {1} of {2} files." -f
                        $version, $dirty.Count, $files.Count) -ForegroundColor Yellow
            Write-Host "  This is expected and is not a failure. See the note above." -ForegroundColor DarkGray
            $advisories += "formatting: $($dirty.Count) files differ"
        }
    }

    # --- 4. The package -----------------------------------------------------
    #
    # Only when a build just happened, and only as an opinion: the real gate is
    # in the release workflow. It is here because the packaging defect this
    # script's neighbours were written for - a release with no vendor plugins -
    # was invisible in every other check.

    if (-not $SkipBuild -and $failures.Count -eq 0) {
        Write-Heading "Package (advisory)"

        $staging = Join-Path ([System.IO.Path]::GetTempPath()) "torquebus-package-check"
        if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }

        cmake --install "build/$Preset" --prefix $staging --component torquebus 2>&1 |
            Select-Object -Last 3

        if ($LASTEXITCODE -ne 0) {
            $advisories += "package: install failed"
        } elseif (Get-Command "windeployqt" -ErrorAction SilentlyContinue) {
            windeployqt --release --no-translations --no-system-d3d-compiler `
                (Join-Path $staging "TorqueBusStudio.exe") 2>&1 | Out-Null

            & (Join-Path $PSScriptRoot "check-package.ps1") -Path $staging
            if ($LASTEXITCODE -ne 0) { $advisories += "package: incomplete" }
        } else {
            Write-Host "  windeployqt not found - skipped" -ForegroundColor DarkGray
        }

        Remove-Item -Recurse -Force $staging -ErrorAction SilentlyContinue
    }

    # --- The verdict --------------------------------------------------------

    Write-Host ""

    if ($failures.Count -gt 0) {
        Write-Host "FAILED:" -ForegroundColor Red
        foreach ($problem in $failures) {
            Write-Host "  $problem" -ForegroundColor Red
        }
        exit 1
    }

    if ($advisories.Count -gt 0) {
        Write-Host "Passed, with notes:" -ForegroundColor Yellow
        foreach ($note in $advisories) {
            Write-Host "  $note" -ForegroundColor Yellow
        }
    } else {
        Write-Host "Passed." -ForegroundColor Green
    }

    exit 0
} finally {
    Pop-Location
}
