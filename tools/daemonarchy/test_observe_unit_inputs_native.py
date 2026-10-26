#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Real hosted FreeBSD BSD-make smoke; controller-only runs are native-unavailable.

The guest needs base sh/make/timeout/sha256/su, not Python/git or package installs.
The host emits the production command/environment factory, the guest runs it as
an existing unprivileged user against a tiny read-only copy of actual inputs,
and the host validates returned bytes with the production metadata decoder.
This observes native parsing, not a selected-release profile, compiler-input
closure, original-source build, authenticated byte-at-build-time proof or receipt.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import shlex
import subprocess
import sys
import unittest
import uuid

KIND = "source"
ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("native_producer",Path(__file__).with_name("observe-unit-inputs.py"))
PRODUCER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PRODUCER)
REPORT_ENV = "DAEMONARCHY_NATIVE_PRODUCER_REPORT"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def shell_argument(value):
    markers={"/__NATIVE_SOURCE__":'"$source_root"',"/__NATIVE_RUN__":'"$run_root"',
             "__ACTUAL_OSVERSION__":'"$osversion"'}
    return "".join(markers.get(part,shlex.quote(part)) for part in
                   re.split(r"(/__NATIVE_SOURCE__|/__NATIVE_RUN__|__ACTUAL_OSVERSION__)",value))


