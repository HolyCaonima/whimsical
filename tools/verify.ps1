param([switch]$Gpu)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
& "$root/third_party/cmake/bin/ctest.exe" --test-dir "$root/build" -C Release --output-on-failure
if($LASTEXITCODE){throw 'Core gameplay tests failed'}
$validator=Join-Path $root 'third_party/validation/spirv-val.exe'
if(Test-Path -LiteralPath $validator){
  foreach($shader in (Get-ChildItem -LiteralPath "$root/build/shaders" -Filter '*.spv')){
    & $validator --target-env vulkan1.2 $shader.FullName
    if($LASTEXITCODE){throw "Invalid SPIR-V: $($shader.Name)"}
  }
}
if($Gpu){
  & "$root/build/bin/Release/Whimsical.exe" --smoke --width 960 --height 600
  if($LASTEXITCODE){throw 'GPU smoke test failed'}
}
