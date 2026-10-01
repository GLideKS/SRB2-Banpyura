param(
    [string]$Dependencies = 'D:/AI-projects/SRB2B-plus/build/deps/vcpkg_installed',
    [string]$Lz4Source = '',
    [string]$Lz4Include = '',
    [int]$Jobs = 6
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$out = Join-Path $repo 'build/host-profile-g1'
if (-not (Test-Path -LiteralPath (Join-Path $repo 'build'))) { throw 'Missing workspace build directory' }
[System.IO.Directory]::CreateDirectory($out) | Out-Null
if (-not $Lz4Source -and -not (Test-Path -LiteralPath (Join-Path $Dependencies 'x64-windows/lib/lz4.lib'))) {
    $externalLz4 = Join-Path $repo 'build/scratch/lz4src/lz4-4.4.5/lz4libs/lz4.c'
    if ((Test-Path -LiteralPath $externalLz4) -and (Test-Path -LiteralPath 'D:/ps2dev/ps2sdk/ports/include/lz4.h')) {
        $Lz4Source = $externalLz4
        if (-not $Lz4Include) { $Lz4Include = 'D:/ps2dev/ps2sdk/ports/include' }
    } else {
        throw 'No existing host LZ4 dependency; supply -Lz4Source and -Lz4Include'
    }
}
$configure = @('-S', $repo, '-B', $out, '-G', 'Visual Studio 18 2026', '-A', 'x64',
    '-DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake',
    '-DVCPKG_MANIFEST_MODE=OFF', "-DVCPKG_INSTALLED_DIR=$Dependencies",
    '-DSRB2_CONFIG_HWRENDER=OFF', '-DSRB2_CONFIG_USE_GME=OFF',
    '-DSRB2_CONFIG_STATIC_STDLIB=OFF', '-DSRB2_CONFIG_PS2REF=ON', '-DSRB2_CONFIG_PS2PROFILE=ON',
    '-DSRB2_SDL2_EXE_NAME=srb2-host-profile-g1',
    "-DSRB2_HOST_PROFILE_LZ4_SOURCE=$Lz4Source", "-DSRB2_HOST_PROFILE_LZ4_INCLUDE_DIR=$Lz4Include")
$build = @('--build', $out, '--config', 'Release', '--parallel', "$Jobs")
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
@{ configure = @('cmake') + $configure; build = @('cmake') + $build } |
    ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $out "commands-$stamp.json")
$ErrorActionPreference = 'Continue' # PowerShell 5.1 wraps native stderr warnings as errors.
& cmake @configure 2>&1 | ForEach-Object { "$_" } | Out-File -Encoding UTF8 -LiteralPath (Join-Path $out "configure-$stamp.log")
if ($LASTEXITCODE -ne 0) { throw "Host profile configure failed; see $out/configure-$stamp.log" }
& cmake @build 2>&1 | ForEach-Object { "$_" } | Out-File -Encoding UTF8 -LiteralPath (Join-Path $out "build-$stamp.log")
if ($LASTEXITCODE -ne 0) { throw "Host profile build failed; see $out/build-$stamp.log" }
$ErrorActionPreference = 'Stop'
"Built $out/bin/Release/srb2-host-profile-g1.exe"
