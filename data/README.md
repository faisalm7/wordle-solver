# Bundled historical dictionary

These are the original Wordle dictionaries, preserved for reproducible solver
benchmarks. They are **historical, not the current New York Times dictionaries**.
The daily answer pool and accepted guesses have changed since this corpus was
published. Use custom dictionaries when matching a different edition of Wordle.

The files are unmodified copies from Alex Selby's
[`alex1770/wordle`](https://github.com/alex1770/wordle) repository at commit
[`8fd3f1bdf884ff6757c24ad1227dc561fab7787d`](https://github.com/alex1770/wordle/commit/8fd3f1bdf884ff6757c24ad1227dc561fab7787d):

| Local file | Words | Upstream file | SHA-256 |
| --- | ---: | --- | --- |
| `answers.txt` | 2,315 | [`wordlist_orig_hidden`](https://github.com/alex1770/wordle/blob/8fd3f1bdf884ff6757c24ad1227dc561fab7787d/wordlist_orig_hidden) | `f40e47975cf360ff9febe7d49c8e285054d8fa3fb9644cea575922060951d6ee` |
| `guesses.txt` | 12,972 | [`wordlist_orig_all`](https://github.com/alex1770/wordle/blob/8fd3f1bdf884ff6757c24ad1227dc561fab7787d/wordlist_orig_all) | `fb66d9b47e0dad70e7c6c886cf76a6f453af3aa75003dcfa129c8326938e7c66` |

`guesses.txt` includes every answer. Each file contains one lowercase ASCII word
per line; the upstream order is retained. These files describe a vocabulary, not
the date or order of daily puzzles.

The upstream repository is distributed under the MIT License, copyright (c)
2022 Alex Selby (github alex1770). Its complete, unmodified license is included as
[`LICENSE-alex1770.txt`](LICENSE-alex1770.txt), copied from
[`LICENSE`](https://github.com/alex1770/wordle/blob/8fd3f1bdf884ff6757c24ad1227dc561fab7787d/LICENSE)
with SHA-256
`034490a63dfdaa13787e46e0e38966e009b1a31ece512d616b2dac6be5b97df5`.
The upstream [README](https://github.com/alex1770/wordle/blob/8fd3f1bdf884ff6757c24ad1227dc561fab7787d/README)
distinguishes these original lists from its later dictionaries.

`scripts/embed_words.py` validates and embeds these checked-in files during the
build. It preserves their bytes as inputs and their word order in the generated
header. Building and running the solver require no dictionary downloads.

## Optimal strategy

[`salet.easy.origwords.tree`](salet.easy.origwords.tree) is an unmodified copy of
Alex Selby's [normal-mode strategy for the original dictionaries](https://github.com/alex1770/wordle/blob/8fd3f1bdf884ff6757c24ad1227dc561fab7787d/salet.easy.origwords.tree)
from the same commit and under the same MIT License. Its SHA-256 is
`32a8663403a8cee0d83a0fb59f10c66f3c16c96c597080f9a196c9f0a5853259`.
The strategy starts with `salet`. It solves all 2,315 historical answers using
7,920 total guesses (3.4211663 per answer under a uniform prior), with a maximum
of five guesses. This is an optimal expected-guess strategy for the original
normal-mode corpus; it is not a claim about every variant or current NYT data.
See the upstream author's [analysis](http://sonorouschocolate.com/notes/index.php?title=The_best_strategies_for_Wordle)
and [repository documentation](https://github.com/alex1770/wordle/blob/8fd3f1bdf884ff6757c24ad1227dc561fab7787d/README).

`scripts/embed_book.py` independently checks every feedback pattern, confirms
that the leaves cover exactly the bundled answer list, and generates a compact
table indexed by the exact remaining candidate set. The generated book contains
2,448 states and 7,920 candidate IDs. A lookup must verify the complete candidate
set after matching its hash. This permits immediate recommendations on book
states and dynamic analysis when a state is absent.
