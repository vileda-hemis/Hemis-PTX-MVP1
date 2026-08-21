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

0. ★★ **Run the installer end-to-end test and require exit 0. This is a gate, not a nicety.**

   ```bash
   PTX_TEST_BINDIR=<dir with Hemisd/Hemis-cli> testnet/operator/install-test.sh
   ```

   It runs `install.sh` three times exactly as `vps-install.sh` drives it, **starts all three
   daemons**, and judges them on what they did rather than what they say: the datadir layout
   (which chain), the kernel's socket table per PID (which ports), credential authentication
   (whether the config was read at all), and per-GM liveness on pairwise-disjoint ports. It then
   reconstructs each of the four `f37bf34`/`e414e77` defects and requires the matching check to
   FAIL against it.

   ★ **A non-zero exit blocks the tag.** The reason it exists is that the previous end-to-end test
   stopped at "installed and configured" and never started a daemon, and all four defects fixed
   between `16d283a` and `e414e77` lived in the first inch past its last assertion — see KDD-100.
   A green run of *that* test is exactly what shipped `v0.1.0-testnet`.

   ★ Also treat a `[RED BROKEN]` line as blocking even when the green half passes. It means a check
   did not fail against the defect it is named for, i.e. the green result it produced is vacuous.

1. Land genesis and the walk-through fixes on `feature/ptx-dkg`; confirm the suite is green.
2. Actions → *Client Build Actions for Hemis* → **Run workflow**, with:
   * **branch**: `feature/ptx-dkg`
   * **release**: ★ **leave UNTICKED — pre-release.** Decided 2026-08-21, and it is a decision, not
     an oversight. Ticking it would make this the repository's `/releases/latest`, and this
     repository is a fork of a mainnet coin: anything reaching for `latest` — including the
     upstream-derived script that still sits on `main` — would then be handed a **PTX testnet**
     binary as though it were the product. The present 404 is a loud failure; a wrong binary is a
     silent one, and silent-wrong is the whole family of defect this release exists to stop
     shipping. The cost is real and accepted: `/releases/latest` and
     `/releases/latest/download/…` return Not Found on this repository (measured 2026-08-21), so a
     future script cannot use them. **That is the intended answer** — an unpinned "latest" is how
     two operators end up on different code, which is the thing `vps-install.sh:27-36` refuses in
     writing. Revisit only when there is a production release to be latest.
   * **tags**: `v0.1.0-testnet`
   * **release-name**: `PTX testnet v0.1.0`
3. When it finishes, confirm the release page carries `Hemis-Linux.tar.gz`, `Hemis-Linux.zip` and
   `SHA256SUMS`. **If `SHA256SUMS` is missing, stop** — that is the job failing quietly at packaging,
   which is exactly the failure mode item 1 above was.
4. **Read the sha256 out of the release and post it in the coordination channel alongside the tag.**
   A checksum served from the same host as the artefact proves the download was not corrupted; it
   does not prove authenticity. The out-of-band copy is what makes it a real pin, and it costs one
   message.
5. Confirm every reference to the tag names the tag — **all four are already flipped**, and the
   list is here because the sweep that produced it found one that had been missed for a week:
   * `testnet/operator/install.sh:22` — `REF="${PTX_REF:-v0.1.0-testnet}"` ✓
   * `vps-install.sh:37` — `TAG="${PTX_TAG:-v0.1.0-testnet}"` ✓
   * `testnet/operator/OPERATOR_GUIDE.md` — `git clone -b v0.1.0-testnet …` ✓ (was
     `-b feature/ptx-dkg`, a **moving branch**, until 2026-08-21)
   * `GM_QUICKSTART.md` and `vps-install.sh`'s own header — the `raw.githubusercontent` URL that
     fetches the bootstrap ✓ (was `/main/`, where a file of the same name is the **upstream Hemis
     mainnet installer** — BUG-044)

   ★ **The pattern to sweep for is not "the tag string"; it is "any ref that fetches PTX tooling".**
   Three of the four above pinned correctly and the fourth floated, and a floating fetch defeats
   all three: it decides which `install.sh` you run before `PTX_REF` gets to decide anything.
6. Tell the operators: the tag, the sha256, and one line on what changed.

### Cutting a fix release

Same procedure with `v0.1.1-testnet`. Do **not** move an existing tag — an operator who already
installed would silently keep old code while believing they had the fix, and `install.sh`'s
fast-forward check would not catch it because a moved tag is not a fast-forward it looks at. New tag,
new message, every time.

### ★ The one exception, and exactly how narrow it is

**A tag NOBODY HAS EVER CONSUMED may be deleted and re-cut under the same name.** The rule above
protects operators who already installed; where there are none, there is nothing to protect and the
version number is better spent meaning what it was meant to mean.

The test is not "we do not think anyone used it" — it is **"no clone, no download, and no reference
outside this repository exists"**, established rather than assumed. For `v0.1.0-testnet` on
2026-08-21 that held: the release was a rehearsal, no operator had been given the tag, and every
reference to the string was in-tree (`install.sh:22`, `vps-install.sh:37`,
`OPERATOR_GUIDE.md`, this file, and two KDD entries that describe the *decisions* taken for the
release named `v0.1.0-testnet` and stay true when it is re-cut).

★ **Delete the RELEASE as well as the tag.** Deleting only the tag leaves the release page standing
with its artefacts still downloadable and now pointing at nothing.

★ **Once a tag has been handed to a single operator this exception is gone for good.** There is no
way to tell afterwards which code they are running, and asking is not the same as knowing.

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
