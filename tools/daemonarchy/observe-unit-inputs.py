#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Observe one native source directory without compiling or claiming admission.

Use an allocated unprivileged FreeBSD worker, exact source commit, explicit
native target/options/flags and digest-pinned src.conf/make.conf. All objects,
home, temporary files and logs go to a fresh external directory. Makefile
expressions can execute programs: --acknowledge-make-evaluation is required.

SRCS/DPSRCS/GENHDRS and shared make inputs provide candidate byte ownership;
DPADD/LDADD/LIBADD remain actual build/link inputs, never runtime-only by
assumption. Compiler header traces, generated producers, library resolution,
sealed ELF runtime/ABI inspection and canonical retained admission are separate
required gates. No source or existing object/cache/package tree is removed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import resource
import signal
import subprocess
import sys
import time

MAKE = "/usr/bin/make"
VARIABLES = ("PROG", "PROGS", "LIB", "SRCS", "DPSRCS", "GENHDRS", "DPADD", "LDADD", "LIBADD",
             "CC", "CXX", "CPP", "LD", "AR", "RANLIB", "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS",
             "CSTD", "CXXSTD", "CPUTYPE", "SYSROOT", "TARGET", "TARGET_ARCH", "MACHINE", "MACHINE_ARCH",
             "SRCTOP", "OBJTOP", ".OBJDIR", ".MAKE.MAKEFILES")
