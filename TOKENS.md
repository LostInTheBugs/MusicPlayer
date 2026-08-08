# Token usage tracking — MusicPlayer

LLM token usage for this project, tallied session by session.

## Cumulative tally (2026-08-08)

| Metric | deepseek-v4-flash | deepseek-v4-pro | **Total** |
|---|---|---|---|
| Dev sessions (Hermes) | 2 | 2 | **4** |
| Scripted agent sessions (API) | 0 | 0 | **0** |
| Messages | 13 881 | — | **13 881** |
| API calls | 4 183 | 19 | **4 202** |
| Input tokens | 8 416 361 | 51 072 | **8 467 433** |
| Output tokens | 4 604 554 | 10 430 | **4 614 984** |
| **Subtotal (input + output)** | **13 020 915** | **61 502** | **13 082 417** |
| Cache read (reused at reduced price) | 1 178 442 624 | 601 472 | **1 179 044 096** |
| **Estimated cost** | **≈ 5.44 USD** | **≈ 0.03 USD** | **≈ 5.47 USD** |

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
- Tally updated on 2026-08-08 — 2026.08.100 (stable). Current session (deepseek-v4-pro, pre-release setup + SPEC-TRANSCRIBE-WHISPER.MD) not yet flushed to state.db — to be tallied on next update.
