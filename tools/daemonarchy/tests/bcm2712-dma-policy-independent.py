#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Black-box production C API against pre-review DTB goldens, not source logic."""
import ctypes as C
import errno
import hashlib
import json
from pathlib import Path
import sys
from types import SimpleNamespace
import argparse
from contextlib import nullcontext
import os
import shutil
import shlex
import struct
import subprocess
import tempfile

U64=(1<<64)-1


HERE=Path(__file__).resolve().parent
u64=C.c_uint64;u32=C.c_uint32;uint=C.c_uint;integer=C.c_int
class Root(C.Structure):_fields_=[('cpu_base',u64),('parent_bus_base',u64),('length',u64),('role',integer),('space_flags',u32)]
class Child(C.Structure):_fields_=[('child_bus_base',u64),('parent_bus_base',u64),('length',u64),('parent_space_flags',u32)]
class Range(C.Structure):_fields_=[('cpu_base',u64),('device_base',u64),('length',u64),('role',integer),('root_space_flags',u32),('child_space_flags',u32),('root_index',uint),('parent_index',uint),('child_index',uint)]
class Policy(C.Structure):_fields_=[('count',uint),('ranges',Range*32)]
class Constraints(C.Structure):_fields_=[('device_mask',u64),('alignment',u64),('boundary',u64),('max_segment_length',u64),('max_segments',uint),('role',integer)]
class Span(C.Structure):_fields_=[('cpu_base',u64),('length',u64)]
class Segment(C.Structure):_fields_=[('cpu_base',u64),('device_base',u64),('length',u64),('range_index',uint)]
class Eligible(C.Structure):_fields_=[('cpu_base',u64),('device_base',u64),('length',u64),('start_modulus',u64),('start_remainder',u64),('boundary',u64),('max_segment_length',u64),('range_index',uint)]
ROLE={'RAM':1,'PEER':2}
def blob(value):return C.string_at(C.addressof(value),C.sizeof(value))
def poison(value):C.memset(C.addressof(value),0xa5,C.sizeof(value));return blob(value)
def fields(value,names):return {name:getattr(value,name) for name in names}
def constraints(values):return Constraints(*(values[k] for k in ['device_mask','alignment','boundary','max_segment_length','max_segments']),ROLE[values['role']])
def decode_dtb(blob):
    header = struct.unpack_from('>10I', blob)
    magic, total, structure, strings, _, _, _, _, string_size, structure_size = header
    assert magic == 0xd00dfeed and total <= len(blob)
    assert strings + string_size <= total and structure + structure_size <= total
    nodes, stack, pos = {}, [], structure
    def word():
        nonlocal pos
        assert pos + 4 <= structure + structure_size
        value = struct.unpack_from('>I', blob, pos)[0]
        pos += 4
        return value
    while pos < structure + structure_size:
        token = word()
        if token == 1:
            end = blob.index(b'\0', pos, structure + structure_size)
            stack.append(blob[pos:end].decode());pos = (end + 4) & ~3
            nodes['/' + '/'.join(stack[1:])] = {}
        elif token == 2:
            assert stack;stack.pop()
        elif token == 3:
            length, offset = word(), word()
            assert offset < string_size and pos + length <= structure + structure_size
            end = blob.index(b'\0', strings + offset, strings + string_size)
            name = blob[strings + offset:end].decode()
            nodes['/' + '/'.join(stack[1:])][name] = blob[pos:pos + length]
            pos = (pos + length + 3) & ~3
        elif token == 4:
            pass
        elif token == 9:
            assert not stack;return nodes
        else:
            raise ValueError('unknown FDT structure token')
    raise ValueError('unterminated FDT structure')


