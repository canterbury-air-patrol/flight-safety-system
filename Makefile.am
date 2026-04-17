AUTOMAKE_OPTIONS = foreign
ACLOCAL_AMFLAGS = -I m4
SUBDIRS = src examples tests
EXTRA_DIST = debian e2e

# Opt-in end-to-end tests. Requires Docker + Python; see e2e/README.md.
# Deliberately not wired into `make check` — CI has a separate job for this.
.PHONY: e2e
e2e:
	cd $(srcdir)/e2e && pytest
