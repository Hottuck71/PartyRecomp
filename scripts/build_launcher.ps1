# Builds Launcher.exe, PairRemote.exe and PairJoyCon.exe with the toolkit's own GCC, then copies the launcher's parts into the toolkit
# (builder/src, builder/game.mk). Dear ImGui and SDL3's headers come from a local aurora/ImGui checkout.
#   scripts/build_launcher.ps1 -Toolkit "D:\Wii Party PC" -ImGui <imgui folder> -Sdl <SDL folder with include\SDL3>
param(
    [Parameter(Mandatory)] [string]$Toolkit,
    [Parameter(Mandatory)] [string]$ImGui,
    [Parameter(Mandatory)] [string]$Sdl
)
$ErrorActionPreference = 'Continue' # compiler warnings arrive on stderr; failures are caught by exit code below
$native = Join-Path $PSScriptRoot '..\native' | Resolve-Path
$bin = Join-Path $Toolkit 'builder\toolchain\bin'
$env:PATH = "$bin;$env:PATH"
$sysroot = "--sysroot=$(Join-Path $Toolkit 'builder\toolchain')"
$obj = Join-Path $Toolkit 'work\obj-launcher'
# Source paths reach the programs through assert messages: keep this PC's folders (and its user name) out of them.
$strip = "-ffile-prefix-map=$ImGui=imgui", "-ffile-prefix-map=$Sdl=SDL", "-ffile-prefix-map=$native=.", "-ffile-prefix-map=$Toolkit=toolkit"
New-Item -ItemType Directory -Force $obj | Out-Null

Push-Location $native
try {
    & gcc $sysroot @strip -O2 -Isrc -c src/real_remote.c -o "$obj\real_remote.o"
    if ($LASTEXITCODE) { throw 'real_remote.c' }
    & gcc $sysroot @strip -O2 -Isrc -c src/joycon.c -o "$obj\joycon.o"
    if ($LASTEXITCODE) { throw 'joycon.c' }
    if (-not (Test-Path "$obj\mii_art.o") -or (Get-Item launcher\mii_art.cpp).LastWriteTime -gt (Get-Item "$obj\mii_art.o").LastWriteTime) {
        & g++ $sysroot @strip -std=c++17 -O1 -c launcher/mii_art.cpp -o "$obj\mii_art.o"
        if ($LASTEXITCODE) { throw 'mii_art.cpp' }
    }
    # windres runs its preprocessor through the shell, which can't take paths with spaces: relative ones only.
    $inc = Resolve-Path -Relative (Join-Path $Toolkit 'builder\toolchain\include')
    $rcobj = (Resolve-Path -Relative $obj) + '\launcher_rc.o'
    & windres "-I$inc" launcher/launcher.rc -O coff -o $rcobj
    if ($LASTEXITCODE) { throw 'launcher.rc' }
    $srcs = 'launcher.cpp', 'mii_db.cpp', 'mii_maker.cpp', 'mii_render.cpp', 'room_code.cpp', 'remote_tab.cpp', 'joycon_tab.cpp', 'builder.cpp' |
        ForEach-Object { "launcher/$_" }
    $srcs += 'imgui.cpp', 'imgui_draw.cpp', 'imgui_tables.cpp', 'imgui_widgets.cpp' | ForEach-Object { Join-Path $ImGui $_ }
    $srcs += 'imgui_impl_sdl3.cpp', 'imgui_impl_sdlrenderer3.cpp' | ForEach-Object { Join-Path $ImGui "backends\$_" }
    & g++ $sysroot @strip -std=c++17 -O2 -Wall -Wno-unused-function -Ilauncher -Isrc "-I$ImGui" "-I$ImGui\backends" "-I$Sdl\include" @srcs `
        "$obj\real_remote.o" "$obj\joycon.o" "$obj\mii_art.o" "$obj\launcher_rc.o" (Join-Path $Toolkit 'SDL3.dll') -o (Join-Path $Toolkit 'Launcher.exe') `
        -mwindows -static-libgcc -static-libstdc++ -lws2_32 -lbcrypt -lhid -lsetupapi -ldwmapi
    if ($LASTEXITCODE) { throw 'Launcher.exe' }
    & gcc $sysroot @strip -O2 -Wall -Isrc tools/pair_remote.c src/real_remote.c -o (Join-Path $Toolkit 'PairRemote.exe') -static-libgcc -lbthprops -lhid -lsetupapi
    if ($LASTEXITCODE) { throw 'PairRemote.exe' }
    & gcc $sysroot @strip -O2 -Wall -Isrc tools/pair_joycon.c src/joycon.c -o (Join-Path $Toolkit 'PairJoyCon.exe') -static-libgcc -lbthprops -lhid -lsetupapi -lm
    if ($LASTEXITCODE) { throw 'PairJoyCon.exe' }
    Copy-Item src\* (Join-Path $Toolkit 'builder\src') -Force
    Copy-Item replacements.txt (Join-Path $Toolkit 'builder\src\replacements.txt') -Force
    Copy-Item builder\game.mk (Join-Path $Toolkit 'builder\game.mk') -Force
    Copy-Item include (Join-Path $Toolkit 'builder') -Recurse -Force # SDL3's headers, for gamepad.c
    'Launcher.exe, PairRemote.exe and PairJoyCon.exe built; builder updated.'
} finally { Pop-Location }
