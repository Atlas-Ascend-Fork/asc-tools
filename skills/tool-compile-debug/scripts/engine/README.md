# Bundled engine

This directory contains the deterministic engine and knowledge base shipped
with the package. The package is self-contained and does not require a source
repository checkout at runtime.

- `harness/engine/` — the engine modules for diagnosis, verification, and redaction.
- `harness/knowledge/` — the 13 knowledge-base datasets read by the engine. `api_signatures` ships as size-capped `part-*.yaml` shards under `harness/knowledge/api_signatures/` (single-file size limit of the hosting repository); the shards concatenate to the same list as the single-file form. `manifest.json` records their complete order, entry counts, and SHA-256 checksums; incomplete or modified shards are rejected.
- `harness/runtime/build_one.sh` — the build helper used by real-compile verification.

## Release ownership

The hosting repository can maintain the Python, shell, and YAML sources
in this package directly. The API shard manifest is regenerated with
the bundled maintenance command; no external source checkout is needed.
See [source maintenance](../../references/maintenance.md) for the
edit, validation, and release workflow.

In a development checkout that generates this directory, change that
checkout's canonical sources and regenerate instead of editing its
generated copy. Runtime installations remain read-only.

## Runtime paths

`harness/engine/common.py` defines `PROJECT_ROOT = Path(__file__).resolve().parents[2]`.
Here it resolves to this `engine/` directory, so relative knowledge-base and
helper paths remain package-local. Runtime scratch and build workspaces belong
under `ACD_SCRATCH_ROOT` or the system temporary directory, never in the
installed package. Report outputs go wherever the caller directs them.
