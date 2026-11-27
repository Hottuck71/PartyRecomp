# Makes a PartyRecomp release from a working toolkit folder: what goes up on GitHub.
#
#   <toolkit>\work\release\PartyRecomp\                 the release, as a folder
#   <toolkit>\work\release\PartyRecomp-<v>.zip          the same, zipped
#   <toolkit>\work\release\PartyRecomp-<v>-Setup.exe    the zip with an installer in front (native/tools/setup.c)
#
# Only the toolkit's own parts are copied, from a fixed list: the launcher, the translator, the compiler, the runtime
# source, the relay, the licences, and the help. Nothing from a player's copy of the game (game\, build\, the built
# game, analysis\), and nothing personal (save\, launcher.ini, logs, work\) can get in, because none of it is on the
# list. Every file is then searched for this PC's user name and user folders, and the release stops if one turns up.
#
#   powershell -ExecutionPolicy Bypass -File scripts\make_release.ps1 -Toolkit "D:\Wii Party PC" [-Version 0.1.0]
param(
    [Parameter(Mandatory)] [string]$Toolkit,
    [string]$Version = '',
    [string[]]$Forbid = @() # more text that mustn't appear anywhere (a home address, say)
)
$ErrorActionPreference = 'Stop'
if (-not $Version) { $Version = (Get-Content (Join-Path $Toolkit 'builder\version.txt') -Raw).Trim() }
$out = Join-Path $Toolkit 'work\release'
$stage = Join-Path $out 'PartyRecomp'
$native = Join-Path $PSScriptRoot '..\native' | Resolve-Path
if (Test-Path $out) { Remove-Item -LiteralPath $out -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null

# 1. The release's files, from the list.
$files = @('Launcher.exe', 'PairRemote.exe', 'PairJoyCon.exe', 'README.txt') + (Get-ChildItem $Toolkit -Filter *.dll | ForEach-Object Name)
foreach ($f in $files) { Copy-Item (Join-Path $Toolkit $f) $stage }
foreach ($d in 'builder', 'relay', 'licenses', 'nand-template') { Copy-Item (Join-Path $Toolkit $d) $stage -Recurse }
Set-Content (Join-Path $stage 'release.txt') "PartyRecomp $Version. This file keeps the launcher's save, build, and reports inside this folder. Don't delete it." -Encoding ascii
foreach ($stray in 'builder\src\*.bak', 'builder\src\*.orig') { Remove-Item (Join-Path $stage $stray) -ErrorAction SilentlyContinue }

# 2. Nothing personal: this PC's user name as a path part, any user folder, and anything asked for.
$name = [regex]::Escape($env:USERNAME)
# (Windows' generic Default and Public user folders aren't anyone's: GNU make names c:/users/default.)
$patterns = @("(?i)[\\/]$name([\\/]|\b)", '(?i)[A-Z]:[\\/]Users[\\/](?!default\b|public\b)') +
    ($Forbid | ForEach-Object { [regex]::Escape($_) })
$found = @()
foreach ($file in Get-ChildItem $stage -Recurse -File) {
    $text = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($file.FullName))
    $wide = [Text.Encoding]::Unicode.GetString([IO.File]::ReadAllBytes($file.FullName))
    foreach ($p in $patterns) {
        if ($text -match $p -or $wide -match $p) { $found += "$($file.FullName.Substring($stage.Length + 1)): $p" }
    }
}
if ($found) { $found | ForEach-Object { Write-Host "personal: $_" }; throw 'Something personal is in the release; nothing was packaged' }
'Checked: nothing personal in ' + (Get-ChildItem $stage -Recurse -File).Count + ' files.'

# 3. The zip, with Windows' own tar (it writes a zip when the name ends in .zip).
$zip = Join-Path $out "PartyRecomp-$Version.zip"
& "$env:SystemRoot\System32\tar.exe" -a -c -f $zip -C $out PartyRecomp
if ($LASTEXITCODE) { throw 'zip' }

# 4. The installer, built with the toolkit's own GCC, with the zip appended and a trailer saying where it starts.
$bin = Join-Path $Toolkit 'builder\toolchain\bin'
$env:PATH = "$bin;$env:PATH"
$sysroot = "--sysroot=$(Join-Path $Toolkit 'builder\toolchain')"
$work = Join-Path $out 'setup-build'
New-Item -ItemType Directory -Force $work | Out-Null
Push-Location (Join-Path $native 'tools')
try {
    $inc = Resolve-Path -Relative (Join-Path $Toolkit 'builder\toolchain\include')
    $rcobj = (Resolve-Path -Relative $work) + '\setup_rc.o'
    & windres "-I$inc" setup.rc -O coff -o $rcobj
    if ($LASTEXITCODE) { throw 'setup.rc' }
    $exe = Join-Path $work 'setup.exe'
    & gcc $sysroot "-ffile-prefix-map=$native=." -O2 -Wall -municode -mwindows "-DSETUP_VERSION=L\`"$Version\`"" setup.c $rcobj -o $exe -lole32 -lshell32 -luuid -lcomctl32
    if ($LASTEXITCODE) { throw 'setup.c' }
} finally { Pop-Location }
$setup = Join-Path $out "PartyRecomp-$Version-Setup.exe"
Copy-Item $exe $setup
$offset = (Get-Item $setup).Length
$stream = [IO.File]::Open($setup, 'Append')
try {
    $in = [IO.File]::OpenRead($zip)
    try { $in.CopyTo($stream) } finally { $in.Close() }
    $trailer = [Text.Encoding]::ASCII.GetBytes('PRSETUP1') + [BitConverter]::GetBytes([uint64]$offset)
    $stream.Write($trailer, 0, $trailer.Length)
} finally { $stream.Close() }
Remove-Item -LiteralPath $work -Recurse -Force
foreach ($f in $zip, $setup) { '{0}  {1:N0} MB  SHA-256 {2}' -f (Split-Path $f -Leaf), ((Get-Item $f).Length / 1MB), (Get-FileHash $f).Hash }