def cells(nodes, path, key):
    raw = nodes[path][key]
    assert len(raw) % 4 == 0
    return list(struct.unpack('>' + 'I' * (len(raw) // 4), raw))


def number(words):
    value = 0
    for word in words:value = (value << 32) | word
    return value


def tuples(words, widths):
    stride = sum(widths);assert len(words) % stride == 0
    result = []
    for offset in range(0, len(words), stride):
        row=[];index=offset
        for width in widths:row.append(number(words[index:index+width]));index += width
        result.append(row)
    return result



def run(library, vector_path, cell_inventory, report_path, verified_dtb=None):
 lib=C.CDLL(str(library));rootfn=lib.bcm2712_dma_policy_root;rootfn.argtypes=[C.POINTER(Policy),C.POINTER(Root),uint];rootfn.restype=integer
 compose=lib.bcm2712_dma_policy_compose;compose.argtypes=[C.POINTER(Policy),C.POINTER(Policy),C.POINTER(Child),uint];compose.restype=integer
 mapper=lib.bcm2712_dma_policy_map;mapper.argtypes=[C.POINTER(Policy),C.POINTER(Constraints),C.POINTER(Span),uint,C.POINTER(Segment),uint,C.POINTER(uint)];mapper.restype=integer
 eligiblefn=lib.bcm2712_dma_policy_eligible;eligiblefn.argtypes=[C.POINTER(Policy),C.POINTER(Constraints),C.POINTER(Eligible),uint,C.POINTER(uint)];eligiblefn.restype=integer
 v=json.loads(vector_path.read_text());n,rawroot,children,rawrp,msi,raw=derive_inventory(cell_inventory,verified_dtb)
 assert v['dtb_sha256']==cell_inventory['dtb_sha256']
 def create(rows):
  p=Policy();array=(Root*len(rows))(*(Root(*row) for row in rows));rc=rootfn(C.byref(p),array,len(array));assert rc==0,('root',rc,rows);return p
 actualroots=[]
 for w in rawroot:actualroots.append((w.cpu,w.device,w.length,ROLE[w.role],raw['root_dma_cells'][w.root_index*7]))
 policies={'root':create(actualroots)}
 cs=(Child*len(children))(*(Child(child,parent,size,flags) for child,parent,size,flags in children));p=Policy();assert compose(C.byref(p),C.byref(policies['root']),cs,len(cs))==0;policies['rp1']=p
 assert p.count==len(v['rp1_policy'])==3
 for i,(actual,expected) in enumerate(zip(p.ranges,v['rp1_policy'])):
  assert fields(actual,['cpu_base','device_base','length','role','root_index','child_index'])=={'cpu_base':expected['cpu'],'device_base':expected['device'],'length':expected['length'],'role':ROLE[expected['role']],'root_index':expected['root_index'],'child_index':expected['child_index']},('compose',i)
  assert actual.parent_index==expected['root_index']
  assert actual.root_space_flags==raw['root_dma_cells'][expected['root_index']*7]
  assert actual.child_space_flags==children[expected['child_index']][3]
 for name,windows in v['custom_policies'].items():policies[name]=create([(w['cpu'],w['device'],w['length'],ROLE[w['role']],0x03000000) for w in windows])
 report={'schema':'private/pi5-production-policy-vs-independent-oracle/1','library':str(library),'dtb_sha256':v['dtb_sha256'],'map':{},'eligible':{},'constructors':{},'ok':False}
 failures=[]
 for case_index,case in enumerate(v['map_vectors']):
  spans=(Span*len(case['spans']))(*(Span(s['cpu_base'],s['length']) for s in case['spans']));out=(Segment*64)();before=poison(out);count=uint(0x5a5a5a5a);cfg=constraints(case['constraints']);code=mapper(C.byref(policies[case['policy']]),C.byref(cfg),spans,len(spans),out,64,C.byref(count));expected=case['expected'];want=0 if expected['error']==0 else getattr(errno,expected['error'])
  error=None
  if code!=want:error=f'error {code}, expected {want}'
  elif code:
   if blob(out)!=before or count.value!=0x5a5a5a5a:error='failure mutated output/count'
  else:
   got=[fields(out[i],['cpu_base','device_base','length','range_index']) for i in range(count.value)]
   if got!=expected['segments']:error=f'segments {got!r}, expected {expected["segments"]!r}'
  record_name=case['name'] if case['name'] not in report['map'] else case['name']+':'+str(case_index)
  report['map'][record_name]={'passed':error is None,'return':code,'error':error}
  if error:failures.append((case['name'],error))
 base={'device_mask':U64,'alignment':1,'boundary':0,'max_segment_length':U64,'max_segments':64,'role':'RAM'}
 eligibility=[('root32','root',{'device_mask':0xffffffff}),('rp1_ram32','rp1',{'device_mask':0xffffffff}),('rp1_ram64','rp1',{}),('root_peer32','root',{'device_mask':0xffffffff,'role':'PEER'}),('rp1_peer32','rp1',{'device_mask':0xffffffff,'role':'PEER'}),('congruence4','alignment_preimage',{'alignment':4})]
 for name,policy,changes in eligibility:
  cfg=constraints({**base,**changes});out=(Eligible*32)();before=poison(out);count=uint(0x5a5a5a5a);code=eligiblefn(C.byref(policies[policy]),C.byref(cfg),out,32,C.byref(count));expected=v['eligible_vectors'][name];error=None
  if code or count.value!=len(expected):error=f'error/count {code}/{count.value}, expected0/{len(expected)}'
  else:
   for actual,e in zip(out,expected):
    if fields(actual,['cpu_base','device_base','length','start_modulus','start_remainder','boundary','max_segment_length'])!={'cpu_base':e['cpu'],'device_base':e['device'],'length':e['length'],'start_modulus':e['alignment_modulus'],'start_remainder':e['cpu_start_remainder'],'boundary':cfg.boundary,'max_segment_length':cfg.max_segment_length}:error='wrong physical preimage/congruence metadata';break
  report['eligible'][name]={'passed':error is None,'return':code,'error':error}
  if error:failures.append((name,error))
 # Failure atomicity at constructor and caller output capacities is independently
 # checked with byte-poisoned objects; never initialize expected outputs to zero.
 bad_roots=[('zero_input_count',[]),('zero_length',[(0,0,0,1,0x03000000)]),('wrapping_cpu',[(U64,0,2,1,0x03000000)]),('wrapping_device',[(0,U64,2,1,0x03000000)]),('overlapping_device',[(0,0,4,1,0x03000000),(8,2,4,1,0x03000000)]),('conflicting_physical_roles',[(0,0,4,1,0x03000000),(0,8,4,2,0x03000000)]),('msi_not_data_role',[(msi[0]['cpu'],msi[0]['pci'],msi[0]['length'],3,0x03000000)])]
 for name,rows in bad_roots:
  p=Policy();before=poison(p);array=(Root*len(rows))(*(Root(*row) for row in rows));rc=rootfn(C.byref(p),array,len(rows));passed=rc==errno.EINVAL and blob(p)==before;report['constructors'][name]={'passed':passed,'return':rc}
  if not passed:failures.append((name,'constructor error/rollback mismatch'))
 report['capacity_rollback']={}
 cfg=constraints({**base,'max_segment_length':16});one_span=(Span*1)(Span(0,32));out=(Segment*64)();before=poison(out);count=uint(0x5a5a5a5a);rc=mapper(C.byref(policies['rp1']),C.byref(cfg),one_span,1,out,1,C.byref(count));passed=rc==errno.EFBIG and blob(out)==before and count.value==0x5a5a5a5a
 report['capacity_rollback']['map_output_capacity']={'passed':passed,'return':rc}
 if not passed:failures.append(('map_output_capacity','error/rollback mismatch'))
 cfg=constraints(base);eligible_out=(Eligible*32)();before=poison(eligible_out);count=uint(0x5a5a5a5a);rc=eligiblefn(C.byref(policies['rp1']),C.byref(cfg),eligible_out,1,C.byref(count));passed=rc==errno.EFBIG and blob(eligible_out)==before and count.value==0x5a5a5a5a
 report['capacity_rollback']['eligible_output_capacity']={'passed':passed,'return':rc}
 if not passed:failures.append(('eligible_output_capacity','error/rollback mismatch'))
 many=create([(i*16,i*16,8,1,0x03000000) for i in range(32)]);many_children=(Child*32)(*(Child(i*4096,0,512,0x03000000) for i in range(32)));out_policy=Policy();before=poison(out_policy);rc=compose(C.byref(out_policy),C.byref(many),many_children,32);passed=rc==errno.EFBIG and blob(out_policy)==before
 report['capacity_rollback']['composed_policy_capacity']={'passed':passed,'return':rc}
 if not passed:failures.append(('composed_policy_capacity','error/rollback mismatch'))
 report['raw_msi']={'separate_typed_route':True,'pci_message_addr':msi[0]['pci'],'unchanged_expected':v['raw_msi']['expected_published_pci_message_addr'],'excluded_from_data_policy':True};assert all(not(w.cpu<=msi[0]['cpu']<w.cpu+w.length) for w in rawroot)
 report['ok']=not failures;report['failures']=failures;report_path.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({'passed':report['ok'],'map':len(report['map']),'eligible':len(report['eligible']),'constructors':len(report['constructors']),'capacity_rollback':len(report['capacity_rollback']),'failures':failures},indent=2));return report['ok']


def derive_inventory(inventory, dtb=None):
    raw=inventory['raw_dtb_cells']
    if dtb is not None:
        data=dtb.read_bytes()
        if hashlib.sha256(data).hexdigest()!=inventory['dtb_sha256']:
            raise ValueError('DTB identity mismatch')
        nodes=decode_dtb(data);host=inventory['selected_host'];rp1=inventory['selected_rp1']
        for node,prop,key in [(host,'dma-ranges','root_dma_cells'),(rp1,'dma-ranges','rp1_dma_cells'),(host,'ranges','root_outbound_cells'),(rp1,'ranges','rp1_outbound_cells')]:
            if cells(nodes,node,prop)!=raw[key]:raise ValueError('actual DTB cells differ: '+key)
        parent=cells(nodes,host,'msi-parent')[0]
        provider=next(path for path,props in nodes.items() if props.get('phandle')==struct.pack('>I',parent))
        if provider!=raw['msi_provider'] or tuples(cells(nodes,provider,'reg'),[2,2])[0]!=raw['msi_reg']:
            raise ValueError('actual DTB MSI provider differs')
    root=[];msi=[]
    for i,(flags,bus,cpu,size) in enumerate(tuples(raw['root_dma_cells'],[1,2,2,2])):
        if cpu==raw['msi_reg'][0]:
            msi.append({'cpu':cpu,'pci':bus,'length':size,'root_index':i,'flags':flags});continue
        root.append(SimpleNamespace(cpu=cpu,device=bus,length=size,role='RAM' if cpu==0 else 'PEER',root_index=i))
    children=[(child,parent,size,flags) for child,flags,parent,size in tuples(raw['rp1_dma_cells'],[2,1,2,2])]
    return None,root,children,None,msi,raw


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root',type=Path,default=HERE.parents[2])
    parser.add_argument('--vectors',type=Path,default=HERE/'fixtures/bcm2712-dma-policy-independent.json')
    parser.add_argument('--cells',type=Path,default=HERE/'fixtures/bcm2712-dma-policy-independent-cells.json')
    parser.add_argument('--dtb',type=Path,help='optional exact enrolled400b DTB; otherwise honest retained-cell-snapshot mode')
    parser.add_argument('--output',type=Path)
    parser.add_argument('--cc',default=os.environ.get('CC','cc'))
    args=parser.parse_args()
    if args.output:
        args.output.mkdir();context=nullcontext(args.output.resolve())
    else:context=tempfile.TemporaryDirectory(prefix='bcm2712-independent-')
    with context as location:
        output=Path(location);source=output/'source';source.mkdir();compat=output/'compat/sys';compat.mkdir(parents=True)
        (compat/'stdint.h').write_text('#include <stdint.h>\n')
        identities={}
        for name in ['bcm2712_dma_policy.c','bcm2712_dma_policy.h']:
            original=args.source_root/'sys/arm64/broadcom'/name
            copied=source/name;shutil.copyfile(original,copied);identities[name]=hashlib.sha256(copied.read_bytes()).hexdigest()
        library=output/('policy.dylib' if sys.platform=='darwin' else 'policy.so')
        command=[*shlex.split(args.cc),'-std=c11','-Wall','-Wextra','-Werror','-fPIC','-dynamiclib' if sys.platform=='darwin' else '-shared','-I'+str(compat.parent),'-I'+str(source),str(source/'bcm2712_dma_policy.c'),'-o',str(library)]
        with (output/'compile.log').open('w') as log:subprocess.run(command,check=True,stdout=log,stderr=subprocess.STDOUT)
        inventory=json.loads(args.cells.read_text());report_path=output/'result.json'
        passed=run(library,args.vectors,inventory,report_path,args.dtb)
        report=json.loads(report_path.read_text());report.update(input_mode='actual-dtb-verified' if args.dtb else 'retained-cell-snapshot',source_sha256=identities,vector_sha256=hashlib.sha256(args.vectors.read_bytes()).hexdigest(),cells_sha256=hashlib.sha256(args.cells.read_bytes()).hexdigest(),compiler_command=command,scope='pure userspace policy C API; no busdma backend, kernel execution, MMIO or hardware acceptance')
        report_path.write_text(json.dumps(report,indent=2)+'\n')
        print('PASS' if passed else 'FAIL',report['input_mode'],report['scope'])
        return 0 if passed else 1


if __name__=='__main__':sys.exit(main())
