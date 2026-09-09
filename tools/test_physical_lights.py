"""GPU physical-light regression against independent surface quadrature (numpy).

Run after Release build. Saves isolated v7 maps, raw linear GPU audits and metrics.
No tonemapping or denoising enters the energy comparison.
"""
import copy
import argparse
import json
import math
import subprocess
import uuid

import numpy as np

from test_nrd_shadows import ROOT, create_project
from test_shader_surfaces import read_asset, write_asset


def plane(camera, w, h):
    yaw, pitch, distance = camera['yaw'], camera['pitch'], camera['distance']
    eye = distance*np.array([np.sin(yaw)*np.cos(pitch), np.sin(pitch), np.cos(yaw)*np.cos(pitch)])
    forward = -eye/np.linalg.norm(eye)
    right = np.cross(forward,[0,1,0]); right /= np.linalg.norm(right)
    up = np.cross(right,forward)
    y,x = np.mgrid[:h,:w]
    f = np.tan(camera['fov']/2)
    direction = forward+((x+.5)/w*2-1)[...,None]*right*f*w/h+(1-(y+.5)/h*2)[...,None]*up*f
    p = eye+direction*(-eye[1]/direction[...,1])[...,None]
    mask = (abs(p[...,0])<3.3)&(abs(p[...,2])<3.3)&(x%4==0)&(y%4==0)
    return p[mask], eye, mask


def brdf_nl(p,eye,direction):
    v=eye-p; v/=np.linalg.norm(v,axis=-1)[...,None]
    half=direction+v; half/=np.linalg.norm(half,axis=-1)[...,None]
    nl=np.maximum(direction[...,1],0); nv=v[...,1]; nh=np.maximum(half[...,1],0)
    vh=np.maximum(np.sum(v*half,axis=-1),0); a=.8**2
    smith=lambda c:2*c/np.maximum(c+np.sqrt(a*a+(1-a*a)*c*c),1.e-20)
    d=a*a/(np.pi*(nh*nh*(a*a-1)+1)**2)
    f=.04+.96*(1-vh)**5
    return nl*(.5/np.pi+d*smith(nv)*smith(nl)*f/np.maximum(4*nv*nl,1.e-5))


def quadrature(kind,c):
    # Independently integrate each emitter patch. All arrays are in world space:
    # emission axis -Y, tangent X, capsule axis Z.
    u,v=np.meshgrid((np.arange(48)+.5)/48,(np.arange(96)+.5)/96,indexing='ij')
    u=u.ravel(); phi=2*np.pi*v.ravel(); zero=np.zeros_like(u)
    r=c.get('radius',0); center=np.array([0,6,0])
    if kind=='rect':
        pos=np.stack([(u-.5)*c['width'],zero,(v.ravel()-.5)*c['height']],axis=-1)
        normals=np.tile([0,-1,0],(len(u),1)); area=c['width']*c['height']; norm=np.pi
    elif kind=='spot':
        pos=np.stack([r*np.sqrt(u)*np.cos(phi),zero,r*np.sqrt(u)*np.sin(phi)],axis=-1)
        normals=np.tile([0,-1,0],(len(u),1)); area=np.pi*r*r
        # Numerical angular integral instead of the renderer's closed form.
        cosines=(np.arange(65536)+.5)/65536
        t=np.clip((cosines-np.cos(c['outerAngle']))/(np.cos(c['innerAngle'])-np.cos(c['outerAngle'])),0,1)
        norm=2*np.pi*np.mean(cosines*t*t*(3-2*t))
    else:
        z=2*u-1; radial=np.sqrt(1-z*z)
        normals=np.stack([radial*np.cos(phi),radial*np.sin(phi),z],axis=-1)
        pos=r*normals
        area=4*np.pi*r*r; norm=np.pi
        if kind=='capsule':
            length=c['length']; pos[:,2]+=np.where(z>=0,.5,-.5)*length
            side_n=np.stack([np.cos(phi),np.sin(phi),zero],axis=-1)
            side=r*side_n; side[:,2]=(u-.5)*length
            weights=np.concatenate([np.full(len(u),area/len(u)),np.full(len(u),2*np.pi*r*length/len(u))])
            pos=np.concatenate([pos,side]); normals=np.concatenate([normals,side_n]); area+=2*np.pi*r*length
            return pos+center,normals,weights,area,norm
    return pos+center,normals,np.full(len(u),area/len(u)),area,norm


