# ardour-nebula: NaxeCode's build of Ardour

This fork builds Ardour from its GPL source for macOS on Apple Silicon, for
the music work on Nebula Homestead. Nothing in Ardour itself is changed; the
branch `nebula-build` is the release tag plus this folder and
`.github/workflows/nebula-macos.yml`.

The DMG is built by GitHub Actions on the free `macos-15` arm64 runners:

- `stack` builds Ardour's dependency stack (ardour-build-tools,
  `x-applearm.sh`) at the revision the official arm64 nightly uses, and
  caches it. A cold build takes up to two six-hour passes; later runs hit the
  cache.
- `ardour` compiles Ardour against that stack and packages it with
  `tools/osx_packaging/osx_build --public`. The DMG is uploaded as the
  `ardour-macos-arm64-dmg` artifact, and as a release asset when a
  `nebula-*` tag is pushed.

The DMG is unsigned. On first launch, right-click the app and choose Open.

## Building on the Mac

The same recipe runs locally, for checking a change in minutes instead of a
CI run. On an M4 the stack takes about 50 minutes once (about 1 GB in
`~/ardour-ci-work`), then Ardour about 11 minutes. waf calls `python`, which
macOS doesn't have, so a link to `python3` goes in the work folder:

```sh
W=~/ardour-ci-work REV=3cbf14d1c2c556f327534141c848ff63460df91f
mkdir -p $W/bin && ln -sf "$(command -v python3)" $W/bin/python
git fetch --depth=30 origin "+refs/tags/9.8:refs/tags/9.8"   # waf reads the version from git describe
PATH="$W/bin:$PATH" nice nebula/ardour-ci/bin/ardour-ci --source "$PWD" --work-dir $W --stack-rev $REV deps build
PATH="$W/bin:$PATH" nice nebula/ardour-ci/bin/ardour-ci --source "$PWD" --work-dir $W --stack-rev $REV build
PATH="$W/bin:$PATH" nebula/ardour-ci/bin/ardour-ci --source "$PWD" --work-dir $W --stack-rev $REV package   # a DMG
```

## nebula/ardour-ci

Vendored from [axsy/ardour-ci](https://github.com/axsy/ardour-ci) at
`034402979eb4b07484bd492248e5fd7260faff21` (GPL-2.0-or-later, see its
`COPYING`). It patches the stack recipe for current Apple Clang, mirrors
unreliable download hosts, and adds per-dependency checkpoints, which the
workflow uses to resume a stack build across jobs.

## Updating

- New Ardour release: rebase `nebula-build` onto the new tag and set
  `ARDOUR_BASE_TAG` in the workflow.
- New build stack: take the `build-stack:` line from
  <https://nightly.ardour.org/i/A_MAC_arm64/info.txt> and set
  `BUILD_STACK_REV`. Changing it, or anything in `nebula/ardour-ci`, starts a
  fresh stack cache.
