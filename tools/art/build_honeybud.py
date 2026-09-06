"""Original Honeybud Court kit. Execute inside Blender through mcp_call.py.

Metres, Blender Z-up -> Afterlight Y-up. Each reusable kit part is one material
primitive; linked instances share both the exported mesh and the renderer BLAS.
"""
import bpy
import math
import random
import json
import struct
import sys
from pathlib import Path
from mathutils import Vector, Matrix
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
CONTENT = ROOT / 'Projects/Afterlight/Content'
SOURCE = ROOT / 'Projects/Afterlight/SourceArt/Honeybud'
SOURCE.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(ROOT / 'tools/animation'))
from asset_format import write_asset, read_asset, asset_reference
random.seed(37)
for obj in list(bpy.data.objects):bpy.data.objects.remove(obj,do_unlink=True)
for material in list(bpy.data.materials):bpy.data.materials.remove(material)
for image in list(bpy.data.images):
    if image.type != 'RENDER_RESULT':bpy.data.images.remove(image)
for collection in list(bpy.data.collections):bpy.data.collections.remove(collection)
stage = bpy.data.collections.new('Honeybud Court')
bpy.context.scene.collection.children.link(stage)
bpy.context.view_layer.active_layer_collection=bpy.context.view_layer.layer_collection.children[stage.name]
library = bpy.data.collections.new('Reusable kit • source meshes')
bpy.context.scene.collection.children.link(library)
library.hide_render = True
library.hide_viewport = True
MATS = {}
MATREF = {}
KIT = {}
PLACEMENTS = []
active = []

def texture_set(name, kind):
    size = 1024
    y,x = np.mgrid[0:size,0:size].astype(np.float32) / size
    rng = np.random.default_rng(12)
    noise = rng.random((size,size)).astype(np.float32)
    base_rgb=None; metallic=np.zeros_like(x); roughness=None
    if kind in ('knit','cushion'):
        # Interlocking diagonal yarn loops, with finer fibres running along them.
        # Two diagonal yarn legs form a stockinette V. Fibres follow each strand.
        xx=(x*(32 if kind=='cushion' else 16))%1; yy=(y*(40 if kind=='cushion' else 20))%1
        centre=.16+.33*np.abs(yy*2-1)
        distance=np.minimum(np.abs(xx-centre),np.abs(xx-(1-centre)))
        yarn=np.exp(-(distance/.125)**2)
        fibre=np.sin((x*160+y*100)*math.tau)*.027
        height=.12+.70*yarn+fibre+noise*.035
        color=.72+.25*yarn+noise*.025
        if kind=='cushion':
            # Running stitch and shallow seam in UV space, not separate mesh threads.
            edge=(np.abs(2*x-1)**2.8+np.abs(2*y-1)**2.8)**(1/2.8)
            seam=np.exp(-((edge-.91)/.009)**2)
            dash=(np.sin(np.arctan2(y-.5,x-.5)*96)>0).astype(float)
            height-=seam*.20; height+=seam*dash*.35
            color=color*(1-seam*.24)+seam*dash*.18
    elif kind == 'wood':
        phase=y*28+.5*np.sin(x*math.tau)+.14*np.sin(x*3*math.tau)
        knot=np.exp(-((x-.53)**2/.012+(y-.43)**2/.024))
        phase+=knot*3
        grain=np.sin(phase*math.tau)
        fine=np.sin((phase*4+x*.6)*math.tau)
        height=.5+.055*grain+.02*fine+noise*.02
        color=.87+.045*grain+.012*fine+.035*noise-knot*.08
    elif kind == 'gingham':
        a=(np.sin(x*16*math.pi)>0).astype(float); b=(np.sin(y*16*math.pi)>0).astype(float)
        color=.65+.16*a+.16*b
        height=.5+.12*np.sin(x*256*math.pi)*np.cos(y*256*math.pi)+noise*.05
    elif kind == 'grass':
        height=.4+.10*noise; color=.68+.13*noise
        # Hundreds of short blades are painted into a tile; mesh tufts provide silhouettes.
        for j in range(420):
            px,py=rng.random(2); a=rng.random()*math.tau
            dx=(x-px+.5)%1-.5;dy=(y-py+.5)%1-.5
            along=dx*math.cos(a)+dy*math.sin(a);across=-dx*math.sin(a)+dy*math.cos(a)
            blade=np.exp(-(across/.0018)**2-(along/.014)**8)
            height+=blade*.25;color+=blade*rng.uniform(-.16,.13)
    elif kind == 'leaf_surface':
        side=np.abs(x-.5)
        midrib=np.exp(-((x-.5)/.013)**2)
        branches=np.exp(-(np.sin((y-side*.65)*math.pi*8)/.10)**2)*np.clip(side*9,0,1)
        height=.4+.22*midrib+.08*branches+.02*noise
        color=.79+.13*y+.08*midrib+.045*branches
    elif kind == 'cup_glaze':
        angle=(x*8+.5)%1-.5
        t=np.clip((y-.15)/.61,0,1)
        width=.58/(math.tau)*8*.5*np.maximum(0,np.sin(math.pi*t))**.56
        distance=np.abs(angle)-width
        valid=((y>.15)&(y<.76)).astype(float)
        inset=np.clip(-distance*160,0,1)*valid
        gold=np.exp(-(distance/.018)**2)*valid
        height=.45+inset*.08+gold*.22+noise*.007
        body=np.array([.64,.77,.73]);petal=np.array([.66,.56,.67]);gilt=np.array([.86,.69,.35])
        base_rgb=body[None,None,:]*(1-inset[:,:,None])+petal[None,None,:]*inset[:,:,None]
        base_rgb=base_rgb*(1-gold[:,:,None])+gilt[None,None,:]*gold[:,:,None]
        color=np.ones_like(x);metallic=gold*.72;roughness=.74+.22*noise
    else:
        height=.45+.18*noise
        color=.86+height*.14
    strength=10 if kind in ('leaf_surface','cup_glaze','grass') else 3
    gx=(np.roll(height,-1,1)-np.roll(height,1,1))*strength
    gy=(np.roll(height,-1,0)-np.roll(height,1,0))*strength
    normal=np.stack((-gx,-gy,np.ones_like(gx)),axis=-1)
    normal/=np.linalg.norm(normal,axis=-1,keepdims=True)
    if base_rgb is None:base_rgb=np.stack((color,color,color),axis=-1)
    arrays = {'baseColor':np.concatenate((base_rgb,np.ones((size,size,1))),axis=-1),
              'normal':np.concatenate((normal*.5+.5,np.ones((size,size,1))),axis=-1),
              'orm':np.stack((np.ones_like(color),roughness if roughness is not None else .88+.12*noise,metallic,np.ones_like(color)),axis=-1)}
    result={}
    for channel,arr in arrays.items():
        if not np.isfinite(arr).all():raise ValueError(f'Non-finite authored pixels: {name}/{channel}')
        arr=np.clip(arr,0,1)
        image=bpy.data.images.new(name+'_'+channel,width=size,height=size)
        image.colorspace_settings.name='sRGB' if channel=='baseColor' else 'Non-Color'
        pixels=arr.copy()
        if channel=='baseColor':pixels[:,:,:3]=np.where(arr[:,:,:3]<=.04045,arr[:,:,:3]/12.92,((arr[:,:,:3]+.055)/1.055)**2.4)
        image.pixels.foreach_set(pixels.astype(np.float32).ravel())
        image.filepath_raw=str(CONTENT/'textures/Honeybud'/f'{name}_{channel}.png')
        Path(image.filepath_raw).parent.mkdir(parents=True,exist_ok=True)
        image.file_format='PNG'; image.save(); image.pack()
        path=CONTENT/'textures/Honeybud'/f'{name}_{channel}.asset'
        write_asset(path,'Texture',b'TEX1'+struct.pack('<II',size,size)+(arr*255+.5).astype(np.uint8).tobytes(),
                    metadata={'colorSpace':'sRGB' if channel=='baseColor' else 'linear','source':Path(image.filepath_raw).name})
        result[channel]=(image,asset_reference(path))
    return result

