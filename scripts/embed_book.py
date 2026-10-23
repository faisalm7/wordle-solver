#!/usr/bin/env python3
"""Turn the strategy tree into a lookup table."""

from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass, field
import hashlib
from pathlib import Path
import re

from embed_words import ROOT, read_words


@dataclass
class State:
    guess: str
    answers: list[str] = field(default_factory=list)
    total_turns: int = 0


def feedback(guess: str, answer: str) -> str:
    result = ["B"] * 5
    remaining = Counter(answer)
    for i, letter in enumerate(guess):
        if letter == answer[i]:
            result[i] = "G"
            remaining[letter] -= 1
    for i, letter in enumerate(guess):
        if result[i] != "G" and remaining[letter]:
            result[i] = "Y"
            remaining[letter] -= 1
    return "".join(result)


def state_hash(ids: tuple[int, ...]) -> int:
    value = 14695981039346656037
    for answer_id in ids:
        for byte in (answer_id & 255, answer_id >> 8):
            value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return value


def parse_tree(path: Path, answers: list[str], guesses: list[str]) -> list[tuple]:
    expected_answers, guess_set = set(answers), set(guesses)
    states: dict[tuple, State] = {}
    seen: set[str] = set()
    previous = ""
    for line_number, line in enumerate(path.read_text(encoding="ascii").splitlines(), 1):
        indent = len(line) - len(line.lstrip(" "))
        if not line.strip() or indent > len(previous):
            raise ValueError(f"{path}:{line_number}: invalid tree indentation")
        expanded = previous[:indent] + line[indent:]
        previous = expanded
        tokens = expanded.split()
        if len(tokens) % 2:
            raise ValueError(f"{path}:{line_number}: incomplete path")
        moves = []
        for depth, i in enumerate(range(0, len(tokens), 2), 1):
            guess, pattern_depth = tokens[i:i + 2]
            match = re.fullmatch(r"([BYG]{5})([1-9])", pattern_depth)
            if guess not in guess_set or not match or int(match[2]) != depth:
                raise ValueError(f"{path}:{line_number}: invalid move {guess} {pattern_depth}")
            moves.append((guess, match[1]))
        answer = moves[-1][0]
        if moves[-1][1] != "GGGGG" or answer not in expected_answers or answer in seen:
            raise ValueError(f"{path}:{line_number}: invalid or repeated answer {answer}")
        seen.add(answer)
        for depth, (guess, pattern) in enumerate(moves):
            if feedback(guess, answer) != pattern:
                raise ValueError(f"{path}:{line_number}: wrong feedback for {guess} against {answer}")
            prefix = tuple(moves[:depth])
            state = states.setdefault(prefix, State(guess))
            if state.guess != guess:
                raise ValueError(f"{path}:{line_number}: inconsistent next guess")
            state.answers.append(answer)
            state.total_turns += len(moves) - depth
    if seen != expected_answers:
        raise ValueError(f"{path}: tree leaves do not exactly cover the answer dictionary")
    answer_ids = {word: i for i, word in enumerate(answers)}
    guess_ids = {word: i for i, word in enumerate(guesses)}
    unique: dict[tuple[int, ...], tuple] = {}
    for state in states.values():
        ids = tuple(sorted(answer_ids[word] for word in state.answers))
        record = (state_hash(ids), ids, guess_ids[state.guess], state.total_turns)
        old = unique.setdefault(ids, record)
        if old != record:
            raise ValueError("strategy has conflicting actions for an identical candidate state")
    return sorted(unique.values())


def string_array(name: str, words: list[str]) -> str:
    rows = [", ".join('"' + w + '"' for w in words[i:i + 10])
            for i in range(0, len(words), 10)]
    return (f"inline constexpr std::array<std::string_view, {len(words)}> {name} = {{{{\n"
            + ",\n".join("    " + row for row in rows) + "\n}};\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--answers", type=Path, default=ROOT / "data/answers.txt")
    parser.add_argument("--guesses", type=Path, default=ROOT / "data/guesses.txt")
    parser.add_argument("--tree", type=Path, default=ROOT / "data/salet.easy.origwords.tree")
    parser.add_argument("--output", type=Path, default=ROOT / "build/generated/book_data.hpp")
    args = parser.parse_args()
    try:
        answers = sorted(read_words(args.answers)[0])
        guesses = sorted(read_words(args.guesses)[0])
        if len(answers) > 65535 or len(guesses) > 65535:
            raise ValueError("the compact book requires fewer than 65536 answers and guesses")
        states = parse_tree(args.tree, answers, guesses)
    except (OSError, UnicodeError, ValueError) as error:
        parser.error(str(error))
    ids: list[int] = []
    rows = []
    for hash_value, candidates, guess_id, total_turns in states:
        rows.append(f"    {{{hash_value}ULL, {len(ids)}U, {len(candidates)}U, {guess_id}U, {total_turns}U}}")
        ids.extend(candidates)
    id_rows = [", ".join(str(i) for i in ids[start:start + 24])
               for start in range(0, len(ids), 24)]
    tree_hash = hashlib.sha256(args.tree.read_bytes()).hexdigest()
    node_data = ",\n".join(rows)
    candidate_data = ",\n".join("    " + row for row in id_rows)
    header = f"""// Generated by scripts/embed_book.py. Do not edit.
// Alex Selby's MIT-licensed optimal original-corpus strategy.
// See data/LICENSE-alex1770.txt.
// Strategy SHA-256: {tree_hash}
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace wordle::generated_book {{

{string_array("kAnswers", answers)}
{string_array("kGuesses", guesses)}
struct Node {{
    std::uint64_t hash;
    std::uint32_t ids_offset;
    std::uint16_t size;
    std::uint16_t guess;
    std::uint32_t total_turns;
}};

inline constexpr std::array<Node, {len(states)}> kNodes = {{{{
{node_data}
}}}};

inline constexpr std::array<std::uint16_t, {len(ids)}> kCandidateIds = {{{{
{candidate_data}
}}}};

}} // namespace wordle::generated_book
"""
    args.output.parent.mkdir(parents=True, exist_ok=True)
    encoded = header.encode("ascii")
    if not args.output.exists() or args.output.read_bytes() != encoded:
        args.output.write_bytes(encoded)
    root = next(s for s in states if len(s[1]) == len(answers))
    print(f"Embedded {len(states)} book states, {len(ids)} candidate IDs; "
          f"root {guesses[root[2]]}: {root[3]}/{len(answers)} attempts")


if __name__ == "__main__":
    main()
