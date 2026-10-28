# Epic 03 — shipping v1.0.0

## Goal

Epics 0–2 produced a working engine, the machinery that proves two
independent implementations agree, and the numbers that show what the
optimization bought. Epic 3 turns that into a public v1.0.0: a repository a
stranger can clone and build, a README that states the results and their
provenance without overclaiming, a license, CI that runs on whichever macOS
runner is available, a recorded demo, and a release checklist that keeps the
published numbers tied to a real idle-machine run.

Phase 1 ends here. FOK/post-only semantics, persistence, networking and
language bindings are Phase 2 and later; the v1.0.0 tag is cut from the Phase 1
tree only.

## How it works

### README structure

`README.md` is the only front door. Its sections are deliberately ordered from
claim to evidence to reproduction:

1. **Intro** — what the project is (single-instrument, price-time priority,
   integer prices, no hot-path allocation, deterministic events, differential
   testing) and the author's background.
2. **Benchmarks** — the array ladder's p50/p90/p99/p99.9/max table and
   mixed-flow throughput, with machine, compiler, flags, latency mode,
   repetitions, date and commit on one line.
3. **Methodology** — the timer's measured granularity and why latency is
   `batchN` on this machine, the histogram, repetitions, the QoS hint, and the
   idle-machine gate.
4. **The optimization** — the tree-to-array change and its before/after table,
   with an honest reading of a few percent rather than a step change.
5. **Correctness** — the event contract, the independent reference engine, the
   differential test and its stress configurations, the fresh-clone audits,
   the allocation test, determinism, and the Tardis-derived flow.
6. **Design** — two small diagrams: the flow-file → book → listener path, and
   the id map → pool → FIFO → ladder path.
7. **Build and run** — prerequisites, clone/build/test, replay and difftest,
   benchmark rendering, and the Tardis conversion, each as a copy-paste block.
8. **Demo** — the terminal recording.
9. **Limitations** — single instrument/single thread, synthetic Tardis flow,
   macOS-specific timing, no self-trade prevention, Phase 2 features absent,
   the 2^24-tick range.
10. **License**.

The epic write-ups (`docs/epic-00-foundation.md` … this file) are linked from
the intro for readers who want the reasoning behind a component. The committed
documentation is exactly the README, `docs/architecture.md` and one file per
roadmap epic (decision C15).

### License

MIT, with the copyright line "2026 Moksh Siruvani" in `LICENSE` (decision
C40). The license is the public-ship default: permissive, no contributor
license agreement, and no dependency that forces a different choice. The
copyright holder can change the line before the repository goes public.

### CI that follows the runner

Every workflow asks for its runner through one repository variable rather than
a literal label:

```yaml
runs-on: ${{ fromJSON(vars.CI_RUNS_ON || '["self-hosted","macOS","ARM64"]') }}
```

`CI_RUNS_ON` is a JSON array, so changing it switches every job at once. The
fallback is the current private-repo setup: one self-hosted runner on this
Mac, with runner-level environment forcing single-threaded, low-priority work
(decision C30). While the repository is private, macOS minutes on
GitHub-hosted runners cost money; after it goes public they are free, so the
switch is part of the ship checklist.

The shared composite action `.github/actions/setup` absorbs the differences
between the two runner kinds:

- installs `cmake`, `ninja`, `pkg-config` and `ccache` with Homebrew only when
  they are missing (a no-op on the self-hosted runner);
- ensures `uv`, installing `astral-sh/setup-uv` at a pinned release when it is
  absent;
- points `VCPKG_ROOT` at `~/.cache/matchbook/vcpkg` and bootstraps the pinned
  vcpkg baseline;
- fingerprints the vcpkg baseline and the clang version once, so the cache
  keys change when the toolchain changes rather than on every unrelated edit;
- restores `~/.cache/ccache` and `~/.cache/vcpkg/archives` with
  `actions/cache` on GitHub-hosted runners only, where the workspace starts
  empty. The self-hosted runner keeps both caches in the user's home directory,
  so the same steps are skipped rather than duplicated.

