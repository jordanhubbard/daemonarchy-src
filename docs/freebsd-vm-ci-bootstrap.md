# Pinned FreeBSD CI bootstrap

The two native CI jobs retain `vmactions/freebsd-vm` commit
`c46abacb49f09938ca4e1702d15d836285d694cc`, its MIT license, FreeBSD 15.0
x86-64 configuration, builder 2.2.8 and AnyVM 0.7.2. Native build, admission,
metadata and result requirements are unchanged.

The action's browser download URL for AnyVM 0.7.2 repeatedly returned HTTP 504
before VM startup in source PR68 runs `42471084664` and `42471122004`, including
failed-job retries. The exact official release asset succeeds through GitHub's
asset API. The fix selects that transport without selecting another runtime,
branch, tag or VM image.

Each job checks out the official action again under `_actions/freebsd-vm`,
with credentials persistence disabled, and downloads asset ID `589232152` using
`gh api -H 'Accept: application/octet-stream'` and the job's read-only GitHub
token. The asset must be 618865 bytes with SHA256
`40a16e846bc07030759936d792343e770acdeb14d4c9991af7409648661a650d`.
No token is retained in the preparation receipt.

`tools/daemonarchy/prepare-freebsd-vm-action.py` requires the exact action HEAD
and clean tracked sources before changing anything. It independently verifies:

| Official input | SHA256 |
| --- | --- |
| `index.js` | `c1e821ad07b8d5957a87298790de4bd50de9f6de87dcd6d91662ec943249d85f` |
| `conf/15.0.conf` | `7979cdfa6380fb55b0d0de013a7c635a9a7ed2a89d2b17c6e6ead7e31a82ccb1` |
| MIT `LICENSE` | `df93e48cff786a5680c3c4832a6e35d61b71a932148a1dabfddbd745275495fb` |

The helper exclusively creates `anyvm.py` from the verified asset. It replaces
only `await downloadFile(anyvmUrl, anyvmPath);` with a version, byte-length and
SHA256 readback assertion. All other action bytes and files are preserved,
including its original line endings, dependencies and license. Repreparation,
modified source, another config pair, wrong assets and occupied outputs refuse.
Publication failure rolls back the helper's own outputs. The action then runs
from the prepared local checkout, rechecking the preseeded runtime immediately
before using it. Receipts bind original/prepared index hashes, configuration,
license, asset identity and helper source; CI uploads them with the fault tests.

Changing `__dirname` would otherwise move cache discovery to quota-limited
`/tmp`. Both matching jobs therefore explicitly use
`$HOME/work/_actions/_vmcache/freebsd-15.0-2.2.8-amd64-v3`, the original upstream
default on Node x64: action input `x86_64` normalizes to the implicit architecture
and the cache key uses `amd64`. The original default `rsync` is explicit too.
Its initial and custom-shell push paths exclude `_actions` at any depth, so the
workspace-local action and the work-volume VM cache are not copied into the
native source tree. This does not change upstream `scp` handling, whose top-level
exclusions do not provide the same guarantee for a nested local action.
The loader's published source commit/tree and feature-input hashes are captured
before the secondary checkout.

Run the production preparation controls against an untouched official checkout
and the official asset:

```sh
python3 tools/daemonarchy/tests/test-freebsd-vm-bootstrap.py \
  --action-checkout /path/to/freebsd-vm-c46abac \
  --runtime /path/to/official-anyvm-0.7.2.py
```

The controls use private sparse clones of the supplied checkout, including a
depth-one checkout. The wrong-pin case creates a private empty commit with a
command-local test author, without requiring a parent commit or deeper production
fetch. They exercise
actual production preparation, preserve input custody, parse the complete
prepared action with `node --check`, and execute the installed JS readback
assertion with correct/wrong versions and valid/truncated/changed/missing
runtime bytes. Partial temporary-index write failure and failed index publication
remove owned temporary/runtime/receipt files while preserving the original index. Passing these controls establishes bootstrap preparation, not VM
startup or a native build. The renewed hosted native jobs remain required.

Primary upstream inputs:
[official action commit](https://github.com/vmactions/freebsd-vm/tree/c46abacb49f09938ca4e1702d15d836285d694cc),
[official runtime release](https://github.com/anyvm-org/anyvm/releases/tag/v0.7.2),
[official asset API](https://api.github.com/repos/anyvm-org/anyvm/releases/assets/589232152).