FLAGS = {"CC", "CXX", "CPP", "LD", "AR", "RANLIB", "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "CPUTYPE", "SYSROOT"}


def require(test, message):
    if not test:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def pairs(items):
    result = {}
    for key, value in items:
        require(key not in result, "duplicate context field")
        result[key] = value
    return result


def read_json(path):
    with path.open("rb") as stream:
        data = stream.read(1024 * 1024 + 1)
    require(len(data) <= 1024 * 1024, "context exceeds byte bound")
    return json.loads(data, object_pairs_hook=pairs)


def relative(value):
    require(isinstance(value, str) and value and PurePosixPath(value).as_posix() == value and
            not PurePosixPath(value).is_absolute() and not any(p in (".", "..") for p in PurePosixPath(value).parts)
            and "\\" not in value and not any(ord(c) < 32 for c in value), "canonical source directory required")
    return value


def validate_context(context):
    require(isinstance(context, dict) and set(context) == {"target", "target_arch", "options", "flags", "src_conf_sha256", "make_conf_sha256", "payload_scope"},
            "explicit source target/configuration/scope required")
    for name in ("target", "target_arch"):
        require(context[name] in {"amd64", "arm64", "aarch64", "i386", "armv7", "riscv", "riscv64"}, "unknown native target")
    require(context["payload_scope"] in {"compile", "link", "headers", "generate"}, "explicit payload action scope required")
    for name in ("src_conf_sha256", "make_conf_sha256"):
        require(isinstance(context[name], str) and re.fullmatch(r"[0-9a-f]{64}", context[name]), "configuration digest required")
    require(isinstance(context["flags"], dict) and set(context["flags"]) <= FLAGS, "unsupported source flag override")
    require(isinstance(context["options"], dict) and all(isinstance(k, str) and
            re.fullmatch(r"(?:WITH|WITHOUT)_[A-Z0-9_]+", k) for k in context["options"]), "explicit WITH_/WITHOUT_ option semantics required")
    for value in (*context["flags"].values(), *context["options"].values()):
        require(isinstance(value, str) and len(value) <= 8192 and not any(ord(c) < 32 for c in value)
                and not any(c in value for c in "$`"), "unreviewed source flag expansion")


def git(repo, *args):
    return subprocess.run(["git", "-C", str(repo), *args], check=True, timeout=20,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          env={**os.environ, "GIT_NO_LAZY_FETCH": "1", "GIT_TERMINAL_PROMPT": "0"}).stdout


def file_bytes(path):
    require(path.is_file() and not path.is_symlink(), "regular input file required: " + str(path))
    with path.open("rb") as stream:
        data = stream.read(32 * 1024 * 1024 + 1)
    require(len(data) <= 32 * 1024 * 1024, "native input exceeds byte bound")
    return data


def member(repo, revision, path):
    path = Path(os.path.abspath(path))
    data = os.readlink(path).encode() if path.is_symlink() else file_bytes(path)
    record = {"sha256": digest(data), "size": len(data)}
    if path.is_relative_to(repo):
        name = path.relative_to(repo).as_posix()
        entries = git(repo, "ls-tree", "-z", revision, "--", name).split(b"\0")
        require(len(entries) == 2 and entries[0], "source input needs an explicit generated producer: " + name)
        metadata, actual = entries[0].split(b"\t", 1)
        mode, kind, oid = metadata.decode().split()
        require(actual.decode() == name and kind == "blob" and mode in {"100644", "100755", "120000"}, "unsupported Git input")
        require(git(repo, "cat-file", "blob", oid) == data, "source input differs from selected Git bytes: " + name)
        record.update(path=name, git_blob=oid, git_mode=mode, owner="source-Component-unregistered")
    else:
        # Protected/local config contents are never embedded in the observation.
        record.update(external_path=str(path), owner="native-SDK-or-config-unadmitted")
    if path.is_symlink():
        record["symlink_target"] = member(repo, revision, path.resolve(strict=True))
    return record


def limit_output():
    resource.setrlimit(resource.RLIMIT_FSIZE, (16 * 1024 * 1024, 16 * 1024 * 1024))


def query(args, env, output, timeout, variables):
    with (output/"make.stdout").open("xb") as stdout, (output/"make.stderr").open("xb") as stderr:
        process = subprocess.Popen(args, env=env, stdout=stdout, stderr=stderr,
                                   start_new_session=True, preexec_fn=limit_output)
        try:
            code = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            raise TimeoutError("source metadata deadline expired; partial logs retained")
        finally:
            if process.poll() is None:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.wait()
            (output/"make.exit").write_text(str(process.returncode)+"\n")
    require(code == 0, "source make metadata exited nonzero; logs retained")
    with (output/"make.stdout").open("rb") as stream:
        raw = stream.read(16*1024*1024+1)
    require(len(raw) <= 16*1024*1024, "source metadata exceeds byte bound")
    lines = raw.decode().splitlines()
    require(len(lines) == len(variables), "source metadata variable inventory is incomplete or multiline")
    return dict(zip(variables, lines))


def observe(repo, revision, unit, context, src_conf, make_conf, output, *, acknowledged, timeout=20):
    require(acknowledged, "Makefile evaluation requires --acknowledge-make-evaluation")
    require(platform.system() == "FreeBSD" and os.geteuid() != 0, "use an allocated unprivileged native FreeBSD worker")
    validate_context(context)
    relative(unit)
    require(isinstance(revision, str) and re.fullmatch(r"[0-9a-f]{40}", revision), "exact source commit required")
    require(type(timeout) is int and 1 <= timeout <= 120, "bounded source metadata deadline required")
    repo, output = repo.resolve(strict=True), output.absolute()
    require((repo/unit).is_dir() and (repo/unit).resolve() == repo/unit, "direct source unit directory required")
    require(not output.is_relative_to(repo) and not repo.is_relative_to(output), "output must be outside source and its ancestors")
    require(output.parent.resolve(strict=True) == output.parent, "output parent must be direct")
    require(git(repo, "rev-parse", "HEAD").decode().strip() == revision, "source checkout differs from selected commit")
    for path, key in ((src_conf, "src_conf_sha256"), (make_conf, "make_conf_sha256")):
        require(digest(file_bytes(path)) == context[key], "protected/local configuration digest changed")
    git(repo, "diff", "--quiet", "--no-ext-diff", "--no-textconv", revision, "--", unit, "share/mk")
    output.mkdir(mode=0o700)
    for name in ("home", "tmp", "obj", "objtop"):
        (output/name).mkdir()
    args = [MAKE, "-C", str(repo/unit), "SRCTOP="+str(repo), "OBJTOP="+str(output/"objtop"),
            "TARGET="+context["target"], "TARGET_ARCH="+context["target_arch"],
            "SRCCONF="+str(src_conf.resolve()), "__MAKE_CONF="+str(make_conf.resolve()), "SRC_ENV_CONF=/dev/null"]
    args += [k+"="+v for k,v in sorted({**context["options"], **context["flags"]}.items())]
    option_variables = sorted({"MK_"+k.split("_",1)[1] for k in context["options"]})
    variables = VARIABLES + tuple(option_variables)
    for variable in variables:
        args.extend(("-V", variable))
    env = {"PATH":"/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/sbin:/usr/local/bin", "HOME":str(output/"home"),
           "TMPDIR":str(output/"tmp"), "LC_ALL":"C", "TZ":"UTC", "MAKEOBJDIR":str(output/"obj"),
           "MAKESYSPATH":str(repo/"share/mk")}
    (output/"request.json").write_text(json.dumps({"revision":revision,"unit":unit,"context":context,"argv":args},sort_keys=True)+"\n")
    started = time.monotonic()
    try:
        values = query(args, env, output, timeout, variables)
        require(values["TARGET"] == context["target"] and values["TARGET_ARCH"] == context["target_arch"], "evaluated native target differs")
        require(Path(values[".OBJDIR"]).resolve() == output/"obj", "source make did not retain isolated object directory")
        for flag in {"CC", "CXX", "CPP", "LD", "AR", "RANLIB", "SYSROOT"} & context["flags"].keys():
            require(values[flag] == context["flags"][flag], "selected native compiler/tool/sysroot differs: " + flag)
        for option in context["options"]:
            expected = "yes" if option.startswith("WITH_") else "no"
            require(values["MK_"+option.split("_",1)[1]] == expected, "evaluated native option differs: "+option)
        inputs, unresolved, link_inputs = [], [], []
        config_inputs = [member(repo, revision, src_conf), member(repo, revision, make_conf)]
        for variable in ("SRCS", "DPSRCS", "GENHDRS", ".MAKE.MAKEFILES"):
            for token in values[variable].split():
                require(time.monotonic()-started <= timeout+60, "source input inventory exceeded total deadline")
                path = Path(token)
                if not path.is_absolute():
                    path = repo/unit/path
                if path.is_file() or path.is_symlink():
                    inputs.append({"declaration":variable, **member(repo,revision,path)})
                else:
                    unresolved.append({"declaration":variable,"input":token,"reason":"generated-or-missing-input-producer"})
                if variable in {"DPSRCS", "GENHDRS"}:
                    unresolved.append({"declaration":variable,"input":token,"reason":"generated-producer-not-admitted"})
        require(values[".MAKE.MAKEFILES"].split(), "source make omitted included build inputs")
        for token in values["DPADD"].split():
            path = Path(token)
            if not path.is_absolute():
                path = output/"obj"/path
            if path.is_file() or path.is_symlink():
                link_inputs.append({"declaration":"DPADD","kind":"build",**member(repo,revision,path)})
            else:
                unresolved.append({"declaration":"DPADD","input":token,"reason":"link-input-producer-or-SDK-missing"})
        for variable in ("LIBADD", "LDADD"):
            for token in values[variable].split():
                unresolved.append({"declaration":variable,"input":token,"reason":"native-link-resolution-required-not-runtime-only"})
        git(repo, "diff", "--quiet", "--no-ext-diff", "--no-textconv", revision, "--", unit, "share/mk")
        report = {"schema":"daemonarchy/source-unit-input-observation@1","revision":revision,
                  "git_tree":git(repo,"rev-parse",revision+"^{tree}").decode().strip(),"unit":unit,
                  "context":context,"variables":values,"configuration_inputs":config_inputs,
                  "source_generated_and_make_inputs":inputs,"link_build_inputs":link_inputs,
                  "unresolved_inputs":unresolved,"elapsed_milliseconds":int((time.monotonic()-started)*1000),
                  "recursive_closure_complete":False,"compiler_header_trace_complete":False,
                  "canonical_ownership_registered":False,"native_profile_admitted":False,"cache_admission":False,
                  "remaining_gates":["compiler/header/static/generated closure", "resolved native library producers",
                                     "sealed ELF runtime ABI inspection", "canonical ownership/retained profiles",
                                     "native original/independent tests and current receipts"]}
        (output/"observation.json").write_text(json.dumps(report,indent=2,sort_keys=True)+"\n")
        return report
    except BaseException as error:
        (output/"failure.json").write_text(json.dumps({"error":str(error),"cache_admission":False})+"\n")
        raise


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for argument in ("repo", "context", "src-conf", "make-conf", "output"):
        parser.add_argument("--"+argument,type=Path,required=True)
    parser.add_argument("--revision",required=True)
    parser.add_argument("--unit",required=True)
    parser.add_argument("--timeout",type=int,default=20)
    parser.add_argument("--acknowledge-make-evaluation",action="store_true")
    args=parser.parse_args()
    try:
        report=observe(args.repo,args.revision,args.unit,read_json(args.context),args.src_conf,args.make_conf,args.output,
                       acknowledged=args.acknowledge_make_evaluation,timeout=args.timeout)
        print(json.dumps({"unit":report["unit"],"cache_admission":False,"recursive_closure_complete":False}))
        return 0
    except (ValueError,OSError,TimeoutError,subprocess.SubprocessError) as error:
        print("source input observation refused: "+str(error),file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
