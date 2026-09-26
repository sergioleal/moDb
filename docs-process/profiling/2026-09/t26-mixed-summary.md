# T26 mixed recheck

## @build/ab-pre26/modb_load.exe load.mixed_oltp.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 8 | 64.164 (2.7%) | 115.693 (4.1%) | 483 | 44.5% | — |
| mixed_oltp | 8 | 18.608 (7.6%) | 18.652 (7.6%) | 2.687 | 0.2% | — |

## @build/relwithdebinfo/modb_load.exe load.mixed_oltp.embedded.10k

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 8 | 65.140 (2.1%) | 116.886 (2.9%) | 483 | 44.3% | — |
| mixed_oltp | 8 | 18.864 (3.6%) | 18.912 (3.7%) | 2.687 | 0.3% | — |

