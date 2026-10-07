# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs the gates CONTRIBUTING.md says every pull request must pass.
#
# --- Why this exists --------------------------------------------------------
#
# `.github/workflows/ci.yml` runs these gates on every pull request, and
# CONTRIBUTING.md says a pull request must pass them. This file runs the same
# ones where the code actually is, because finding out locally beats finding out
# from a red tick eight minutes later.
#
# It was written before the repository had a remote, when no workflow had ever
# executed and this was the only place the gates ran at all. That is over - CI
# runs - and this is the convenient copy of it, not the only one.
#
# It deliberately runs the *strict* preset. That is the one with warnings as
# errors, and it is what CI configures; passing Debug and failing strict is the
# usual way a change looks fine locally and is rejected remotely.
#
#   pwsh tools/check-before-push.ps1            # Debug + strict, then the rest
#   pwsh tools/check-before-push.ps1 -All       # and Release, which is what ships
#   pwsh tools/check-before-push.ps1 -Tidy      # and clang-tidy on what changed
#   pwsh tools/check-before-push.ps1 -SkipBuild # only the cheap checks
#
# Needs the MSVC and Qt environment: run it from tools\torquebus-prompt.bat, or
# from any prompt where cl.exe and windeployqt resolve.

[CmdletBinding()]
param(
    # Skips configure, build and tests - useful when you have just run them and
    # only want the formatting and packaging opinion.
    [switch] $SkipBuild,

    # Both configurations, which is what CONTRIBUTING.md actually asks for.
    # Without it this checks windows-msvc-strict only - Debug with warnings as
    # errors - and Release goes untested. That gap is not theoretical: a test
    # that passed in Debug failed in RelWithDebInfo the first time anybody ran
    # it there, because it was measuring how the optimiser balanced a producer
    # against a consumer rather than measuring the pipeline.
    [switch] $All,

    # Runs clang-tidy over the files you are about to push.
    #
    # Off by default because it is slow - twenty seconds a file, against a
    # compilation database - and a check that turns a two-minute gate into a
    # thirty-minute one is a check people stop running. Scoped to changed files
    # for the same reason: the whole tree is a one-off exercise, and it has been
    # done (see .clang-tidy, which records what the first run found).
    [switch] $Tidy,

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
        $presets = @($Preset)
        if ($All -and $Preset -ne "windows-msvc-release") {
            $presets += "windows-msvc-release"
        }

        foreach ($current in $presets) {
            Write-Heading "Configure ($current)"

            # The layering rules in cmake/TorqueBusLayering.cmake fail here, not
            # at build time, so a configure that succeeds has already checked
            # them.
            cmake --preset $current 2>&1 | Select-Object -Last 12
            if ($LASTEXITCODE -ne 0) { $failures += "$current : configure failed"; continue }

            Write-Heading "Build ($current)"
            cmake --build --preset $current 2>&1 | Select-Object -Last 12
            if ($LASTEXITCODE -ne 0) { $failures += "$current : build failed"; continue }

            Write-Heading "Tests ($current)"
            Push-Location (Join-Path $root "build/$current")
            try {
                ctest --output-on-failure --label-exclude hardware -j 4 2>&1 |
                    Select-Object -Last 10
                if ($LASTEXITCODE -ne 0) { $failures += "$current : tests failed" }
            } finally {
                Pop-Location
            }
        }
    }

    # --- 3. Static analysis, on request --------------------------------------
    #
    # .clang-tidy is a real configuration that went years without running: CI
    # never executed, and the step that claimed to run it ran `echo`. Its first
    # actual run found six places an exception could escape a thread or a
    # destructor, every one of them a std::terminate. That is the kind of thing
    # worth a switch.

    if ($Tidy) {
        Write-Heading "Static analysis (advisory)"

        $clangTidy = Get-Command "clang-tidy" -ErrorAction SilentlyContinue
        $database = Join-Path $root "build/$Preset/compile_commands.json"

        if (-not $clangTidy) {
            Write-Host "  clang-tidy not found - skipped" -ForegroundColor DarkGray
        } elseif (-not (Test-Path $database)) {
            Write-Host "  no compile_commands.json in build/$Preset - configure first" -ForegroundColor DarkGray
        } else {
            # What you are about to push: tracked changes plus new files. Not
            # the whole tree - see the note on the switch.
            $changed = @(
                (& git diff --name-only HEAD -- "*.cpp") +
                (& git ls-files --others --exclude-standard -- "*.cpp")
            ) | Where-Object { $_ -and $_ -notlike "third_party/*" } | Select-Object -Unique

            if ($changed.Count -eq 0) {
                Write-Host "  no changed .cpp files" -ForegroundColor DarkGray
            } else {
                Write-Host "  $($changed.Count) changed file(s)" -ForegroundColor DarkGray

                $findings = @()

                foreach ($file in $changed) {
                    if (-not (Test-Path $file)) { continue }

                    # -Wno-unused-command-line-argument because the database
                    # carries MSVC's /Zc:preprocessor, which clang accepts and
                    # does not use; without it every file reports an error
                    # before a single check runs.
                    $output = & {
                        $ErrorActionPreference = "Continue"
                        & clang-tidy -p (Join-Path $root "build/$Preset") --quiet `
                            --extra-arg=-Wno-unused-command-line-argument $file 2>&1
                    }

                    $findings += @($output |
                        ForEach-Object { $_.ToString() } |
                        Select-String -Pattern ": (warning|error): " |
                        ForEach-Object { $_.Line })
                }

                if ($findings.Count -eq 0) {
                    Write-Host "  clean" -ForegroundColor Green
                } else {
                    foreach ($finding in ($findings | Select-Object -First 30)) {
                        Write-Host "  $finding" -ForegroundColor Yellow
                    }
                    if ($findings.Count -gt 30) {
                        Write-Host "  ... and $($findings.Count - 30) more" -ForegroundColor Yellow
                    }
                    $advisories += "clang-tidy: $($findings.Count) finding(s)"
                }
            }
        }
    }

    # --- 4. Formatting ------------------------------------------------------
    #
    # A failure now, matching CI. It was advisory while 250 of 267 files
    # disagreed with the config; the tree was reformatted in one commit and the
    # check can pass, so it is allowed to fail.
    #
    # The version matters and is reported when it disagrees: CI pins one exact
    # clang-format, and a different one formats differently. A local 19 saying
    # 30 files differ, against a CI that is happy, is a version gap and not a
    # formatting problem - and Visual Studio bundles exactly such a 19.
    #
    # The pinned version is read out of the workflow rather than written down a
    # second time here. This script said 17 for as long as it did because a
    # number in a comment does not notice when the thing it describes moves.
    #
    # And a missing clang-format is a note in the verdict, not a silent skip:
    # "Passed." with no formatting check behind it is how a change looks fine
    # here and is rejected there.

    Write-Heading "Formatting"

    $clangFormat = Get-Command "clang-format" -ErrorAction SilentlyContinue

    $pinnedVersion = $null
    $workflow = Join-Path $root ".github/workflows/ci.yml"
    if (Test-Path $workflow) {
        $pin = Select-String -Path $workflow -Pattern 'clang-format==([0-9][0-9.]*)' |
            Select-Object -First 1
        if ($pin) { $pinnedVersion = $pin.Matches[0].Groups[1].Value }
    }
    $pinnedText = if ($pinnedVersion) { $pinnedVersion } else { "the version in ci.yml" }

    if (-not $clangFormat) {
        Write-Host "  clang-format not found - formatting NOT checked" -ForegroundColor Yellow
        Write-Host "  CI checks it with clang-format $pinnedText; see docs/development/getting-started.md" `
            -ForegroundColor DarkGray
        $advisories += "formatting: not checked, clang-format is not installed (CI runs $pinnedText)"
    } else {
        $versionText = (& clang-format --version) -join " "
        $version = if ($versionText -match 'version ([0-9]+(\.[0-9]+)*)') { $Matches[1] } else { "unknown" }
        $versionGap = $pinnedVersion -and ($version -ne $pinnedVersion)

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
            if ($versionGap) {
                # Clean under a different version proves less than it looks like.
                $advisories += "formatting: checked with clang-format $version, CI runs $pinnedVersion - a clean result here can still fail there"
            }
        } else {
            Write-Host ("  clang-format {0} would change {1} of {2} files:" -f
                        $version, $dirty.Count, $files.Count) -ForegroundColor Red
            foreach ($file in ($dirty | Select-Object -First 20)) {
                Write-Host "    $file" -ForegroundColor Red
            }
            if ($dirty.Count -gt 20) {
                Write-Host ("    ... and {0} more" -f ($dirty.Count - 20)) -ForegroundColor Red
            }
            Write-Host "  clang-format -i on those files fixes it." -ForegroundColor DarkGray
            if ($versionGap) {
                Write-Host "  CI pins clang-format $pinnedVersion and this is $version - that may be why." `
                    -ForegroundColor Yellow
            }

            $failures += "formatting: $($dirty.Count) files differ"
        }
    }

    # --- 5. The package -----------------------------------------------------
    #
    # From the *release* preset, always, whatever $Preset the gates above used.
    #
    # This step used to package whatever had just been built, which by default
    # is windows-msvc-strict - and that inherits windows-msvc-debug, so it is a
    # Debug binary importing Qt6Cored.dll, Qt6Guid.dll and Qt6Widgetsd.dll.
    # Running `windeployqt --release` on it deployed the release Qt beside a
    # binary that needs the debug one, and the result would not start:
    # "Qt6Guid.dll was not found". check-package.ps1 passed it anyway, because
    # it was looking for Qt6Core.dll by name and the release Qt6Core.dll was
    # right there.
    #
    # Packaging a Debug build was never meaningful - nobody ships one - so the
    # fix is not to teach this step about --debug but to package the thing that
    # actually ships.

    if (-not $SkipBuild -and $failures.Count -eq 0) {
        Write-Heading "Package (advisory)"

        $packagePreset = "windows-msvc-release"
        $packageBuild = Join-Path $root "build/$packagePreset"

        if (-not (Test-Path $packageBuild)) {
            Write-Host "  $packagePreset has not been built - skipped" -ForegroundColor DarkGray
            Write-Host "  (run with -All, or build that preset, to include this)" -ForegroundColor DarkGray
            $advisories += "package: not checked, no $packagePreset build"
        } else {
            $staging = Join-Path ([System.IO.Path]::GetTempPath()) "torquebus-package-check"
            if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }

            cmake --install $packageBuild --prefix $staging --component torquebus 2>&1 |
                Select-Object -Last 3

            if ($LASTEXITCODE -ne 0) {
                $advisories += "package: install failed"
            } elseif (Get-Command "windeployqt" -ErrorAction SilentlyContinue) {
                # --qmldir: Qt's QML modules are plugins the engine loads by name,
                # and the cluster is the first QML in the application.
                windeployqt --release --no-translations --no-system-d3d-compiler `
                    --qmldir (Join-Path $root "src/ui/dashboard/cluster/qml") `
                    (Join-Path $staging "TorqueBusStudio.exe") 2>&1 | Out-Null

                # And once per plugin. windeployqt asks the binary it is given
                # what it needs, and the executable does not link Qt6::SerialBus
                # - that is the point of PEAK being a plugin. Pointed only at the
                # executable, it produced a package whose PEAK plugin could not
                # load, and nothing said so.
                #
                # The Kvaser plugin links no Qt at all, so windeployqt refuses it
                # with "does not seem to be a Qt executable". That is the right
                # answer and not a failure - so the error preference is lowered
                # for these calls, which otherwise turn a correct refusal into a
                # terminating error and take this script with it.
                & {
                    $ErrorActionPreference = "Continue"
                    Get-ChildItem (Join-Path $staging "plugins") -Filter "*.dll" `
                        -ErrorAction SilentlyContinue |
                        ForEach-Object {
                            windeployqt --release --no-translations --no-system-d3d-compiler `
                                --dir $staging $_.FullName 2>&1 | Out-Null
                        }
                }

                & (Join-Path $PSScriptRoot "check-package.ps1") -Path $staging
                if ($LASTEXITCODE -ne 0) { $advisories += "package: incomplete" }
            } else {
                Write-Host "  windeployqt not found - skipped" -ForegroundColor DarkGray
            }

            Remove-Item -Recurse -Force $staging -ErrorAction SilentlyContinue
        }
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
