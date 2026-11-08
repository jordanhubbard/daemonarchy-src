#!/usr/bin/env python3
"""Compile production pure DMA policy with enrolled raw DTB-cell controls.

No busdma backend, callbacks, MMIO, or native DMA execution is qualified here.
"""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[3]


def generate_fixture():
    data=json.loads((ROOT/'tools/daemonarchy/tests/fixtures/pi500-dma-ranges.json').read_text())
    def u64(a,b):return (a<<32)|b
    raw=data['root_dma_cells'];roots=[];msi=None
    assert len(raw)==len(data['root_roles'])*7
    for i,role in enumerate(data['root_roles']):
        a=raw[i*7:i*7+7];bus=u64(a[1],a[2]);cpu=u64(a[3],a[4]);size=u64(a[5],a[6])
        if role=='raw-msi':msi=(bus,cpu,size)
        else:
            assert role in ('ram','peer')
            roots.append(f'{{UINT64_C({cpu}),UINT64_C({bus}),UINT64_C({size}),BCM2712_DMA_{role.upper()},UINT32_C({a[0]})}}')
    raw=data['child_dma_cells'];children=[]
    assert len(raw)%7==0
    for i in range(0,len(raw),7):
        a=raw[i:i+7]
        children.append(f'{{UINT64_C({u64(a[0],a[1])}),UINT64_C({u64(a[3],a[4])}),UINT64_C({u64(a[5],a[6])}),UINT32_C({a[2]})}}')
    assert msi is not None
    return ('static const struct bcm2712_dma_root_window selected_root[]={'+','.join(roots)+'};\n'
            'static const struct bcm2712_dma_child_window selected_child[]={'+','.join(children)+'};\n'
            'static const struct bcm2712_dma_msi_route selected_msi={'+','.join(f'UINT64_C({n})' for n in msi)+'};\n')


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--cc',default='cc');p.add_argument('--sanitize',action='store_true');a=p.parse_args()
    with tempfile.TemporaryDirectory(prefix='bcm2712-dma-policy-') as tmp:
        path=Path(tmp);(path/'selected-dma-fixture.h').write_text(generate_fixture())
        if not Path('/usr/include/sys/stdint.h').exists():
            (path/'sys').mkdir();(path/'sys/stdint.h').write_text('#include <stdint.h>\n')
        version=subprocess.check_output([a.cc,'--version'],text=True).lower()
        cmd=[a.cc,'-O2','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
             '-I'+str(path),'-I'+str(ROOT/'sys/arm64/broadcom'),
             str(ROOT/'sys/arm64/broadcom/bcm2712_dma_policy.c'),
             str(ROOT/'tools/daemonarchy/tests/bcm2712-dma-policy.c'),'-o',str(path/'control')]
        if a.sanitize:
            sanitizers='address,undefined,unsigned-integer-overflow' if 'clang' in version else 'address,undefined'
            cmd[1:1]=['-fsanitize='+sanitizers,'-fno-omit-frame-pointer']
        subprocess.run(cmd,check=True);subprocess.run([str(path/'control')],check=True)
        # Exercise the actual kernel include branch with the repository headers.
        # This is a compile-only smoke check, not a native GENERIC build.
        includes=path/'kernel-include';includes.mkdir()
        (includes/'machine').symlink_to(ROOT/'sys/arm64/include',target_is_directory=True)
        smoke=[a.cc,'-D_KERNEL','-nostdinc','-Wall','-Wextra','-Werror',
               '-I'+str(ROOT/'sys'),'-I'+str(includes),'-c',
               str(ROOT/'sys/arm64/broadcom/bcm2712_dma_policy.c'),
               '-o',str(path/'policy-kernel.o')]
        if 'clang' in version:
            smoke.insert(1,'--target=aarch64-unknown-freebsd16')
        subprocess.run(smoke,check=True)
        print('PASS production _KERNEL/-nostdinc include closure using repository ARM64 headers (compile only)')


if __name__=='__main__':main()
