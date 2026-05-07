This run produced a “vault overlay” because the Codex sandbox cannot write to `/Users/woonyong/vault`.

To apply the changes yourself, run one of the following commands in your local terminal (outside Codex):

1) Overlay-copy (recommended):

`rsync -av --progress /Users/woonyong/workspace/Krafton-Jungle/SW_AI-W08-SQL/os-wiki-staging/vault-overlay/ /Users/woonyong/vault/`

2) Manual copy:

- Copy `traces/os/context-switch-trace.md` into `/Users/woonyong/vault/traces/os/`
- Replace the two map files in `/Users/woonyong/vault/maps/os/` with the overlay versions.

Overlay root:

`/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W08-SQL/os-wiki-staging/vault-overlay`
