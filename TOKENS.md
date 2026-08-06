# Token usage tracking — MusicPlayer

LLM token usage for this project, tallied session by session.

## Cumulative tally (2026-08-05)

| Metric | deepseek-v4-flash | deepseek-v4-pro | **Total** |
|---|---|---|---|
| Dev sessions (Hermes) | 1 | 1 | **2** |
| Scripted agent sessions (API) | 0 | 0 | **0** |
| Messages | 412 | 233 | **645** |
| API calls | 2 621 | 104 | **2 725** |
| Input tokens | 5 176 596 | 294 597 | **5 471 193** |
| Output tokens | 3 064 273 | 42 005 | **3 106 278** |
| **Subtotal (input + output)** | **8 240 869** | **336 602** | **8 577 471** |
| Cache read (reused at reduced price) | 813 276 288 | 12 218 240 | **825 494 528** |
| **Estimated cost** | **≈ 3.86 USD** | **≈ 0.21 USD** | **≈ 4.07 USD** |

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
- Tally updated on 2026-08-05 — 045-c3 (plugin versions).
