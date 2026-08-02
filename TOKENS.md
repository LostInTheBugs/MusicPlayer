# Suivi des tokens consommés — MusicPlayer

Relevé du nombre de tokens (LLM) consommés pour ce projet, session par
session de développement.

## Session 2026-08-02 (création + plugins visuels + barre de progression)

| Métrique | Valeur |
|---|---|
| Session | `20260802_181701_f23c7f` |
| Modèle | deepseek-v4-flash (provider deepseek) |
| Messages | 425 |
| Appels API | 172 |
| Tokens d'entrée (input) | 125 497 |
| Tokens de sortie (output) | 250 676 |
| Dont raisonnement | 143 149 |
| Cache lecture (cache_read) | 33 302 912 |
| Cache écriture (cache_write) | 0 |
| **Total (input + output)** | **376 173** |
| Coût estimé | ≈ 0,181 USD |

> Le gros du coût est amorti par le cache de lecture (33,3 M de tokens
> relus à prix réduit) : coût facturable ≈ 0,18 $ pour toute la session.

## Comment relire le compteur

La base de sessions de Hermes (SQLite) contient les compteurs exacts :

```bash
sqlite3 ~/.hermes/state.db "SELECT id, started_at, model,
  input_tokens, output_tokens, cache_read_tokens, cache_write_tokens,
  reasoning_tokens, estimated_cost_usd
  FROM sessions WHERE cwd LIKE '%MusicPlayer%'
  ORDER BY started_at;"
```

Après chaque session de développement, copier la ligne correspondante
dans le tableau ci-dessus.

## Notes

- Relevé effectué via `~/.hermes/state.db` (table `sessions`) — ce sont
  les compteurs réels du runtime, pas une estimation.
- `reasoning_tokens` est probablement inclus dans `output_tokens`
  (à confirmer selon le fournisseur).
- Relevés précédents : 163 174 tokens / 0,055 $ à la fin de la création
  du projet (2026.08.001).
