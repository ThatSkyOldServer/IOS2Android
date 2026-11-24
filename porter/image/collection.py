"""Link the IPA's real ARM64 dependency closure into one relocatable ELF arena.

Each Mach-O keeps its native segment spacing and its own dyld identity. Binds
use their original library ordinals; embedded definitions never become host
placeholder classes. System imports continue through the generic bridge.
"""
import json
import posixpath
import struct

from .. import IMAGE_ABI_VERSION
from .unwind import prepare_guest_unwind
from .macho import mapped_segments


def _arm64(raw):
    if raw[:4] in (b'\xca\xfe\xba\xbe', b'\xca\xfe\xba\xbf'):
        wide=raw[:4]==b'\xca\xfe\xba\xbf'
        if len(raw)<8:raise ValueError('Truncated FAT header')
        count=struct.unpack_from('>I',raw,4)[0]
        if count>64 or 8+count*(32 if wide else 20)>len(raw):raise ValueError('Invalid embedded FAT architecture count')
        choices=[]
        for i in range(count):
            cpu,subtype,offset,size,*_=struct.unpack_from('>IIQQII' if wide else '>IIIII',raw,8+i*(32 if wide else 20))
            if cpu==0x100000c and subtype&0xffffff==0:
                if offset+size>len(raw):raise ValueError('Embedded ARM64 slice exceeds archive member')
                choices.append(raw[offset:offset+size])
        if len(choices)!=1:raise ValueError('Expected one ordinary embedded ARM64 slice')
        raw=choices[0]
    if len(raw)<32 or raw[:4]!=b'\xcf\xfa\xed\xfe' or struct.unpack_from('<I',raw,4)[0]!=0x100000c or struct.unpack_from('<I',raw,8)[0]&0xffffff:
        raise ValueError('Embedded dependency is not ARM64 Mach-O')
    return raw