TEX={k:texture_set(k,k) for k in ['knit','wood','gingham','grass','plaster','cushion','leaf_surface','cup_glaze']}

def mat(name, color, rough=.65, metal=0, texture=None, uv=1, emission=None):
    m=bpy.data.materials.new(name); m.diffuse_color=(*color,1); m.use_nodes=True
    bs=m.node_tree.nodes.get('Principled BSDF')
    bs.inputs['Base Color'].default_value=(*color,1)
    bs.inputs['Roughness'].default_value=rough; bs.inputs['Metallic'].default_value=metal
    data={'albedoRoughness':[*color,rough],'emissionMetallic':[*(emission or [0,0,0]),metal],'surface':[uv,uv,.5,0]}
    if emission:
        bs.inputs['Emission Color'].default_value=(*emission,1); bs.inputs['Emission Strength'].default_value=1
    if texture:
        nodes=m.node_tree.nodes; links=m.node_tree.links
        coord=nodes.new('ShaderNodeTexCoord'); mapping=nodes.new('ShaderNodeVectorMath'); mapping.operation='SCALE'
        mapping.inputs[3].default_value=uv; links.new(coord.outputs['UV'],mapping.inputs[0])
        for channel,(image,ref) in TEX[texture].items():
            data[channel]=ref
            node=nodes.new('ShaderNodeTexImage'); node.image=image; links.new(mapping.outputs[0],node.inputs[0])
            if channel=='baseColor':
                mult=nodes.new('ShaderNodeMixRGB'); mult.blend_type='MULTIPLY'; mult.inputs[0].default_value=1; mult.inputs[2].default_value=(*color,1)
                links.new(node.outputs['Color'],mult.inputs[1]); links.new(mult.outputs[0],bs.inputs['Base Color'])
            elif channel=='normal':
                normal=nodes.new('ShaderNodeNormalMap'); normal.inputs[0].default_value=.5
                links.new(node.outputs[0],normal.inputs['Color']); links.new(normal.outputs[0],bs.inputs['Normal'])
            else:
                separate=nodes.new('ShaderNodeSeparateColor');links.new(node.outputs[0],separate.inputs[0])
                multiply=nodes.new('ShaderNodeMath');multiply.operation='MULTIPLY';multiply.inputs[1].default_value=rough
                links.new(separate.outputs[1],multiply.inputs[0]);links.new(multiply.outputs[0],bs.inputs['Roughness'])
                metallic=nodes.new('ShaderNodeMath');metallic.operation='MULTIPLY';metallic.inputs[1].default_value=metal
                links.new(separate.outputs[2],metallic.inputs[0]);links.new(metallic.outputs[0],bs.inputs['Metallic'])
    MATS[name]=m
    path=CONTENT/'materials/Honeybud'/f'{name}.asset'
    write_asset(path,'Material',json.dumps(data).encode())
    MATREF[name]=asset_reference(path)
    return m

