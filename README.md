# wordle solver

A C++ Wordle solver that runs in the terminal.

## Run

You'll need a C++20 compiler, Make, and Python 3.10+.

```sh
make -j
./build/wordle
```

Enter the feedback as five digits: `0` for gray, `1` for yellow, `2` for green.
Play the suggested word in Wordle first. The terminal asks for its colors.
If you played a different word, type it first, or enter `crane 01002`.

Type `help` for an example, `why` to see what a guess tells you, `list` for the remaining words,
or `undo` to fix a mistake. `reset` starts over and `quit` exits.

You can also use it without the interactive prompt:

```sh
./build/wordle suggest --history crane:01002
./build/wordle why raise --history crane:01002
./build/wordle solve apple
```

See `./build/wordle --help` for the other options. `make test` runs the tests.

## How it works

It follows a precomputed `salet` strategy when possible. Other positions are
handled by scoring the feedback groups for each possible guess, with an exact
search for small endgames. Small positions skip the full feedback table.

It starts with the original 2,315 answers. If none fit your clues, it checks all
12,972 known words against every clue, including answers like `trill` that were
missing from the original list. This still isn't a live copy of today's Wordle.
Use `--answers` and `--guesses` to supply your own; an explicit answer list stays strict.

The word lists and precomputed strategy come from
[Alex Selby's solver](https://github.com/alex1770/wordle/tree/8fd3f1bdf884ff6757c24ad1227dc561fab7787d).
His MIT license is included in [data/LICENSE-alex1770.txt](data/LICENSE-alex1770.txt).
