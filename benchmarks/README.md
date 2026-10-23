# Numbers, not adjectives

Measured October 22, 2026 on an Apple M3, 8 logical CPUs, 16 GiB RAM, macOS, Apple Clang 21. The portable `-O3` build was used, with automatic worker selection. No `-march=native`, fast-math, or link-time optimization.

The [raw measurements](results-apple-m3.json) include samples, dictionary hashes, binary hash, compiler version, commands, and the clean source commit: [`126f321`](https://github.com/faisalm7/wordle-solver/commit/126f321bd34e4585118c4f528c45ed3c546acdef).

| Operation | Median | p95 |
| --- | ---: | ---: |
| Book recommendation + feedback statistics | 0.069 ms | 0.075 ms |
| Fresh process, book suggestion, including JSON output | 4.959 ms | 6.532 ms |
| Dynamic recommendation after `crane:01002` (40 answers), no matrix | 1.128 ms | 1.514 ms |
| Fresh process for that dynamic suggestion, including JSON output | 5.525 ms | 6.329 ms |
| Exhaustive dynamic rank, 2,315 answers | 6.208 ms | 8.110 ms |
| Exhaustive dynamic rank, 100 answers | 0.739 ms | 0.991 ms |
| Exhaustive dynamic rank, 10 answers | 1.060 ms | 1.717 ms |

Fresh-process measurements use 31 runs. Internal recommendation timing excludes dictionary setup, process startup, and JSON output. Dynamic rank measurements use 101 samples after an untimed warmup, with the feedback matrix already in memory. Rankings scan all 12,972 allowed guesses. The 100-answer case uses multiple workers; the 10-answer case uses one, so latency need not fall monotonically with candidate count.

Matrix setup, measured once per mode:

| Mode | Time |
| --- | ---: |
| Build in memory, without saving | 47.951 ms |
| Build and save a new cache | 84.468 ms |
| Load and validate the existing cache | 48.008 ms |

The matrix occupies 30,030,180 bytes. Cache validation reads and checksums the entire table; its measured time is comparable to a parallel rebuild on this machine. The book and small fresh states avoid this allocation and setup.

## Solve quality

Both evaluations cover every historical answer with uniform probability and a six-turn limit. There are no excluded failures in either mean.

| Policy | Solved | Mean guesses | Maximum | Full evaluation |
| --- | ---: | ---: | ---: | ---: |
| Published optimal book | 2,315 / 2,315 | 3.421166 | 5 | 60.900 ms |
| Dynamic expected-survivor ranking + exact endgames | 2,315 / 2,315 | 3.480778 | 6 | 2.408 s |

The book's distribution is 78 answers in two guesses, 1,225 in three, 971 in four, and 41 in five. Its 7,920 total guesses come from Alex Selby's published strategy; this project validates and executes that strategy. The dynamic evaluation shares recommendations for repeated candidate states, like traversing a decision tree. Its time includes building the matrix.

## Reproduce

```sh
make clean && make -j
python3 scripts/benchmark.py --iterations 101 \
  --build-flags='-O3 -DNDEBUG -std=c++20 -Wall -Wextra -Wpedantic -pthread' \
  --output benchmarks/local-results.json
```

The benchmark driver starts fresh processes and uses a temporary matrix cache. It does not flush operating-system file caches, pin CPUs, or control thermal state. Matrix setup has one sample per mode; it is not a percentile measurement. These results describe this implementation and machine, with no claim of a world runtime record.
