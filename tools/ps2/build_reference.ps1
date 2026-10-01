param([string]$Dependencies = 'D:/AI-projects/SRB2B-plus/build/deps/vcpkg_installed')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
Push-Location $repo
try {
    cmake -S . -B build/pc-golden -G 'Visual Studio 18 2026' -A x64 `
        -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake `
        -DVCPKG_MANIFEST_MODE=OFF "-DVCPKG_INSTALLED_DIR=$Dependencies" `
        -DSRB2_CONFIG_HWRENDER=OFF -DSRB2_CONFIG_USE_GME=OFF `
        -DSRB2_CONFIG_STATIC_STDLIB=OFF -DSRB2_CONFIG_PS2REF=ON *> build/pc-golden/configure.log
    if ($LASTEXITCODE -ne 0) { throw 'PC reference configure failed; see configure.log' }
    cmake --build build/pc-golden --config Release --parallel 6 *> build/pc-golden/build-final.log
    if ($LASTEXITCODE -ne 0) { throw 'PC reference build failed; see build-final.log' }
} finally { Pop-Location }
