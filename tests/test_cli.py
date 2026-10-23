#!/usr/bin/env python3
"""Exercise the shipped CLI using an independent Python Wordle reference."""

from __future__ import annotations

import argparse
from collections import Counter
import json
import math
import os
from pathlib import Path
import random
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


def feedback(guess: str, answer: str) -> str:
    """Reserve green matches first, then consume remaining answer letters."""
    result = ["0"] * 5
    remaining = Counter(answer.lower())
    for i, letter in enumerate(guess.lower()):
        if letter == answer.lower()[i]:
            result[i] = "2"
            remaining[letter] -= 1
    for i, letter in enumerate(guess.lower()):
        if result[i] != "2" and remaining[letter] > 0:
            result[i] = "1"
            remaining[letter] -= 1
    return "".join(result)


class CLIIntegration(unittest.TestCase):
    binary: Path

    @classmethod
    def setUpClass(cls) -> None:
        cls.temporary = tempfile.TemporaryDirectory(prefix="wordle-cli-")
        cls.scratch = Path(cls.temporary.name)
        cls.answer_path = cls.scratch / "answers.txt"
        cls.guess_path = cls.scratch / "guesses.txt"
        cls.answer_path.write_bytes(b"CIGAR\r\nrebut\nsissy\nhumph\napple\napple\n\n")
        cls.guess_path.write_text("raise\nsoare\ncigar\ncrane\n", encoding="ascii")
        cls.small_answers = {"cigar", "rebut", "sissy", "humph", "apple"}
        cls.small_guesses = cls.small_answers | {"raise", "soare", "crane"}
        cls.answers = (ROOT / "data/answers.txt").read_text().splitlines()
        cls.guesses = set((ROOT / "data/guesses.txt").read_text().splitlines())
        cls.cache = cls.scratch / "matrix.bin"

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temporary.cleanup()

    def command(self, *arguments: str, status: int = 0, stdin: str | None = None,
                env: dict[str, str] | None = None) -> subprocess.CompletedProcess:
        result = subprocess.run([str(self.binary), *map(str, arguments)], input=stdin,
                                text=True, capture_output=True, cwd=self.scratch, timeout=60,
                                env=None if env is None else {**os.environ, **env})
        self.assertEqual(result.returncode, status,
                         f"Arguments: {arguments}\nstdout: {result.stdout}\nstderr: {result.stderr}")
        return result

    def document(self, *arguments: str, status: int = 0) -> dict:
        result = self.command(*arguments, "--json", "--cache", str(self.cache), status=status)
        try:
            data = json.loads(result.stdout)
        except json.JSONDecodeError as error:
            self.fail(f"Invalid JSON for {arguments}: {result.stdout!r}: {error}")
        self.assertIsInstance(data, dict)
        return data

    def custom(self, command: str, *arguments: str, status: int = 0) -> dict:
        return self.document(command, *arguments, "--answers", str(self.answer_path),
                             "--guesses", str(self.guess_path), status=status)

    def assert_game(self, game: dict, answer: str) -> None:
        self.assertEqual(game["answer"], answer)
        self.assertTrue(game["solved"])
        self.assertEqual(game["turns"], len(game["steps"]))
        self.assertLessEqual(game["turns"], 6)
        for step in game["steps"]:
            self.assertEqual(step["feedback"], feedback(step["guess"], answer))
            self.assertGreaterEqual(step["remaining"], 1)
        self.assertEqual(game["steps"][-1]["guess"], answer)
        self.assertEqual(game["steps"][-1]["feedback"], "22222")

    def assert_explanation(self, explanation: dict, candidates: set[str], guess: str) -> None:
        counts = Counter(feedback(guess, answer) for answer in candidates)
        self.assertEqual(explanation["word"], guess)
        self.assertEqual(explanation["candidate_count"], len(candidates))
        self.assertEqual(explanation["outcome_count"], len(counts))
        outcomes = explanation["outcomes"]
        self.assertEqual(len(outcomes), len(counts))
        self.assertEqual({o["feedback"]: o["count"] for o in outcomes}, dict(counts))
        self.assertTrue(all(isinstance(o["count"], int) and o["count"] > 0 for o in outcomes))
        self.assertEqual(sum(o["count"] for o in outcomes), len(candidates))
        expected = sum(count * count for count in counts.values()) / len(candidates)
        entropy = -sum((count / len(candidates)) * math.log2(count / len(candidates))
                       for count in counts.values())
        # JSON retains six decimal places even when the terminal uses two.
        self.assertAlmostEqual(explanation["expected_remaining"], expected, delta=0.00000051)
        self.assertAlmostEqual(explanation["entropy_bits"], entropy, delta=0.00000051)
        self.assertEqual(explanation["worst_case"], max(counts.values()))

        families: dict[str, set[str]] = {}
        for answer in candidates:
            for position in range(5):
                mask = answer[:position] + "?" + answer[position + 1:]
                families.setdefault(mask, set()).add(answer)
        expected_traps = sorted(((mask, words) for mask, words in families.items() if len(words) >= 3),
                                key=lambda family: (-len(family[1]), family[0]))[:3]
        self.assertEqual([(trap["mask"], set(trap["words"])) for trap in explanation["traps"]],
                         expected_traps)
        for trap in explanation["traps"]:
            mask, words = trap["mask"], trap["words"]
            self.assertEqual(mask.count("?"), 1)
            self.assertGreaterEqual(len(words), 3)
            self.assertEqual(len(words), len(set(words)))
            self.assertTrue(set(words) <= candidates)
            position = mask.index("?")
            self.assertEqual(len({word[position] for word in words}), len(words))
            self.assertTrue(all(word[:position] + "?" + word[position + 1:] == mask for word in words))

    def test_help_and_version(self) -> None:
        help_text = self.command("--help").stdout
        for token in ("suggest", "solve", "why", "--history", "--answers", "--dynamic"):
            self.assertIn(token, help_text)
        self.assertRegex(self.command("--version").stdout, r"wordle\s+\d+\.\d+\.\d+")

    def test_feedback_duplicate_letters(self) -> None:
        pairs = [("allee", "apple"), ("sassy", "assay"), ("array", "rarer"),
                 ("eeeee", "eerie"), ("eerie", "eeeee"), ("cigar", "cigar"),
                 ("cigar", "humph"), ("belle", "level"), ("puppy", "poppy")]
        randomizer = random.Random(20221022)
        pairs.extend(("".join(randomizer.choices("aabcde", k=5)),
                      "".join(randomizer.choices("aabcde", k=5))) for _ in range(64))
        self.assertEqual(feedback("sassy", "assay"), "11202")
        for guess, answer in pairs:
            with self.subTest(guess=guess, answer=answer):
                self.assertEqual(self.command("feedback", guess, answer).stdout.strip(), feedback(guess, answer))
        self.assertEqual(self.document("feedback", "ALLEE", "APPLE")["feedback"], "21002")

    def test_default_suggestion_and_history(self) -> None:
        initial = self.document("suggest")
        self.assertEqual(initial["candidate_count"], 2315)
        self.assertEqual(set(initial["candidates"]), set(self.answers))
        self.assertEqual(initial["recommendation"]["word"], "salet")
        pattern = feedback("salet", "apple")
        continued = self.document("suggest", "--history", "salet:" + pattern)
        expected = {a for a in self.answers if feedback("salet", a) == pattern}
        self.assertEqual(set(continued["candidates"]), expected)
        self.assertEqual(continued["candidate_count"], len(expected))
        self.assertIn(continued["recommendation"]["word"], self.guesses)
        singleton = self.document("suggest", "--history", "apple:GGGGG")
        self.assertEqual(singleton["candidates"], ["apple"])
        self.assertEqual(singleton["recommendation"]["word"], "apple")

    def test_solve_and_first_guess(self) -> None:
        self.assert_game(self.document("solve", "apple"), "apple")
        custom = self.custom("solve", "humph", "--first-guess", "raise")
        self.assert_game(custom, "humph")
        self.assertEqual(custom["steps"][0]["guess"], "raise")
        one = self.custom("solve", "cigar", "--first-guess", "cigar", "--max-turns", "1")
        self.assertEqual(one["turns"], 1)
        self.assert_game(one, "cigar")

    def test_simulation_history_and_candidate_restrictions(self) -> None:
        history = "raise:" + feedback("raise", "humph")
        continued = self.custom("solve", "humph", "--history", history, "--max-turns", "2")
        self.assert_game(continued, "humph")
        self.assertEqual(continued["turns"], 2)
        self.assertEqual(continued["steps"][0]["guess"], "raise")
        exhausted = self.custom("solve", "humph", "--history", history, "--max-turns", "1", status=2)
        self.assertFalse(exhausted["solved"])
        self.assertEqual(exhausted["turns"], 1)
        solved = self.custom("solve", "humph", "--history", "humph:22222", "--max-turns", "1")
        self.assert_game(solved, "humph")
        self.assertEqual(solved["turns"], 1)
        options = ["--answers", str(self.answer_path), "--guesses", str(self.guess_path)]
        for arguments in (("--first-guess", "raise", "--candidate-only"),
                          ("--first-guess", "raise", "--max-turns", "1"),
                          ("--first-guess", "cigar", "--candidate-only", "--history", history),
                          ("--history", "humph:22222", "--history", "humph:22222"),
                          ("--history", history, "--history", history, "--max-turns", "1")):
            self.command("solve", "humph", *arguments, *options, status=1)
        self.command("--json", status=1)

    def test_custom_rankings_and_options(self) -> None:
        for strategy in ("expected", "entropy", "minimax"):
            with self.subTest(strategy=strategy):
                result = self.custom("suggest", "--dynamic", "--strategy", strategy,
                                     "--top", "99", "--exact-threshold", "0", "--threads", "2")
                self.assertEqual(set(result["candidates"]), self.small_answers)
                self.assertEqual(len(result["ranking"]), len(self.small_guesses))
                previous = None
                for entry in result["ranking"]:
                    counts = Counter(feedback(entry["word"], a) for a in self.small_answers)
                    expected = sum(n * n for n in counts.values()) / len(self.small_answers)
                    self.assertAlmostEqual(entry["expected_remaining"], expected, places=5)
                    self.assertEqual(entry["worst_case"], max(counts.values()))
                    self.assertEqual(entry["possible_answer"], entry["word"] in self.small_answers)
                    score = (-entry["entropy_bits"] if strategy == "entropy" else
                             entry["worst_case"] if strategy == "minimax" else expected)
                    if previous is not None:
                        self.assertLessEqual(previous, score + 1e-5)
                    previous = score
        restricted = self.custom("suggest", "--candidate-only", "--top", "99")
        self.assertEqual({r["word"] for r in restricted["ranking"]}, self.small_answers)
        self.assertTrue(restricted["recommendation"]["exact"])
        pattern = feedback("raise", "apple")
        filtered = self.custom("suggest", "--history", "raise:" + pattern)
        self.assertEqual(set(filtered["candidates"]),
                         {a for a in self.small_answers if feedback("raise", a) == pattern})

    def test_custom_evaluation_and_limit(self) -> None:
        evaluated = self.custom("evaluate")
        self.assertEqual(evaluated["answers"], 5)
        self.assertEqual(evaluated["solved"], 5)
        self.assertEqual(evaluated["failed"], 0)
        self.assertEqual(sum(evaluated["distribution"].values()), 5)
        limited = self.custom("evaluate", "--limit", "2")
        self.assertEqual(limited["answers"], 2)
        failed = self.custom("evaluate", "--max-turns", "1", status=2)
        self.assertEqual(failed["solved"], 1)
        self.assertEqual(failed["failed"], 4)

    def test_why_histograms_and_traps(self) -> None:
        self.assert_explanation(self.custom("why", "RAISE"), self.small_answers, "raise")
        next_word = self.custom("suggest")["recommendation"]["word"]
        self.assert_explanation(self.custom("why"), self.small_answers, next_word)

        trap_answers = {"light", "might", "night", "right", "sight", "tight", "eerie"}
        trap_path = self.scratch / "trap-answers.txt"
        trap_path.write_text("\n".join(sorted(trap_answers)) + "\n")
        pattern = feedback("eerie", "night")
        surviving = {answer for answer in trap_answers if feedback("eerie", answer) == pattern}
        explained = self.document("why", "raise", "--answers", str(trap_path),
                                  "--guesses", str(self.guess_path), "--history", "eerie:" + pattern)
        self.assert_explanation(explained, surviving, "raise")
        self.assertGreater(len(explained["traps"]), 0)
        self.assertEqual(explained["traps"][0]["mask"], "?ight")

        history = "crane:01002"
        full_candidates = {answer for answer in self.answers if feedback("crane", answer) == "01002"}
        self.assertEqual(len(full_candidates), 40)
        self.assert_explanation(self.document("why", "raise", "--history", history), full_candidates, "raise")
        next_word = self.document("suggest", "--history", history)["recommendation"]["word"]
        self.assert_explanation(self.document("why", "--history", history), full_candidates, next_word)

    def test_cache_load_and_corruption_recovery(self) -> None:
        self.cache.unlink(missing_ok=True)
        # An explicit ranking over the full corpus requires the matrix. Tiny
        # custom states intentionally use direct feedback without a disk cache.
        cold = self.document("suggest", "--top", "1")
        self.assertFalse(cold["cache_loaded"])
        self.assertTrue(self.cache.exists())
        warm = self.document("suggest", "--top", "1")
        self.assertTrue(warm["cache_loaded"])
        self.assertEqual(cold["recommendation"], warm["recommendation"])
        self.assertEqual(cold["ranking"], warm["ranking"])
        self.cache.write_bytes(b"invalid cache")
        recovered = self.document("suggest", "--top", "1")
        self.assertFalse(recovered["cache_loaded"])
        self.assertEqual(cold["recommendation"], recovered["recommendation"])
        self.assertEqual(cold["ranking"], recovered["ranking"])

        disabled_path = self.scratch / "disabled-cache.bin"
        result = self.command("suggest", "--top", "1", "--cache", str(disabled_path),
                              "--no-cache", "--json")
        uncached = json.loads(result.stdout)
        self.assertFalse(uncached["cache_loaded"])
        self.assertFalse(disabled_path.exists())
        self.assertEqual(cold["recommendation"], uncached["recommendation"])
        self.assertEqual(cold["ranking"], uncached["ranking"])

    def test_invalid_inputs(self) -> None:
        cases = [("feedback", "abcd", "apple"), ("feedback", "apple"),
                 ("suggest", "--unknown"), ("suggest", "--threads", "-1"),
                 ("suggest", "--threads", "257"), ("suggest", "--exact-threshold", "13"),
                 ("suggest", "--strategy", "invalid"), ("suggest", "--top"),
                 ("suggest", "--history", "bad"),
                 ("suggest", "--history", "apple:XXXXX", "--history", "apple:GGGGG"),
                 ("why", "zzzzz"), ("why", "raise", "crane"),
                 ("solve", "zzzzz"), ("unknown-command",)]
        for arguments in cases:
            with self.subTest(arguments=arguments):
                result = self.command(*arguments, status=1)
                self.assertIn("wordle:", result.stderr)
        for contents in ("", "apple\nnot-a-word\n"):
            invalid = self.scratch / "invalid.txt"
            invalid.write_text(contents)
            self.command("suggest", "--answers", str(invalid), status=1)

    def test_interactive_quit(self) -> None:
        expected = self.document("suggest", "--answers", str(self.answer_path))["recommendation"]["word"]
        result = self.command("--answers", str(self.answer_path), "--no-cache", stdin="quit\n")
        self.assertIn(f"Play {expected} ({len(self.small_answers)} left).", result.stdout)
        self.assertNotIn("\x1b", result.stdout + result.stderr)

    def test_interactive_state_and_no_color_redirection(self) -> None:
        pattern = feedback("crane", "apple")
        surviving = {answer for answer in self.small_answers if feedback("crane", answer) == pattern}
        initial_word = self.custom("suggest")["recommendation"]["word"]
        next_word = self.custom("suggest", "--history", "crane:" + pattern)["recommendation"]["word"]
        commands = ["why", "list", "invalid", "crane " + pattern, "why", "list", "invalid",
                    "undo", "list", "crane " + pattern, "reset", "list", "quit"]
        result = self.command("--answers", str(self.answer_path), "--guesses", str(self.guess_path),
                              "--no-cache", stdin="\n".join(commands) + "\n",
                              env={"NO_COLOR": "1", "TERM": "xterm-256color"})
        self.assertNotIn("\x1b", result.stdout + result.stderr)
        expected_sizes = [5, 5, 5, 5, len(surviving), len(surviving), len(surviving), len(surviving),
                          5, 5, len(surviving), 5, 5]
        prompts = re.findall(r"Play ([a-z]{5}) \((\d+) left\)\.", result.stdout)
        self.assertEqual(prompts, [(initial_word if count == 5 else next_word, str(count))
                                   for count in expected_sizes])
        self.assertEqual(result.stdout.count("Invalid feedback:"), 2)
        self.assertIn(f"{initial_word} splits 5 answers", result.stdout)
        self.assertIn(f"{next_word} splits {len(surviving)} answers", result.stdout)
        listed = re.findall(r": ([a-z \n]+)\n(\d+) possible answers", result.stdout)
        self.assertEqual([(set(words.split()), int(count)) for words, count in listed],
                         [(self.small_answers, 5), (surviving, len(surviving)),
                          (self.small_answers, 5), (self.small_answers, 5)])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path, nargs="?", default=ROOT / "build/wordle")
    arguments = parser.parse_args()
    CLIIntegration.binary = arguments.binary.resolve()
    unittest.main(argv=[__file__], verbosity=2)
