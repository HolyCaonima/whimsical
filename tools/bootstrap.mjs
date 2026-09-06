import { mkdir, writeFile, readFile, rename } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { spawnSync } from 'node:child_process';
import path from 'node:path';
const root = path.resolve(import.meta.dirname, '..');
const deps = [
  ['onnxruntime', 'https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/onnxruntime-win-x64-1.20.1.zip', 'onnxruntime-win-x64-1.20.1'],
  ['cmake', 'https://github.com/Kitware/CMake/releases/download/v3.31.8/cmake-3.31.8-windows-x86_64.zip', 'cmake-3.31.8-windows-x86_64'],
  ['vulkan', 'https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.3.290.zip', 'Vulkan-Headers-1.3.290'],
  ['volk', 'https://github.com/zeux/volk/archive/refs/tags/vulkan-sdk-1.3.290.0.zip', 'volk-vulkan-sdk-1.3.290.0'],
  ['glm', 'https://github.com/g-truc/glm/archive/refs/tags/1.0.1.zip', 'glm-1.0.1'],
  ['duktape', 'https://duktape.org/duktape-2.7.0.tar.xz', 'duktape-2.7.0'],
  ['glslang', 'https://github.com/KhronosGroup/glslang/releases/download/16.5.0/glslang-16.5.0-windows-x86_64-release.zip', ''],
  ['dxc', 'https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.8.2505/dxc_2025_05_24.zip', ''],
  ['nrd', 'https://github.com/NVIDIA-RTX/NRD/archive/refs/tags/v4.17.3.zip', 'NRD-4.17.3'],
  ['shadermake', 'https://github.com/NVIDIA-RTX/ShaderMake/archive/18f5a344e7ca8fa65daaf079d07bc8ce38453e05.zip', 'ShaderMake-18f5a344e7ca8fa65daaf079d07bc8ce38453e05'],
  ['mathlib', 'https://github.com/NVIDIA-RTX/MathLib/archive/refs/tags/v11.zip', 'MathLib-11'],
];
await mkdir(path.join(root, 'third_party'), {recursive:true});
await mkdir(path.join(root, 'tools/cache'), {recursive:true});
const lockPath = path.join(root, 'tools/dependencies.lock.json');
const lock = existsSync(lockPath) ? JSON.parse(await readFile(lockPath, 'utf8')) : {};
for (let start=0; start<deps.length; start+=3) {
  await Promise.all(deps.slice(start,start+3).map(async ([name,url,folder]) => {
    const destination=path.join(root,'third_party',name);
    if(existsSync(path.join(destination,'.ready'))) {lock[name]={url,sha256:(await readFile(path.join(destination,'.ready'),'utf8')).trim()};return;}
    console.log('Fetching '+name);
    const archive=path.join(root,'tools/cache',name+(url.endsWith('.xz')?'.tar.xz':'.zip'));
    let bytes;
    if(existsSync(archive)) bytes=await readFile(archive);
    else {const response=await fetch(url);if(!response.ok) throw Error(url+': '+response.status);bytes=Buffer.from(await response.arrayBuffer());await writeFile(archive,bytes);}
    const sha256=createHash('sha256').update(bytes).digest('hex');
    if(lock[name] && (lock[name].url!==url || lock[name].sha256!==sha256)) throw Error('Dependency integrity mismatch: '+name);
    lock[name]={url,sha256};
    const extraction=folder?path.join(root,'tools/cache',name+'-extract'):destination;
    await mkdir(extraction,{recursive:true});
    const result=url.endsWith('.xz')
      ?spawnSync(path.join(root,'third_party/cmake/bin/cmake.exe'),['-E','tar','xf',archive],{cwd:extraction,stdio:'inherit',windowsHide:true})
      :spawnSync('tar.exe',['-xf',archive,'-C',extraction],{stdio:'inherit',windowsHide:true});
    if(result.status!==0) throw Error('Extraction failed: '+name);
    if(folder) await rename(path.join(extraction,folder),destination);
    await writeFile(path.join(destination,'.ready'),sha256+'\n');
    console.log('Ready '+name);
  }));
}
await writeFile(lockPath,JSON.stringify(lock,null,2)+'\n');
console.log('All dependencies ready. Run tools/build.ps1');
