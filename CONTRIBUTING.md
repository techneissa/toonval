# Contributing

Open an issue before a large change. For a fix, a pull request is enough.

## Build and test

`just build` and `just test` (see the README for prerequisites). Run `just test` and `just format` before you open a pull request. Code is formatted with clang-format (`.clang-format`).

## Rules

- `core/` holds the music logic and has no SDL, MIDI, audio, file-system or clock code. Its public headers use plain structs and value types, and no exception crosses the API.
- Every behaviour in `core/` has a doctest test in `tests/`.
- New source files start with `// SPDX-License-Identifier: GPL-3.0-or-later`.
- Dependencies are release tarballs pinned by SHA-256 in `cmake/Dependencies.cmake`, with a GPL-3.0-compatible licence.
- Comments state the rule they follow; they don't point at documents outside the repository.
- Code must work for any keyboard range, not only the 61-key reference keyboard (`KeyRange`).

By contributing you agree to license your work under the GPL-3.0-or-later.
