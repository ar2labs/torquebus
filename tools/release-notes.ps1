# SPDX-License-Identifier: GPL-3.0-or-later
#
# Pulls one version's section out of CHANGELOG.md, to be the body of a release.
#
# The alternative is what the workflow used to do: let GitHub generate notes
# from commit subjects. That produces an accurate list and a useless one - the
# subjects here are full sentences, several of them describing a bug found
# rather than a feature shipped, and a reader looking at a download wants to
# know what the build does, not what its author discovered on the way.
#
# A hand-written section is also the only place the honest framing survives.
# "The package did not contain the vendor plugins" is worth saying out loud in
# a release note; no generator will write that line.
#
#   pwsh tools/release-notes.ps1 -Version v0.17.0
#
# Prints the section to standard output. Exits non-zero, saying so, when the
# version has no section - a release whose notes silently came out empty is
# worse than one that failed to build.

[CmdletBinding()]
param(
    # Accepted with or without the leading "v", because a tag has one and a
    # heading does not, and whoever calls this should not have to care.
    [Parameter(Mandatory = $true)]
    [string] $Version,

    [string] $Path = "CHANGELOG.md"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $Path)) {
    Write-Error "no changelog at $Path"
    exit 1
}

$wanted = $Version.TrimStart("v", "V")
$lines = Get-Content -LiteralPath $Path

# Headings look like "## 0.17.0 - Plugins - 2026-09-12". Match on the version
# as a whole field so that 0.1.0 cannot match inside 0.1.0-rc or 10.1.0.
$escaped = [regex]::Escape($wanted)
$start = -1

for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match "^##\s+v?$escaped(\s|$|\s*[-–—])") {
        $start = $i
        break
    }
}

if ($start -lt 0) {
    Write-Error @"
CHANGELOG.md has no section for $wanted.

Add one before tagging. A release note is the only part of a build that
explains itself to somebody who was not here, and generating it from commit
subjects is how that turns into a wall of sentences nobody reads.
"@
    exit 1
}

# Everything up to the next "## " heading, or the end.
$body = New-Object System.Collections.Generic.List[string]

for ($i = $start + 1; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match "^##\s") {
        break
    }
    $body.Add($lines[$i])
}

# Trim blank lines and the horizontal rules that separate sections in the file
# but mean nothing in a release body.
while ($body.Count -gt 0 -and ($body[0].Trim() -eq "" -or $body[0].Trim() -eq "---")) {
    $body.RemoveAt(0)
}
while ($body.Count -gt 0 -and ($body[-1].Trim() -eq "" -or $body[-1].Trim() -eq "---")) {
    $body.RemoveAt($body.Count - 1)
}

if ($body.Count -eq 0) {
    Write-Error "the section for $wanted is empty"
    exit 1
}

$body -join "`n"
