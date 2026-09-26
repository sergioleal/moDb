# T5.3 batch

## load.create_only.embedded.10k --batch 1

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 3 | 741 (0.1%) | 68.624 (0.8%) | 33.417 | 98.9% | — |

## load.create_only.embedded.10k --batch 10

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 3 | 6.384 (1.8%) | 129.339 (2.1%) | 3.788 | 95.1% | — |

## load.create_only.embedded.10k --batch 100

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 3 | 32.887 (11.4%) | 155.271 (11.3%) | 820 | 78.8% | — |

## load.create_only.embedded.10k --batch 1000

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 3 | 64.489 (4.6%) | 177.501 (4.8%) | 483 | 63.7% | — |

## load.create_only.embedded.10k --batch 10000

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 3 | 70.061 (4.3%) | 172.807 (4.8%) | 446 | 59.5% | — |

## load.create_only.embedded.10k --batch 1 --durability disabled_diagnostic

| fase | reps | ops/s (CV) | motor ops/s (CV) | WAL B/op | overhead harness | atribuído |
|---|---|---|---|---|---|---|
| create | 3 | 7.249 (2.2%) | 153.909 (1.5%) | 33.417 | 95.3% | — |

