$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
$AppManifest = Join-Path $Root "app/Cargo.toml"
$BuildDir = Join-Path $Root "build/native"
$DistDir = Join-Path $Root "dist"

Write-Host "Building End-scaling (Rust UI + C++/Direct3D 11 core)..."
cargo build --manifest-path $AppManifest --release

Write-Host "Building native smoke-test target..."
cmake --build $BuildDir --config Release
ctest --test-dir $BuildDir -C Release --output-on-failure

if (Test-Path $DistDir) { Remove-Item $DistDir -Recurse -Force }
New-Item -ItemType Directory -Path $DistDir | Out-Null
Copy-Item (Join-Path $Root "app/target/release/EndScaling.exe") $DistDir
Copy-Item (Join-Path $Root "app/target/release/shaders") (Join-Path $DistDir "shaders") -Recurse
Copy-Item (Join-Path $Root "app/target/release/third_party") (Join-Path $DistDir "third_party") -Recurse

Write-Host "Build ready: $DistDir/EndScaling.exe"
