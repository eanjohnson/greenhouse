import struct
FEAT={0x1:'Doubles',0x2:'NoRefactor',0x4:'UAVsAtEveryStage',0x8:'64UAVs',
 0x10:'MinPrecision',0x20:'11_1_DoubleExt',0x40:'11_1_ShaderExt',0x80:'LEVEL9',
 0x100:'TiledResources',0x200:'StencilRef',0x400:'InnerCoverage',0x800:'TypedUAVLoadAdditional',
 0x1000:'ROVs',0x2000:'VPAndRTArrayIndex',0x4000:'WaveOps',0x8000:'Int64',
 0x10000:'ViewID',0x20000:'Barycentrics',0x40000:'NativeLowPrecision',
 0x80000:'ShadingRate',0x100000:'Raytracing_Tier1_1',0x200000:'SamplerFeedback',
 0x400000:'AtomicInt64OnTypedResource',0x800000:'AtomicInt64OnGroupShared',
 0x1000000:'DerivativesInMeshAndAmpShaders',0x2000000:'ResDescHeap',
 0x4000000:'SamplerDescHeap',0x8000000:'AtomicInt64OnHeapResource'}
def featnames(f):
    n=[v for k,v in FEAT.items() if f&k]
    return '|'.join(n) if n else '0'
def parts(b):
    assert b[:4]==b'DXBC', 'not DXBC'
    n=struct.unpack_from('<I', b, 0x1C)[0]
    offs=struct.unpack_from('<%dI'%n, b, 0x20)
    out=[]
    for o in offs:
        fcc=b[o:o+4].decode('latin1'); sz=struct.unpack_from('<I',b,o+4)[0]
        out.append((fcc,o+8,sz))
    return out
def info(b):
    d={'size':len(b),'parts':[],'sm':None,'kind':None,'feat':0}
    for fcc,o,sz in parts(b):
        d['parts'].append(fcc)
        if fcc in ('DXIL','ILDB'):
            # DxilProgramHeader: version dword then size
            ver=struct.unpack_from('<I',b,o)[0]
            maj=(ver>>4)&0xF; mnr=ver&0xF; kind=(ver>>16)&0xFFFF
            d['sm']='%d.%d'%(maj,mnr); d['kind']=kind
        elif fcc=='SFI0':
            d['feat']=struct.unpack_from('<Q',b,o)[0] if sz>=8 else 0
    return d
KIND={0:'PS',1:'VS',2:'GS',3:'HS',4:'DS',5:'CS',6:'LIB',
      7:'RG',8:'IS',9:'AH',10:'CH',11:'MS',12:'MISS',13:'CALL',14:'MESH',15:'AMP'}
