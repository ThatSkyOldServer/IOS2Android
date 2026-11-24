"""Decode named CoreUI bitmap renditions at build time, without a macOS runtime.

BOM/CSI field reference: https://github.com/Timac/CARParser (Car.h).
Supports LZFSE BGRA/GA8, encoding-3 GA8 RLE and internal atlas references.
Unknown encodings remain explicit; no replacement artwork is synthesized.
"""
import hashlib
import json
import struct
import zlib
import lzfse
from pathlib import Path
from ..files import file_metadata


def png(width, height, pixels):
    def chunk(kind, data):
        return struct.pack('>I', len(data))+kind+data+struct.pack('>I', zlib.crc32(kind+data))
    rows=b''.join(b'\0'+pixels[y*width*4:(y+1)*width*4] for y in range(height))
    return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(rows))+chunk(b'IEND',b'')


def decode_rle(data, width, height, channels):
    if len(data) < 12 + height * 4:
        raise ValueError('Truncated CoreUI RLE table')
    encoding, stored_width, stored_height = struct.unpack_from('<3I', data)
    if encoding != 3 or channels != 2 or (stored_width, stored_height) != (width, height):
        raise ValueError('Unsupported CoreUI RLE layout')
    offsets = struct.unpack_from('<' + 'I' * height, data, 12)
    rows = []
    for offset in offsets:
        if offset < 12 + height * 4 or offset >= len(data):
            raise ValueError('CoreUI RLE row outside payload')
        row = bytearray()
        while len(row) < width * channels:
            if offset + 4 > len(data):
                raise ValueError('Truncated CoreUI RLE run')
            run, = struct.unpack_from('<I', data, offset)
            offset += 4
            count = run & 0x7fffffff
            size = channels if run & 0x80000000 else count * channels
            if not count or len(row) + count * channels > width * channels or offset + size > len(data):
                raise ValueError('Invalid CoreUI RLE run')
            pixels = data[offset:offset + size]
            row.extend(pixels * count if run & 0x80000000 else pixels)
            offset += size
        rows.append(row)
    return b''.join(rows)


