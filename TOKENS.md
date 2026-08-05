# Token usage tracking — MusicPlayer

LLM token usage for this project, tallied session by session.

## Cumulative tally (2026-08-05)

| Metric | deepseek-v4-flash | deepseek-v4-pro | **Total** |
|---|---|---|---|
| Dev sessions (Hermes) | 1 | 1 | **2** |
| Scripted agent sessions (API) | 0 | 0 | **0** |
| Messages | 626 | 200 | **826** |
| API calls | 2 033 | 88 | **2 121** |
| Input tokens | 4 025 552 | 127 730 | **4 153 282** |
| Output tokens | 2 523 352 | 37 028 | **2 560 380** |
| **Subtotal (input + output)** | **6 548 904** | **164 758** | **6 713 662** |
| Cache read (reused at reduced price) | 633 507 072 | 9 852 544 | **643 359 616** |
| **Estimated cost** | **≈ 3.04 USD** | **≈ 0.12 USD** | **≈ 3.17 USD** |

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
- Tally updated on 2026-08-05 — added deepseek-v4-pro session (fix c7).
