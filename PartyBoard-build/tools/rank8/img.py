# Minimal RGBA image IO: DDS (uncompressed RGBA8) and 8-bit PNG (RGB/RGBA, non-interlaced).
import struct, zlib
def read_dds(p):
    b=open(p,'rb').read(); h,w=struct.unpack('<II',b[12:20]); d=b[128:128+w*h*4]
    return w,h,[[tuple(d[(y*w+x)*4:(y*w+x)*4+4]) for x in range(w)] for y in range(h)]
def read_png(p):
    b=open(p,'rb').read(); i=8; idat=b''
    while i<len(b):
        n,t=struct.unpack('>I4s',b[i:i+8]); c=b[i+8:i+8+n]; i+=12+n
        if t==b'IHDR': w,h,bd,ct=struct.unpack('>IIBB',c[:10])
        elif t==b'IDAT': idat+=c
    assert bd==8 and ct in (2,6); bpp=4 if ct==6 else 3
    raw=zlib.decompress(idat); st=w*bpp; prev=bytearray(st); rows=[]; o=0
    for y in range(h):
        f=raw[o]; line=bytearray(raw[o+1:o+1+st]); o+=1+st
        for x in range(st):
            a=line[x-bpp] if x>=bpp else 0; up=prev[x]; c=prev[x-bpp] if x>=bpp else 0
            if f==1: line[x]=(line[x]+a)&255
            elif f==2: line[x]=(line[x]+up)&255
            elif f==3: line[x]=(line[x]+(a+up)//2)&255
            elif f==4:
                pp=a+up-c; pa,pb,pc=abs(pp-a),abs(pp-up),abs(pp-c)
                line[x]=(line[x]+(a if pa<=pb and pa<=pc else up if pb<=pc else c))&255
        rows.append([tuple(line[x*bpp:x*bpp+bpp])+((255,) if bpp==3 else ()) for x in range(w)]); prev=line
    return w,h,rows
def write_png(p,px):
    h=len(px); w=len(px[0])
    raw=b''.join(b'\0'+bytes(v for q in r for v in q) for r in px)
    ch=lambda t,d: struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d)&0xffffffff)
    open(p,'wb').write(b'\x89PNG\r\n\x1a\n'+ch(b'IHDR',struct.pack('>IIBBBBB',w,h,8,6,0,0,0))+ch(b'IDAT',zlib.compress(raw,9))+ch(b'IEND',b''))
def ascii(px,x0=0,y0=0,w=None,h=None):
    w=w or len(px[0]); h=h or len(px)
    for y in range(y0,y0+h):
        s=''
        for x in range(x0,x0+w):
            r,g,b,a=px[y][x]
            s+=' ' if a<16 else ('#' if r+g+b<60 else ('o' if r+g+b>600 else '+'))
        print('%3d %s'%(y,s))
