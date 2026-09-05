param([ValidateSet('Debug','Release')][string]$Configuration='Release', [switch]$Test)
$ErrorActionPreference='Stop'
$rtsRoot=Split-Path -Parent $PSScriptRoot
$rtsCmake=Join-Path $rtsRoot 'third_party/cmake/bin/cmake.exe'
if(-not (Test-Path -LiteralPath $rtsCmake)){throw 'Run node tools/bootstrap.mjs first'}
& $rtsCmake -S $rtsRoot -B "$rtsRoot/build" -G 'Visual Studio 16 2019' -A x64
if($LASTEXITCODE){throw 'CMake configuration failed'}
& $rtsCmake --build "$rtsRoot/build" --config $Configuration --parallel 8
if($LASTEXITCODE){throw 'Build failed'}
if($Test){
  & "$rtsRoot/third_party/cmake/bin/ctest.exe" --test-dir "$rtsRoot/build" -C $Configuration --output-on-failure
  if($LASTEXITCODE){throw 'Tests failed'}
}
