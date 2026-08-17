#!/usr/bin/env python3
"""Find where the road-agent 0x50-byte record's +0x00 (target speed, mph) is
WRITTEN. The speed law FUN_0019F560 reads [EDI+0x00] as the cruise profile;
this scan looks for the producer so we know whether the per-car cruise is
recoverable from the image or is runtime-only.

Approach: the agent record is 0x50 bytes; FUN_0019F560 takes it in EDI. The
caller FUN_001A20F0 (and the vehicle init) build it. We scan the whole .text
for MOVSS/MOVDQU/STPS that store to a base reg with disp 0x00 where the base
is plausibly the agent (EDI/ESI/EBX/ECX/EDX). We then filter to the traffic
region (0x0019xxxx-0x001a2xxx and the vehicle-init region) to keep it readable.
"""
import struct, sys

data = open('build/burnout3.elf','rb').read()
e_phoff = struct.unpack_from('<I', data, 28)[0]
e_phnum = struct.unpack_from('<H', data, 44)[0]
segs=[]
for i in range(e_phnum):
    off = e_phoff + i*32
    (pt, offv, vaddr, paddr, filesz, memsz, flags, align) = struct.unpack_from('<IIIIIIII', data, off)
    if pt == 1 and filesz:
        segs.append((vaddr, offv, filesz, align))
# pick the executable (r-x) segment
exef = None
for vb, fo, fs, al in segs:
    pass
# find segment with flags containing 1 (exec)
exef=None
for i in range(e_phnum):
    off = e_phoff + i*32
    (pt, offv, vaddr, paddr, filesz, memsz, flags, align) = struct.unpack_from('<IIIIIIII', data, off)
    if pt==1 and (flags & 1) and (flags & 4):
        exef=(vaddr, offv, filesz)
if not exef:
    print("no exec seg"); sys.exit(1)
vb, fo, fs = exef
text = data[fo:fo+fs]

def v2o(v): return v - vb + fo

# scan for store instructions to [base+disp] with disp==0x00 and base a GPR.
# We only care about 4-byte (SS) stores of a single float, or 16-byte dq.
# MOVSS r32, [reg+disp]      : F3 0F 11 /r   (store)  ; MODRM with ModRM disp8/32.
# We'll do a coarse scan for the F3 0F 11 prefix and check the modrm base != SIB.
# This is heuristic; the point is to surface candidates in the traffic region.
cands=[]
i=0
n=len(text)
while i < n-3:
    # F3 0F 11  XX  where XX is modrm
    if text[i]==0xF3 and text[i+1]==0x0F and text[i+2]==0x11:
        modrm=text[i+3]
        base=(modrm & 7)
        mod=(modrm>>6)&3
        reg=(modrm>>3)&7
        # base 4 => SIB, base in sib
        if base==4:
            i+=4; continue
        # disp
        disp=0
        j=i+4
        if mod==0 and base!=5:
            pass
        elif mod==1:
            disp=struct.unpack_from('<b', text, j)[0]; j+=1
        else:
            disp=struct.unpack_from('<i', text, j)[0]; j+=4
        v=vb+(i-fo)
        if disp==0:
            cands.append((v, reg, base, 'movss'))
    # MOVSD  (128-bit) F2 0F 11 XX
    if text[i]==0xF2 and text[i+1]==0x0F and text[i+2]==0x11:
        modrm=text[i+3]
        base=(modrm & 7)
        if base==4:
            i+=4; continue
        disp=0
        j=i+4
        mod=(modrm>>6)&3
        if mod==0 and base!=5:
            pass
        elif mod==1:
            disp=struct.unpack_from('<b', text, j)[0]; j+=1
        else:
            disp=struct.unpack_from('<i', text, j)[0]; j+=4
        v=vb+(i-fo)
        if disp==0:
            cands.append((v, reg, base, 'movsd'))
    i+=1

# Filter to the traffic road-agent + vehicle-init regions.
def inreg(v, lo, hi): return lo<=v<hi
regs=[(0x00198000,0x001A2C00)]  # traffic road-agent manager region
init=[(0x001A1000,0x001A2000)]
for v,reg,base,kind in cands:
    # base reg codes: 0 EAX,1 ECX,2 EDX,3 EBX,4 EBP,5 ESP,6 ESI,7 EDI
    bname={0:'EAX',1:'ECX',2:'EDX',3:'EBX',4:'EBP',5:'ESP',6:'ESI',7:'EDI'}[base]
    rname={0:'EAX',1:'ECX',2:'EDX',3:'EBX',4:'ESP',5:'EBP',6:'ESI',7:'EDI'}[reg]
    tag=''
    for lo,hi in regs+init:
        if inreg(v,lo,hi): tag='TRAFFIC'; break
    if tag:
        print('0x%08x  %s %s, [0x%02x]  ; store to [reg+0]' % (v, kind, rname, base))