Compile jobs configure with `-DCMAKE_CXX_COMPILER_LAUNCHER=ccache`, and every
checkout uses `clean: false`. On the self-hosted runner that keeps `build/`
alive between jobs and pushes, so the `test` job reuses the `build` job's
release tree; on a hosted runner it is a harmless no-op.

`ci.yml` runs required checks `build`, `test`, `asan`, `format` and `hygiene`,
plus `tsan` on every pull request and push, and `tidy` on manual dispatch only.
`hygiene` scans the PR title, PR body and every commit in the PR for disallowed
attribution text and for an author/committer address other than
`EXPECTED_GIT_EMAIL`; the patterns live in repository variables, never in the
repository (decision C18). `format` runs the pinned
`uvx clang-format==23.1.3` and the Python report tests with
`uv run --frozen pytest -q scripts/`. The nightly workflow is scheduled and
manual only; it builds both presets and runs the long differential test on
each.

### Demo recording

`assets/demo.gif` is produced by `scripts/record_demo.sh` (decision C41). The
script records with `asciinema` and renders with `agg`, both installed with
Homebrew if missing. It runs the demo in a temporary directory with a plain
`$` prompt and relative command paths, so the recording contains no user name,
home-directory path or other machine detail. The recorded session generates a
one-million-order flow, replays it on both engines (identical digests) and runs
a two-million-operation differential test ending in `mismatches: 0`.

Wall-clock fields are filtered out of the recording, and the script refuses to
render the GIF if a timing field survives: the demo demonstrates correctness,
while published timing comes only from `scripts/run_benchmarks.sh` (decision
C42). Re-record it whenever the output format or the recorded commands change.

### Release checklist

The tag is cut by hand at the end of Phase 1. In order:

1. **Phase 1 merged.** Every Phase 1 PR is merged, `main` is green on the
   required checks, and the Phase 2 pull requests are open but unmerged
   (decision C46).
2. **Idle-machine numbers.** With no agents, builds or CI running, run
   `scripts/run_benchmarks.sh --compare` on `main` and record the commit.
   `scripts/run_benchmarks.sh` re-runs its idle check and refuses to start if
   the machine is busy.
3. **Numbers refreshed.** Replace the README tables and this epic's provenance
   line from that run only; no number from an earlier or noisier run survives
   (decision C42). If the numbers moved, say so in the release notes rather
   than averaging runs.
4. **Fresh-clone verification.** Clone `main` into a temporary directory,
   bootstrap vcpkg, build `release`, `release-tree`, `debug`, `asan` and
   `tsan`, run every test preset, run the README command blocks, and run one
   10,000,000-operation difftest per ladder. This is the same audit performed
   at each epic close.
5. **Repository hygiene.** Confirm `LICENSE`, the README links, the demo asset
   and the committed docs; confirm no secrets, no build outputs and no local
   orchestration files are tracked; confirm the attribution scan and the
   commit identity check are green.
6. **Tags.** Create the annotated tags `epic-03-complete` and `v1.0.0` on the
   release commit and push them (seed decision 12). A tag is never moved after
   it is published.
7. **Switch CI before going public.** Set `CI_RUNS_ON` to the GitHub-hosted
   macOS labels and remove the self-hosted runners from the repository, as
   they must not remain registered once the repository is public. Open a small
   pull request (or dispatch `ci`) and confirm every required check reports on
   the hosted runner.
8. **Go public.** Make the repository public and publish release notes from
   the `v1.0.0` tag.
9. **Keep the merge rules.** The PR-required, merge-commit-only branch ruleset
   stays in place; the Phase 2 merge-only wave can then land.

## Key decisions and reasoning

- **MIT, copyright 2026 Moksh Siruvani.** The simplest license that lets the
  code be read, used and cited; no copyleft obligations for a personal
  project (decision C40).
- **One self-hosted runner while private.** A single runner makes CI the
  bottleneck by design; it keeps parallel agents from saturating the
  developer's Mac and keeps the benchmark machine usable (decisions C30, C45).
  The cost is queue time, not correctness.
