#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Execute actual Newbus driver lookup/probe and selected GPIO OFW forwarding.

Typed host services substitute registration storage only; no controller MMIO.
"""
import argparse,hashlib,json,re,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
def function(text,name):
 match=re.search(r'^[ \t]*'+re.escape(name)+r'\(',text,re.M);assert match,name;pos=match.start();start=text.rfind('\n',0,pos-1)+1
 if text[start:pos].strip()=='':raise AssertionError(name)
 body=text.index('{',pos);end=body+1;depth=1
 while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[start:end]
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--cc',default='cc');p.add_argument('--driver',type=Path);p.add_argument('--expect-unbound',action='store_true');p.add_argument('--out',type=Path);a=p.parse_args();out=a.out or Path(tempfile.mkdtemp(prefix='gpio-binding-'));out.mkdir(parents=True,exist_ok=True)
 driver=a.driver or ROOT/'sys/dev/gpio/bcmstb_gpio.c';d=driver.read_text();bus=(ROOT/'sys/kern/subr_bus.c').read_text();ofw=(ROOT/'sys/dev/gpio/ofw_gpiobus.c').read_text();default=(ROOT/'sys/dev/ofw/ofw_bus_if.m').read_text()
 assert re.search(r'static driver_t bcmstb_gpio_driver\s*=\s*{\s*"bcmstbgpio"',d)
 bound=bool(re.search(r'EARLY_DRIVER_MODULE\(ofw_gpiobus,\s*bcmstbgpio,\s*ofw_gpiobus_driver',d))
 forwarding=bool(re.search(r'DEVMETHOD\(ofw_bus_get_node,\s*bcmstb_gpio_get_node\)',d))
 funcs=[function(bus,n) for n in ['devclass_find_driver_internal','first_matching_driver','next_matching_driver','device_probe_child']]+[function(default,'ofw_bus_default_get_node'),function(ofw,'ofw_gpiobus_probe')]
 if forwarding:funcs.append(function(d,'bcmstb_gpio_get_node'))
 else:funcs.append('static phandle_t bcmstb_gpio_get_node(device_t bus,device_t child) {return ofw_bus_default_get_node(bus,child);}')
 prefix=(ROOT/'tools/daemonarchy/tests/bcmstb-gpio-binding-prefix.h').read_text();test=(ROOT/'tools/daemonarchy/tests/bcmstb-gpio-binding.c').read_text();(out/'unit.c').write_text(prefix+'\n'+'\n'.join(funcs)+f'\n#define ACTUAL_REGISTRATION {int(bound)}\n#define ACTUAL_FORWARDING {int(forwarding)}\n#define EXPECT_UNBOUND {int(a.expect_unbound)}\n'+test)
 cmd=[a.cc,'-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-function','-Wno-sign-compare','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(out/'unit.c'),'-o',str(out/'test')]
 if 'clang' in Path(a.cc).name:
  probe=subprocess.run([a.cc,'-Werror','-Wno-gcc-install-dir-libstdcxx','-x','c','-fsyntax-only','-'],input='',text=True,capture_output=True)
  if probe.returncode==0:cmd.insert(1,'-Wno-gcc-install-dir-libstdcxx')
 subprocess.run(cmd,check=True);subprocess.run([str(out/'test')],check=True)
 (out/'result.json').write_text(json.dumps({'ok':True,'driver_sha256':hashlib.sha256(d.encode()).hexdigest(),'actual_registration':bound,'actual_forwarding':forwarding,'expected_unbound':a.expect_unbound,'scope':'actual Newbus lookup/probe and selected child node method; host services/no GPIO MMIO','source_hashes':{str(x.relative_to(ROOT)):hashlib.sha256(x.read_bytes()).hexdigest() for x in [ROOT/'sys/kern/subr_bus.c',ROOT/'sys/dev/gpio/ofw_gpiobus.c',ROOT/'sys/dev/ofw/ofw_bus_if.m']}},indent=2)+'\n');print(out)
if __name__=='__main__':main()
