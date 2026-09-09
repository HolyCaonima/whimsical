param([ValidateSet('Debug','Release')][string]$Configuration='Release', [switch]$Test)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$cmake=Join-Path $root 'third_party/cmake/bin/cmake.exe'
if(-not (Test-Path -LiteralPath $cmake)){throw 'Run node tools/bootstrap.mjs first'}
& $cmake -S $root -B "$root/build" -G 'Visual Studio 16 2019' -A x64
if($LASTEXITCODE){throw 'CMake configuration failed'}
& $cmake --build "$root/build" --config $Configuration --parallel 8
if($LASTEXITCODE){throw 'Build failed'}
if($Test){
  & "$root/third_party/cmake/bin/ctest.exe" --test-dir "$root/build" -C $Configuration --output-on-failure
  if($LASTEXITCODE){throw 'Tests failed'}
}
