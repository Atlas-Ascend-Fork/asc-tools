# Bundled engine

This directory contains the deterministic engine and diagnostic configuration shipped
with the package. The package is self-contained and does not require a source
repository checkout at runtime.

- `harness/engine/` — the engine modules for diagnosis, verification, and redaction.
- `harness/config/` — the 4 diagnostic and support-policy configurations read by the engine.
- `harness/runtime/build_one.sh` — the build helper used by real-compile verification.

## Release ownership

Generated from canonical engine sources. Domain knowledge is supplied by cannbot-knowledge; this directory contains diagnostic rules and product policy only.

## Runtime paths

`harness/engine/common.py` defines `PROJECT_ROOT = Path(__file__).resolve().parents[2]`.
Here it resolves to this `engine/` directory, so relative knowledge-base and
helper paths remain package-local. Runtime scratch and build workspaces belong
under `ACD_SCRATCH_ROOT` or the system temporary directory, never in the
installed package. Report outputs go wherever the caller directs them.
