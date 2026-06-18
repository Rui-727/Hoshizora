# Deferred Features

Deliberate simplifications are marked `deferred:` in source code, naming
the ceiling and upgrade path. To see all of them:

```bash
grep -rn 'deferred:' src/ scripts/ tests/ Makefile
```

This file was a manually-maintained ledger of those markers. It was removed
because it duplicated source comments and went stale (line numbers shifted
with every commit). The source is the source of truth.
