#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Prepare exact retained metadata from reviewed native inputs; grant no execution."""
import argparse
from dataclasses import replace
import hashlib
import json
from pathlib import Path

from literate_ai.adapters.component_lock_application import project_component_roots
from literate_ai.adapters.locked_generation_authority import FilesystemLockedGenerationAuthorityReader
from literate_ai.adapters.retained_project_inputs import discover_retained_project_inputs
from literate_ai.cli.dispatch import _parser
from literate_ai.cli.execution_workers import select_execution_worker
from literate_ai.contracts import ContentIdentity, canonical_json_bytes
from literate_ai.contracts.retained_component_ownership import RetainedComponentOwner
from literate_ai.contracts.retained_project import RetainedProjectLimits
from literate_ai.contracts.retained_project_acceptance import RetainedProjectAcceptancePolicy
from literate_ai.contracts.retained_project_lifecycle import RetainedProjectAction, RetainedProjectProfile
from literate_ai.contracts.retained_project_tests import RetainedProjectExpectedCase, RetainedProjectTestPolicy
from literate_ai.projects import LoadedProject, ProjectConfigurationStore

PREFIX = 'tools/daemonarchy/retained'


def digest(path):
    return ContentIdentity.parse_uri('sha256:' + hashlib.sha256(path.read_bytes()).hexdigest())


