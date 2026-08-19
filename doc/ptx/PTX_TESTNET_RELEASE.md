# PTX testnet — cutting the operator release

**Decision (2026-08-19): tag, do not merge to `main`.**

`main` is 26 commits of Discord wiring, README churn and workflow deletions that have nothing to do
with this line. Merging would make `main` the branch this work is actively developed on, which is the
wrong shape while fixes are still landing. Instead: **cut a tag from `feature/ptx-dkg`**, point
`install.sh` at the tag, and keep developing on the branch. A fix means a new tag and a message to
five people.

Tag name: **`v0.1.0-testnet`**. Cut it once genesis is settled and the walk-through fixes are in.

---

## Why this is not just bookkeeping

`install.sh` currently defaults `PTX_REF` to `feature/ptx-dkg`. A branch **moves**. The moment five
operators are pulling, "it worked for me an hour ago" stops being a useful sentence: two operators
running the same command on the same day can be on different code. A tag fixes the ref; a published
checksum fixes the binary. Both are needed, and neither exists yet.

---

## 1. What the release must contain

| artefact | why |
|---|---|
| `Hemis-Linux.tar.gz` | `Hemisd`, `Hemis-cli`, `Hemis-tx` for x86_64 Linux. **This is the one operators need.** |
| `SHA256SUMS` | so an operator can tell whether they got what we published. `install.sh` refuses to install without it (or an explicitly passed hash). |
| `Hemis-Linux.zip` | same binaries, already produced by the workflow; keep for parity with mainnet releases. |
| Windows / macOS / ARM | already produced; not on the operator path, no extra work. |

★ **Build from source is not the operator path.** It needs Boost, BDB 4.8 and a matching toolchain to
line up on a machine we do not control, it takes tens of minutes, and a failure lands at step one of
a guide whose whole point is that step one should not be where people get stuck. `install.sh` prints
the source route but does not attempt it.

---

## 2. What has to be fixed before the first cut — the release job cannot currently produce the tarball

These were found by reading `.github/workflows/build-and-release.yml` against this repository's
actual name, and are fixed in the same commit as this document.

1. ★★ **Hardcoded repository name.** Ten packaging and upload steps referenced
   `/home/runner/work/Hemis/Hemis/…`. `$GITHUB_WORKSPACE` is `/home/runner/work/<repo>/<repo>`, so
   in **Hemis-PTX-MVP1** that path does not exist and **every Linux artefact step fails** — no
   tarball, no zip, before any question of checksums. Replaced with `${{ github.workspace }}`.
2. **The tarball was written into the directory it was tarring** (`cd …/bin && tar -czvf
   Hemis-Linux.tar.gz *`), racing against its own output, and the **pre-release** upload then looked
   for it at the workspace root where it did not exist. Now written once, at the root, referenced
   consistently by both upload paths.
3. **No checksums were produced at all.** Added a `Checksums` step; `SHA256SUMS` now ships with both
   the release and pre-release Linux uploads.

**Still open, deliberately not changed under launch pressure:**

* ★ `symbol_check` is **off** for the x86_64 Linux job, so `--enable-glibc-back-compat`'s declared
  floor of **GLIBC 2.27** (`contrib/devtools/symbol-check.py: MAX_VERSIONS`) is never enforced.
  `install.sh` therefore gates at the conservative **2.31** (Ubuntu 20.04) rather than the declared
  2.27. Turn `symbol_check: true` on for that job, watch it pass once, and the gate can drop to 2.27
  honestly. Flipping an untested CI gate the week of a launch is how you find out your release job is
  broken at the worst moment.
* The `lint` job installs shellcheck and then never runs it (`git checkout -qf $GITHUB_SHA` is the
  whole step). Not blocking; worth knowing it is decorative.

---

## 3. How the release is cut

The workflow is `workflow_dispatch` and **creates the tag itself** via `softprops/action-gh-release`
(`tag_name` input), at the commit of whatever ref you dispatch it from. That resolves the ordering
problem cleanly: there is no window where a tag exists without artefacts.

1. Land genesis and the walk-through fixes on `feature/ptx-dkg`; confirm the suite is green.
2. Actions → *Client Build Actions for Hemis* → **Run workflow**, with:
   * **branch**: `feature/ptx-dkg`
   * **release**: tick for a real release, leave unticked for a pre-release
   * **tags**: `v0.1.0-testnet`
   * **release-name**: `PTX testnet v0.1.0`
3. When it finishes, confirm the release page carries `Hemis-Linux.tar.gz`, `Hemis-Linux.zip` and
   `SHA256SUMS`. **If `SHA256SUMS` is missing, stop** — that is the job failing quietly at packaging,
   which is exactly the failure mode item 1 above was.
4. **Read the sha256 out of the release and post it in the coordination channel alongside the tag.**
   A checksum served from the same host as the artefact proves the download was not corrupted; it
   does not prove authenticity. The out-of-band copy is what makes it a real pin, and it costs one
   message.
5. Flip the two defaults to the tag:
   * `testnet/operator/install.sh`: `REF="${PTX_REF:-feature/ptx-dkg}"` → `REF="${PTX_REF:-v0.1.0-testnet}"`
   * `testnet/operator/OPERATOR_GUIDE.md`: the `git clone -b feature/ptx-dkg …` line → `-b v0.1.0-testnet`
   Commit those on the branch **after** the tag is cut; they are what the *next* clone reads.
6. Tell the operators: the tag, the sha256, and one line on what changed.

### Cutting a fix release

Same procedure with `v0.1.1-testnet`. Do **not** move an existing tag — an operator who already
installed would silently keep old code while believing they had the fix, and `install.sh`'s
fast-forward check would not catch it because a moved tag is not a fast-forward it looks at. New tag,
new message, every time.

---

## 4. What `install.sh` does with it

Already implemented (see `testnet/operator/install.sh` section 3, "Binaries"):

* **A tag ref derives its own artefact.** Given `PTX_REF=v0.1.0-testnet` the script builds
  `…/releases/download/v0.1.0-testnet/Hemis-Linux.tar.gz` and fetches `SHA256SUMS` from the same
  release. A **branch** ref deliberately derives nothing — a branch has no release, and guessing one
  would produce a 404 instead of the accurate "this checkout is source only" message.
* **A URL without a checksum is refused**, not warned about. `PTX_BIN_SHA256` overrides, and is the
  hook for the out-of-band hash from step 4 above.
* On mismatch it prints both hashes and stops.
* After installing it **runs `Hemis-cli -version`** and fails if the binary will not execute — which
  is what catches an architecture or glibc mismatch, at install time, with a sentence that says so.
* Symlinks `Hemisd`/`Hemis-cli` into `/usr/local/bin`, and says so if it could not.

Tested on px1 against a locally served artefact: correct sha installs and runs; wrong sha refuses;
URL-without-sha refuses. **Untested until a release exists:** the tag-derived URL and the
`SHA256SUMS` fetch — both are network paths with nothing to point at yet. Exercise them on the
pre-release before telling anyone the tag is ready.