def prepare_collection(source,prefix,executable,work,prepared):
    import lief
    names=set(source.namelist())
    directory=work/'guest-images';directory.mkdir(exist_ok=True)
    main_image=prepared.image
    images=[];by_path={};order=[];visiting=set();missing=[]
    def load(path,raw=None):
        if path in by_path:return by_path[path]
        raw=_arm64(source.read(prefix+path) if raw is None else raw)
        index=len(images);folder=directory/str(index) if index else work;folder.mkdir(exist_ok=True)
        binary=folder/'image.bin' if index else prepared.executable
        if index:binary.write_bytes(raw)
        image=main_image if index==0 else lief.MachO.parse(str(binary)).at(0)
        if image.has_encryption_info and image.encryption_info.crypt_id:
            raise ValueError('Encrypted embedded dependency: '+path)
        if any(int(c.command)==0x80000034 for c in image.commands):
            raise ValueError('Chained fixups need decoding for embedded dependency: '+path)
        dependencies=[c for c in image.libraries if int(c.command)!=13]
        install=next((c.name for c in image.libraries if int(c.command)==13),path)
        row=dict(index=index,path=path,image=image,raw=raw,folder=folder,binary=binary,dependencies=dependencies,
                 install=install,targets=[],exports={},rpaths=[r.path for r in image.rpaths])
        images.append(row);by_path[path]=row
        return row
    main=load(executable,prepared.executable.read_bytes())
    def expand(value,owner):
        if value=='@executable_path':return ''
        if value.startswith('@executable_path/'):return value[17:]
        if value=='@loader_path':return posixpath.dirname(owner['path'])
        if value.startswith('@loader_path/'):return posixpath.join(posixpath.dirname(owner['path']),value[13:])
        return value
    def dependency_path(name,owner,inherited):
        if name.startswith('@rpath/'):
            candidates=[posixpath.join(expand(r,owner),name[7:]) for r in owner['rpaths']+inherited]
        else:candidates=[expand(name,owner)]
        for candidate in candidates:
            candidate=posixpath.normpath(candidate)
            if candidate.startswith('/') or candidate=='..' or candidate.startswith('../'):continue
            if prefix+candidate in names:return candidate
        return None
    def visit(row,inherited):
        if row['index'] in visiting:return
        visiting.add(row['index'])
        paths=[expand(r,row) for r in row['rpaths']]+inherited
        for command in row['dependencies']:
            path=dependency_path(command.name,row,inherited)
            target=load(path) if path else None
            row['targets'].append(target)
            if target:visit(target,paths)
            elif command.name.startswith('@'):
                weak=int(command.command)==0x80000018
                missing.append(dict(image=row['path'],library=command.name,weak=weak))
                if not weak:raise ValueError('Required bundled dependency is absent: '+command.name+' (from '+row['path']+')')
        order.append(row)
    visit(main,[])
    arena=0;segments=[];assembly=[];layout=['SECTIONS {']
    lists={key:[] for key in ('INIT','LOAD_CLASSES','LOAD_CATEGORIES','CLASSES','CATEGORIES')}
    section_names=dict(INIT='__mod_init_func',LOAD_CLASSES='__objc_nlclslist',LOAD_CATEGORIES='__objc_nlcatlist',CLASSES='__objc_classlist',CATEGORIES='__objc_catlist')
    for row in images:
        image=row['image'];base=image.imagebase;row['base']=base
        row['arena']=(arena+16383)&~16383;row['label']=f'guest_image_{row["index"]}'
        mapped=mapped_segments(image,len(row['raw']))
        previous=base
        for s in mapped:
            previous=s.virtual_address+s.virtual_size
            offset=row['arena']+s.virtual_address-base
            number=len(segments);label=f'guest_segment_{number}';section=f'.guest_segment_{number}'
            flags='a'+('w' if s.init_protection&2 else '')+('x' if s.init_protection&4 else '')
            origin='ALIGN(16384)' if not number else f'ADDR(.guest_segment_0_file)+{offset}'
            if s.file_size:
                assembly += [f'.section {section}_file,"{flags}",%progbits','.balign 4096',f'.global {label}',f'.hidden {label}',label+':',f'.incbin "{row["binary"].relative_to(work).as_posix()}",{s.file_offset},{s.file_size}']
                layout.append(f'{section}_file {origin} : {{ KEEP(*({section}_file)) }}')
            if s.virtual_size>s.file_size:
                assembly += [f'.section {section}_zero,"{flags}",%nobits','.balign 1']
                if not s.file_size:assembly += [f'.global {label}',f'.hidden {label}',label+':']
                assembly.append(f'.space {s.virtual_size-s.file_size}')
                layout.append(f'{section}_zero ADDR(.guest_segment_0_file)+{offset+s.file_size} (NOLOAD) : {{ KEEP(*({section}_zero)) }}')
            if s==mapped[0]:assembly += [f'.global {row["label"]}',f'.hidden {row["label"]}',f'.set {row["label"]},{label}']
            if row is main and s.name in ('__TEXT','__DATA','__LINKEDIT'):
                alias={'__TEXT':'text','__DATA':'data','__LINKEDIT':'links'}[s.name]
                assembly += [f'.global guest_{alias}',f'.hidden guest_{alias}',f'.set guest_{alias},{label}']
            segments.append(dict(name=s.name,label=label,offset=offset,file_size=s.file_size,memory_size=s.virtual_size,maximum=s.max_protection,initial=s.init_protection,flags=int(s.flags),image=row['index']))
        row['mapped']=mapped;row['size']=previous-base;arena=row['arena']+row['size']
        if arena>0xffffffff:raise ValueError('Guest dependency arena exceeds 32-bit offsets')
        for symbol in image.symbols:
            if symbol.has_export_info:
                export=symbol.export_info
                if int(export.flags)&8:raise ValueError('Embedded reexport requires explicit resolution: '+symbol.name)
                if int(export.flags)&16:raise ValueError('Embedded resolver export requires resolver execution: '+symbol.name)
                kind=int(export.kind)
                # The export trie is authoritative. Stripped trie-only symbols
                # have a zero nlist value even when the exported function is nonzero.
                row['exports'][symbol.name]=(int(export.address)+(base if kind!=2 else 0),kind)
        unwind=prepare_guest_unwind(image,row['folder'])
        assembly += [line.replace('guest_text',row['label']).replace('.Lguest_',f'.Limage_{row["index"]}_') for line in unwind]
    def slot(row,address,size=8):
        for s in row['mapped']:
            if s.virtual_address<=address and address+size<=s.virtual_address+s.virtual_size:
                if not s.max_protection&2:raise ValueError('Relocation into nonwritable dependency segment')
                return s
        raise ValueError('Relocation outside dependency image: '+row['path'])
    def raw_word(row,address):
        s=slot(row,address);delta=address-s.virtual_address
        return struct.unpack_from('<Q',row['raw'],s.file_offset+delta)[0] if delta+8<=s.file_size else 0
    def exported(row,name):
        value=row['exports'].get(name)
        if value is None:return None
        address,kind=value
        if kind==2:return ('absolute',address)
        if kind!=0:raise ValueError('Thread-local exports require a TLS descriptor bridge: '+name)
        if not row['base']<=address<row['base']+row['size']:raise ValueError('Export outside dependency: '+name)
        return ('relative',row['arena']+address-row['base'])
    host_names=set();function_names=set();rebases={};host_binds={};absolute_binds={}
    for row in images:
        image=row['image'];base=row['base'];start=row['arena']
        stubs=image.get_section('__stubs')
        if stubs and stubs.reserved2:
            indirect=list(image.dynamic_symbol_command.indirect_symbols)
            function_names.update(s.name for s in indirect[stubs.reserved1:stubs.reserved1+stubs.size//stubs.reserved2])
        for relocation in image.relocations:
            address=relocation.address;pointer=raw_word(row,address)
            if not pointer:continue
            target=pointer&0x00ffffffffffffff
            if not base<=target<base+row['size']:raise ValueError('Rebase points outside embedded image: '+row['path'])
            rebases[start+address-base]=(pointer-base+start)&0xffffffffffffffff
        for binding in image.bindings:
            slot(row,binding.address);offset=start+binding.address-base;name=binding.symbol.name
            rebases.pop(offset,None);host_binds.pop(offset,None);absolute_binds.pop(offset,None)
            target=None;ordinal=binding.library_ordinal
            if ordinal>0:
                if ordinal>len(row['targets']):raise ValueError('Dependency ordinal outside load commands')
                owner=row['targets'][ordinal-1]
                if owner:target=exported(owner,name)
                if owner and target is None and not binding.weak_import:raise ValueError('Missing embedded export '+name+' in '+owner['path'])
                if owner and target is None:
                    absolute_binds[offset]=0
                    continue
            elif ordinal==0:target=exported(row,name)
            elif ordinal==-1:target=exported(main,name)
            elif ordinal in (-2,-3):
                target=next((value for owner in images if (value:=exported(owner,name)) is not None),None)
            else:raise ValueError('Unsupported dependency ordinal')
            if target:
                if target[0]=='relative':rebases[offset]=(target[1]+binding.addend)&0xffffffffffffffff
                else:absolute_binds[offset]=(target[1]+binding.addend)&0xffffffffffffffff
            elif binding.symbol.value:
                rebases[offset]=start+binding.symbol.value+binding.addend-base
            else:
                host_names.add(name);host_binds[offset]=(name,binding.addend)
    imports=sorted(host_names);indices={name:i for i,name in enumerate(imports)}
    if len(imports)>65536:raise ValueError('Guest import gate count exceeds ARM64 gate ABI')
    (work/'rebases.bin').write_bytes(b''.join(struct.pack('<IQ',offset,value) for offset,value in sorted(rebases.items())))
    (work/'binds.bin').write_bytes(b''.join(struct.pack('<IIq',offset,indices[name],addend) for offset,(name,addend) in sorted(host_binds.items())))
    for name in ('rebases','binds'):
        assembly += ['.section .rodata','.balign 8',f'.global {name}',f'.hidden {name}',name+':',f'.incbin "{name}.bin"']
    assembly += ['.text','.balign 4','.global guest_gates','.hidden guest_gates','guest_gates:']
    for i in range(len(imports)):assembly += [f'mov w0, #{i}','b missing_call']
    header=[f'#define I2A_IMAGE_ABI {IMAGE_ABI_VERSION}',f'#define GUEST_IMAGE_BASE {main["base"]}UL',f'#define IMAGE_END {arena}U',f'#define ENTRY {main_image.entrypoint-main["base"]}',f'#define REBASE_COUNT {len(rebases)}',f'#define BIND_COUNT {len(host_binds)}',f'#define I2A_HOST_SYMBOL_INDEX_CAPACITY {max(4096,1<<(2*len(imports)+512).bit_length())}U']
    for alias,name in (('DATA','__DATA'),('LINKS','__LINKEDIT')):header.append(f'#define GUEST_{alias}_OFFSET {main_image.get_segment(name).virtual_address-main["base"]}UL')
    header += ['struct I2AImageSegment { const unsigned char *address; uint32_t offset,file_size,memory_size,maximum,initial,flags; };']
    header += [f'extern unsigned char {s["label"]}[];' for s in segments]
    header += ['static const struct I2AImageSegment guest_segments[] = {']
    header += [f'{{{s["label"]},{s["offset"]}U,{s["file_size"]}U,{s["memory_size"]}U,{s["maximum"]}U,{s["initial"]}U,{s["flags"]}U}},' for s in segments]+['};']
    initialization=[]
    for row in order:
        starts={key:len(values) for key,values in lists.items()}
        for key,section_name in section_names.items():
            for section in row['image'].sections:
                if section.name!=section_name:continue
                for offset in range(0,section.size,8):
                    location=row['arena']+section.virtual_address-row['base']+offset
                    if location in rebases:lists[key].append(rebases[location])
                    elif location in host_binds:raise ValueError('Bound class/category pointer requires runtime list materialization')
                    elif raw_word(row,section.virtual_address+offset):raise ValueError('Unrelocated class/category pointer')
        initialization.append([row['index'],starts['LOAD_CLASSES'],len(lists['LOAD_CLASSES'])-starts['LOAD_CLASSES'],starts['LOAD_CATEGORIES'],len(lists['LOAD_CATEGORIES'])-starts['LOAD_CATEGORIES'],starts['INIT'],len(lists['INIT'])-starts['INIT']])
    for key,values in lists.items():
        label='guest_all_'+key.lower()
        assembly += ['.section .data','.balign 8',f'.global {label}',f'.hidden {label}',label+':']+[f'.quad guest_text+{v}' for v in values]
        if not values:assembly.append('.quad 0')
        header += [f'extern unsigned char {label}[];',f'#define {key}_OFFSET ((uintptr_t){label}-(uintptr_t)guest_text)',f'#define {key}_COUNT {len(values)}']
    header += ['struct I2AGuestImage { const unsigned char *address; uint64_t native_base; uint32_t size; const char *path,*install_name; };']
    header += [f'extern unsigned char {r["label"]}[];' for r in images]
    header += ['static const struct I2AGuestImage guest_images[]={']+[f'{{{r["label"]},{r["base"]}ULL,{r["size"]}U,{json.dumps(r["path"])},{json.dumps(r["install"])}}},' for r in images]+['};']
    header += ['#define I2A_GUEST_COLLECTION 1','struct I2AGuestExport { const char *name; uint32_t image,kind; uint64_t value; };','static const struct I2AGuestExport guest_defined_exports[]={']
    exports=[]
    for row in images:
        for name,(value,kind) in sorted(row['exports'].items()):
            if not name.startswith('$ld$'):exports.append((name,row['index'],kind,value if kind==2 else value-row['base']))
    header += [f'{{{json.dumps(name)},{index}U,{kind}U,{value}ULL}},' for name,index,kind,value in sorted(exports)]
    header += ['};','static const struct {uint32_t image,classes_first,classes_count,categories_first,categories_count,init_first,init_count;} guest_initialization_order[]={']+['{'+','.join(str(v)+'U' for v in values)+'},' for values in initialization]+['};']
    header += ['static const struct {uint32_t offset; uint64_t value;} guest_absolute_binds[]={']+[f'{{{off}U,{value}ULL}},' for off,value in sorted(absolute_binds.items())]+['};']
    header += ['static const char *const guest_dependencies[]={']+[json.dumps(c.name)+',' for r in images for c in r['dependencies']]+['NULL};']
    header += ['static const char *const symbols[]={']+[json.dumps(name)+',' for name in imports]+['};','static const unsigned char functions[]={',','.join(str(int(name in function_names)) for name in imports),'};']
    layout.append('} INSERT AFTER .bss;');assembly.append('.section .note.GNU-stack,"",%progbits')
    (work/'layout.ld').write_text('\n'.join(layout)+'\n',encoding='utf8')
    (work/'image.S').write_text('\n'.join(assembly)+'\n',encoding='utf8')
    (work/'image.h').write_text('\n'.join(header)+'\n',encoding='utf8')
    (work/'imports.json').write_text(json.dumps(imports,indent=2),encoding='utf8')
    report=dict(abi=IMAGE_ABI_VERSION,arena_bytes=arena,images=[dict(path=r['path'],install_name=r['install'],native_base=r['base'],arena_offset=r['arena'],size=r['size']) for r in images],initialization_order=[r['path'] for r in order],missing=missing,segments=segments)
    (work/'image-collection.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf8')
    print(f'Guest dependency collection: {len(images)} images, {len(rebases)} rebases, {len(host_binds)} host bindings',flush=True)