mat('cream',(0.77,.59,.37),.83,texture='plaster',uv=3)
mat('ivory',(.93,.80,.58),.58)
mat('rose',(.48,.115,.10),.68,texture='plaster',uv=2)
mat('roof_light',(.68,.24,.17),.65)
mat('sage',(.22,.37,.19),.68,texture='wood',uv=2)
mat('wood',(.50,.28,.105),.7,texture='wood',uv=2)
mat('dark_wood',(.19,.09,.041),.73,texture='wood',uv=2)
mat('brass',(.72,.45,.12),.3,.65)
mat('ceramic',(.30,.53,.48),.24)
mat('water',(.16,.53,.59),.13,.12)
mat('leaf',(.22,.43,.073),.68,texture='leaf_surface')
mat('leaf_light',(.39,.58,.12),.7,texture='leaf_surface')
mat('grass',(.24,.40,.10),.94,texture='grass',uv=12)
mat('petal',(.69,.29,.36),.6)
mat('lavender',(.36,.25,.59),.65)
mat('honey',(.93,.56,.12),.81,texture='cushion',uv=1)
mat('linen',(.85,.72,.50),.94,texture='gingham',uv=2)
mat('blue_cloth',(.20,.38,.55),.9,texture='cushion',uv=1)
mat('soil',(.19,.105,.055),.98,texture='plaster',uv=3)
mat('stone',(.51,.39,.24),.85,texture='plaster',uv=2)
mat('glow',(1,.71,.25),.35,emission=[1.4,.67,.16])

def finish(o, name, material):
    o.name=name; o.data.materials.clear(); o.data.materials.append(MATS[material]); active.append(o)
    return o

def cube(name, p, size, material, bevel=.07, rot=(0,0,0)):
    bpy.ops.mesh.primitive_cube_add(size=1,location=p,rotation=rot)
    o=bpy.context.object; o.scale=size
    bpy.ops.object.transform_apply(location=False,rotation=False,scale=True)
    if bevel:
        mod=o.modifiers.new('Soft crafted edges','BEVEL'); mod.width=bevel; mod.segments=2
        mod=o.modifiers.new('Weighted corner normals','WEIGHTED_NORMAL')
    return finish(o,name,material)

def sphere(name,p,size,material,seg=24,rings=12):
    seg=min(seg,16);rings=min(rings,8)
    bpy.ops.mesh.primitive_uv_sphere_add(segments=seg,ring_count=rings,radius=1,location=p)
    o=bpy.context.object; o.scale=size
    for f in o.data.polygons:f.use_smooth=True
    return finish(o,name,material)

def cylinder(name,p,r,depth,material,vertices=32,rot=(0,0,0)):
    vertices=min(vertices,32)
    bpy.ops.mesh.primitive_cylinder_add(vertices=vertices,radius=r,depth=depth,location=p,rotation=rot)
    o=bpy.context.object
    mod=o.modifiers.new('Rounded rim','BEVEL'); mod.width=min(.045,depth*.15); mod.segments=2
    mod=o.modifiers.new('Normals','WEIGHTED_NORMAL')
    return finish(o,name,material)

def curve(name,points,r,material,cyclic=False):
    cu=bpy.data.curves.new(name,'CURVE'); cu.dimensions='3D';cu.resolution_u=1 if len(points)>12 else 4; cu.bevel_depth=r;cu.bevel_resolution=0 if r<.04 else 1
    s=cu.splines.new('BEZIER');s.bezier_points.add(len(points)-1)
    for b,p in zip(s.bezier_points,points):b.co=p;b.handle_left_type='AUTO';b.handle_right_type='AUTO'
    s.use_cyclic_u=cyclic
    o=bpy.data.objects.new(name,cu);stage.objects.link(o)
    return finish(o,name,material)

def ring(name,p,r,tube,material,rot=(0,0,0)):
    bpy.ops.mesh.primitive_torus_add(major_radius=r,minor_radius=tube,major_segments=32,minor_segments=6,location=p,rotation=rot)
    o=bpy.context.object
    for f in o.data.polygons:f.use_smooth=True
    return finish(o,name,material)