- **Runner-agnostic workflows, not a second workflow file.** The `CI_RUNS_ON`
  variable and the setup action mean the hosted switch is a variable change.
  A parallel `ci-hosted.yml` would drift from `ci.yml` and double the
  maintenance.
- **No published number without an idle run.** Benchmarks are not a required
  check and are never taken from CI or a busy laptop. The script's idle gate
  and the provenance line are the enforcement (decision C42).
- **The demo asset lives in `assets/`, not `docs/`.** It is a README asset, and
  decision C15 keeps the committed docs list fixed (decision C41).
- **`pkg-config` is a documented prerequisite.** vcpkg's fix-up step for the
  `hdr-histogram` port shells out to it; without it a fresh machine fails
  during the dependency build, before any project code compiles.
- **`tidy` stays advisory.** Homebrew LLVM drifts independently of the pinned
  compiler, so treating clang-tidy as required would make CI fail for reasons
  unrelated to the change (decision C2).
- **`tsan` runs but is not yet required.** The job was added during the
  Phase 1 CI work and passes on every PR; adding it to the ruleset is a
  repository setting, taken after it has a stable history.

## Interfaces for future epics

- **`.github/actions/setup`.** Future jobs add tools, caches or environment
  variables here once; both runner kinds inherit them. A job that only reads
  files can turn off `bootstrap-vcpkg` and `ccache` as the format job does.
- **`CI_RUNS_ON`.** One variable selects the runner for every workflow,
  including the nightly. No workflow hard-codes a label.
- **Scripts.** `scripts/run_benchmarks.sh` (single-build and `--compare`),
  `scripts/bench_report.py` (single, before/after and `--merge`),
  `scripts/profile_ladder.sh`, `scripts/check_determinism.sh`,
  `scripts/record_demo.sh` and their tests under `scripts/` are the supported
  way to produce a report, a profile or a recording. Later epics extend the
  scripts rather than adding one-off commands to CI.
- **Docs and tags.** One `docs/epic-NN-<slug>.md` per roadmap epic and the
  annotated `epic-NN-complete` tags give each phase a stable reference point;
  `vX.Y.Z` tags mark public releases.
- **Required-check set.** The ruleset, not the workflow, decides what blocks a
  merge. Promoting `tsan` or adding a future job is a settings change with no
  workflow edit.

## Deviations

- The GitHub-hosted path is implemented but not exercised while the repository
  is private: macOS minutes on private repositories are billed, so the first
  real run is the ship pull request. The self-hosted path is the one every
  merge in this phase used.
- `tsan` is green on pull requests but is not in the required-check set yet,
  although the CI decision anticipated promoting it once it had reported
  (decision C51).
- The first demo recording included `wall_ms` fields; it was re-recorded with
  the timing filter in place, and the script now fails rather than publish a
  recording that still contains them.
- The Epic 2 numbers were published at the Epic 2 close, before the v1.0.0 tag,
  so the release checklist re-runs them on the release commit rather than
  assuming they still hold.
- The release checklist is prose plus scripts, not a release workflow. The
  human makes the public/private, runner and ruleset changes, which GitHub
  Actions cannot do without a privileged token.

## Known limitations

- **CI is macOS-only.** There is no Linux or Windows job; the engine and tools
  are standard C++20, but portability is not continuously tested in this
  phase.
- **One runner.** Required checks queue behind each other and behind the
  nightly; a 20–40 minute wait is normal and is a deliberate resource choice,
  not a fault.
- **Benchmarks are not automated.** A human starts the idle run, and the
  numbers belong to the machine, compiler and flags in the provenance line.
  There is no historical trend database.
- **The hosted-runner path is unmeasured.** Cache warm-up, install time and
  queue behaviour on GitHub-hosted macOS runners will be known only after the
  repository is public.
- **The release is manual.** Tags, visibility, runner removal and ruleset
  changes are checklist steps performed by the maintainer; a missed step is
  possible, and the fresh-clone audit is the guard.
- **The demo is a GIF.** It is a few hundred kilobytes, has no narration and
  is not regenerated automatically when output changes.
