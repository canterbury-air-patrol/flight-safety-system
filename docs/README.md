# Design record

This directory is the project's tracked design record. It holds the things a
future maintainer cannot recover from the code or from `git log`:

- **`decisions/`** — one file per settled design decision, including the
  alternatives that were considered and *rejected*, and the evidence behind the
  rejection. These are written to be read years later, by someone who has only
  the repository.
- **`release-checklist.md`** — the steps that must be performed when cutting a
  release, including the ABI check that a soname bump depends on.

## What does *not* live here

Open work items, scratch plans and in-progress investigation notes live in the
gitignored `todo/` directory (see `.gitignore`, "Local working notes"). Those
are transient by nature. When a `todo/` item ships and leaves behind a decision
worth keeping, the decision is promoted into `decisions/` and the plan file is
deleted.

## Numbering

Decision files are named `NN-slug.md`, where `NN` is the number of the `todo/`
item the decision came from. The numbering is deliberately preserved so that
existing citations stay resolvable: a commit message reading
`feat(server): todo/49 …`, or a code comment reading "see todo/49", refers to
the same decision now recorded in `decisions/49-server-command-id-semantics.md`.

Numbers are historical identifiers, not an ordering. A decision that spans
several `todo/` items is filed under all of them (`34-45-47-…`).

## The record

| Decision | Subject |
|---|---|
| [24 — database reads report failure by status, never throw](decisions/24-database-read-error-control-flow.md) | Database seam |
| [26 — client send timeouts use `TCP_USER_TIMEOUT`, not `SO_SNDTIMEO`](decisions/26-client-send-timeout.md) | Transport |
| [31 — duplicate asset identity rejects the newcomer](decisions/31-duplicate-identity-policy.md) | Server policy |
| [34/45/47 — the database fail-safe is one latched degraded state](decisions/34-45-47-db-failsafe-latch.md) | Safety behaviour |
| [43 — `secure_string` scope](decisions/43-secure-string-scope.md) | Security boundary |
| [46 — no `statement_timeout`; DB stalls are bounded at the socket](decisions/46-no-statement-timeout.md) | Database seam |
| [48 — a terminal command ack is final for its dispatch](decisions/48-command-ack-terminal-finality.md) | Protocol / audit |
| [49 — the FMU command identifier is per-server](decisions/49-server-command-id-semantics.md) | Protocol |
| [51 — optional trailing wire fields are prefix-closed](decisions/51-optional-trailing-field-rule.md) | Protocol |

## Citing a decision

New code comments, `CHANGELOG.md` entries and commit messages should cite the
tracked path (`docs/decisions/49-…`) rather than the bare `todo/49`, so the
reference resolves in any clone. Existing `todo/NN` citations in the tree are
retargeted opportunistically, whenever a file is touched for other reasons —
the table above is the map in the meantime.

Citations to *other repositories'* todo items (cap-fmu, `CAP/test-plan`) can
never resolve locally, so they always carry a one-line gloss of what the
referenced item is about.