def lathe(name,profile,material,segments=64,p=(0,0,0),petals=0):
    segments=min(segments,48)
    verts=[];faces=[]
    for r,z in profile:
        for j in range(segments):
            a=j*math.tau/segments; rr=r*(1+.065*math.cos(a*petals)) if petals else r
            verts.append((p[0]+rr*math.cos(a),p[1]+rr*math.sin(a),p[2]+z))
    for k in range(len(profile)-1):
        for j in range(segments):
            face=(k*segments+j,k*segments+(j+1)%segments,(k+1)*segments+(j+1)%segments,(k+1)*segments+j)
            faces.append(tuple(reversed(face)) if profile[0][1]>profile[-1][1] else face)
    mesh=bpy.data.meshes.new(name);mesh.from_pydata(verts,[],faces);mesh.update()
    uv=mesh.uv_layers.new()
    for poly in mesh.polygons:
        poly.use_smooth=True
        for li in poly.loop_indices:
            vi=mesh.loops[li].vertex_index
            u=(vi%segments)/segments
            if poly.index%segments==segments-1 and u==0:u=1
            uv.data[li].uv=(u,(vi//segments)/(len(profile)-1))
    o=bpy.data.objects.new(name,mesh);stage.objects.link(o)
    return finish(o,name,material)

def text(name,body,p,size,material):
    cu=bpy.data.curves.new(name,'FONT');cu.body=body;cu.size=size;cu.align_x='CENTER';cu.extrude=.008;cu.bevel_depth=.003
    o=bpy.data.objects.new(name,cu);stage.objects.link(o);o.location=p;o.rotation_euler=(math.pi/2,0,0)
    return finish(o,name,material)

def begin():
    active.clear()

def end(key):
    # Bake only part-local transforms and modifiers. Materials split into reusable draw primitives.
    groups={}
    for o in list(active):
        bpy.ops.object.select_all(action='DESELECT');o.select_set(True);bpy.context.view_layer.objects.active=o
        bpy.ops.object.convert(target='MESH')
        bpy.ops.object.transform_apply(location=True,rotation=True,scale=True)
        groups.setdefault(o.data.materials[0].name,[]).append(o)
    parts=[]
    for material,objects in groups.items():
        bpy.ops.object.select_all(action='DESELECT')
        for o in objects:o.select_set(True)
        bpy.context.view_layer.objects.active=objects[0]
        if len(objects)>1:bpy.ops.object.join()
        o=objects[0];o.name=key+'__'+material
        triangulate=o.modifiers.new('Export triangulation','TRIANGULATE')
        bpy.ops.object.modifier_apply(modifier=triangulate.name)
        if not o.data.uv_layers:
            bpy.ops.object.mode_set(mode='EDIT');bpy.ops.mesh.select_all(action='SELECT');bpy.ops.uv.smart_project(island_margin=.02);bpy.ops.object.mode_set(mode='OBJECT')
        o.data.calc_loop_triangles();o.data.calc_tangents()
        vertices=[];indices=[];dedup={}
        tint=o.data.color_attributes.get('Tint')
        for tri in o.data.loop_triangles:
            for li in tri.loops:
                loop=o.data.loops[li];v=o.data.vertices[loop.vertex_index];p=v.co;n=loop.normal;t=loop.tangent;uv=o.data.uv_layers.active.data[li].uv
                color=tint.data[loop.vertex_index if tint.domain=='POINT' else li].color[:3] if tint else (1,1,1)
                row=(p.x,p.z,-p.y,n.x,n.z,-n.y,uv.x,uv.y,t.x,t.z,-t.y,loop.bitangent_sign,*color)
                packed=struct.pack('<15f',*row)
                if packed not in dedup:dedup[packed]=len(vertices);vertices.append(packed)
                indices.append(dedup[packed])
        path=CONTENT/'models/Honeybud'/f'{o.name}.asset'
        write_asset(path,'StaticMesh',b'STM1'+struct.pack('<II',len(vertices),len(indices))+b''.join(vertices)+struct.pack('<'+'I'*len(indices),*indices),metadata={'authoring':'Blender MCP','kit':key,'material':material})
        parts.append({'object':o,'mesh':asset_reference(path),'material':material,'triangles':len(indices)//3})
        for coll in list(o.users_collection):coll.objects.unlink(o)
        library.objects.link(o)
        o.select_set(False)
    KIT[key]=parts;active.clear()

def place(key,p=(0,0,0),angle=0,scale=1):
    PLACEMENTS.append({'kit':key,'position':p,'angle':angle,'scale':scale})
    for part in KIT[key]:
        o=part['object'].copy();o.data=part['object'].data;stage.objects.link(o)
        o.location=p;o.rotation_euler.z=angle;o.scale=(scale,)*3

# Architecture: squat walls, generous eaves, overlapping rounded clay tiles.
begin()
cube('Plaster cottage',(0,0,1.75),(4.7,3.4,3.5),'cream',.22)
cube('Foundation',(0,0,.22),(5,3.7,.42),'stone',.14)
for x in [-2.22,2.22]:cube('Painted corner post',(x,-1.72,1.75),(.19,.18,3.35),'ivory',.04)
cube('Door inset',(0,-1.77,1.25),(1.38,.12,2.45),'dark_wood',.24)
cube('Sage door',(0,-1.86,1.23),(1.16,.13,2.25),'sage',.18)
for x in [-.36,-.12,.12,.36]:cube('Door plank',(x,-1.942,1.14),(.019,.012,1.78),'wood',.005)
sphere('Door knob',(.38,-2.00,1.2),(.085,.07,.085),'brass')
for x in [-1.48,1.48]:
    cube('Window frame',(x,-1.79,1.99),(1.01,.17,1.24),'ivory',.16)
    cube('Warm glass',(x,-1.895,2.0),(.77,.07,.98),'dark_wood',.12)
    cube('Golden interior',(x,-1.94,2.0),(.64,.025,.83),'glow',.08)
    cube('Window mullion',(x,-1.975,2.0),(.07,.04,.98),'ivory',.01)
    cube('Window transom',(x,-1.975,2.0),(.78,.04,.07),'ivory',.01)
    cube('Window box',(x,-2.0,1.38),(1.10,.42,.35),'sage',.06)
    for dx in [-.32,0,.32]:sphere('Box foliage',(x+dx,-2.02,1.63),(.27,.25,.26),'leaf')
cube('Doorstep',(0,-2.03,.14),(1.7,.66,.25),'ivory',.11)
for side in [-1,1]:
    angle=-side*.60
    for row in range(5):
        y=side*(.17+row*.43);z=4.78-row*.295
        for col in range(10):
            x=-2.5+col*.55+(row%2)*.12
            cube('Overlapping rounded shingle',(x,y,z),(.60,.63,.13),'roof_light' if (row+col)%4==0 else 'rose',.12,(angle,0,0))
    curve('Cream rolled eave',[(-2.85,side*2.27,3.42),(0,side*2.3,3.36),(2.85,side*2.27,3.42)],.09,'ivory')
curve('Ridge cap',[(-2.8,0,4.90),(0,0,4.96),(2.8,0,4.90)],.13,'rose')
cube('Chimney',(1.6,.65,4.75),(.57,.59,1.43),'cream',.09)
cube('Chimney cap',(1.6,.65,5.49),(.78,.78,.18),'rose',.07)
cube('Shop plaque',(0,-1.86,3.04),(2.4,.15,.51),'wood',.19)
text('Honeybud sign','HONEYBUD',(0,-1.96,2.93),.28,'ivory')
end('cottage')

# Ceramic flower-cup fountain with lip, petal piping and curling handles.
begin()
lathe('Basin',[(0,0),(1.7,0),(1.85,.15),(1.88,.38),(1.78,.51),(1.65,.48),(1.55,.25),(0,.25)],'ceramic',petals=8)
ring('Basin rim',(0,0,.44),1.77,.095,'ivory')
cylinder('Still water',(0,0,.31),1.64,.035,'water',64)
lathe('Foot',[(0,.25),(.53,.25),(.65,.40),(.34,.54),(.25,1.0),(.42,1.1)],'ceramic')
for z,r in [(1.0,1.12),(2.14,.78)]:
    lathe('Scalloped cup',[(0,z),(.30*r,z),(.52*r,z+.1),(.83*r,z+.48),(r,z+.8),(.96*r,z+.87),(.88*r,z+.80),(.73*r,z+.48),(.28*r,z+.16),(0,z+.16)],'ceramic',petals=8)
    ring('Gold edged lip',(0,0,z+.83),r*.97,.045,'brass')
    cylinder('Cup water',(0,0,z+.75),r*.85,.025,'water',64)
    for j in range(8):
        a=j*math.tau/8
        pts=[(.38*r*math.cos(a),.38*r*math.sin(a),z+.11),(.65*r*math.cos(a-.17),.65*r*math.sin(a-.17),z+.39),(.88*r*math.cos(a),.88*r*math.sin(a),z+.73)]
        curve('Raised petal piping',pts,.028,'ivory')
    curve('Cup handle',[(r*.85,0,z+.67),(r*1.42,0,z+.91),(r*1.54,0,z+.42),(r*.62,0,z+.18)],.075,'ivory')
for j in range(3):
    a=j*math.tau/3+.3
    curve('Water ribbon',[(.60*math.cos(a),.60*math.sin(a),2.91),(.87*math.cos(a),.87*math.sin(a),2.55),(.83*math.cos(a),.83*math.sin(a),1.78)],.053,'water')
    curve('Waterfall',[(1.00*math.cos(a),1.00*math.sin(a),1.78),(1.30*math.cos(a),1.30*math.sin(a),1.05),(1.38*math.cos(a),1.38*math.sin(a),.36)],.065,'water')
lathe('Bud spire',[(0,2.82),(.12,2.83),(.19,3.13),(.12,3.30),(0,3.42)],'brass',32)
end('petal_fountain')

begin()
for x in [-1.6,1.6]:
    for y in [-1.35,1.35]:
        cube('Pergola post',(x,y,1.6),(.19,.19,3.2),'wood',.06)
        sphere('Finial',(x,y,3.24),(.17,.17,.17),'ivory')
for y in [-1.35,1.35]:cube('Header',(0,y,2.96),(3.8,.21,.26),'wood',.06)
for x in [-1.6,1.6]:cube('Side beam',(x,0,2.97),(.22,3.25,.25),'wood',.06)
for x in [-1.6,-.8,0,.8,1.6]:cube('Roof slat',(x,0,3.13),(.13,3.4,.14),'ivory',.04)
for k in range(7):
    x=-1.45+k*.48
    curve('Lattice',[(max(-1.5,x-.9),1.36,.55),(x,1.36,1.45),(min(1.5,x+.9),1.36,2.35)],.045,'wood')
    curve('Lattice',[(max(-1.5,x-.9),1.36,2.35),(x,1.36,1.45),(min(1.5,x+.9),1.36,.55)],.045,'wood')
curve('Climbing vine',[(-1.65,-1.3,.2),(-1.73,-1.42,1.4),(-1.56,-1.33,2.6),(-1,-1.4,3.26),(0,-1.3,3.27),(1.7,-1.28,3.20)],.055,'leaf')
end('pergola')

begin()
for x in [-1.22,1.22]:
    for y in [-.42,.42]:cube('Bench leg',(x,y,.38),(.16,.18,.76),'wood',.05)
cube('Seat frame',(0,0,.79),(2.9,1.2,.19),'sage',.15)
cube('Cushioned seat',(0,-.05,.98),(2.63,1.08,.30),'linen',.14)
cube('Rounded upholstered back',(0,.47,1.57),(2.9,.26,1.35),'sage',.22)
for x in [-1.4,1.4]:curve('Curved arm',[(x,-.49,.98),(x,-.40,1.40),(x,.42,1.48)],.10,'wood')
for x in [-.87,.87]:
    sphere('Round honey cushion',(x,.23,1.59),(.49,.21,.52),'honey')
    sphere('Covered button',(x,-.005,1.59),(.08,.025,.08),'ivory')
sphere('Centre cushion',(0,.19,1.62),(.54,.23,.58),'blue_cloth')
curve('Stitched pillow piping',[(-.38,.0,1.29),(-.49,.0,1.66),(0,-.045,2.17),(.49,.0,1.66),(.38,.0,1.29),(0,-.045,1.06)],.013,'ivory',True)
end('cushion_bench')

begin()
lathe('Jam pot',[(0,0),(.35,0),(.45,.12),(.47,.53),(.37,.76),(.38,.86),(0,.86)],'rose',40)
lathe('Fabric lid',[(0,.91),(.32,.91),(.43,.88),(.47,.73),(.49,.70)],'blue_cloth',48,petals=10)
ring('Twine tie',(0,0,.84),.41,.022,'ivory')
curve('Bow',[(-.02,-.44,.82),(-.27,-.48,1.02),(-.32,-.46,.88),(0,-.46,.82),(.27,-.48,1.02),(.30,-.46,.88),(0,-.46,.82)],.018,'ivory')
curve('Tie ends',[(0,-.46,.82),(-.12,-.49,.58),(-.18,-.47,.54)],.017,'ivory')
cube('Paper label',(0,-.457,.40),(.43,.026,.31),'linen',.055)
sphere('Berry stamp',(0,-.481,.40),(.085,.018,.09),'rose')
end('jam_jar')

begin()
cylinder('Table top',(0,0,.83),.81,.15,'wood',48)
for x,y in [(-.42,-.36),(.42,-.36),(0,.43)]:cube('Table leg',(x,y,.42),(.13,.13,.84),'sage',.035)
end('tea_table')
begin()
lathe('Teacup',[(0,0),(.14,0),(.19,.07),(.23,.30),(.21,.34),(.18,.30),(.13,.08),(0,.08)],'ivory',32)
ring('Cup handle',(.24,0,.19),.11,.027,'ceramic',(math.pi/2,0,0))
cylinder('Tea',(0,0,.28),.18,.01,'dark_wood')
cylinder('Saucer',(0,0,-.01),.34,.045,'ceramic')
end('tea_cup')

begin()
for x in [-.9,0,.9]:
    cube('Picket',(x,0,.61),(.25,.10,1.04),'wood',.09)
    sphere('Rounded top',(x,0,1.12),(.125,.055,.12),'wood',16,8)
for z in [.30,.78]:cube('Fence rail',(0,.045,z),(2.0,.10,.12),'ivory',.025)
end('fence')

begin()
cube('Raised bed',(0,0,.24),(2.35,1.45,.48),'sage',.10)
cube('Soil',(0,0,.48),(2.09,1.19,.08),'soil',.08)
for x in [-1.10,1.10]:cube('End trim',(x,0,.50),(.13,1.48,.12),'ivory',.03)
end('planter')

for flower,color in [('daisy','ivory'),('pink_flower','petal'),('violet','lavender')]:
    begin()
    curve('Stem',[(0,0,0),(.05,0,.36),(0,0,.72)],.024,'leaf')
    for a in [0,2.8]:
        o=sphere('Leaf',(.14*math.cos(a),.14*math.sin(a),.30),(.24,.08,.05),'leaf_light',16,8);o.rotation_euler=(0,-.35,a)
    for j in range(7):
        a=j*math.tau/7
        o=sphere('Petal',(.17*math.cos(a),.17*math.sin(a),.73),(.19,.095,.055),color,16,8);o.rotation_euler.z=a
    sphere('Flower heart',(0,0,.77),(.10,.10,.07),'honey',16,8)
    end(flower)

begin()
lathe('Terracotta pot',[(0,0),(.30,0),(.42,.62),(.47,.64),(.47,.72),(.40,.74),(.36,.62),(0,.62)],'roof_light',32)
cylinder('Pot earth',(0,0,.66),.36,.025,'soil')
for j in range(9):
    a=j*2.4
    o=sphere('Succulent leaf',(.18*math.cos(a),.18*math.sin(a),.85+j*.022),(.31,.10,.12),'leaf_light',16,8);o.rotation_euler=(0,-.6,a)
end('potted_plant')

begin()
for j in range(7):
    a=j*2.4
    sphere('Cloud shrub',(.32*math.cos(a),.27*math.sin(a),.38+(j%3)*.16),(.47,.41,.44),'leaf' if j%2 else 'leaf_light',20,10)
end('shrub')

begin()
for j in range(6):
    a=j*2.4
    curve('Tall blade',[(0,0,0),(.13*math.cos(a),.13*math.sin(a),.29),(.24*math.cos(a),.24*math.sin(a),.47)],.025,'leaf_light')
end('grass_tuft')

begin()
o=sphere('River stone',(0,0,.025),(.50,.37,.035),'stone',16,8)
end('stepping_stone')

begin()
cube('Lantern foot',(0,0,.10),(.49,.49,.20),'stone',.08)
cylinder('Lamp post',(0,0,1.12),.055,2.10,'brass',16)
cube('Lantern body',(0,0,2.18),(.40,.40,.54),'glow',.08)
for x in [-.19,.19]:
    for y in [-.19,.19]:cube('Lantern frame',(x,y,2.20),(.035,.035,.57),'brass',.007)
lathe('Lantern hat',[(0,2.72),(.15,2.61),(.34,2.49),(.33,2.45),(0,2.45)],'sage',32)
ring('Hanger',(0,0,2.79),.10,.02,'brass',(math.pi/2,0,0))
end('lantern')

begin()
cube('Robot feet',(-.23,-.04,.10),(.34,.43,.20),'dark_wood',.07)
cube('Robot feet',(.23,-.04,.10),(.34,.43,.20),'dark_wood',.07)
for x in [-.22,.22]:cylinder('Robot shin',(x,0,.33),.06,.40,'brass',16)
cube('Robot apron',(0,0,.62),(.62,.40,.48),'sage',.10)
cube('Wooden head',(0,0,1.18),(.94,.53,.72),'wood',.15)
cube('Face bezel',(0,-.285,1.20),(.80,.08,.54),'brass',.13)
cube('Face screen',(0,-.34,1.20),(.68,.045,.41),'dark_wood',.11)
for x in [-.18,.18]:sphere('Friendly eye',(x,-.37,1.25),(.047,.02,.08),'glow',16,8)
curve('Smile',[(-.11,-.38,1.10),(0,-.39,1.06),(.11,-.38,1.10)],.015,'ivory')
for side in [-1,1]:
    curve('Bent arm',[(side*.3,0,.77),(side*.52,0,.66),(side*.57,-.22,.91)],.045,'brass')
    sphere('Mitten',(side*.58,-.22,.92),(.10,.09,.11),'ivory')
ring('Windup loop',(-.13,.11,1.74),.14,.036,'brass',(math.pi/2,0,0))
ring('Windup loop',(.13,.11,1.74),.14,.036,'brass',(math.pi/2,0,0))
cylinder('Windup stem',(0,.11,1.58),.035,.26,'brass',12)
end('garden_robot')

# Rounded terrain island, pathways are real stones with visible grass joints.
begin()
cube('Earth island',(0,0,-.39),(21.8,18.0,.76),'stone',.48)
cube('Moss lawn',(0,0,-.055),(21.4,17.6,.13),'grass',.32)
end('garden_ground')
# Refinement is authored through the same kit/export API, before any instances exist.
from types import SimpleNamespace
sys.path.insert(0,str(Path(__file__).parent))
import importlib, honeybud_detail
importlib.reload(honeybud_detail)
honeybud_detail.refine(SimpleNamespace(**globals()))
place('garden_ground')
place('cottage',(1,5.1,0))
place('petal_fountain',(-.6,-.1,0))
place('pergola',(-6.2,3.0,0))
place('cushion_bench',(-6.2,3.35,0))
place('cushion_bench',(6.0,1.6,0),-.28)
place('tea_table',(5.4,-.25,0))
place('tea_cup',(5.20,-.40,.93))
place('jam_jar',(5.66,-.10,.93),scale=.56)
place('jam_jar',(-4.9,1.5,0),scale=.75)
place('jam_jar',(-5.8,1.4,0),scale=.55)
place('garden_robot',(-3.1,-.5,0),-.30)
for x in [-8.6,-6.0]:
    place('planter',(x,-3.55,0))
    for dx in [-.75,0,.75]:
        for dy in [-.35,.35]:place('pink_flower',(x+dx,-3.55+dy,.51),scale=.7)
for x in [-9,-7,-5,-3,-1,1,3,5,7,9]:place('fence',(x,8.1,0))
for y in [-6,-4,-2,0,2,4,6]:
    place('fence',(-10.0,y,0),math.pi/2)
    place('fence',(10.0,y,0),math.pi/2)
for x in [-9,-7,-5,5,7,9]:place('fence',(x,-8.0,0))
for y in np.arange(-7.5,3.35,.70):
    x=1.15+math.sin(y*.40)*.60
    for dx in [-.58,.19,.85]:
        if (x+dx+.6)**2+(y+.1)**2>4.1:place('stepping_stone',(float(x+dx),float(y),0),random.random()*3,random.uniform(.75,1.03))
for x in np.arange(-8.7,8.8,.85):
    if abs(x) > 2.2:place('stepping_stone',(float(x),-.8+math.sin(x)*.17,0),random.random()*3,.9)
for p in [(-1.85,3.4,0),(3.8,3.3,0),(-7.85,1.5,0),(7.8,1.8,0)]:place('potted_plant',p)
for p in [(-3.1,-3,0),(3.0,-3.2,0),(-8.1,5.1,0),(8.5,5.2,0)]:place('lantern',p)
for x,y in [(-8,6.5),(-5,6.2),(-3.5,6.6),(5,6.9),(7,6.6),(8.7,6.5),(-8.9,.7),(8.8,3.6),(8.4,-4.8),(6.5,-5.9),(-8.6,-6.0)]:
    place('shrub',(x,y,0),random.random()*4,random.uniform(.9,1.3))
    for j in range(3):place(random.choice(['daisy','pink_flower','violet']),(x+random.uniform(-.9,.9),y+random.uniform(-.6,.4),0),random.random()*6,random.uniform(.7,1.15))
for x in [-7.75,-6.7,-5.7,-4.8]:place('violet',(x,1.67,2.98),random.random()*6,.65)
for i in range(32):
    x=random.uniform(-9.5,9.5);y=random.choice([-1,1])*random.uniform(5.8,7.8)
    place('grass_tuft',(x,y,0),random.random()*6,random.uniform(.6,1.0))
# Oversized flowers make the garden feel like a hand-made miniature.
for key,p,s in [('pink_flower',(-8.5,6.5,0),3.4),('daisy',(-4.4,7.2,0),3.8),('violet',(7.5,6.9,0),4.0)]:place(key,p,.2,s)

# Manifest for the native Map importer; Blender source and kit use the same placements.
honeybud_detail.dress(SimpleNamespace(**globals()))
manifest={'name':'Honeybud Court','materials':MATREF,'kits':{},'placements':PLACEMENTS}
for key,parts in KIT.items():
    manifest['kits'][key]=[{k:v for k,v in part.items() if k!='object'} for part in parts]
from honeybud_budget import audit
budget=audit(manifest)
(SOURCE/'mesh-budget.json').write_text(json.dumps(budget,indent=2),encoding='utf-8')
(SOURCE/'manifest.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')

scene=bpy.context.scene
world=bpy.data.worlds.new('Apricot afternoon');scene.world=world;world.use_nodes=True
world.node_tree.nodes['Background'].inputs[0].default_value=(.45,.56,.66,1)
world.node_tree.nodes['Background'].inputs[1].default_value=.4
def area(name,p,energy,color,size):
    data=bpy.data.lights.new(name,'AREA');data.energy=energy;data.color=color;data.shape='DISK';data.size=size
    o=bpy.data.objects.new(name,data);stage.objects.link(o);o.location=p;o.rotation_euler=(Vector((0,1,0))-o.location).to_track_quat('-Z','Y').to_euler()
area('Warm afternoon key',(-7,-6,14),2600,(1,.77,.51),9)
area('Soft sky fill',(8,-1,11),1500,(.63,.78,1),10)
area('Golden rim',(1,9,12),2300,(1,.81,.56),7)
bpy.ops.object.camera_add(location=(20,-28,23))
camera=bpy.context.object;camera.name='Honeybud overview';camera.rotation_euler=(Vector((0,.4,1.3))-camera.location).to_track_quat('-Z','Y').to_euler()
camera.data.type='ORTHO';camera.data.ortho_scale=27.5;scene.camera=camera
scene.render.engine='CYCLES';scene.cycles.samples=32;scene.cycles.use_denoising=True
scene.render.resolution_x=1500;scene.render.resolution_y=1125;scene.render.resolution_percentage=100
scene.view_settings.view_transform='AgX'
scene.render.filepath=str(SOURCE/'blender_overview.png')
bpy.data.orphans_purge(do_recursive=True)
bpy.ops.wm.save_as_mainfile(filepath=str(SOURCE/'HoneybudCourt.blend'))
print(json.dumps({'kits':len(KIT),'meshParts':sum(len(p) for p in KIT.values()),'placements':len(PLACEMENTS),'materials':len(MATS),'source':str(SOURCE)},indent=2))