def emit_probe(output):
    output.mkdir(parents=True,exist_ok=False)
    private=output/"private"
    private.mkdir(mode=0o700)
    empty=sha(b"")
    source,run=Path("/__NATIVE_SOURCE__"),Path("/__NATIVE_RUN__")
    if KIND=="source":
        unit="usr.bin/truncate"
        roots=[unit,"share/mk","sys/conf","sys/sys/param.h","tools/build/mk","COPYRIGHT","usr.bin/Makefile.inc"]
        context={"target":"amd64","target_arch":"amd64","options":{"WITHOUT_TESTS":"yes"},
                 "flags":{"CC":"/usr/bin/cc"},"src_conf_sha256":empty,"make_conf_sha256":empty,"payload_scope":"compile"}
        args,variables,environment=PRODUCER.native_invocation(source,unit,context,run/"src.conf",run/"make.conf",run)
    else:
        unit="archivers/liblz4"
        roots=[unit,"Mk","Templates","Keywords"]
        context={"architecture":"amd64","osversion":"__ACTUAL_OSVERSION__","flavor":None,
                 "options":{"TEST":False},"make_variables":{"CC":"/usr/bin/cc"},
                 "package_database":{"mode":"empty-diagnostic","identity":None}}
        args=PRODUCER.command(source,unit,run,context)
        variables=PRODUCER.VARIABLES
        environment=PRODUCER.query_environment(run)
    roots=[name for name in roots if (ROOT/name).exists()]
    paths=set()
    for name in roots:
        path=ROOT/name
        paths.update(p for p in path.rglob("*") if p.is_file() or p.is_symlink()) if path.is_dir() else paths.add(path)
    regular,links,hashes=[],[],[]
    for path in sorted(paths):
        name=path.relative_to(ROOT).as_posix()
        if not re.fullmatch(r"[A-Za-z0-9_./+-]+",name):
            raise ValueError("native smoke requires unambiguous fixture filenames")
        if path.is_symlink():
            target=os.readlink(path)
            if any(ord(c)<32 for c in target):
                raise ValueError("unsupported native fixture link")
            links.append((name,sha(target.encode())))
        else:
            regular.append(name)
            hashes.append(sha(path.read_bytes()))
    (output/"selected-roots.txt").write_text("\n".join(roots)+"\n")
    (output/"regular.names").write_text("\n".join(regular)+"\n")
    (output/"regular.sha256").write_text("\n".join(hashes)+"\n")
    (output/"links.txt").write_text("".join(name+" "+identity+"\n" for name,identity in links))
    request={"schema":"daemonarchy/hosted-native-producer-smoke-request@1","kind":KIND,"unit":unit,
             "source_commit":subprocess.check_output(["git","-C",str(ROOT),"rev-parse","HEAD"],text=True).strip(),
             "source_tree":subprocess.check_output(["git","-C",str(ROOT),"rev-parse","HEAD^{tree}"],text=True).strip(),
             "context":context,"variables":list(variables),"regular_inputs":len(regular),
             "producer_identity":sha(Path(PRODUCER.__file__).read_bytes()),"smoke_driver_identity":sha(Path(__file__).read_bytes()),
             "selected_input_identity":sha((output/"regular.names").read_bytes()+(output/"regular.sha256").read_bytes()+(output/"links.txt").read_bytes()),
             "selected_release_context_verified":False}
    (output/"request.json").write_text(json.dumps(request,indent=2,sort_keys=True)+"\n")
    query="exec timeout -k 5 45 env -i "+" ".join(shell_argument(k+"="+v) for k,v in sorted(environment.items()))+" "+" ".join(shell_argument(v) for v in args)
    body=r'''#!/bin/sh
set -eu
PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/sbin:/usr/local/bin
export PATH
repo=$PWD
out=$repo/@@OUTPUT@@
private=$out/private
mkdir -p "$private"
chmod 700 "$private"
uname -s > "$private/system"
uname -r > "$private/release"
uname -m > "$private/machine"
if test "$(cat "$private/system")" != FreeBSD; then
    echo native-unavailable > "$private/status"
    echo actual-FreeBSD-required > "$private/reason"
    exit 78
fi
for tool in make timeout sha256 su cc tar xargs cmp mktemp realpath; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo native-unavailable > "$private/status"
        echo "base-tool-unavailable:$tool" > "$private/reason"
        exit 78
    fi
done
if test "$(cat "$private/machine")" != amd64; then
    echo native-unavailable > "$private/status"
    echo supported-amd64-context-unavailable > "$private/reason"
    exit 78
fi
if command -v python3 >/dev/null 2>&1; then echo available; else echo unavailable; fi > "$private/python"
if command -v git >/dev/null 2>&1; then echo available; else echo unavailable; fi > "$private/git"
sysctl -n kern.osreldate > "$private/osversion"
osversion=$(cat "$private/osversion")
cc --version > "$private/compiler-version"
sha256 -q "$(realpath "$(command -v cc)")" > "$private/compiler-sha256"
run_root=$(mktemp -d /var/tmp/daemonarchy-producer.XXXXXXXX)
printf '%s\n' "$run_root" > "$private/run-root"
chmod 755 "$run_root"
source_root=$run_root/source
mkdir -p "$source_root"
tar -C "$repo" -cf - -T "$out/selected-roots.txt" | tar -C "$source_root" -xf -
chmod -R a-w "$source_root"
for directory in home tmp obj objtop error workspace work distfiles packages options pkgdb pkgcache evidence; do
    mkdir -m 700 "$run_root/$directory"
done
: > "$run_root/src.conf"
: > "$run_root/make.conf"
chmod 444 "$run_root/src.conf" "$run_root/make.conf"
(cd "$source_root" && xargs sha256 -q < "$out/regular.names") > "$private/inputs.before"
cmp "$out/regular.sha256" "$private/inputs.before"
while read -r name expected; do
    actual=$(readlink "$source_root/$name" | tr -d '\n' | sha256 -q)
    test "$actual" = "$expected"
done < "$out/links.txt"
if test -f /var/db/pkg/local.sqlite; then sha256 -q /var/db/pkg/local.sqlite; else echo unavailable; fi > "$private/pkgdb.before"
cat > "$run_root/query.sh" <<'DAEMONARCHY_QUERY'
#!/bin/sh
set -eu
run_root=$1
source_root=$2
osversion=$3
id -u > "$run_root/evidence/uid"
@@QUERY@@
DAEMONARCHY_QUERY
chmod 755 "$run_root/query.sh"
result=0
if test "$(id -u)" = 0; then
    if ! id nobody >/dev/null 2>&1; then
        echo native-unavailable > "$private/status"
        echo existing-unprivileged-user-unavailable > "$private/reason"
        exit 78
    fi
    for directory in home tmp obj objtop error workspace work distfiles packages options pkgdb pkgcache evidence; do
        chown nobody "$run_root/$directory"
    done
    su -m nobody -c "/bin/sh $run_root/query.sh $run_root $source_root $osversion" > "$private/make.stdout" 2> "$private/make.stderr" || result=$?
else
    /bin/sh "$run_root/query.sh" "$run_root" "$source_root" "$osversion" > "$private/make.stdout" 2> "$private/make.stderr" || result=$?
fi
printf '%s\n' "$result" > "$private/exit"
cat "$run_root/evidence/uid" > "$private/uid" || echo unavailable > "$private/uid"
(cd "$source_root" && xargs sha256 -q < "$out/regular.names") > "$private/inputs.after"
if test -f /var/db/pkg/local.sqlite; then sha256 -q /var/db/pkg/local.sqlite; else echo unavailable; fi > "$private/pkgdb.after"
find "$run_root/obj" "$run_root/objtop" "$run_root/packages" -type f \( -name '*.o' -o -name '*.a' -o -name '*.so' -o -name '*.ko' -o -name '*.pkg' \) > "$private/unexpected-products"
if test "$result" = 0 && cmp -s "$private/inputs.before" "$private/inputs.after" && cmp -s "$private/pkgdb.before" "$private/pkgdb.after" && test ! -s "$private/unexpected-products"; then
    echo observed > "$private/status"
else
    echo failed > "$private/status"
    exit 1
fi
'''
    relative=output.relative_to(ROOT).as_posix()
    if not re.fullmatch(r"[A-Za-z0-9_./+-]+",relative) or ".." in Path(relative).parts:
        raise ValueError("canonical report path required")
    (output/"probe.sh").write_text(body.replace("@@OUTPUT@@",relative).replace("@@QUERY@@",query))
    (output/"probe.sh").chmod(0o700)


