AUTOMAKE_OPTIONS = foreign
ACLOCAL_AMFLAGS = -I m4
SUBDIRS = src examples tests
EXTRA_DIST = debian e2e .clang-format certs LICENSE.md CHANGELOG.md

# `make distcheck` must build everything the release ships — the server,
# the tests, and the example client — so a header missing from the tarball
# (e.g. one only reached via --enable-server) fails the check. The systemd
# unit directory is an absolute path; redirect it under the distcheck install
# tree so `make install` does not try to write to the real /usr/lib.
DISTCHECK_CONFIGURE_FLAGS = \
	--enable-server --enable-tests --enable-fake-client \
	--with-systemdsystemunitdir=$$dc_install_base/$(systemdsystemunitdir)

# Opt-in end-to-end tests. Requires Docker + Python; see e2e/README.md.
# Deliberately not wired into `make check` — CI has a separate job for this.
# Target is named `e2e-test` (not `e2e`) so it does not collide with the
# `e2e` directory in EXTRA_DIST, which would make `make dist` run pytest.
.PHONY: e2e-test
e2e-test:
	cd $(srcdir)/e2e && pytest

# EXTRA_DIST copies the e2e/ tree recursively; strip developer-local Python
# cruft (virtualenvs, caches) and generated test reports so they never land
# in the release tarball.
dist-hook:
	rm -rf $(distdir)/e2e/.venv $(distdir)/e2e/.pytest_cache $(distdir)/e2e/.ruff_cache
	rm -f $(distdir)/e2e/traceability.json
	find $(distdir)/e2e -name __pycache__ -type d -prune -exec rm -rf {} +
