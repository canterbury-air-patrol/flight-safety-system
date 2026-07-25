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

## Citing a decision

New code comments, `CHANGELOG.md` entries and commit messages should cite the
tracked path (`docs/decisions/49-…`) rather than the bare `todo/49`, so the
reference resolves in any clone. Existing `todo/NN` citations in the tree are
retargeted opportunistically, whenever a file is touched for other reasons —
the table above is the map in the meantime.

Citations to *other repositories'* todo items (cap-fmu, `CAP/test-plan`) can
never resolve locally, so they always carry a one-line gloss of what the
referenced item is about.
