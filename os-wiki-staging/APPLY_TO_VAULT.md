This staging area contains vault overlays produced when the Codex sandbox could not write to `/Users/woonyong/vault`.

Latest run overlay:

`/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W08-SQL/os-wiki-staging/2026-05-08-syscall-register-snapshot/vault-overlay`

This run deepens syscall register snapshots:

- `traces/os/syscall-register-snapshot-trace.md`
- `maps/os/concept-to-code-map.md`
- `maps/os/week-2-user-programs-map.md`
- `notes/os/cpu-register-execution.md`
- `traces/os/syscall-end-to-end.md`

Apply only after reviewing the current vault, because `/Users/woonyong/vault` already has local dirty changes outside this overlay.

```sh
rsync -av --progress /Users/woonyong/workspace/Krafton-Jungle/SW_AI-W08-SQL/os-wiki-staging/2026-05-08-syscall-register-snapshot/vault-overlay/ /Users/woonyong/vault/
```

Previous fd close/remove run:

As of the end of this run, the actual vault already contains a separate committed version of the fd close/remove Lab:

```text
bd4b07f docs: fd close와 remove 생명주기 Lab을 추가
```

Treat this overlay as a sandbox artifact for comparison, not as a blind overwrite target for the current vault.

Current run overlay:

`/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W08-SQL/os-wiki-staging/2026-05-08-fd-close-reuse/vault-overlay`

If the vault does not contain the fd close/remove Lab, apply this run from a local terminal outside Codex:

```sh
rsync -av --progress /Users/woonyong/workspace/Krafton-Jungle/SW_AI-W08-SQL/os-wiki-staging/2026-05-08-fd-close-reuse/vault-overlay/ /Users/woonyong/vault/
```

Files in this run:

- `labs/os/fd-close-reuse-lab.md`
- `maps/os/concept-to-code-map.md`
- `maps/os/week-2-user-programs-map.md`
- `maps/os/학습-가이드.md`
- `notes/os/file-descriptor-knowledge.md`

The older `os-wiki-staging/vault-overlay/` directory is preserved as prior-run output and is not the recommended apply target.
