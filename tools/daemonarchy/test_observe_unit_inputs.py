#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Portable native-source observer controls; fake make proves no native build."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

SCRIPT=Path(__file__).with_name("observe-unit-inputs.py")
spec=importlib.util.spec_from_file_location("source_observer",SCRIPT)
observer=importlib.util.module_from_spec(spec)
spec.loader.exec_module(observer)


class SourceObserverTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.root=Path(self.temp.name)
        self.repo=self.root/"repo"
        self.repo.mkdir()
        self.git("init","-q")
        self.git("config","user.email","test@example.invalid")
        self.git("config","user.name","Source fixture")
        for name,data in (("usr.bin/tool/Makefile","PROG=tool\n"),("usr.bin/tool/tool.c","int main(void){return 0;}\n"),
                          ("share/mk/bsd.prog.mk","# shared native rules\n")):
            path=self.repo/name
            path.parent.mkdir(parents=True,exist_ok=True)
            path.write_text(data)
        self.git("add",".")
        self.git("commit","-qm","fixture")
        self.revision=self.git("rev-parse","HEAD")
        self.src_conf=self.root/"protected-src.conf"
        self.make_conf=self.root/"protected-make.conf"
        self.src_conf.write_bytes(b"# protected/local configuration contents\n")
        self.make_conf.write_bytes(b"# selected local make configuration\n")
        self.context={"target":"amd64","target_arch":"amd64","options":{"WITH_TESTS":"yes"},
                      "flags":{"CC":"/usr/bin/cc","CFLAGS":"-O2"},
                      "src_conf_sha256":observer.digest(self.src_conf.read_bytes()),
                      "make_conf_sha256":observer.digest(self.make_conf.read_bytes()),"payload_scope":"compile"}
        self.values={name:"" for name in observer.VARIABLES}
        self.values.update(PROG="tool",SRCS="tool.c",CC="/usr/bin/cc",CFLAGS="-O2",TARGET="amd64",TARGET_ARCH="amd64",MK_TESTS="yes")
        self.values[".MAKE.MAKEFILES"]="Makefile "+str(self.repo/"share/mk/bsd.prog.mk")
        self.values_path=self.root/"values.json"
        self.fake=self.root/"fake-make"
        self.fake.write_text("#!"+sys.executable+"\nimport json,os,sys,time\nfrom pathlib import Path\n"+
            "v=json.loads(Path("+repr(str(self.values_path))+").read_text())\n"+
            "if '__sleep' in v:\n Path("+repr(str(self.root/"child.pid"))+").write_text(str(os.getpid()))\n time.sleep(v['__sleep'])\n"+
            "v['.OBJDIR']=v.get('.OBJDIR') or os.environ['MAKEOBJDIR']\n"+
            "for i,arg in enumerate(sys.argv):\n if arg=='-V': print(v[sys.argv[i+1]])\n")
        self.fake.chmod(0o755)
        self.output=self.root/"evidence"
        self.sentinel=self.root/"retained-object"
        self.sentinel.write_bytes(b"keep existing object bytes")

    def tearDown(self):
        self.temp.cleanup()

    def git(self,*args):
        return subprocess.check_output(["git","-C",str(self.repo),*args],text=True).strip()

    def observe(self,**kwargs):
        self.values_path.write_text(json.dumps(self.values))
        with patch.object(observer,"MAKE",str(self.fake)),patch.object(observer.platform,"system",return_value="FreeBSD"), \
                patch.object(observer.os,"geteuid",return_value=1000):
            return observer.observe(self.repo,self.revision,"usr.bin/tool",self.context,self.src_conf,self.make_conf,
                                    self.output,acknowledged=True,**kwargs)

    def test_source_and_shared_bytes_config_hashes_and_isolation_are_recorded(self):
        report=self.observe()
        self.assertIn("usr.bin/tool/tool.c",{r.get("path") for r in report["source_generated_and_make_inputs"]})
        self.assertIn("share/mk/bsd.prog.mk",{r.get("path") for r in report["source_generated_and_make_inputs"]})
        self.assertEqual(report["configuration_inputs"][0]["sha256"],self.context["src_conf_sha256"])
        self.assertNotIn("protected/local configuration contents",(self.output/"observation.json").read_text())
        self.assertFalse(report["compiler_header_trace_complete"])
        self.assertFalse(report["native_profile_admitted"])
        self.assertFalse(report["cache_admission"])
        self.assertEqual(Path(report["variables"][".OBJDIR"]),self.output/"obj")
        self.assertEqual(self.sentinel.read_bytes(),b"keep existing object bytes")
        self.assertEqual(self.git("status","--porcelain"),"")

    def test_library_and_static_inputs_cannot_be_classified_runtime_only(self):
        archive=self.root/"libfixture.a"
        archive.write_bytes(b"actual static archive input")
        self.values.update(DPADD=str(archive),LIBADD="util",LDADD="-lfixture")
        report=self.observe()
        self.assertEqual(report["link_build_inputs"][0]["kind"],"build")
        self.assertEqual(report["link_build_inputs"][0]["sha256"],observer.digest(archive.read_bytes()))
        self.assertEqual({r["declaration"] for r in report["unresolved_inputs"]},{"LIBADD","LDADD"})
        self.assertTrue(all("not-runtime-only" in r["reason"] for r in report["unresolved_inputs"]))

    def test_missing_generated_source_and_headers_remain_unqualified(self):
        self.values.update(SRCS="tool.c generated.c",DPSRCS="generated.h",GENHDRS="generated.h")
        report=self.observe()
        self.assertIn("generated-or-missing-input-producer",{r["reason"] for r in report["unresolved_inputs"]})
        self.assertIn("generated-producer-not-admitted",{r["reason"] for r in report["unresolved_inputs"]})
        self.assertFalse(report["recursive_closure_complete"])

    def test_protected_configuration_change_refuses_before_evaluation(self):
        self.src_conf.write_bytes(b"changed configuration")
        with self.assertRaisesRegex(ValueError,"configuration digest"):
            self.observe()
        self.assertFalse(self.output.exists())

    def test_changed_shared_source_refuses_before_evaluation(self):
        (self.repo/"share/mk/bsd.prog.mk").write_text("changed rules")
        with self.assertRaises(subprocess.CalledProcessError):
            self.observe()
        self.assertFalse(self.output.exists())

    def test_make_must_retain_isolated_object_directory(self):
        self.values[".OBJDIR"]=str(self.repo/"usr.bin/tool")
        with self.assertRaisesRegex(ValueError,"isolated object"):
            self.observe()
        self.assertTrue((self.output/"failure.json").exists())

    def test_native_target_and_option_mismatch_refuse(self):
        self.values["MK_TESTS"]="no"
        with self.assertRaisesRegex(ValueError,"native option"):
            self.observe()
        self.assertTrue((self.output/"make.stdout").exists())

    def test_no_existing_evidence_or_retained_objects_are_overwritten(self):
        self.observe()
        original=(self.output/"observation.json").read_bytes()
        with self.assertRaises(FileExistsError):
            self.observe()
        self.assertEqual((self.output/"observation.json").read_bytes(),original)
        self.assertEqual(self.sentinel.read_bytes(),b"keep existing object bytes")

    def test_bounded_query_reaps_process_and_retains_failure(self):
        self.values["__sleep"]=10
        with self.assertRaises(TimeoutError):
            self.observe(timeout=1)
        with self.assertRaises(ProcessLookupError):
            os.kill(int((self.root/"child.pid").read_text()),0)
        self.assertTrue((self.output/"make.exit").exists())
        self.assertFalse(json.loads((self.output/"failure.json").read_bytes())["cache_admission"])

    def test_context_cannot_inject_output_or_unreviewed_expansions(self):
        context=copy.deepcopy(self.context)
        context["flags"]["OBJTOP"]="/retained/objects"
        with self.assertRaises(ValueError):
            observer.validate_context(context)
        context=copy.deepcopy(self.context)
        context["flags"]["CC"]="$(unreviewed)"
        with self.assertRaises(ValueError):
            observer.validate_context(context)

    def test_cli_help_and_native_guard_do_not_build(self):
        result=subprocess.run([sys.executable,str(SCRIPT),"--help"],capture_output=True)
        self.assertEqual(result.returncode,0)
        self.assertIn(b"without compiling",result.stdout)
        with self.assertRaisesRegex(ValueError,"acknowledge"):
            observer.observe(self.repo,self.revision,"usr.bin/tool",self.context,self.src_conf,self.make_conf,
                             self.output,acknowledged=False)
        self.assertFalse(self.output.exists())


if __name__=="__main__":
    unittest.main()