class Catalog:
    def __init__(self, raw):
        if raw[:8]!=b'BOMStore': raise ValueError('Not a CoreUI BOM store')
        self.raw=raw; self.decoded={}
        pointer=struct.unpack_from('>I',raw,16)[0]
        count=struct.unpack_from('>I',raw,pointer)[0]
        self.blocks=[]
        for i in range(count):
            offset,size=struct.unpack_from('>II',raw,pointer+4+i*8)
            unused=(offset,size)==(0xffffffff,0xffffffff)
            if not unused and size and offset+size>len(raw): raise ValueError('Catalog block outside file')
            self.blocks.append(raw[offset:offset+size] if size and not unused else b'')
        offset=struct.unpack_from('>I',raw,24)[0]
        count=struct.unpack_from('>I',raw,offset)[0];offset+=4;self.named={}
        for _ in range(count):
            block,n=struct.unpack_from('>IB',raw,offset);offset+=5
            self.named[raw[offset:offset+n].decode()]=block;offset+=n
        fmt=self.blocks[self.named['KEYFORMAT']]
        count=struct.unpack_from('<I',fmt,8)[0]
        self.key_types=struct.unpack_from('<'+'I'*count,fmt,12)
        self.renditions=[]
        for value,key in self.tree('RENDITIONS'):
            values=struct.unpack('<'+'H'*len(self.key_types),self.blocks[key])
            self.renditions.append((dict(zip(self.key_types,values)),value))

    def tree(self,name):
        root=struct.unpack_from('>I',self.blocks[self.named[name]],8)[0]
        visited=set()
        def walk(node):
            if node in visited: raise ValueError('Cyclic BOM tree')
            visited.add(node);data=self.blocks[node]
            leaf,count=struct.unpack_from('>HH',data)
            for i in range(count):
                value,key=struct.unpack_from('>II',data,12+i*8)
                if leaf: yield value,key
                else: yield from walk(value)
        return walk(root)

    def attributes(self,data,offset):
        count=struct.unpack_from('<H',data,offset+4)[0]
        return dict(struct.unpack_from('<HH',data,offset+6+i*4) for i in range(count))

    def decode(self,block,rendition_key=None):
        context=tuple(sorted((rendition_key or {}).items()))
        cache_key=(block,context)
        if cache_key in self.decoded:return self.decoded[cache_key]
        b=self.blocks[block]
        if b[:4]!=b'ISTC':raise ValueError('Missing CSI header')
        w,h,scale=struct.unpack_from('<III',b,12);fmt=b[24:28]
        layout=struct.unpack_from('<H',b,36)[0];tlv=struct.unpack_from('<I',b,168)[0]
        if not w or not h or w*h>16777216:raise ValueError('Non-bitmap rendition')
        if layout==1003:
            pos=184;link=None
            while pos<184+tlv:
                kind,n=struct.unpack_from('<II',b,pos);data=b[pos+8:pos+8+n];pos+=8+n
                if data[:4]==b'KLNI':link=data
            if link is None:raise ValueError('Missing internal atlas reference')
            x,y,cw,ch=struct.unpack_from('<4I',link,8)
            # uint16 key kind, uint32 byte length, then type/value pairs.
            n=struct.unpack_from('<I',link,26)[0]//4
            attrs=dict(struct.unpack_from('<HH',link,30+i*4) for i in range(n))
            targets=[(key,i) for key,i in self.renditions if all(key.get(k,0)==v for k,v in attrs.items())]
            if not targets:
                raise ValueError('Unsupported internal atlas reference: rendition absent from catalog')
            traits=[kind for kind in self.key_types if kind not in attrs and kind!=17]
            if rendition_key and len({i for _,i in targets})>1:
                targets=[(key,i) for key,i in targets if all(key.get(k,0)==rendition_key.get(k,0) for k in traits)]
                if not targets:raise ValueError('Unsupported internal atlas trait combination')
            identities={i for _,i in targets}
            if len(identities)!=1:raise ValueError('Ambiguous internal atlas')
            key,target=targets[0]
            aw,ah,_,atlas=self.decode(target,key)
            if (cw,ch)!=(w,h) or x+w>aw or y+h>ah:raise ValueError('Atlas crop outside image')
            # KLNI rectangles use a bottom-left origin, while decoded scanlines
            # and PNG use a top-left origin. Convert the crop, not the pixels.
            top=ah-y-h
            pixels=b''.join(atlas[((top+row)*aw+x)*4:((top+row)*aw+x+w)*4] for row in range(h))
        else:
            data=b[184+tlv:]
            if len(data)<16 or data[:4]!=b'MLEC':raise ValueError('Unsupported catalog compression')
            channels={b'BGRA':4,b' 8AG':2}.get(fmt)
            compression=struct.unpack_from('<I',data,8)[0]
            if compression==1:
                length=struct.unpack_from('<I',data,12)[0]
                if length!=len(data)-16:raise ValueError('Truncated CoreUI RLE payload')
                raw=decode_rle(data[16:],w,h,channels)
            elif compression==4:
                payload=data[16:]
                if payload[:4] in (b'bvx2',b'bvxn',b'bvx-'):
                    if struct.unpack_from('<I',data,12)[0]!=len(payload):raise ValueError('Truncated direct CSI stream')
                    raw=lzfse.decompress(payload)
                else:
                    chunks=struct.unpack_from('<I',data,12)[0];pos=16;decoded=[]
                    for _ in range(chunks):
                        tag,_,_,rows,n=struct.unpack_from('<4s4I',data,pos);pos+=20
                        if tag!=b'KCBC' or pos+n>len(data):raise ValueError('Invalid CSI chunk')
                        decoded.append(lzfse.decompress(data[pos:pos+n]));pos+=n
                    raw=b''.join(decoded)
            else:raise ValueError('Unsupported catalog compression')
            if not channels or len(raw)%h:raise ValueError('Unsupported catalog pixel layout')
            stride=len(raw)//h
            if stride<w*channels:raise ValueError('Truncated catalog scanline')
            pixels=bytearray()
            # CSI scanlines retain their top-to-bottom order after decompression.
            for y in range(h):
                for x in range(w):
                    at=y*stride+x*channels
                    if channels==4:blue,green,red,alpha=raw[at:at+4]
                    else:gray,alpha=raw[at:at+2];red=green=blue=gray
                    pixels.extend([min(255,(c*255+alpha//2)//alpha) if alpha else 0 for c in (red,green,blue)]+[alpha])
        result=(w,h,scale,bytes(pixels));self.decoded[cache_key]=result;return result

    def images(self, exclusions=(), unsupported=None, include_keys=False):
        for token,name_block in self.tree('FACETKEYS'):
            name=self.blocks[name_block].rstrip(b'\0').decode('utf8');attrs=self.attributes(self.blocks[token],0)
            for key,block in self.renditions:
                if all(key.get(k,0)==v for k,v in attrs.items()):
                    try:w,h,scale,pixels=self.decode(block,key)
                    except ValueError as e:
                        if any(rule['name']==name and key.get(rule['key'])==rule['value'] for rule in exclusions):continue
                        if unsupported is not None and (str(e).startswith('Unsupported ') or str(e)=='Non-bitmap rendition'):
                            unsupported.append(dict(name=name,reason=str(e)));continue
                        raise ValueError(f'{name}: {e}') from e
                    suffix='' if scale==100 else '@'+str(scale//100)+'x'
                    idiom={1:'~iphone',2:'~ipad'}.get(key.get(15),'')
                    result=(name+suffix+idiom+'.png',png(w,h,pixels))
                    yield (*result,dict(key)) if include_keys else result


def prepare_asset_catalog(source,prefix,work,exclusions=()):
    root=work/'catalog-assets';root.mkdir(exist_ok=True)
    members=sorted((item for item in source.infolist() if item.filename.startswith(prefix) and item.filename.endswith('/Assets.car')),key=lambda item:item.filename)
    if not members:
        (root/'manifest.json').write_text(json.dumps({'images':[]}),encoding='utf-8')
        return []
    inputs=dict(catalogues=[dict(path=member.filename,crc=member.CRC,bytes=member.file_size) for member in members],exclusions=list(exclusions),generator=file_metadata([Path(__file__)]))
    stamp=root/'manifest.json'
    if stamp.exists():
        manifest=json.loads(stamp.read_text(encoding='utf8'))
        if manifest.get('inputs')==inputs and all((root/name).is_file() for name in manifest['images']):return [(name,root/name) for name in manifest['images']]
    images=set();unsupported=[];variants=[];candidates={}
    def write(target,data):
        destination=root/target
        if not destination.resolve().is_relative_to(root.resolve()):raise ValueError('Catalog image path escapes its output directory')
        destination.parent.mkdir(parents=True,exist_ok=True)
        destination.write_bytes(data);images.add(target)
    for member in members:
        folder=member.filename[len(prefix):].removesuffix('Assets.car')
        unavailable=[]
        for name,data,key in Catalog(source.read(member)).images(exclusions if not folder else (),unavailable,include_keys=True):
            target=folder+name
            token=json.dumps([target,key],sort_keys=True,separators=(',',':')).encode('utf8')
            variant=folder+'.i2a/catalog/'+hashlib.sha256(token).hexdigest()+'.png'
            digest=hashlib.sha256(data).hexdigest()
            if variant in images and hashlib.sha256((root/variant).read_bytes()).hexdigest()!=digest:
                raise ValueError('Conflicting pixels for one CoreUI rendition key: '+target)
            write(variant,data)
            row=dict(catalogue=member.filename,name=target,key=key,path=variant,sha256=digest)
            variants.append(row);candidates.setdefault(target,[]).append(row)
        for item in unavailable:unsupported.append(dict(path=member.filename,name=item['name'],reason=item['reason']))
    aliases={}
    for target,rows in sorted(candidates.items()):
        # Keep each exact rendition independently. The existing named-image
        # interface uses neutral traits; it must not get the last dark or
        # placeholder variant merely because the BOM happened to list it last.
        keys=set().union(*(row['key'] for row in rows))-{1,2,12,15,17}
        varying={kind for kind in keys if len({row['key'].get(kind,0) for row in rows})>1}
        neutral=[row for row in rows if all(row['key'].get(kind,0)==0 for kind in varying|{5,7})]
        if not neutral or len({row['sha256'] for row in neutral})!=1:
            unsupported.append(dict(path=rows[0]['catalogue'],name=target,reason='Unsupported neutral trait alias; exact rendition variants retained'))
            continue
        chosen=min(neutral,key=lambda row:row['path'])
        write(target,(root/chosen['path']).read_bytes());aliases[target]=chosen['path']
    if unsupported:print(f'CoreUI: {len(unsupported)} renditions or trait aliases have no bitmap conversion; original catalogues are retained')
    manifest=dict(inputs=inputs,images=sorted(images),unsupported=unsupported,variants=variants,aliases=aliases)
    stamp.write_text(json.dumps(manifest,indent=2),encoding='utf8')
    return [(name,root/name) for name in sorted(images)]
