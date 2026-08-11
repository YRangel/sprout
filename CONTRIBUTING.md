# Contributing

Standard GitHub flow: fork, branch (`feat/x` or `fix/y`), PR to `main`.

## Rules that are enforced by CI

- `cargo fmt --check` and `cargo clippy --all-targets -- -D warnings`
- `cargo test --workspace` must pass on ubuntu-24.04-arm **and** on an
  Android device (via the self-hosted Termux runner, when available)
- the C unit tests under `sprout-preload/csrc/tests/` must pass natively
- new behavior → a test; new *strategy* → an ADR in `docs/src/adr/`

## Rules that are enforced by review

- No `.text` patching, ever (ADR-0003).
- No `unsafe` in Rust without the exact preconditions documented inline.
- No allocation in the C11 preload hot path (`SP_PATH_MAX` arenas only).
- Path-translation rules change only with a test showing the old and new
  behavior.
- The proot-compat matrix (`docs/src/guide/proot-compat.md`) stays honest:
  if a flag stops working, the changelog says so.

## Docs

Every user-visible change updates the relevant page under `docs/src/`.
Ensure `mdbook build docs` is clean before asking for review.

## Discussion

Open a draft PR early if the change is bigger than ~100 lines; smaller
changes can land directly. If you're unsure whether something fits, open
an issue first — scope creep is the most common review failure mode.
