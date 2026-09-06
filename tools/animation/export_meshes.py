"""Bake the original GLB skins into engine meshes; source files remain untouched."""
from asset_format import read_asset, write_asset
import io
import json
import struct
from pathlib import Path
import numpy as np
from PIL import Image
from export_onnx import ROOT, UPSTREAM, quaternion_matrix


def export(source, controller, output):
    data = source.read_bytes()
    size = struct.unpack_from('<I', data, 12)[0]
    gltf = json.loads(data[20:20+size])
    blob = data[28+size:]
    def accessor(index):
        a = gltf['accessors'][index]; v = gltf['bufferViews'][a['bufferView']]
        dtype = {5126:'<f4',5125:'<u4',5123:'<u2',5121:'u1'}[a['componentType']]
        width = {'SCALAR':1,'VEC2':2,'VEC3':3,'VEC4':4,'MAT4':16}[a['type']]
        offset = v.get('byteOffset',0)+a.get('byteOffset',0)
        itemsize = np.dtype(dtype).itemsize
        values = np.ndarray((a['count'],width), dtype=dtype, buffer=blob, offset=offset,
                            strides=(v.get('byteStride',itemsize*width),itemsize)).copy()
        if a.get('normalized') and a['componentType'] != 5126:
            values = values.astype(np.float32)/np.iinfo(dtype).max
        return values
    nodes = gltf['nodes']; parents = {c:i for i,n in enumerate(nodes) for c in n.get('children',[])}
    worlds = {}
    def world(i):
        if i not in worlds:
            n=nodes[i]; t=np.eye(4);t[:3,:3]=quaternion_matrix(n.get('rotation',[0,0,0,1]));t[:3,3]=n.get('translation',[0,0,0])
            worlds[i]=world(parents[i])@t if i in parents else t
        return worlds[i]
    metadata=json.loads(read_asset(controller)[1]); joints=metadata['joints']; names=[j['name'] for j in joints]
    rest=[]
    for j in joints:
        t=np.eye(4);t[:3,:3]=quaternion_matrix(j['rotation_xyzw']);t[:3,3]=j['position']
        rest.append(rest[j['parent']]@t if j['parent']>=0 else t)
    bindings=[]; vertices=[]; indices=[]
    for node in nodes:
        if 'mesh' not in node or 'skin' not in node: continue
        skin=gltf['skins'][node['skin']]; inverse=accessor(skin['inverseBindMatrices']).reshape(-1,4,4).transpose(0,2,1)
        first_binding=len(bindings)
        for k,source_joint in enumerate(skin['joints']):
            ancestor=source_joint
            while nodes[ancestor].get('name') not in names: ancestor=parents[ancestor]
            target=names.index(nodes[ancestor]['name'])
            correction=np.linalg.inv(rest[target])@world(source_joint)@inverse[k]
            bindings.append((names[target],correction))
        for prim in gltf['meshes'][node['mesh']]['primitives']:
            assert prim.get('mode',4)==4
            attrs=prim['attributes'];p=accessor(attrs['POSITION']);n=accessor(attrs['NORMAL'])
            weights=accessor(attrs['WEIGHTS_0']);weights/=weights.sum(axis=1,keepdims=True)
            ids=accessor(attrs['JOINTS_0']).astype(np.uint32)+first_binding
            material=gltf['materials'][prim.get('material',0)].get('pbrMetallicRoughness',{})
            colors=np.tile(material.get('baseColorFactor',[1,1,1,1])[:3],(len(p),1))
            if 'baseColorTexture' in material:
                texture=gltf['textures'][material['baseColorTexture']['index']]
                image=gltf['images'][texture['source']];view=gltf['bufferViews'][image['bufferView']]
                pixels=np.asarray(Image.open(io.BytesIO(blob[view.get('byteOffset',0):view.get('byteOffset',0)+view['byteLength']])).convert('RGB'))/255.
                uv=accessor(attrs['TEXCOORD_0']);h,w=pixels.shape[:2]
                tex=pixels[np.minimum((uv[:,1]%1*h).astype(int),h-1),np.minimum((uv[:,0]%1*w).astype(int),w-1)]
                colors*=np.where(tex<=.04045,tex/12.92,((tex+.055)/1.055)**2.4)
            base=len(vertices)
            for a,b,c,j,v in zip(p,n,colors,ids,weights): vertices.append((a,b,c,j,v))
            indices.extend((accessor(prim['indices']).reshape(-1)+base).tolist())
    output.parent.mkdir(parents=True,exist_ok=True)
    with io.BytesIO() as f:
        f.write(b'SKN1');f.write(struct.pack('<III',len(vertices),len(indices),len(bindings)))
        for name,bind in bindings:
            s=name.encode();f.write(struct.pack('<I',len(s)));f.write(s);f.write(bind.T.astype('<f4').tobytes())
        for p,n,c,j,w in vertices:
            f.write(np.concatenate((p,n,c)).astype('<f4').tobytes());f.write(j.astype('<u4').tobytes());f.write(w.astype('<f4').tobytes())
        f.write(np.asarray(indices,dtype='<u4').tobytes())
        write_asset(output, "SkinnedMesh", f.getvalue(), {"license": "CC-BY-NC-4.0"})
    # Verify bind-space bounds using the same palette as the native implementation.
    palette=[rest[names.index(name)]@b for name,b in bindings]
    deformed=np.array([sum(w[k]*(palette[j[k]]@np.r_[p,1])[:3] for k in range(4)) for p,n,c,j,w in vertices])
    print(output.name,len(vertices),'vertices',len(indices)//3,'triangles; bounds',deformed.min(0),deformed.max(0))


if __name__=='__main__':
    for name,path in [('biped','Geno/Model.glb'),('dog','Quadruped/Dog.glb')]:
        export(UPSTREAM/'Demos/_ASSETS_'/path,ROOT/'Projects/Afterlight/Content/animations/ai4animation'/('biped' if name=='biped' else 'quadruped')/'metadata.asset',ROOT/'Projects/Afterlight/Content/models'/f'{name}.asset')
