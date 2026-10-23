# five

A little overkill for five letters.

A small terminal Wordle solver by [faisalm7](https://github.com/faisalm7). Type the colors you got; it tells you what to play. Ask `why` when you want to see the branches, or spot those annoying `?ight` families before burning through your guesses.

```text
five / by faisalm7
0 gray · 1 yellow · 2 green
why · list · undo · reset · quit

Play salet (2315 left).
Feedback, or 'crane 01220':
```

Feedback tiles use color in a terminal. Pipes, JSON, and `NO_COLOR=1` stay plain.

## Run it

Requires a C++20 compiler, Make, and Python 3.10+ for generating the embedded data and running tests. The executable has no third-party runtime dependencies. CI targets macOS and Linux.

```sh
git clone https://github.com/faisalm7/wordle-solver.git
cd wordle-solver
make -j
./build/wordle
```

No data downloads, API keys, or internet connection are needed to build or play. Both word lists and the strategy are bundled. `make native` enables compiler optimizations for your machine; the default build is portable within its target architecture.

## The useful bits

```sh
# Next move after a guess. Repeat --history for each previous guess.
./build/wordle suggest --history crane:01002

# Inspect a word's feedback branches and surviving one-letter families.
./build/wordle why raise --history crane:01002

# Full dynamic ranking, even when the instant strategy book applies.
./build/wordle suggest --top 10

# Follow the solver through a complete game.
./build/wordle solve apple

# Readable JSON for scripts.
./build/wordle suggest --json

# Check the exact duplicate-letter feedback rule.
./build/wordle feedback allee apple
# 21002
```

Interactive commands are `why`, `list`, `undo`, `reset`, and `quit`. Enter five feedback digits for the suggested guess, or `WORD PATTERN` after playing a different word. `B/Y/G` feedback letters work too. Contradictory feedback leaves the current state intact. `reset` starts a fresh puzzle.

`why` shows every possible feedback outcome in JSON, and the largest five branches in text. Its one-letter family detector groups surviving words that differ at exactly one position. The figures come from the actual remaining answers with uniform probabilities.

## Fast in two different ways

The normal path follows Alex Selby's published optimal `salet` strategy for the original **2,315 answers / 12,972 guesses**. A compact state lookup returns the move without building a feedback matrix. Every strategy path is checked against the feedback rules during generation and again in the tests. On that historical corpus it solves all 2,315 answers in **7,920 total guesses: 3.421166 on average, at most five**.

That optimal strategy is credited to [Alex Selby](https://github.com/alex1770/wordle); the bundled tree is his work. This project adds the terminal workflow, fast state lookup, dynamic engine, explanations, tests, and benchmarks. See [data provenance](data/README.md) for the pinned source and MIT license.

An unfamiliar history or a custom dictionary uses the dynamic engine:

- One byte per guess/answer feedback pair: **30,030,180 bytes** for the bundled corpus.
- Parallel table generation and exhaustive histogram ranking across every allowed guess.
- Expected survivors, entropy, and minimax scoring; deterministic ties prefer possible answers, then alphabetical order.
- Memoized exact expected-guess search for endgames with at most eight answers. Equivalent partitions are evaluated once.
- Direct one- and two-answer decisions without a table allocation.
- Fresh states with at most 128 answers score feedback directly, avoiding a full table build. Small rankings visit only nonempty feedback buckets.
- A dictionary-fingerprinted, checksummed disk cache. Invalid or stale caches are rebuilt.

`--dynamic` skips the strategy book. `--strategy entropy` and `--strategy minimax` also select the dynamic engine. Small endgames still use exact expected-guess search; use `--exact-threshold 0` for purely heuristic decisions. Exact search minimizes expected guesses from that state, without a fixed turn horizon. It does not prove global optimality for arbitrary dictionaries.

`--candidate-only` restricts guesses to the surviving answers. This is stricter than Wordle's usual hard mode. `solve` and `evaluate` also choose a candidate on the final allowed turn. The normal book needs at most five turns on the bundled corpus.

`solve --history` includes past guesses in its transcript and turn limit. A solved history returns immediately. `--first-guess` forces the next simulated move, subject to candidate restrictions.

## Different dictionaries

The supplied lists are historical, **not a live NYT answer schedule**. Bring another list if a word is missing:

```sh
./build/wordle suggest --answers answers.txt --guesses guesses.txt --top 10
```

One five-letter ASCII word per line; uppercase is normalized and duplicates are removed. Answers are always legal guesses. With `--answers` alone, guesses default to that same list. Exact book compatibility is checked against both word sets. The dynamic feedback matrix has a 512 MiB allocation limit.

The cache defaults to `.wordle-cache.bin` in the working directory. Set `--cache PATH` to share it between runs, or `--no-cache` to keep the table in memory only.

`--top` ranks the immediate feedback split. A book or exact-search recommendation can differ because it minimizes guesses all the way to the answer.

## Check the claims

```sh
make test
make sanitize
make clean && make -j  # restore the optimized build after sanitizer checks

# Full historical corpus, default strategy.
./build/wordle evaluate --json

# Dynamic engine only, including exact endgames.
./build/wordle evaluate --dynamic --json

# Matrix setup plus median/p95 uncached dynamic rankings.
./build/wordle bench --iterations 101 --json
```

The tests compare repeated-letter feedback with an independent scorer, brute-force small exact-search states, validate corrupted caches, exercise the CLI, and walk every bundled strategy path. CI runs on macOS and Linux, plus address/undefined-behavior sanitizers on Linux.

Benchmarks distinguish lookup latency, process startup, matrix creation/loading, ranking, and solve quality. Reproduce the recorded results with `python3 scripts/benchmark.py --help`. There is no claim of a world runtime record: that would require controlled comparisons against other implementations. The historical book's average-guess optimum and this implementation's measured speed are separate claims.

See the [recorded Apple M3 results](benchmarks/README.md) for timings, methodology, and the raw measurements.

MIT. Upstream data and strategy notices are preserved under `data/`.
