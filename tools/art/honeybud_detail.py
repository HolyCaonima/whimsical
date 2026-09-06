"""Second art pass: tailored cloth, embossed ceramic and botanical geometry.

The kit API owns exporting and resource identity. This module authors shapes and
surface treatments only; the same meshes/materials feed Blender and Afterlight.
"""
import math
import random
import bpy
import numpy as np
from mathutils import Vector, Matrix

TAU=math.tau

class Detail:
    def __init__(self,k):
        self.k=k
        self.rng=random.Random(901)

    def replace(self,key,keep=None):
        self.k.begin()
        for part in self.k.KIT.get(key,[]):
            obj=part['object']
            if keep is not None and part['material'] in keep:
                self.k.library.objects.unlink(obj);self.k.stage.objects.link(obj)
                self.k.active.append(obj)
            else:bpy.data.objects.remove(obj,do_unlink=True)

    def mesh(self,name,vertices,faces,material,uv=None,colors=None):
        mesh=bpy.data.meshes.new(name);mesh.from_pydata(vertices,[],faces);mesh.update()
        layer=mesh.uv_layers.new(name='SurfaceUV')
        for poly in mesh.polygons:
            poly.use_smooth=True
            for li in poly.loop_indices:
                vi=mesh.loops[li].vertex_index
                layer.data[li].uv=uv[vi] if uv is not None else (vertices[vi][0],vertices[vi][1])
        if colors is not None:
            tint=mesh.color_attributes.new(name='Tint',type='FLOAT_COLOR',domain='POINT')
            for target,color in zip(tint.data,colors):target.color=(*color,1)
        obj=bpy.data.objects.new(name,mesh);self.k.stage.objects.link(obj)
        return self.k.finish(obj,name,material)

    def surface(self,name,fn,nu,nv,material,tint=None,reverse=False):
        vertices=[];uv=[];colors=[];faces=[]
        for j in range(nv+1):
            for i in range(nu+1):
                u=i/nu;v=j/nv;vertices.append(fn(u,v));uv.append((u,v))
                if tint:colors.append(tint(u,v))
        for j in range(nv):
            for i in range(nu):
                a=j*(nu+1)+i
                face=(a,a+1,a+nu+2,a+nu+1)
                faces.append(tuple(reversed(face)) if reverse else face)
        return self.mesh(name,vertices,faces,material,uv,colors if tint else None)

    def stitches(self,name,points,material='thread',width=.007):
        verts=[];faces=[]
        for i in range(len(points)-1):
            a=Vector(points[i]);b=Vector(points[i+1]);delta=b-a
            side=delta.cross(Vector((0,-1,0))).normalized()*width
            if side.length<.001:side=Vector((width,0,0))
            start=a+delta*.18;end=a+delta*.70
            n=len(verts);verts.extend([start-side,end-side,end+side,start+side]);faces.append((n,n+1,n+2,n+3))
        return self.mesh(name,verts,faces,material)

    def leaf(self,name,base,length,width,angle,tilt,material='leaf',curl=.18,veins=False):
        rotation=Matrix.Rotation(angle,4,'Z')@Matrix.Rotation(tilt,4,'Y')
        def position(u,v):
            t=v;side=u*2-1
            w=width*(math.sin(math.pi*t)**.8)*(.9+.1*math.sin(t*19))
            p=Vector((length*t,side*w,curl*math.sin(t*math.pi)*length-.06*abs(side)*math.sin(math.pi*t)+.055*math.sin(t*TAU)*side*side))
            return Vector(base)+rotation@p
        o=self.surface(name,position,2,5,material,lambda u,v:(.65+.33*v,.76+.24*v,.60+.35*v),reverse=True)
        solid=o.modifiers.new('Leaf thickness','SOLIDIFY');solid.thickness=.007
        # Leaf midrib and side veins live in leaf_surface normal/baseColor maps.
        return o

    def pillow(self,p,rx,rz,material,button=True,power=.72):
        k=self.k;cx,cy,cz=p
        def outline(a):
            c=math.cos(a);s=math.sin(a)
            return rx*math.copysign(abs(c)**power,c),rz*math.copysign(abs(s)**power,s)
        def front(u,v):
            a=u*TAU;r=.001+.999*v;x,z=outline(a)
            bulge=.225*(max(0,1-r*r)**.60)
            tuft=.105*math.exp(-r*r/.030) if button else 0
            pleat=.022*math.sin(a*22+r*15)*r**9
            return cx+x*r,cy-bulge+tuft+pleat,cz+z*r
        obj=self.surface('Tailored stuffed cushion',front,32,8,material,reverse=True)
        # Planar grain keeps knit columns parallel instead of radiating from a pole.
        for loop in obj.data.loops:
            p=obj.data.vertices[loop.vertex_index].co
            obj.data.uv_layers.active.data[loop.index].uv=((p.x-cx)/(2*rx)+.5,(p.z-cz)/(2*rz)+.5)
        solid=obj.modifiers.new('Stuffed back','SOLIDIFY');solid.thickness=.075
        outline_points=[];sew=[]
        for j in range(33):
            a=j/32*TAU;x,z=outline(a)
            outline_points.append((cx+x,cy-.008,cz+z))
            sew.append(front(j/32,.945))
        k.curve('Rolled linen piping',outline_points,.015,'thread',False)
        # Small running stitches are encoded in the cushion texture.
        if button:
            k.cylinder('Fabric covered tuft button',(cx,cy-.132,cz),.07,.025,material,32,(math.pi/2,0,0))
            for dx in [-.019,.019]:
                k.curve('Button stitch',[(cx+dx,cy-.153,cz-.018),(cx+dx,cy-.153,cz+.018)],.005,'thread')
        return obj

    def heart_patch(self,p,scale=.40):
        cx,cy,cz=p;outline=[]
        for j in range(81):
            a=j/80*TAU;x=math.sin(a)**3
            z=(13*math.cos(a)-5*math.cos(2*a)-2*math.cos(3*a)-math.cos(4*a))/17
            outline.append((cx+x*scale,cy-.012+abs(x)*.025,cz+z*scale))
        verts=[(cx,cy-.035,cz)]+outline[:-1]
        self.mesh('Sewn terracotta heart applique',verts,[(0,(j+1)%80+1,j+1) for j in range(80)],'patch')
        self.stitches('Applique blanket stitch',outline,width=.005)

    def bench(self):
        k=self.k;self.replace('cushion_bench')
        for x in [-1.3,1.3]:
            for y in [-.44,.43]:
                k.lathe('Turned wooden leg',[(.095,0),(.11,.08),(.06,.19),(.075,.43),(.12,.49),(.10,.69),(.12,.79)],'wood',32,p=(x,y,0))
        k.cube('Mortised seat frame',(0,0,.78),(3.06,1.25,.22),'sage',.12)
        for x in [-1.47,1.47]:
            k.curve('Scroll arm',[(x,-.50,.90),(x,-.54,1.27),(x,-.34,1.42),(x,.39,1.40),(x,.53,1.72)],.095,'wood')
            k.sphere('Arm cap',(x,-.5,1.26),(.13,.11,.11),'brass')
        k.curve('Rounded arched back frame',[(-1.43,.50,.99),(-1.43,.50,1.9),(-1.08,.5,2.23),(0,.5,2.34),(1.08,.5,2.23),(1.43,.5,1.9),(1.43,.5,.99)],.10,'sage')
        for x in np.linspace(-1.24,1.24,17):
            k.curve('Back spindle',[(float(x),.52,.97),(float(x),.55,1.53),(float(x),.52,2.23-.10*abs(float(x)))],.027,'wood')
        for z in np.arange(1.0,2.20,.11):
            k.curve('Woven cross rail',[(-1.29,.525,float(z)),(0,.56,float(z)+.035),(1.29,.525,float(z))],.018,'wood')
        start=len(k.active)
        self.pillow((0,0,0),1.36,.49,'linen',False,power=.40)
        matrix=Matrix.Translation((0,-.05,.99))@Matrix.Rotation(-math.pi/2,4,'X')
        for o in k.active[start:]:o.matrix_world=matrix@o.matrix_world
        self.pillow((-.89,.14,1.57),.43,.46,'honey')
        self.pillow((.89,.14,1.57),.43,.46,'honey')
        self.pillow((0,.10,1.67),.53,.60,'blue_cloth',False,power=.82)
        self.heart_patch((0,-.155,1.68),.27)
        def throw(u,v):
            x=-1.20+u*.61;progress=min(v/.65,1)
            y=.36-progress*1.06-.035*math.sin(u*12+v*3)
            z=1.18-(max(v-.65,0)/.35)*.82
            z+=.027*math.sin(u*math.pi*12+v*4)*(1+.5*v)
            return x,y,z
        obj=self.surface('Draped knitted throw',throw,24,12,'rose_cloth')
        solid=obj.modifiers.new('Blanket thickness','SOLIDIFY');solid.thickness=.015
        k.curve('Blanket bound edge',[throw(j/80,1) for j in range(81)],.012,'thread')
        for j in range(17):
            p=Vector(throw(j/16,1));k.curve('Twisted fringe',[p,p+Vector((.012,-.01,-.09)),p+Vector((-.012,0,-.14))],.008,'rose_cloth')
        k.end('cushion_bench')

    def fountain(self):
        k=self.k;self.replace('petal_fountain')
        k.lathe('Heavy garden basin',[(0,0),(1.72,0),(1.91,.10),(1.94,.40),(1.87,.53),(1.68,.51),(1.59,.24),(0,.24)],'ceramic',128,petals=10)
        for j in range(24):
            a=j*TAU/24
            k.cube('Basin mosaic tile',(1.898*math.cos(a),1.898*math.sin(a),.27),(.09,.21,.19),'terracotta' if j%3==0 else 'ivory',.022,(0,0,a))
        k.ring('Glazed basin lip',(0,0,.49),1.77,.075,'ivory')
        self.surface('Rippled water',lambda u,v:(1.61*(2*u-1),1.61*(2*v-1),.30+.009*math.sin(u*38+v*12)),16,16,'water')
        # Crop square water surface to a disk, preserving UVs and normals.
        water=k.active[-1]
        import bmesh
        bm=bmesh.new();bm.from_mesh(water.data)
        bmesh.ops.delete(bm,geom=[v for v in bm.verts if v.co.x*v.co.x+v.co.y*v.co.y>1.61**2],context='VERTS')
        bm.to_mesh(water.data);bm.free()
        k.lathe('Turned pedestal',[(0,.25),(.60,.25),(.65,.33),(.48,.46),(.25,.67),(.29,1.08),(.54,1.14)],'ceramic',96)
        for radius,z,cx in [(1.14,1.03,0),(.80,2.20,-.14)]:
            profile=[(.20,0),(.35,.035),(.50,.12),(.68,.30),(.82,.53),(.99,.82),(1,.90),(.95,.92),(.92,.86),(.80,.54),(.64,.32),(.40,.18),(.18,.15)]
            vertices=[];uv=[];faces=[]
            for r,h in profile:
                for j in range(65):
                    a=j/64*TAU;wave=.07*math.cos(a*8)*(h/.92)**3
                    rr=radius*r*(1+.04*math.cos(a*8))
                    vertices.append((cx+rr*math.cos(a),rr*math.sin(a),z+h+wave));uv.append((j/64,h))
            for row in range(len(profile)-1):
                for j in range(64):
                    n=row*65+j;faces.append((n,n+1,n+66,n+65))
            self.mesh('Eight lobed porcelain cup',vertices,faces,'ceramic_cup',uv)
            points=[]
            for j in range(65):
                a=j/64*TAU;r=radius*.975*(1+.04*math.cos(a*8))
                points.append((cx+r*math.cos(a),r*math.sin(a),z+.903+.069*math.cos(a*8)))
            k.curve('Undulating gilded lip',points,.025,'brass')
            # Painted insets, gilt borders and shallow relief use cup_glaze PBR maps.
            k.curve('Swan neck ceramic handle',[(cx+radius*.87,0,z+.74),(cx+radius*1.29,0,z+1.11),(cx+radius*1.55,0,z+.85),(cx+radius*1.35,0,z+.35),(cx+radius*.60,0,z+.16)],.077,'ivory')
            k.curve('Handle inlay',[(cx+radius*.99,-.073,z+.78),(cx+radius*1.30,-.073,z+1.05),(cx+radius*1.47,-.073,z+.84),(cx+radius*1.29,-.073,z+.42)],.015,'brass')
            k.cylinder('Water in cup',(cx,0,z+.79),radius*.87,.026,'water',96)
        for j in range(3):
            a=j*TAU/3+.40
            for radius,ztop,zbottom in [(.73,2.99,1.83),(1.02,1.83,.33)]:
                def sheet(u,v,a=a,radius=radius,ztop=ztop,zbottom=zbottom):
                    angle=a+(u-.5)*.16
                    r=radius+.24*math.sin(v*math.pi/2)
                    z=ztop+(zbottom-ztop)*(v**1.7)+.008*math.sin(u*28+v*45)
                    return -.07+r*math.cos(angle),r*math.sin(angle),z
                obj=self.surface('Thin falling water sheet',sheet,2,12,'water')
                solid=obj.modifiers.new('Water thickness','SOLIDIFY');solid.thickness=.016
            for t in [.18,.30,.45]:
                k.ring('Splash ripple',((1.30)*math.cos(a),(1.30)*math.sin(a),.315),t,.012,'water_highlight')
        k.lathe('Porcelain bud',[(0,3.02),(.12,3.02),(.19,3.28),(.11,3.45),(0,3.62)],'brass',64)
        k.end('petal_fountain')

    def cottage(self):
        k=self.k
        self.replace('cottage',{'cream','stone','ivory','dark_wood','sage','wood','brass','glow'})
        # Each shingle has a scalloped lower edge and shallow longitudinal flutes.
        for side in [-1,1]:
            for row in range(6):
                for col in range(11):
                    x=-2.72+col*.525+(row%2)*.08
                    centre=Vector((x,side*(.05+row*.40),4.86-row*.269))
                    rotation=Matrix.Rotation(-side*.592,4,'X')
                    def shingle(u,v,centre=centre,rotation=rotation,side=side):
                        sx=(u-.5)*.555
                        sy=(v-.5)*.54
                        if v>.6:sy+=side*.09*math.sin(u*math.pi)*((v-.6)/.4)
                        h=.005*math.cos(u*6*math.pi)+.012*math.sin(v*math.pi)
                        return centre+rotation@Vector((sx,sy,h))
                    obj=self.surface('Hand pressed scalloped roof tile',shingle,4,2,'roof_light' if (col+row)%5==0 else 'rose')
                    solid=obj.modifiers.new('Clay tile thickness','SOLIDIFY');solid.thickness=.095
                    bevel=obj.modifiers.new('Soft tile edge','BEVEL');bevel.width=.025;bevel.segments=1
            k.curve('Rolled clay ridge', [(-2.96,0,4.94),(0,0,4.99),(2.96,0,4.94)],.15,'rose')
            for x in np.linspace(-2.7,2.7,21):
                k.sphere('Scalloped eave trim',(float(x),side*2.31,3.36),(.17,.05,.12),'ivory',20,10)
        # Close the roof's end gables and add inset joinery to exposed plaster.
        for x in [-2.34,2.34]:
            self.mesh('Plaster gable',[(x,-1.7,3.4),(x,1.7,3.4),(x,0,4.72)],[(0,1,2)],'cream')
        for x in [-1.47,1.47]:
            for j in range(5):
                for dx in [-.33,.33]:
                    k.cube('Shutter louvre',(x+dx,-1.997,1.82+j*.11),(.25,.05,.032),'sage',.012)
            for dx in [-.25,0,.25]:
                self.leaf('Window planter leaf',(x+dx,-2.07,1.54),.42,.10,self.rng.uniform(-3,3),-.80,veins=True)
        for j in range(14):
            x=-2.23+j*.343
            k.cube('Foundation brick',(x,-1.862,.28),(.31,.06,.23),'terracotta' if j%4==0 else 'stone',.025)
        # A pitched linen canopy with draped, bound scallops.
        def canopy(u,v):
            return (u-.5)*2.04,-1.90-v*.78,3.09-.18*v-.06*math.sin(u*math.pi)
        self.surface('Striped canvas door awning',canopy,8,3,'awning')
        for j in range(9):
            x=-.92+j*.23
            self.surface('Canvas valance',lambda u,v,x=x:(x+(u-.5)*.25,-2.68,2.91-v*(.12+.11*math.sin(math.pi*u))),6,1,'awning')
        k.curve('Awning front piping',[(-1.02,-2.69,2.91),(0,-2.69,2.85),(1.02,-2.69,2.91)],.017,'thread')
        for x in [-1.02,1.02]:k.curve('Brass awning bracket',[(x,-1.78,2.57),(x,-2.63,2.89),(x,-1.82,3.06)],.028,'brass')
        k.text('House number','07',(1.01,-1.90,1.0),.14,'brass')
        k.end('cottage')

    def plants(self):
        k=self.k;self.replace('shrub')
        for j in range(54):
            a=j*2.39996;r=.13+.65*math.sqrt((j%17)/16);z=.18+.63*(1-r/.95)+self.rng.uniform(-.10,.13)
            self.leaf('Individual curled shrub leaf',(r*math.cos(a),r*math.sin(a),z),self.rng.uniform(.24,.42),.09,a,-.22,self.rng.choice(['leaf','leaf_light']),veins=j%7==0)
        for j in range(9):
            a=j*2.4;k.curve('Woody branch',[(0,0,0),(.25*math.cos(a),.25*math.sin(a),.35),(.6*math.cos(a),.6*math.sin(a),.65)],.019,'dark_wood')
        k.end('shrub')
        for key,color in [('daisy','ivory'),('pink_flower','petal'),('violet','lavender')]:
            self.replace(key)
            k.curve('Organic stem',[(0,0,0),(.07,0,.30),(-.02,.025,.64),(.01,0,.82)],.020,'leaf')
            for a,z in [(0,.20),(2.4,.35)]:self.leaf('Veined flower leaf',(0,0,z),.40,.12,a,-.22,veins=True)
            count=9 if key=='daisy' else 6
            for j in range(count):
                a=j*TAU/count
                def petal(u,v,a=a):
                    r=.035+.34*v;w=.12*math.sin(v*math.pi)**.65*(u*2-1)
                    z=.80+.075*math.sin(v*math.pi)-.06*v+.025*(u*2-1)**2
                    return r*math.cos(a)-w*math.sin(a),r*math.sin(a)+w*math.cos(a),z
                o=self.surface('Cupped petal with raised midrib',petal,3,6,color,lambda u,v:(.72+.28*v,.72+.28*v,.78+.22*v),reverse=True)
                solid=o.modifiers.new('Petal body','SOLIDIFY');solid.thickness=.01
            k.sphere('Flower centre',(.01,0,.82),(.095,.095,.068),'honey',32,16)
            # Pollen grain reads from the textured centre, not dozens of micro-spheres.
            k.end(key)
        self.replace('potted_plant')
        k.lathe('Pot with rolled rim',[(0,0),(.28,0),(.30,.05),(.42,.60),(.46,.62),(.47,.73),(.39,.76),(.36,.65),(0,.63)],'terracotta',64)
        k.ring('Pot slip band',(0,0,.46),.38,.016,'ivory')
        k.cylinder('Potting soil',(0,0,.65),.36,.03,'soil',64)
        for j in range(16):
            a=j*2.4;self.leaf('Succulent sculpted leaf',(.02*math.cos(a),.02*math.sin(a),.71+(j//6)*.09),.46-(j//6)*.08,.115,a,-.35-(j//6)*.25,'leaf_light',.24,True)
        k.end('potted_plant')

    def jam(self):
        k=self.k;self.replace('jam_jar')
        k.lathe('Glazed jam crock',[(0,0),(.33,0),(.40,.04),(.47,.18),(.49,.56),(.38,.75),(.38,.84),(0,.84)],'jam_glaze',80)
        def cloth(u,v):
            a=u*TAU;r=v*.53
            fold=math.sin(a*13+.12*math.cos(a*4))
            z=.89-.22*max(0,(v-.65)/.35)+.042*fold*max(0,(v-.53)/.47)
            r+=.024*fold*v**5
            return r*math.cos(a),r*math.sin(a),z
        obj=self.surface('Gathered cloth cover',cloth,40,8,'blue_cloth',reverse=True)
        for loop in obj.data.loops:
            p=obj.data.vertices[loop.vertex_index].co
            obj.data.uv_layers.active.data[loop.index].uv=(p.x+.5,p.y+.5)
        hem=[cloth(j/120,1) for j in range(121)]
        k.curve('Cloth hem',hem,.009,'thread')
        k.ring('Twine round neck',(0,0,.78),.445,.016,'twine')
        k.ring('Second twine strand',(0,0,.81),.440,.011,'twine')
        k.curve('Tied linen bow',[(-.02,-.46,.79),(-.24,-.49,1.03),(-.32,-.47,.91),(0,-.49,.79),(.24,-.49,.99),(.30,-.47,.88),(0,-.49,.79)],.016,'twine')
        for side in [-1,1]:k.curve('Loose cord end',[(0,-.49,.79),(side*.13,-.50,.55),(side*.15,-.47,.48)],.015,'twine')
        k.cube('Paper label',(0,-.487,.38),(.48,.035,.34),'linen',.07)
        k.text('Jam label','BERRY',(0,-.513,.40),.082,'dark_wood')
        for x in [-.06,.04]:k.sphere('Berry label stamp',(x,-.52,.30),(.055,.013,.054),'patch',16,8)
        k.end('jam_jar')

    def stones(self):
        k=self.k;self.replace('stepping_stone')
        profile=[]
        for j in range(10):
            a=j*TAU/10;r=self.rng.uniform(.90,1.1)
            profile.append((.49*r*math.cos(a),.36*r*math.sin(a)))
        vertices=[(x,y,z) for z in [.0,.10] for x,y in profile]
        faces=[tuple(reversed(range(10))),tuple(range(10,20))]+[(j,(j+1)%10,(j+1)%10+10,j+10) for j in range(10)]
        o=self.mesh('Irregular hand cut paving',vertices,faces,'paving')
        bevel=o.modifiers.new('Worn stone corners','BEVEL');bevel.width=.045;bevel.segments=3
        normals=o.modifiers.new('Broad stone faces','WEIGHTED_NORMAL')
        k.end('stepping_stone')

    def grass_patch(self):
        k=self.k;self.replace('grass_patch')
        vertices=[];faces=[];uv=[];colors=[]
        for j in range(96):
            a=self.rng.random()*TAU;r=math.sqrt(self.rng.random())*.58
            x=math.cos(a)*r;y=math.sin(a)*r
            height=self.rng.uniform(.04,.115);width=self.rng.uniform(.004,.012)
            angle=self.rng.random()*TAU;dx=math.cos(angle);dy=math.sin(angle)
            bend=self.rng.uniform(.01,.05)
            n=len(vertices)
            vertices.extend([(x-width*dy,y+width*dx,.012),(x+width*dy,y-width*dx,.012),
                             (x+dx*bend+width*dy*.4,y+dy*bend-width*dx*.4,height*.55),
                             (x+dx*bend-width*dy*.4,y+dy*bend+width*dx*.4,height*.55),
                             (x+dx*bend*1.5,y+dy*bend*1.5,height)])
            uv.extend([(0,0),(1,0),(1,.6),(0,.6),(.5,1)])
            colors.extend([(.43,.58,.36),(.43,.58,.36),(.70,.82,.51),(.70,.82,.51),(.96,1,.68)])
            faces.extend([(n,n+1,n+2,n+3),(n+3,n+2,n+4)])
        self.mesh('Dense tapered grass blades',vertices,faces,'grass_blade',uv,colors)
        k.end('grass_patch')

    def details(self):
        k=self.k;self.replace('pergola',{'wood','ivory','leaf'})
        for x in [-1.6,1.6]:
            for y in [-1.35,1.35]:
                for z in [.18,2.62]:k.cube('Post collar',(x,y,z),(.27,.27,.12),'brass',.026)
        for j in range(22):
            t=j/21
            self.leaf('Pergola trailing foliage',(-1.7+3.2*t,-1.40,3.10+.10*math.sin(t*5)),.40,.13,j*2.3,.15,'leaf',.22,True)
        k.end('pergola')
        self.replace('tea_table',{'sage'})
        for j in range(6):
            y=(j-2.5)*.25
            length=2*math.sqrt(max(.01,.79*.79-y*y))
            k.cube('Individual tabletop board',(0,y,.85),(length,.235,.13),'wood',.044)
        k.ring('Bentwood table edge',(0,0,.87),.80,.038,'wood')
        k.end('tea_table')
        self.replace('setting')
        # Warm enclosing architecture keeps the presentation grounded at character height.
        k.cube('Apricot boundary wall',(0,9.6,2.2),(29,.65,4.5),'apricot',.28)
        k.cube('Wall coping',(0,9.6,4.50),(29.3,.88,.20),'ivory',.09)
        for x in [-13,-9,-5,-1,3,7,11]:
            k.cube('Wall panel stile',(x,9.18,2.2),(.10,.12,4.0),'terracotta',.025)
            k.sphere('Coping cap',(x,9.6,4.72),(.20,.22,.25),'ivory',24,12)
        k.cube('Surrounding ochre ground',(0,0,-.62),(80,80,.2),'surround',.06)
        for j in range(28):
            x=-10.5+j*.78
            k.cube('Warm wooden boardwalk',(x,-9.1,-.06),(.74,2.1,.17),'wood',.035)
        k.end('setting')

def refine(k):
    # Retain the established five texture families and their asset identities.
    # Distinct roughness and tint separate glazing, textile and mineral surfaces.
    k.mat('ceramic_cup',(1,1,1),.30,1,texture='cup_glaze')
    k.mat('thread',(.76,.60,.37),.93)
    k.mat('twine',(.54,.32,.14),.90,texture='wood',uv=1)
    k.mat('patch',(.48,.065,.018),.96,texture='gingham',uv=2)
    k.mat('rose_cloth',(.38,.085,.07),.97,texture='knit',uv=1)
    k.mat('petal_glaze',(.29,.20,.31),.25,texture='plaster',uv=1)
    k.mat('jam_glaze',(.32,.055,.025),.19,texture='plaster',uv=1)
    k.mat('terracotta',(.50,.205,.095),.79,texture='plaster',uv=3)
    k.mat('paving',(.58,.32,.17),.86,texture='plaster',uv=2)
    k.mat('leaf_vein',(.30,.40,.11),.70)
    k.mat('pollen',(.91,.45,.075),.65)
    k.mat('water_highlight',(.36,.66,.67),.12)
    k.mat('grass_blade',(.17,.28,.046),.92)
    k.mat('apricot',(.49,.25,.16),.92,texture='plaster',uv=4)
    k.mat('surround',(.35,.22,.13),.97,texture='plaster',uv=20)
    k.mat('awning',(.65,.26,.16),.92,texture='gingham',uv=1)
    d=Detail(k)
    d.bench();d.fountain();d.cottage();d.plants();d.jam();d.stones();d.grass_patch();d.details()

def dress(k):
    k.place('setting')
    rng=random.Random(651)
    # Grass patches interlock into borders, avoiding the walkable central stone routes.
    for x in np.arange(-9.35,9.5,.88):
        for y in [-6.8,-5.95,6.1,7.05]:
            if abs(x-1.1)<1.45 and y<0:continue
            k.place('grass_patch',(float(x)+rng.uniform(-.15,.15),y+rng.uniform(-.18,.18),0),rng.random()*TAU,rng.uniform(.8,1.14))
    for side in [-1,1]:
        for y in np.arange(-5.1,5.8,.87):k.place('grass_patch',(side*8.8,float(y),0),rng.random()*TAU,1.15)
    for x,y in [(-2.5,1.5),(1.7,1.6),(-2.5,-1.8),(2.0,-2.2),(4.2,.9),(7.8,.6),(-4.1,1.5),(-7.7,1.4)]:
        k.place('grass_patch',(x,y,0),rng.random()*TAU,.80)
    for x,y in [(-8,5.2),(-7.2,5.7),(4.5,6.6),(6.0,6.6),(8.2,4.9)]:
        for j in range(5):k.place('pink_flower' if j%2 else 'daisy',(x+rng.uniform(-.7,.7),y+rng.uniform(-.4,.4),0),rng.random()*6,rng.uniform(.40,.8))
