#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Compile actual selected frontend bodies with fault-injected Newbus primitives.

These controls are not controller emulation or native PCIe/DMA qualification.
The DT fixture is the enrolled raw-cell snapshot; --dtb verifies original bytes.
"""
import argparse,hashlib,json,re,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--dtb',type=Path);p.add_argument('--cc',default='cc');p.add_argument('--out',type=Path);a=p.parse_args();out=a.out or Path(tempfile.mkdtemp(prefix='pcie-wrapper-'));out.mkdir(parents=True,exist_ok=True)
 fixture=ROOT/'tools/daemonarchy/tests/fixtures/pi500-pcie2-providers.json';f=json.loads(fixture.read_text());mode='retained-provider-snapshot'
 if a.dtb:
  import runpy
  blob=a.dtb.read_bytes();assert hashlib.sha256(blob).hexdigest()==f['source_dtb_sha256'];nodes=runpy.run_path(str(ROOT/'tools/daemonarchy/tests/bcm2712-dma-policy-independent.py'))['decode_dtb'](blob)
  for path,props in f['nodes'].items():
   for key,value in props.items():assert nodes[path][key].hex()==value
  mode='actual-dtb-provider-snapshot-verified'
 paths=list(f['nodes']);ids={path:i+1 for i,path in enumerate(paths)};rows=[];data=[]
 for path,props in f['nodes'].items():
  for name,value in props.items():
   raw=bytes.fromhex(value);i=len(data);data.append('static const unsigned char p%d[]={%s};'%(i,','.join(str(b) for b in raw) or '0'));rows.append('{%d,%s,p%d,%d}'%(ids[path],json.dumps(name),i,len(raw)))
 data += ['struct prop {int node;const char *name;const unsigned char *data;unsigned size;};','static const struct prop props[]={'+','.join(rows)+'};','static const int parents[]={0,'+','.join(str(ids.get(path.rsplit('/',1)[0] or '/',0)) if path!='/' else '0' for path in paths)+'};']
 data += ['#define FIXTURE_HOST %d'%ids['/axi/pcie@1000120000'],'#define FIXTURE_GPIO %d'%ids['/soc@107c000000/gpio@7d517c00'],'#define FIXTURE_MSI %d'%ids['/axi/msi-controller@1000130000']]
 (out/'provider-fixture.h').write_text('\n'.join(data)+'\n')
 source=ROOT/'sys/arm64/broadcom/bcm2712_pcie.c';s=source.read_text();s=re.sub(r'^#include .*\n','',s,flags=re.M)
 # Tests compile every production method and the real class table against typed API stubs.
 pre=ROOT/'tools/daemonarchy/tests/bcm2712-pcie-wrapper-prefix.h';test=ROOT/'tools/daemonarchy/tests/bcm2712-pcie-wrapper.c'
 generic=ROOT/'sys/dev/pci/pci_host_generic.c';g=generic.read_text();inherited=[]
 for name in ['generic_pcie_containing_range','generic_pcie_translate_resource','generic_pcie_map_resource','generic_pcie_unmap_resource','generic_pcie_get_dma_tag']:
  begin=g.index(name+'(');begin=g.rfind('static ',0,begin);body=g.index('{',begin);depth=1;end=body+1
  while depth:
   depth+=(g[end]=='{')-(g[end]=='}');end+=1
  inherited.append(g[begin:end])
 (out/'unit.c').write_text(pre.read_text()+'\n'+'\n'.join(inherited)+'\n'+s+'\n'+test.read_text());(out/'sys').mkdir(exist_ok=True);(out/'sys/stdint.h').write_text('#include <stdint.h>\n')
 if not (out/'arm64').exists():(out/'arm64').symlink_to(ROOT/'sys/arm64',target_is_directory=True)
 cmd=[a.cc,'-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-parameter','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(out),str(out/'unit.c'),str(ROOT/'sys/arm64/broadcom/bcm2712_pcie_hw.c'),str(ROOT/'sys/arm64/broadcom/bcm2712_dma_policy.c'),'-o',str(out/'test')]
 if 'clang' in Path(a.cc).name:
  probe=subprocess.run([a.cc,'-Werror','-Wno-gcc-install-dir-libstdcxx','-x','c','-fsyntax-only','-'],input='',text=True,capture_output=True)
  if probe.returncode==0:cmd.insert(1,'-Wno-gcc-install-dir-libstdcxx')
 subprocess.run(cmd,check=True);subprocess.run([str(out/'test')],check=True)
 files=[source,pre,test,Path(__file__),generic,fixture];(out/'result.json').write_text(json.dumps({'ok':True,'fixture_mode':mode,'scope':'production frontend control flow with injected Newbus/OFW/resource primitives; not hardware execution','sources':{str(f.relative_to(ROOT)):hashlib.sha256(f.read_bytes()).hexdigest() for f in files}},indent=2)+'\n');print(out)
if __name__=='__main__':main()
