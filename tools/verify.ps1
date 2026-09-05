param([switch]$Gpu)
$ErrorActionPreference='Stop'
$rtsRoot=Split-Path -Parent $PSScriptRoot
& "$rtsRoot/third_party/cmake/bin/ctest.exe" --test-dir "$rtsRoot/build" -C Release --output-on-failure
if($LASTEXITCODE){throw 'Core gameplay tests failed'}
$rtsValidator=Join-Path $rtsRoot 'third_party/validation/spirv-val.exe'
if(Test-Path -LiteralPath $rtsValidator){
  foreach($rtsShader in (Get-ChildItem -LiteralPath "$rtsRoot/build/shaders" -Filter '*.spv')){
    & $rtsValidator --target-env vulkan1.2 $rtsShader.FullName
    if($LASTEXITCODE){throw "Invalid SPIR-V: $($rtsShader.Name)"}
  }
}
if($Gpu){
  & "$rtsRoot/build/bin/Release/Afterlight.exe" --smoke --width 960 --height 600
  if($LASTEXITCODE){throw 'GPU smoke test failed'}
}