def reference(kind,c,p,eye):
    color=np.array(c['color']); power=c['intensity']*color/color.sum()
    if kind=='directional':
        return brdf_nl(p,eye,np.array([0,1,0]))[:,None]*power
    positions,normals,weights,area,norm=quadrature(kind,c)
    result=np.zeros(len(p))
    for start in range(0,len(positions),32):
        delta=positions[start:start+32,None,:]-p
        d2=np.sum(delta*delta,axis=-1); direction=delta/np.sqrt(d2)[...,None]
        cos=np.maximum(np.sum(normals[start:start+32,None,:]*(-direction),axis=-1),0)
        profile=1
        if kind=='spot':
            t=np.clip((direction[...,1]-np.cos(c['outerAngle']))/(np.cos(c['innerAngle'])-np.cos(c['outerAngle'])),0,1)
            profile=t*t*(3-2*t)
        result+=np.sum(weights[start:start+32,None]*cos/d2*profile*brdf_nl(p,eye,direction),axis=0)/(area*norm)
    return result[:,None]*power


def capture(project,scene,tag,case,extra=(),frames=128):
    write_asset(project/'Content/Test.asset','Map',scene)
    name=tag+'-'+case
    args=[str(ROOT/'build/bin/Release/Whimsical.exe'),'--project',str(project),'--frames',str(frames),
          '--width','480','--height','303','--validation','--no-hud','--present','immediate','--capture','--audit',name,*extra]
    print('Validating '+name,flush=True)
    with (project/(case+'.log')).open('w') as log:
        subprocess.run(args,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True)
    folder=ROOT/'captures'/name
    report=json.loads((folder/'render-report.json').read_text())
    audit=json.loads((folder/'audit.json').read_text())
    assert report['validationActive'] and report['validationErrors']==0,report
    assert audit['nonFinite']==0,audit
    actual=np.fromfile(folder/'direct.f32',dtype='<f4').reshape(audit['height'],audit['width'],5)[...,:3]
    return actual,audit,folder


def run(temporal=False):
    tag='physical-lights-'+uuid.uuid4().hex[:8]
    project=create_project(tag)
    _,scene=read_asset(project/'Content/Test.asset')
    scene['entities']=scene['entities'][:1]
    ground=copy.deepcopy(scene['entities'][0])
    metrics={}
    for kind in ('directional','point','spot','rect','capsule'):
        c=dict(type=kind,color=[1,.8,.6],intensity=800,radius=.5,width=3,height=2,length=3,
               innerAngle=.3,outerAngle=.65,angularRadius=0)
        if kind=='directional': c['intensity']=8
        light=dict(id=uuid.uuid4().hex,name=kind,enabled=True,components=dict(
            transform=dict(position=[0,6,0],rotation=[math.sqrt(.5),0,0,math.sqrt(.5)]),light=c))
        scene['entities']=[ground,light]
        actual,audit,folder=capture(project,scene,tag,kind,
            extra=['--audit-light','0'] if temporal else [],frames=72 if temporal else 128)
        p,eye,mask=plane(scene['camera'],audit['width'],audit['height'])
        if temporal:
            c=copy.deepcopy(c); c['intensity']*=.25
            p=p-np.array([2,0,0]); eye=eye-np.array([2,0,0])
        expected=reference(kind,c,p,eye); measured=actual[mask]
        energy=float(np.max(abs(measured.mean(0)/expected.mean(0)-1)))
        error=float(np.mean(abs(measured-expected))/np.mean(expected))
        metrics[kind]=dict(relativeEnergyError=energy,relativePixelError=error)
        print(kind+': '+json.dumps(metrics[kind]),flush=True)
        assert energy<(.05 if temporal else .025) and error<(.15 if temporal else .08),(kind,metrics[kind])
        if kind=='directional' and not temporal:
            blocker=copy.deepcopy(ground)
            blocker.update(id=uuid.uuid4().hex,name='Distant blocker')
            blocker['components']['transform']['position']=[0,300,0]
            blocker['components']['render']['scale']=[30,1,30]
            scene['entities'].append(blocker)
            dark,_,_=capture(project,scene,tag,'directional-shadow-300m')
            assert np.max(dark[mask])<1.e-7,'Directional shadow ray was truncated to local scene distance'
    scene['entities']=[ground]
    actual,_,_=capture(project,scene,tag,'zero')
    assert np.max(abs(actual))==0,'Empty light list has residual direct illumination'
    (project/'physical-metrics.json').write_text(json.dumps(metrics,indent=2))
    print('PASS: '+str(project),flush=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--temporal',action='store_true',help='Measure first eight frames after light power/position edit')
    run(parser.parse_args().temporal)
