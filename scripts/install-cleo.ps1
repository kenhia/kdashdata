<#
.SYNOPSIS
    Fetch, verify and install kdash-pub.exe on cleo from the homelab package store.

.DESCRIPTION
    This stands in for a knarr feature and should be read that way.

    knarr is the fleet deploy runner, but its install step is `install -m 0755`
    over ssh, which is not the Windows shape -- so it deploys kai and kubs0 and
    leaves cleo behind. That gap is invisible unless something closes it: the
    deploy reports two matching versions and cleo quietly keeps running
    whatever it had, which for a Claude Code hook binary means the hook keeps
    publishing under an old contract with nothing to show for it.

    So this script exists to close that gap, and deliberately does no more than
    knarr's own install step does: resolve a version from the store, fetch it,
    verify its SHA256, rotate the outgoing binary aside, install, and confirm by
    re-reading `--version` on the target.

    KEEP IT SMALL. This is the SECOND copy of this script in the homelab
    (kpolice has the first), which was the agreed trigger to ask knarr for real
    Windows support rather than copy it a third time. That request is filed as
    knarr WI 1763 and this file is on its retire list. Do not extend it; extend
    knarr.

.NOTES
    THIS FILE IS DELIBERATELY PURE ASCII. Do not add em-dashes or smart quotes,
    however much the rest of the repo uses them.

    cleo runs Windows PowerShell 5.1, which reads a BOM-less .ps1 as the system
    ANSI codepage rather than UTF-8. A 3-byte UTF-8 em-dash then arrives as
    mojibake, and where one sits inside a double-quoted string it terminates the
    string early -- so the whole file fails to parse with errors pointing at
    lines 60 lines below the actual character. Verified the hard way on the
    kpolice copy, 2026-08-22.

    A UTF-8 BOM would also fix it, but BOMs get stripped by tooling and the
    failure comes back silently. ASCII has no such failure mode.

.PARAMETER Version
    Store version to install, e.g. `0.1.0-abc1234`. Defaults to whatever the
    store's `latest` pointer resolves to.

.PARAMETER Store
    Base URL of the package store. Defaults to the homelab store on kubsdb.

    Use the TAILNET HTTPS name, not `http://kubsdb:4880`. The store answers on
    both, but not from everywhere: kai reaches kubsdb's plain-HTTP listener
    directly, while from cleo that same host:port is TLS-terminated by
    Tailscale, so a plain HTTP request comes back `400 Bad Request` -- which
    reads like a broken store and is actually a broken scheme.

.PARAMETER Dest
    Install path. Defaults to C:\tools\bin\kdash-pub.exe -- the homelab tools
    directory on cleo, which is first on PATH.

    This path is a CONTRACT, not a convenience. kdeskdash's claude-pub.sh execs
    it as an absolute path (`/c/tools/bin/kdash-pub.exe` under Git Bash),
    because a Claude Code hook context's PATH is not the interactive one.
    Moving it is a change to that hook, not just to this default.
#>
param(
    [string]$Version = 'latest',
    [string]$Store   = 'https://kubsdb.encke-wahoo.ts.net:4880',
    [string]$Dest    = 'C:\tools\bin\kdash-pub.exe'
)

$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'   # Invoke-WebRequest is ~10x faster without it

# Every failure here is something an operator has to read and act on, so it gets
# one clean line rather than PowerShell's default stack trace with the message
# buried in a FullyQualifiedErrorId. `trap` catches terminating errors at script
# scope, which -- with ErrorActionPreference Stop -- is all of them, including
# the ones Invoke-WebRequest raises. It must be declared before anything can fail.
trap {
    [Console]::Error.WriteLine("kdash-pub-install: $($_.Exception.Message)")
    exit 1
}

$name = 'kdash-pub'
$file = 'kdash-pub-x86_64-windows.exe'

# cleo runs Windows PowerShell 5.1, not PowerShell 7. Two consequences this
# script depends on:
#
#   * `-UseBasicParsing` is required, or Invoke-WebRequest wants Internet
#     Explorer's DOM engine.
#   * `.Content` comes back as **System.Byte[]**, not a string, for responses
#     the store serves without a text content-type -- which is all of these.
#     Calling `.Trim()` on it fails with "You cannot call a method on a
#     null-valued expression", which looks nothing like the encoding problem it
#     actually is. Hence FetchText.
function FetchText($url) {
    $c = (Invoke-WebRequest -Uri $url -UseBasicParsing -TimeoutSec 30).Content
    if ($c -is [byte[]]) { [System.Text.Encoding]::UTF8.GetString($c) } else { [string]$c }
}

