# Token usage tracking — MusicPlayer

LLM token usage for this project, tallied session by session.

## Cumulative tally (2026-08-05)

| Metric | deepseek-v4-flash | deepseek-v4-pro | **Total** |
|---|---|---|---|
| Dev sessions (Hermes) | 1 | 1 | **2** |
| Scripted agent sessions (API) | 0 | 0 | **0** |
| Messages | 561 | 210 | **771** |
| API calls | 2 481 | 94 | **2 575** |
| Input tokens | 4 579 691 | 129 132 | **4 708 823** |
| Output tokens | 2 951 193 | 38 529 | **2 989 722** |
| **Subtotal (input + output)** | **7 530 884** | **167 661** | **7 698 545** |
| Cache read (reused at reduced price) | 771 440 512 | 10 767 488 | **782 208 000** |
| **Estimated cost** | **≈ 3.63 USD** | **≈ 0.13 USD** | **≈ 3.76 USD** |

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
