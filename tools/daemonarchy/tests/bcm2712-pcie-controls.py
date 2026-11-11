#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Compile the actual pure PCIe2 plan/encoder/startup translation unit."""
import argparse,hashlib,json,runpy,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--cc',default='cc');p.add_argument('--out',type=Path);p.add_argument('--dtb',type=Path);a=p.parse_args();out=a.out or Path(tempfile.mkdtemp(prefix='bcm2712-pcie2-'));out.mkdir(parents=True,exist_ok=True)
 fixture=ROOT/'tools/daemonarchy/tests/fixtures/pi500-pcie2-cells.json';f=json.loads(fixture.read_text());mode='retained-cell-snapshot'
 if a.dtb:
  blob=a.dtb.read_bytes();assert hashlib.sha256(blob).hexdigest()==f['source_dtb_sha256'];api=runpy.run_path(str(ROOT/'tools/daemonarchy/tests/bcm2712-dma-policy-independent.py'));nodes=api['decode_dtb'](blob)
  for key,value in f['cells'].items():assert api['cells'](nodes,f['path'],key)==value,key
  assert list(nodes[f['path']]['brcm,vdm-qos-map'])==f['qos'];mode='actual-dtb-verified'
 fields={'reg':'reg','ranges':'ranges','dma':'dma-ranges','resets':'resets','interrupts':'interrupts','interrupt_map':'interrupt-map'};lines=['/* Generated from retained raw DT cells; no driver implementation. */']
 for name,key in fields.items():lines.append('static const uint32_t fixture_'+name+'[]={'+','.join(hex(v) for v in f['cells'][key])+'};')
 lines+=['static struct bcm2712_pcie_dt selected_dt(void) {','struct bcm2712_pcie_dt d={0};']
 for name,key in fields.items():lines.append(f'd.{name}=fixture_{name}; d.n{name}={len(f["cells"][key])};')
 for name,value in f['scalars'].items():lines.append(f'd.{name}={value};')
 lines += [f'd.parent_acells={f["parent_acells"]};d.parent_scells={f["parent_scells"]};']
 for i,v in enumerate(f['qos']):lines.append(f'd.qos[{i}]={v};')
 for i,v in enumerate(f['cells']['interrupt-map-mask']):lines.append(f'd.interrupt_mask[{i}]={v};')
 lines+=['return d;}'];(out/'selected-pcie2.h').write_text('\n'.join(lines)+'\n');(out/'sys').mkdir(exist_ok=True);(out/'sys/stdint.h').write_text('#include <stdint.h>\n')
 if not (out/'arm64').exists():(out/'arm64').symlink_to(ROOT/'sys/arm64',target_is_directory=True)
 files=['sys/arm64/broadcom/bcm2712_pcie_hw.c','sys/arm64/broadcom/bcm2712_dma_policy.c','tools/daemonarchy/tests/bcm2712-pcie-hw.c'];cmd=[a.cc,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-I'+str(out),*[str(ROOT/x) for x in files],'-o',str(out/'test')]
 if 'clang' in Path(a.cc).name:
  cmd.insert(1,'-fsanitize=unsigned-integer-overflow')
  suppression='-Wno-gcc-install-dir-libstdcxx'
  probe=subprocess.run([a.cc,'-Werror',suppression,'-x','c','-fsyntax-only','-'],input='',text=True,capture_output=True)
  if probe.returncode==0:cmd.insert(1,suppression)
 subprocess.run(cmd,check=True);subprocess.run([str(out/'test')],check=True)
 sources=files+['sys/arm64/broadcom/bcm2712_pcie_hw.h','tools/daemonarchy/tests/fixtures/pi500-pcie2-cells.json','tools/daemonarchy/tests/bcm2712-pcie-controls.py'];(out/'result.json').write_text(json.dumps({'ok':True,'scope':'production encoder/state unit with injected primitives; no native controller execution','fixture_mode':mode,'source_hashes':{x:hashlib.sha256((ROOT/x).read_bytes()).hexdigest() for x in sources}},indent=2)+'\n');print(out)
if __name__=='__main__':main()
