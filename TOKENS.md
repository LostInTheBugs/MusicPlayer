# Token usage tracking — MusicPlayer

LLM token usage for this project, tallied session by session.

## Cumulative tally (2026-08-08)

| Metric | deepseek-v4-flash | deepseek-v4-pro | **Total** |
|---|---|---|---|
| Dev sessions (Hermes) | 2 | 1 | **3** |
| Scripted agent sessions (API) | 0 | 0 | **0** |
| Messages | 14 144 | 112 | **14 256** |
| API calls | 5 848 | 48 | **5 896** |
| Input tokens | 8 648 169 | 120 201 | **8 768 370** |
| Output tokens | 4 631 505 | 23 118 | **4 654 623** |
| **Subtotal (input + output)** | **13 279 674** | **143 319** | **13 422 993** |
| Cache read (reused at reduced price) | 1 183 647 488 | 2 564 864 | **1 186 212 352** |
| **Estimated cost** | **≈ 5.48 USD** | **≈ 0.08 USD** | **≈ 5.56 USD** |

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

- Tally taken from `~/.hermes/state.db` (table `sessions` + `session_model_usage`) — these
  are the real runtime counters, not an estimate.
- « Scripted agent sessions (API) » = `api-*` sessions driven by scripts
  (audits, releases, background tasks) attached to this project.
- `reasoning_tokens` is probably included in `output_tokens`
  (to be confirmed with the provider).
- Tally updated on 2026-08-08 — 2026.08.100 (stable). Current session
  (deepseek-v4-pro, pre-release setup + SPEC-TRANSCRIBE-WHISPER.MD) has not
  yet been flushed to state.db — to be added at the next tally.
