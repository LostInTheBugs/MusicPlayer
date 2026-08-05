# Token usage tracking — MusicPlayer

LLM token usage for this project, tallied session by session.

## Cumulative tally (2026-08-02)

| Metric | Value |
|---|---|
| Dev sessions (Hermes) | 1 |
| Scripted agent sessions (API) | 0 |
| Models | deepseek-v4-flash |
| Messages | 437 |
| API calls | 2140 |
| Input tokens | 4 717 821 |
| Output tokens | 2 933 597 |
| **Total (input + output)** | **7 651 418** |
| Cache read (reused at reduced price) | 619 951 488 |
| **Estimated cost** | **≈ 2.99 USD** |

## How to re-read the counter

The Hermes session database (SQLite) holds the exact counters:

```bash
sqlite3 ~/.hermes/state.db "SELECT id, started_at, model,
  input_tokens, output_tokens, cache_read_tokens, cache_write_tokens,
  reasoning_tokens, estimated_cost_usd
  FROM sessions WHERE cwd LIKE '%MusicPlayer%'
  ORDER BY started_at;"
```

After each dev session, copy the matching row into the table above.

## Notes

- Tally taken from `~/.hermes/state.db` (table `sessions`) — these are the
  real runtime counters, not an estimate.
- « Scripted agent sessions (API) » = `api-*` sessions driven by scripts
  (audits, releases, background tasks) attached to this project.
- `reasoning_tokens` is probably included in `output_tokens`
  (to be confirmed with the provider).
- Tally generated on 2026-08-02 from the session database.
