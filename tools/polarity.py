"""Is the difference between two captures an INVERSION or a desaturation?

Written for the colormap polarity test, where a screenshot of an InverseMap
frame is consistent with both readings by eye: a greyscale-looking frame can be
a photographic negative or a mere loss of saturation, and the orientation test
that produced it could not tell them apart.

The discriminator is the SIGN of the luminance correlation between the two
images, not the size of any difference:

    desaturation  preserves luminance   -> r near +1, slope near +1
    inversion     reverses luminance    -> r NEGATIVE

Only the sign carries the verdict. Expect |r| well below 1 on a real frame:
the HUD is untouched by a scene-only pass and pulls r toward +1, and the
tonemap/exposure curve is nonlinear. Neither can flip the sign, which is why
the test survives them.

Usage:  python3 tools/polarity.py before.png after.png

Stdlib only -- this machine has neither PIL nor ImageMagick. Samples every
third pixel inside the same analysis box the other tools use.
"""

import sys, zlib, struct
def read(p):
    d=open(p,'rb').read(); assert d[:8]==b'\x89PNG\r\n\x1a\n'
    i=8; idat=b''; 
    while i<len(d):
        ln=struct.unpack('>I',d[i:i+4])[0]; t=d[i+4:i+8]; c=d[i+8:i+8+ln]
        if t==b'IHDR': w,h,bd,ct=struct.unpack('>IIBB',c[:10])
        elif t==b'IDAT': idat+=c
        i+=12+ln
    raw=zlib.decompress(idat); nch={2:3,6:4,0:1}[ct]; stride=w*nch; out=bytearray(); prev=bytearray(stride)
    pos=0
    for y in range(h):
        f=raw[pos]; pos+=1; line=bytearray(raw[pos:pos+stride]); pos+=stride
        for x in range(stride):
            a=line[x-nch] if x>=nch else 0; b=prev[x]; cc=prev[x-nch] if x>=nch else 0
            if f==1: line[x]=(line[x]+a)&255
            elif f==2: line[x]=(line[x]+b)&255
            elif f==3: line[x]=(line[x]+(a+b)//2)&255
            elif f==4:
                p=a+b-cc; pa,pb,pc=abs(p-a),abs(p-b),abs(p-cc)
                pr=a if (pa<=pb and pa<=pc) else (b if pb<=pc else cc)
                line[x]=(line[x]+pr)&255
        out+=line; prev=line
    return w,h,nch,bytes(out)
w,h,n,a=read(sys.argv[1]); _,_,n2,b=read(sys.argv[2])
sx=sy=sxx=syy=sxy=0.0; N=0
for y in range(69,618,3):
    for x in range(72,1368,3):
        o=(y*w+x)*n
        la=0.299*a[o]+0.587*a[o+1]+0.114*a[o+2]
        lb=0.299*b[o]+0.587*b[o+1]+0.114*b[o+2]
        sx+=la; sy+=lb; sxx+=la*la; syy+=lb*lb; sxy+=la*lb; N+=1
mx,my=sx/N,sy/N
cov=sxy/N-mx*my; vx=sxx/N-mx*mx; vy=syy/N-my*my
r=cov/((vx*vy)**0.5)
print(f"N={N} mean_off={mx:.2f} mean_on={my:.2f}")
print(f"pearson r = {r:+.4f}   slope = {cov/vx:+.3f}")