def verify_report(output):
    private=output/"private"
    request=json.loads((output/"request.json").read_bytes())
    get=lambda name:(private/name).read_text().strip() if (private/name).exists() else None
    summary={"schema":"daemonarchy/hosted-native-producer-smoke@1","kind":KIND,
             "status":get("status") or "native-unavailable","native_make_observed":False,
             "system":get("system"),"release":get("release"),"architecture":get("machine"),
             "actual_osversion":get("osversion"),"compiler_sha256":get("compiler-sha256"),
             "guest_python_available":get("python")=="available","guest_git_available":get("git")=="available",
             "compiler_version_identity":sha((private/"compiler-version").read_bytes()) if (private/"compiler-version").exists() else None,
             "source_commit":request["source_commit"],"source_tree":request["source_tree"],
             "selected_input_identity":request["selected_input_identity"],"unit":request["unit"],
             "producer_identity":request["producer_identity"],"smoke_driver_identity":request["smoke_driver_identity"],
             "selected_release_context_verified":False,"native_profile_admitted":False,"cache_admission":False,
             "production_python_wrapper_executed_in_guest":False,"production_factory_and_decoder_used":True,
             "build_targets_requested":[],"recursive_closure_complete":False,"reason":get("reason")}
    try:
        if summary["status"]!="observed":
            raise ValueError("native metadata unavailable or failed")
        if summary["system"]!="FreeBSD" or summary["architecture"]!="amd64" or get("exit")!="0" or int(get("uid"))==0:
            raise ValueError("actual native host/unprivileged execution was not observed")
        if KIND=="source":
            values=PRODUCER.decode_values((private/"make.stdout").read_bytes(),request["variables"])
            if values["TARGET"]!="amd64" or values["TARGET_ARCH"]!="amd64" or values["MK_TESTS"]!="no":
                raise ValueError("actual source target/options differ")
            if not values["SRCS"] or not values[".MAKE.MAKEFILES"] or values[".OBJDIR"]!=get("run-root")+"/obj":
                raise ValueError("real source metadata is incomplete")
        else:
            values=PRODUCER.decode_values((private/"make.stdout").read_bytes())
            if values["ARCH"]!="amd64" or values["OPSYS"]!="FreeBSD" or values["OSVERSION"]!=get("osversion"):
                raise ValueError("actual ports OSVERSION/target differs; no release version override is accepted")
            if set(values["COMPLETE_OPTIONS_LIST"].split())!={"TEST"} or values["PORT_OPTIONS"].split() or values["FLAVOR"]:
                raise ValueError("actual port option/flavor context differs")
            PRODUCER.dependency_edges(values)
            if not values["PKGNAME"] or not values[".MAKE.MAKEFILES"]:
                raise ValueError("real port metadata is incomplete")
            summary["package_database_mode"]="empty-diagnostic-not-selected-release-context"
        if (private/"inputs.before").read_bytes()!=(private/"inputs.after").read_bytes() or (private/"pkgdb.before").read_bytes()!=(private/"pkgdb.after").read_bytes():
            raise ValueError("original copied source or installed package database changed")
        summary.update(status="observed",native_make_observed=True,original_package_database_unchanged=True,
                       native_stdout_identity=sha((private/"make.stdout").read_bytes()),
                       native_stderr_identity=sha((private/"make.stderr").read_bytes()),reason=None)
    except (ValueError,TypeError,OSError) as error:
        summary["reason"]=str(error)
        if (private/"make.stderr").exists():
            message=(private/"make.stderr").read_text(errors="replace")[:2048]
            root=get("run-root")
            summary["native_error_excerpt"]=message.replace(root,"@NATIVE_SANDBOX@") if root else "native stderr retained by digest"
    (output/"summary.json").write_text(json.dumps(summary,indent=2,sort_keys=True)+"\n")
    return summary


class NativeProducerSmoke(unittest.TestCase):
    @unittest.skipUnless(platform.system()=="FreeBSD" or os.environ.get(REPORT_ENV),
                         "native-unavailable: actual FreeBSD BSD-make report required")
    def test_actual_native_bsd_make_metadata(self):
        report=os.environ.get(REPORT_ENV)
        if report:
            output=Path(report)
        else:
            output=ROOT/"_build"/("producer-native-"+uuid.uuid4().hex)
            emit_probe(output)
            subprocess.run(["/bin/sh",str(output/"probe.sh")],cwd=ROOT,timeout=240,check=False)
        summary=verify_report(output)
        self.assertTrue(summary["native_make_observed"],summary)
        self.assertFalse(summary["native_profile_admitted"])
        self.assertFalse(summary["cache_admission"])


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emit-probe",type=Path)
    parser.add_argument("--verify-report",type=Path)
    args=parser.parse_args()
    if args.emit_probe:
        emit_probe(args.emit_probe.absolute())
        return 0
    if args.verify_report:
        summary=verify_report(args.verify_report)
        print(json.dumps(summary,sort_keys=True))
        return 0 if summary["native_make_observed"] else 1
    unittest.main(argv=[sys.argv[0]])


if __name__=="__main__":
    raise SystemExit(main())
