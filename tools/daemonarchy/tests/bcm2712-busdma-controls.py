#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Compile production ARM64 busdma operations with injected VM primitives.

This is a host control, not native busdma or device-DMA acceptance. Source
functions are copied verbatim at test time; the scheduler/VM/cache primitives
are mocks. Full native kernel and actual offset DMA remain separate gates.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]

def function(text, name):
    match = re.search(r"\n(?:static )?[\w *]+\n" + name + r"\(", text)
    if not match:
        raise ValueError(name)
    start = match.start() + 1
    pos = text.index("{", match.end())
    depth = 1
    end = pos + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end] + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    out = args.out or Path(tempfile.mkdtemp(prefix="bcm2712-busdma-"))
    out.mkdir(parents=True, exist_ok=True)
    arm = (ROOT / "sys/arm64/arm64/busdma_bounce.c").read_text()
    common = (ROOT / "sys/kern/subr_busdma_bounce.c").read_text()
    machine = (ROOT / "sys/arm64/arm64/busdma_machdep.c").read_text()
    mi = (ROOT / "sys/kern/subr_bus_dma.c").read_text()
    structs = arm[arm.index("enum {"):arm.index("static bool _bus_dmamap_pagesneeded")]
    structs = re.sub(r"static SYSCTL_NODE\(.*?;\n", "", structs, flags=re.S)
    macros = arm[arm.index("#define\tdmat_alignment"):arm.index('#include "../../kern/subr_busdma_bounce.c"')]
    shared_structs = common[common.index("struct bounce_page {"):common.index("static void busdma_thread")]
    shared_structs = re.sub(r"SYSCTL_INT\(.*?;\n", "", shared_structs, flags=re.S)
    funcs = []
    for name in ["common_bus_dma_tag_create", "bus_dma_template_clone", "bus_dma_tag_set_domain"]:
        funcs.append(function(machine, name))
    shared = ["reserve_bounce_pages", "_bus_dmamap_reserve_pages", "addr_needs_bounce", "add_bounce_page", "free_bounce_pages", "_bus_dmamap_addseg", "_bus_dmamap_addsegs"]
    # ARM64 branch of the two conditional add_bounce_page signatures.
    common = re.sub(r"#if defined\(__amd64__\) \|\| defined\(__i386__\)\nstatic bus_addr_t\nadd_bounce_page\(.*?#else\n", "", common, count=1, flags=re.S)
    common = common.replace("bus_addr_t addr, bus_size_t size)\n#endif", "bus_addr_t addr, bus_size_t size)", 1)
    for name in shared:
        funcs.append(function(common, name))
    selected = ["ram_map_busy", "bounce_bus_dmamap_addsegs", "bounce_bus_dma_zone_setup", "cacheline_bounce", "alignment_bounce", "might_bounce", "must_bounce", "ram_ref_acquire", "bounce_bus_dma_tag_create_internal", "bounce_bus_dma_tag_create", "bus_dma_tag_create_bcm2712_ram", "bus_dma_bounce_domain_limit", "bus_dma_bounce_ram_translated", "bounce_bus_dma_tag_destroy", "bounce_bus_dma_tag_set_domain", "bounce_bus_dma_id_mapped", "alloc_dmamap", "bounce_bus_dmamap_create", "bounce_bus_dmamap_destroy", "bounce_bus_dmamem_alloc", "bounce_bus_dmamem_free", "_bus_dmamap_pagesneeded", "_bus_dmamap_count_phys", "_bus_dmamap_count_pages", "ram_load_check", "ram_source_check", "ram_count_phys", "ram_count_buffer", "ram_validate_buffer", "bounce_bus_dmamap_load_phys", "bounce_bus_dmamap_load_buffer", "ram_counted", "bounce_bus_dmamap_waitok", "bounce_bus_dmamap_complete", "bounce_bus_dmamap_unload", "dma_preread_safe", "dma_dcache_sync", "bounce_bus_dmamap_sync", "bounce_bus_dmamap_load_kmsan"]
    for name in selected:
        funcs.append(function(arm, name))
    # Actual MI traversal supports each memdesc kind. Tests provide small
    # versions of external mbuf/uio definitions, not substitute map policy.
    for name in ["_bus_dmamap_load_vlist", "_bus_dmamap_load_plist", "_bus_dmamap_load_mbuf_epg", "_bus_dmamap_load_single_mbuf", "_bus_dmamap_load_mbuf_sg", "bus_dmamap_load_ma_triv", "_bus_dmamap_load_uio", "bus_dmamap_load_mem", "bus_dma_template_init", "bus_dma_template_fill", "bus_dma_template_tag"]:
        funcs.append(function(mi, name))
    prototypes = "\n".join(f[:f.index("{")].replace("\n", " ") + ";" for f in funcs)
    prefix = Path(__file__).with_name("bcm2712-busdma-prefix.c").read_text()
    dma_header = (ROOT / "sys/sys/bus_dma.h").read_text()
    template_start = dma_header.index("typedef struct {\n\tbus_dma_tag_t\t\tparent;")
    template_end = dma_header.index("} bus_dma_template_t;", template_start) + len("} bus_dma_template_t;")
    prefix = prefix.replace("/* INSERT_TEMPLATE */", dma_header[template_start:template_end])
    params_start = dma_header.index("typedef enum {\n\tBD_PARAM_INVALID")
    params_end = dma_header.index("} bus_dma_param_t;", params_start) + len("} bus_dma_param_t;")
    prefix = prefix.replace("/* INSERT_PARAMS */", dma_header[params_start:params_end])
    tests = Path(__file__).with_name("bcm2712-busdma-test.c").read_text()
    source = prefix + structs + macros + shared_structs + prototypes + "\n" + "\n".join(funcs) + tests
    # Use the deferred worker's exact saved-lock/replay/callback/unlock body.
    worker = function(common, "busdma_thread")
    begin = worker.index("\t\t\tdmat = map->dmat;")
    end = worker.index("\n\t\t}", begin)
    replay = "static void replay_one(bus_dmamap_t map) { bus_dma_tag_t dmat; struct bounce_zone *bz; bus_dma_lock_t *saved_lockfunc; void *saved_lockarg;\n" + worker[begin:end] + "\n}\n"
    source = source.replace("/* INSERT_REPLAY */", replay)
    (out / "control.c").write_text(source)
    (out / "sys").mkdir(exist_ok=True)
    (out / "sys/stdint.h").write_text("#include <stdint.h>\n")
    if not (out / "arm64").exists():
        (out / "arm64").symlink_to(ROOT / "sys/arm64", target_is_directory=True)
    cc = [args.cc, "-std=gnu11", "-g", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-parameter", "-Wno-sign-compare", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", str(out), str(out / "control.c"), str(ROOT / "sys/arm64/broadcom/bcm2712_dma_policy.c"), str(ROOT / "sys/arm64/broadcom/bcm2712_dma_ram.c"), "-o", str(out / "control")]
    if "clang" in Path(args.cc).name:
        cc.insert(1, "-Wno-gcc-install-dir-libstdcxx")
    subprocess.run(cc, check=True)
    subprocess.run([str(out / "control")], check=True)
    hashes = {path: hashlib.sha256((ROOT/path).read_bytes()).hexdigest() for path in ["sys/arm64/arm64/busdma_bounce.c", "sys/arm64/arm64/busdma_machdep.c", "sys/kern/subr_busdma_bounce.c", "sys/kern/subr_bus_dma.c", "sys/arm64/broadcom/bcm2712_dma_ram.c", "sys/arm64/broadcom/bcm2712_dma_policy.c", "sys/arm64/broadcom/bcm2712_dma_policy.h", "sys/arm64/broadcom/bcm2712_dma_ram.h", "sys/arm64/include/bus_dma.h", "sys/arm64/include/bus_dma_impl.h", "sys/sys/bus_dma.h", "sys/conf/files.arm64", "tools/daemonarchy/tests/bcm2712-busdma-controls.py", "tools/daemonarchy/tests/bcm2712-busdma-prefix.c", "tools/daemonarchy/tests/bcm2712-busdma-test.c"]}
    (out / "result.json").write_text(json.dumps({"ok": True, "scope": "host-production-functions-injected-VM", "sources": hashes, "compiler": subprocess.check_output([args.cc, "--version"], text=True).splitlines()[0], "compiler_path": str(Path(shutil.which(args.cc)).resolve()), "compiler_sha256": hashlib.sha256(Path(shutil.which(args.cc)).read_bytes()).hexdigest(), "command": cc, "binary_sha256": hashlib.sha256((out / "control").read_bytes()).hexdigest()}, indent=2)+"\n")
    print(out)

if __name__ == "__main__":
    main()
