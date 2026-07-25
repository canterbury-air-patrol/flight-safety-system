# Release checklist

Branching model is in the README's *Release strategy* section: releases are cut
from `release/X.Y`, `develop` is the integration branch, `master` points at the
latest stable release.

This file is the mechanical checklist. The ABI step exists because an ABI break
once reached a tag unbumped, and was only caught because release planning
happened to diff the headers by hand (todo/61).

## 1. Check the ABI before choosing the version

**Run this before deciding whether the release is X.Y+1 or X.Y.Z+1, not after.**

Install the candidate and the last tag into separate prefixes and compare:

```sh
# candidate (the libraries and their headers build by default)
./configure --prefix=/tmp/abi-new && make && make install

# last released tag, in a clean worktree
git worktree add /tmp/abi-old-src <last-tag>
cd /tmp/abi-old-src && ./autogen.sh && ./configure --prefix=/tmp/abi-old \
  && make && make install

abidiff --headers-dir1 /tmp/abi-old/include --headers-dir2 /tmp/abi-new/include \
        /tmp/abi-old/lib/libfss-transport.so /tmp/abi-new/lib/libfss-transport.so
```

Repeat for each of the four installed libraries: `libfss`, `libfss-transport`,
`libfss-transport-ssl`, `libfss-client-ssl`.

If `abidiff` reports **any** incompatible change, `-version-info` in
`src/Makefile.am` must be bumped for every library — the project keeps all four
sonames in lockstep, and the form used is `N:0:0`, i.e. each bump declares a
clean break with no backward compatibility (`age` is always 0). A same-soname
mix of an old library and new headers is exactly the failure this step prevents,
and it does not fail to link — it corrupts at runtime.

Things that have historically broken the ABI without looking like it:

- a new **data member** on `fss_connection` (layout change), and
- a new **virtual function** anywhere in a class (vtable slot shift, and
  inserting one mid-class shifts every later slot).

Two build traps this step walks straight into:

- This tree is commonly configured with `--disable-dependency-tracking`, so a
  header edit recompiles **nothing**. Build the candidate from a clean tree
  (`make clean && make`), or the comparison is against a half-stale library.
- Re-running `configure` with a different `--prefix` does not force a relink. A
  library that automake considers up to date keeps the **old** `libdir` baked
  into its `.la`, and `make install` then writes to the previous prefix. Remove
  the `.la` files and `make` again before `make install`.

## 2. Version numbers

Bump, in this order, and keep them consistent:

- `AC_INIT` in `configure.ac`
- `-version-info` in `src/Makefile.am` — only if step 1 says so
- `debian/changelog` — a new stanza
- `CHANGELOG.md` — move the accumulated entries under the new version heading,
  and call out any soname change explicitly under a "consumers must rebuild"
  note

## 3. Verify

- `./check-code.sh` — clean
- `make check` — the full unit suite
- ThreadSanitizer: configure with `--enable-tsan --enable-tests`, which
  instruments the whole tree so plain `make check` runs the unit suite
  race-checked; `make check-tsan` in `tests/` runs the dedicated concurrency
  binary. 0 races, with only the one documented `disconnect` suppression
- `make distcheck` — this builds the server, the tests and the example client,
  so a header missing from the tarball fails here rather than at a user's site
- the e2e suite (`make e2e-test`; requires Docker and Python)

## 4. Tag and publish

Tag on the `release/X.Y` branch. Fast-forward `master` to the tag.

Publishing to any remote — pushing branches or tags, opening PRs — is a separate,
explicit step; nothing above pushes.
