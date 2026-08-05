# Token usage tracking — MusicPlayer

LLM token usage for this project, tallied session by session.

## Cumulative tally (2026-08-05)

| Metric | deepseek-v4-flash | deepseek-v4-pro | **Total** |
|---|---|---|---|
| Dev sessions (Hermes) | 1 | 1 | **2** |
| Scripted agent sessions (API) | 0 | 0 | **0** |
| Messages | 712 | 210 | **922** |
| API calls | 2 069 | 94 | **2 163** |
| Input tokens | 4 048 021 | 129 132 | **4 177 153** |
| Output tokens | 2 566 617 | 38 529 | **2 605 146** |
| **Subtotal (input + output)** | **6 614 638** | **167 661** | **6 782 299** |
| Cache read (reused at reduced price) | 647 943 168 | 10 767 488 | **658 710 656** |
| **Estimated cost** | **≈ 3.10 USD** | **≈ 0.13 USD** | **≈ 3.23 USD** |

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
- Tally updated on 2026-08-05 — podcasts (2026.08.043) added.