function FetchFile($url, $outFile) {
    Invoke-WebRequest -Uri $url -OutFile $outFile -UseBasicParsing -TimeoutSec 300
}

# A failed Invoke-WebRequest carries the server's whole response BODY in its
# message, so interpolating $_ into an error pastes an nginx 404 HTML page into
# the middle of the sentence. Render the status instead, and fall back to the
# raw message only for non-HTTP failures (DNS, TLS, timeout), where it is the
# useful part.
function WebErr($e) {
    $r = $e.Exception.Response
    if ($r -and $r.StatusCode) { return "HTTP $([int]$r.StatusCode)" }
    return $e.Exception.Message
}

# --- resolve ---------------------------------------------------------------
# `latest` is a pointer file holding a version string, not a directory. It does
# not exist until the first publish from `main`: every branch publish passes
# --no-latest, so on a freshly-published project this 404s and it reads like a
# store fault. It is not -- it means nothing has shipped from main yet.
if ($Version -eq 'latest') {
    try {
        $Version = (FetchText "$Store/artifacts/$name/latest").Trim()
    } catch {
        throw "cannot resolve $Store/artifacts/$name/latest ($(WebErr $_)). If nothing has been published from main yet that pointer does not exist -- pass -Version explicitly."
    }
}
$base = "$Store/artifacts/$name/$Version"
Write-Host "==> $name $Version from $Store"

# --- fetch + verify --------------------------------------------------------
# SHA256SUMS FIRST, deliberately. It is a few hundred bytes against the
# binary's ~1MB, and more importantly it is the check that produces a good
# error message: a version missing the Windows artifact surfaces as a sentence
# saying so, rather than a raw nginx 404 stack trace pointing at
# Invoke-WebRequest.
#
# SHA256SUMS is `<hash>  <filename>` per line, covering every artifact in the
# version -- Linux and Windows together, because they are one publish.
$sums = try { FetchText "$base/SHA256SUMS" } catch {
    throw "cannot read $base/SHA256SUMS ($(WebErr $_)) -- is '$Version' a version this store holds?"
}
$line = $sums -split "`r?`n" | Where-Object { $_ -match ('\s' + [regex]::Escape($file) + '$') } | Select-Object -First 1
if (-not $line) {
    throw "no SHA256SUMS entry for $file in $Version -- that version carries no Windows binary. Pass -Version for one published by ``just publish``, which always publishes both."
}
$want = ($line -split '\s+')[0]

# Verify BEFORE touching the destination, so a bad download cannot leave the
# host worse off than it started. knarr verifies once before touching any host
# for the same reason.
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) "$name-$Version.exe"
FetchFile "$base/$file" $tmp

$got = (Get-FileHash -Algorithm SHA256 $tmp).Hash.ToLower()
if ($got -ne $want.ToLower()) {
    Remove-Item $tmp -Force
    throw "SHA256 mismatch for ${file}: want $want, got $got"
}
Write-Host "    sha256 ok  $got"

# --- install ---------------------------------------------------------------
# Rotate the outgoing binary aside rather than overwriting it: that is the
# rollback that still works when the store is down, and it mirrors what knarr
# does on the Linux hosts (`/usr/local/bin/kdash-pub.prev`).
$destDir = Split-Path -Parent $Dest
if (-not (Test-Path $destDir)) { New-Item -ItemType Directory -Path $destDir -Force | Out-Null }
if (Test-Path $Dest) { Move-Item $Dest "$Dest.prev" -Force }
Move-Item $tmp $Dest -Force
Write-Host "    installed  $Dest"

# --- confirm ---------------------------------------------------------------
# Re-read the installed binary and require the store label to appear in its
# output. This is knarr's confirm step, and it is why build.rs emits the label
# verbatim: a `0.1.0 (abc1234)` shape would fail this on a perfectly good
# install.
$stamp = (& $Dest --version 2>&1 | Out-String).Trim()
if ($stamp -notlike "*$Version*") {
    throw "confirm failed: installed binary reports '$stamp', which does not contain '$Version'"
}
Write-Host "    confirmed  $stamp"

# --- housekeeping ----------------------------------------------------------
# A stale copy anywhere else on PATH is a landmine: it shadows nothing today
# (C:\tools\bin sorts first) but it will outlive the memory of why it is there.
# The kpolice copy of this script found one in .cargo\bin from an old
# `cargo install --path .`; the same check costs nothing here.
$stale = Join-Path $env:USERPROFILE '.cargo\bin\kdash-pub.exe'
if (Test-Path $stale) {
    Remove-Item $stale -Force
    Write-Host "    removed stale copy  $stale"
}

Write-Host "==> ok"