def secondary_identity(name):
    """Observe an existing account; profile preparation never creates one."""
    if 'secondary_access' not in RetainedProjectProfile.__dataclass_fields__:
        raise ValueError('secondary profiles require a framework with enforcing secondary access')
    import os
    import pwd
    from literate_ai.contracts.retained_test_identity import RetainedSecondaryTestIdentity
    from literate_ai.adapters.retained_test_identity import require_current_secondary_test_identity
    account = pwd.getpwnam(name)
    reviewed = RetainedSecondaryTestIdentity(name, account.pw_uid, account.pw_gid,
        tuple(sorted(set(os.getgrouplist(name, account.pw_gid)))))
    return require_current_secondary_test_identity(reviewed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project', type=Path, default=Path.cwd())
    parser.add_argument('--name', required=True)
    parser.add_argument('--secondary-account', help='existing reviewed nonroot test account')
    args = parser.parse_args()
    if not args.name or any(c not in 'abcdefghijklmnopqrstuvwxyz0123456789-' for c in args.name):
        raise ValueError('profile name must be a short lowercase identifier')
    account = secondary_identity(args.secondary_account) if args.secondary_account is not None else None
    root = args.project.resolve(strict=True)
    snapshot = ProjectConfigurationStore.discover(root)
    if snapshot.root != root:
        raise ValueError('profile must select the project root')
    project = LoadedProject(root, snapshot.definition)
    destination = root / PREFIX / 'generated' / args.name
    destination.mkdir(parents=True)
    profile_path = destination / 'profile.json'
    policy_path = destination / 'test-policy.json'
    inventory = json.loads((root / PREFIX / 'original-cases.json').read_bytes())
    test_policy = RetainedProjectTestPolicy(project.definition.test_receipt_policy.identity,
        tuple(RetainedProjectExpectedCase(c['id'], 'original-tests') for c in inventory['cases']),
        (('original-tests', 'original-tests.json'),))
    policy_path.write_bytes(canonical_json_bytes(test_policy.to_dict()))
    bootstrap = PREFIX + '/bootstrap'
    tools = tuple((name, bootstrap + '/tree/' + path, digest(root / bootstrap / 'tree' / path))
                  for name, path in [('python', 'usr/local/bin/python3.12'), ('make', 'usr/bin/make'),
                                     ('shell', 'bin/sh'), ('compiler', 'usr/bin/cc'),
                                     ('kyua', 'usr/bin/kyua'), ('atf-shell', 'usr/libexec/atf-sh'),
                                     ('linker', 'usr/bin/ld.lld')])
    selected = {bootstrap: ('toolchain', 'native-bootstrap'),
                'share/mk': ('build-definition', 'native-build-support'),
                'literate.release.json': ('release-policy', 'native-build-support'),
                policy_path.relative_to(root).as_posix(): ('test-policy', 'native-build-support'),
                'tools/daemonarchy/prepare-retained-profile.py': ('build-definition', 'native-build-support'),
                'tools/daemonarchy/inventory-bootstrap.py': ('build-definition', 'native-build-support'),
                'tools/daemonarchy/capture-seed.py': ('build-definition', 'native-build-support'),
                'tools/daemonarchy/retained-source-stage.py': ('build-definition', 'native-build-support'),
                'tools/daemonarchy/retained/original-cases.json': ('test-definition', 'native-build-support'),
                'tools/daemonarchy/report-kyua.py': ('test-definition', 'native-build-support'),
                'tools/daemonarchy/retained/bootstrap-selection.json': ('build-definition', 'native-build-support'),
                'lib/libutil/libutil.h': ('source', 'libutil-header'),
                'lib/libutil/expand_number.c': ('source', 'expand-number'),
                'lib/libutil/expand_number.3': ('docs-definition', 'expand-number'),
                'lib/libutil/tests/expand_number_test.c': ('test-definition', 'expand-number'),
                'usr.bin/truncate/truncate.c': ('source', 'truncate'),
                'usr.bin/truncate/truncate.1': ('docs-definition', 'truncate'),
                'usr.bin/truncate/tests/truncate_test.sh': ('test-definition', 'truncate')}
    for name in ('sys/conf/newvers.sh', 'sys/sys/param.h', 'COPYRIGHT',
                 'share/examples/etc/bsd-style-copyright'):
        selected[name] = ('build-definition', 'native-build-support')
    for name in ('lib/Makefile.inc', 'lib/libutil/Makefile', 'lib/libutil/Makefile.inc',
                 'lib/libutil/tests/Makefile', 'usr.bin/Makefile.inc', 'usr.bin/truncate/Makefile',
                 'usr.bin/truncate/Makefile.inc', 'usr.bin/truncate/tests/Makefile'):
        if (root / name).is_file():
            selected[name] = ('build-definition', 'native-build-support')
    component_directories = {path.name: path for path in project_component_roots(project)}
    for owner in sorted({name for _role, name in selected.values()}):
        metadata = component_directories[owner]
        selected[metadata.relative_to(root).as_posix()] = ('build-definition', owner)
    limits = RetainedProjectLimits(12000, 128 * 1024 * 1024, 512 * 1024 * 1024)
    initial = discover_retained_project_inputs(root, {k:v[0] for k,v in selected.items()},
                                              limits, allow_internal_symlinks=True)
    arguments = _parser().parse_args(['build', '--project', str(root), '--target', 'freebsd'])
    worker = select_execution_worker(arguments, project_root=root, target_profile='freebsd').worker
    secondary = None
    if account is not None:
        from literate_ai.contracts.retained_secondary_access import RetainedReadOnlyView, RetainedSecondaryAccessPolicy
        secondary = RetainedSecondaryAccessPolicy(worker.identity, account, ('original-tests',),
            (RetainedReadOnlyView('runtime', 'inputs', bootstrap+'/tree'),
             RetainedReadOnlyView('products', 'exports', 'products')), ('work',))
    acceptance = RetainedProjectAcceptancePolicy.from_dict(json.loads(
        (root / 'verification/retained-project-acceptance.json').read_bytes()))
    def action(name, stage, requires, items, outputs):
        command = ( '-B', '{input_root}/tools/daemonarchy/retained-source-stage.py',
                    {'build':'build','test':'tests','docs':'docs','accept':'accept'}[stage],
                    '--inputs', '{input_root}', '--objects', '{object_root}', '--outputs', '{output_root}')
        if secondary is not None:
            command += ('--runtime-view', '{secondary_view:runtime}',
                        '--product-view', '{secondary_view:products}',
                        '--fixture-view', '{secondary_fixture:work}',
                        '--secondary-user', '{secondary_account}')
        return RetainedProjectAction(name, stage, 'python', command, requires, items, outputs, 300)
    profile = RetainedProjectProfile('freebsd', worker.identity,
        initial.role_identity({'sdk','toolchain'}), initial.role_identity({'dependency'}),
        digest(policy_path), digest(root/'literate.release.json'), tools,
        (action('build-original','build',(),('parser-library','truncate-utility','original-test-programs','public-header'),('products',)),
         action('docs-original','docs',('build-original',),('truncate-manual','parser-manual'),('documentation',)),
         action('original-tests','test',('build-original',),tuple(c['id'] for c in inventory['cases']),('original-tests.json',)),
         action('accept-truncate','accept',('build-original','docs-original','original-tests'),
                tuple(c.case_id for c in acceptance.cases),())),
        ('products/bin/truncate','products/lib/libdaemonarchy_expand_number_pie.a',
         'products/include/libutil.h','documentation'),
        digest(root/'verification/retained-project-acceptance.json'),
        (bootstrap+'/tree/lib',bootstrap+'/tree/usr/lib',bootstrap+'/tree/usr/local/lib'),
        ('products/bin',),
        **({'secondary_access': secondary} if secondary is not None else {}))
    profile_path.write_bytes(canonical_json_bytes(profile.to_dict()))
    selected[profile_path.relative_to(root).as_posix()] = ('build-definition','native-build-support')
    manifest = discover_retained_project_inputs(root, {k:v[0] for k,v in selected.items()},
                                               limits, allow_internal_symlinks=True)
    catalog = FilesystemLockedGenerationAuthorityReader().load_catalog(project_component_roots(project)[0])
    authorings = {a.coordinate.name:a for a in catalog.authorings}
    owners = tuple(RetainedComponentOwner(prefix, authorings[name].coordinate, authorings[name].identity)
                   for prefix,(_role,name) in sorted(selected.items()))
    manifest = replace(manifest,component_ownership=owners)
    catalog.require_unchanged()
    path = destination/'manifest.json'
    path.write_bytes(canonical_json_bytes(manifest.to_dict()))
    print(json.dumps({'schema':'daemonarchy/native-profile-preparation@1',
          'profile':profile_path.relative_to(root).as_posix(), 'manifest':path.relative_to(root).as_posix(),
          'members':len(manifest.members),'canonical_owners':len({o.component.uri for o in owners}),
          'manifest_identity':manifest.identity.uri,'execution_authorized':False,'admitted':False}))


if __name__ == '__main__':
    main()
