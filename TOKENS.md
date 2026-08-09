# Token usage tracking — MusicPlayer

LLM token usage for this project, tallied session by session.

## Cumulative tally (2026-08-09)

| Metric | deepseek-v4-flash | deepseek-v4-pro | **Total** |
|---|---|---|---|
| Dev sessions (Hermes) | 2 | 1 | **3** |
| Scripted agent sessions (API) | 0 | 0 | **0** |
| Messages | 1 363 | 454 | **1 817** |
| API calls | 4 929 | 229 | **5 158** |
| Input tokens | 9 854 124 | 344 314 | **10 198 438** |
| Output tokens | 5 098 194 | 110 536 | **5 208 730** |
| **Subtotal (input + output)** | **14 952 318** | **454 850** | **15 407 168** |
| Cache read (reused at reduced price) | 1 387 431 936 | 31 611 264 | **1 419 043 200** |
| **Estimated cost** | **≈ 6.32 USD** | **≈ 0.36 USD** | **≈ 6.67 USD** |

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

- Tally taken from `~/.hermes/state.db` (table `session_model_usage`,
  filtered by session id) — real runtime counters, not an estimate.
- « Scripted agent sessions (API) » = `api-*` sessions driven by scripts
  (audits, releases, background tasks) attached to this project.
- `reasoning_tokens` is probably included in `output_tokens`
  (to be confirmed with the provider).
- Tally updated on 2026-08-09 — 100-c1 → c8 (podcast fixes: 562-episode
  list, CDATA, playlist limits, sync block, repo regeneration). The
  current session will be added at the next tally.
