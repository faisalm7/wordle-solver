# wordle solver

A C++ Wordle solver that runs in the terminal.

## Run

You'll need a C++20 compiler, Make, and Python 3.10+.

```sh
make -j
./build/wordle
```

Enter the feedback as five digits: `0` for gray, `1` for yellow, `2` for green.
If you played a different word, enter it with the feedback, like `crane 01002`.

Type `why` to see what a guess tells you, `list` for the remaining words,
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

The bundled lists are the original 2,315 answers and 12,972 guesses, so they may
differ from today's Wordle. Use `--answers` and `--guesses` to supply your own.

The word lists and precomputed strategy come from
[Alex Selby's solver](https://github.com/alex1770/wordle/tree/8fd3f1bdf884ff6757c24ad1227dc561fab7787d).
His MIT license is included in [data/LICENSE-alex1770.txt](data/LICENSE-alex1770.txt).
