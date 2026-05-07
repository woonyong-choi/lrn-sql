This run produced a “vault overlay” because the Codex sandbox cannot write to `/Users/woonyong/vault`.

To apply the changes yourself, run one of the following commands in your local terminal (outside Codex):

1) Overlay-copy (recommended):

`rsync -av --progress /Users/woonyong/workspace/Krafton-Jungle/SW_AI-W08-SQL/os-wiki-staging/vault-overlay/ /Users/woonyong/vault/`

2) Manual copy:

- Copy these into `/Users/woonyong/vault/` (same relative paths):
  - `traces/os/thread-scheduler-trace.md`
  - `traces/os/context-switch-trace.md`
  - `maps/os/concept-to-code-map.md`
  - `maps/os/week-1-threads-map.md`

Overlay root:

`/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W08-SQL/os-wiki-staging/vault-overlay`
