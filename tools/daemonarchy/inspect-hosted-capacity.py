#!/usr/bin/env python3
"""Inspect a hosted controller's capacity; never execute or qualify FreeBSD."""
import concurrent.futures
import datetime
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys

GIB = 1024**3

def command(argv):
    try:
        result = subprocess.run(argv, capture_output=True, text=True, timeout=8)
        return result.stdout.strip() if result.returncode == 0 else None
    except (OSError, subprocess.TimeoutExpired):
        return None

system = platform.system()
cpus = os.cpu_count() or 0
if hasattr(os, 'sched_getaffinity'):
    cpus = min(cpus, len(os.sched_getaffinity(0)))
if system == 'Linux':
    memory = {}
    for line in Path('/proc/meminfo').read_text().splitlines():
        key, value = line.split(':', 1)
        if key in ('MemTotal', 'MemAvailable'):
            memory[key] = int(value.split()[0]) * 1024
    total_memory, available_memory = memory['MemTotal'], memory['MemAvailable']
    current_cgroup = next((x.split(':', 2)[2] for x in Path('/proc/self/cgroup').read_text().splitlines() if x.startswith('0::')), '/')
    cgroup = Path('/sys/fs/cgroup') / current_cgroup.lstrip('/')
    try:
        quota, period = (cgroup / 'cpu.max').read_text().split()
        if quota != 'max':
            cpus = min(cpus, int(quota) // int(period))
    except (OSError, ValueError):
        pass
    try:
        limit = (cgroup / 'memory.max').read_text().strip()
        if limit != 'max':
            available_memory = min(available_memory, max(0, int(limit) - int((cgroup / 'memory.current').read_text())))
    except (OSError, ValueError):
        pass
    kvm = Path('/dev/kvm')
    acceleration = kvm.exists() and (os.access(kvm, os.R_OK | os.W_OK) or command(['sudo', '-n', '/usr/bin/test', '-r', '/dev/kvm', '-a', '-w', '/dev/kvm']) is not None)
    profile = dict(jobs=4, guest_memory_bytes=12*GIB, host_memory_reserve_bytes=2*GIB)
    candidates = ['/usr/local/lib/android', '/usr/share/dotnet', '/opt/ghc', '/opt/hostedtoolcache']
elif system == 'Darwin':
    total_memory = int(command(['sysctl', '-n', 'hw.memsize']) or 0)
    vm = command(['vm_stat']) or ''
    import re
    page = re.search(r'page size of (\d+) bytes', vm)
    counts = [re.search(r'^' + label + r':\s+(\d+)\.', vm, re.M) for label in ('Pages free', 'Pages inactive', 'Pages speculative')]
    available_memory = int(page[1]) * sum(int(x[1]) for x in counts) if page and all(counts) else None
    acceleration = command(['sysctl', '-n', 'kern.hv_support']) == '1'
    profile = dict(jobs=3, guest_memory_bytes=10*GIB, host_memory_reserve_bytes=2*GIB)
    candidates = ['/Applications', '/Library/Developer/CommandLineTools']
else:
    total_memory = available_memory = None
    acceleration = False
    profile = dict(jobs=4, guest_memory_bytes=12*GIB, host_memory_reserve_bytes=2*GIB)
    candidates = []

# Inspect possible bootstrap filesystems separately, without aggregating their
# capacity or admitting a multi-volume profile before its layout is reviewed.
storage_observations = []
locations = [('workspace', Path.cwd()), ('temporary', Path('/tmp'))]
if os.environ.get('RUNNER_TEMP'):
    locations.append(('runner-temporary', Path(os.environ['RUNNER_TEMP'])))
if system == 'Linux':
    locations.append(('ephemeral-mount', Path('/mnt')))
for role, location in locations:
    try:
        selected = location.resolve(strict=True)
        volume = shutil.disk_usage(selected)
        observation = dict(role=role, path=str(selected), device_id=selected.stat().st_dev,
                           total_bytes=volume.total, free_bytes=volume.free,
                           directly_writable=os.access(selected, os.W_OK),
                           state='observed')
        if system == 'Linux':
            observation['mount'] = command(
                ['findmnt', '-n', '-T', str(selected), '-o', 'FSTYPE,SOURCE,MAJ:MIN'])
        storage_observations.append(observation)
    except OSError:
        storage_observations.append(dict(role=role, path=str(location), state='unknown'))

storage = shutil.disk_usage('.')
# Keep the native controller's 80 GiB output requirement plus bootstrap space.
required_disk = 90 * GIB
reasons = []
if platform.machine().lower() not in ('x86_64', 'amd64'):
    reasons.append('host architecture is not amd64')
if cpus < profile['jobs']:
    reasons.append('insufficient assigned CPUs')
if available_memory is None or available_memory < profile['guest_memory_bytes'] + profile['host_memory_reserve_bytes']:
    reasons.append('insufficient or unknown available host memory')
if storage.free < required_disk:
    reasons.append('less than 80 GiB native workspace plus 10 GiB bootstrap reserve')
if not acceleration:
    reasons.append('hardware virtualization unavailable or not accessible')

def inventory(path):
    if not Path(path).exists():
        return dict(path=path, state='absent')
    value = command(['du', '-sk', path])
    return dict(path=path, state='measured' if value else 'unknown', bytes=int(value.split()[0])*1024 if value else None)

cleanup_candidates = []
if storage.free < required_disk:
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        cleanup_candidates = list(pool.map(inventory, candidates))
report = dict(schema='daemonarchy/hosted-worker-capacity@1',
    observed_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
    source_revision=os.environ.get('GITHUB_SHA'),
    host=dict(os=system, architecture=platform.machine(), assigned_cpus=cpus,
              total_memory_bytes=total_memory, available_memory_bytes=available_memory,
              storage_total_bytes=storage.total, storage_free_bytes=storage.free,
              hardware_virtualization_supported=acceleration, hypervisor_execution_tested=False),
    proposed_profile=profile, required_host_free_bytes=required_disk,
    host_capacity_sufficient=not reasons, reasons=reasons,
    storage_observations=storage_observations,
    multi_volume_profile_admitted=False,
    cleanup_candidates=cleanup_candidates, deletion_performed=False,
    qualification=dict(native_freebsd_execution=False, aggregate_build=False,
                       sdk=False, performance=False, release=False))
out = Path(sys.argv[1]); out.parent.mkdir(parents=True, exist_ok=True)
with out.open('x') as stream:
    json.dump(report, stream, indent=2); stream.write('\n')
print(json.dumps(report, indent=2))
