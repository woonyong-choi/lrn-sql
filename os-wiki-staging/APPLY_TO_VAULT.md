This run produced a vault overlay because the Codex sandbox cannot write to `/Users/woonyong/vault`.

Current run overlay:

`/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W08-SQL/os-wiki-staging/2026-05-08-lazy-file-backed-page/vault-overlay`

To apply this run to the vault from a local terminal outside Codex:

```sh
rsync -av --progress /Users/woonyong/workspace/Krafton-Jungle/SW_AI-W08-SQL/os-wiki-staging/2026-05-08-lazy-file-backed-page/vault-overlay/ /Users/woonyong/vault/
```

Files in this run:

- `traces/os/lazy-file-backed-page-trace.md`
- `maps/os/concept-to-code-map.md`
- `maps/os/week-3-4-virtual-memory-map.md`
- `notes/os/mmap-file-backed-page-knowledge.md`
- `traces/os/page-fault-trace.md`
- `traces/os/frame-eviction-trace.md`

The older `os-wiki-staging/vault-overlay/` directory is preserved as prior-run output and is not the recommended apply target for this run.
