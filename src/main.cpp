#include "solver.hpp"
#include "book.hpp"
#include "words.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;
using wordle::CandidateSet;
using wordle::Recommendation;
struct Arguments {
    std::string command = "interactive", answers, guesses, cache = ".wordle-cache.bin", first;
    std::vector<std::string> positional, history;
    wordle::Strategy strategy = wordle::Strategy::Expected;
    std::size_t threads = 0, top = 0, max_turns = 6, iterations = 21, limit = 0, exact = 8;
    bool json = false, candidate_only = false, dynamic = false;
};
const char* help = R"(wordle solver

Usage:
  wordle                              Interactive assistant
  wordle suggest [options]            Suggest the next guess
  wordle why [WORD] [options]         Explain a guess and expose word families
  wordle solve ANSWER [options]        Simulate a solution
  wordle evaluate [options]            Solve every bundled answer
  wordle bench [options]               Measure uncached ranking latency
  wordle feedback GUESS ANSWER         Print exact Wordle feedback

Options:
  --history WORD:01220  Repeat for each prior guess (0 gray, 1 yellow, 2 green)
  --strategy NAME       expected (default), entropy, or minimax
  --top N               Also rank N guesses with the dynamic engine
  --dynamic             Bypass the optimal historical strategy book
  --candidate-only      Restrict each guess to remaining possible answers
  --exact-threshold N   Exact expected-guess endgames up to N answers (0..12; default 8)
  --threads N           Worker threads (0 = automatic)
  --cache PATH          Feedback table cache (default .wordle-cache.bin)
  --no-cache            Build the table in memory without a disk cache
  --answers FILE        Use only this answer list (one five-letter word per line)
  --guesses FILE        Custom allowed guesses (answers are always included)
  --first-guess WORD    Force the next simulated guess for solve/evaluate
  --max-turns N         Turn limit for solve/evaluate (default 6)
  --limit N             Evaluate the first N answers (default all)
  --iterations N        Benchmark samples per candidate size (default 21)
  --json                Machine-readable output
  --help, --version

Examples:
  wordle suggest --history crane:01002 --json
  wordle solve apple
  wordle evaluate --json > evaluation.json

The default historical strategy uses a validated optimal decision book.
Dynamic ranking is exhaustive over the supplied guesses. Small-state best guesses use
exact expected-guess search; --exact-threshold 0 selects pure heuristic scoring.
When no original answer fits, all known words are checked against your clues.
An explicit --answers list stays strict. The bundled lists aren't live NYT data.
)";

bool color_enabled() {
#if defined(_WIN32)
    return false;
#else
    const char* term = std::getenv("TERM");
    return isatty(STDOUT_FILENO) && !std::getenv("NO_COLOR") && term && std::string_view(term) != "dumb";
#endif
}
std::string tiles(const std::string& guess, const std::string& pattern) {
    if (!color_enabled()) return guess + "  " + pattern;
    std::string result;
    for (std::size_t i = 0; i < 5; ++i) {
        result += pattern[i] == '2' ? "\033[48;5;107m\033[30m" : pattern[i] == '1' ? "\033[48;5;179m\033[30m" : "\033[48;5;238m\033[97m";
        result += ' ';
        result += static_cast<char>(guess[i] - 'a' + 'A');
        result += " \033[0m ";
    }
    return result + " " + pattern;
}

std::size_t number(const std::string& text, const std::string& option) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument(option + " requires a nonnegative integer");
    const auto n = std::stoull(text);
    if (n > 1000000) throw std::invalid_argument(option + " is out of range");
    return static_cast<std::size_t>(n);
}
std::string word(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    if (s.size() != 5 || s.find_first_not_of("abcdefghijklmnopqrstuvwxyz") != std::string::npos)
        throw std::invalid_argument("words must contain exactly five ASCII letters: " + s);
    return s;
}
Arguments parse(int argc, char** argv) {
    Arguments a;
    int i = 1;
    if (i < argc && argv[i][0] != '-') a.command = argv[i++];
    for (; i < argc; ++i) {
        const std::string flag = argv[i];
        auto value = [&]() -> std::string {
            if (++i == argc) throw std::invalid_argument("missing value for " + flag);
            return argv[i];
        };
        if (flag == "--help" || flag == "-h") a.command = "help";
        else if (flag == "--version") a.command = "version";
        else if (flag == "--json") a.json = true;
        else if (flag == "--candidate-only") a.candidate_only = true;
        else if (flag == "--dynamic") a.dynamic = true;
        else if (flag == "--no-cache") a.cache.clear();
        else if (flag == "--cache") a.cache = value();
        else if (flag == "--answers") a.answers = value();
        else if (flag == "--guesses") a.guesses = value();
        else if (flag == "--history") a.history.push_back(value());
        else if (flag == "--first-guess") a.first = word(value());
        else if (flag == "--threads") a.threads = number(value(), flag);
        else if (flag == "--top") a.top = number(value(), flag);
        else if (flag == "--max-turns") a.max_turns = number(value(), flag);
        else if (flag == "--iterations") a.iterations = number(value(), flag);
        else if (flag == "--limit") a.limit = number(value(), flag);
        else if (flag == "--exact-threshold") a.exact = number(value(), flag);
        else if (flag == "--strategy") {
            const auto v = value();
            if (v == "expected") a.strategy = wordle::Strategy::Expected;
            else if (v == "entropy") a.strategy = wordle::Strategy::Entropy;
            else if (v == "minimax") a.strategy = wordle::Strategy::Minimax;
            else throw std::invalid_argument("unknown strategy: " + v);
        } else if (flag.starts_with('-')) throw std::invalid_argument("unknown option: " + flag);
        else a.positional.push_back(flag);
    }
    if (!a.max_turns || !a.iterations || a.exact > 12 || a.threads > 256)
        throw std::invalid_argument("max-turns and iterations must be positive; exact-threshold <= 12; threads <= 256");
    const std::size_t wanted = a.command == "feedback" ? 2 : a.command == "solve" ? 1 : 0;
    if (a.command != "help" && a.command != "version" &&
        (a.command == "why" ? a.positional.size() > 1 : a.positional.size() != wanted))
        throw std::invalid_argument("unexpected number of arguments; run wordle --help");
    if (a.command == "interactive" && a.json)
        throw std::invalid_argument("JSON output requires a noninteractive command");
    return a;
}
std::vector<std::string> read_words(const std::string& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open dictionary: " + path);
    std::vector<std::string> words;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        try { words.push_back(word(line)); }
        catch (const std::exception&) { throw std::invalid_argument(path + ": invalid word on line " + std::to_string(line_number)); }
    }
    if (input.bad()) throw std::runtime_error("failed reading dictionary: " + path);
    if (words.empty()) throw std::invalid_argument("empty dictionary: " + path);
    return words;
}
std::pair<std::string, std::uint8_t> observation(const std::string& item) {
    if (item.size() != 11 || item[5] != ':') throw std::invalid_argument("history must be WORD:01220");
    return {word(item.substr(0, 5)), wordle::parse_pattern(item.substr(6))};
}
struct Step { std::string guess, pattern; std::size_t remaining; };

class Puzzle {
public:
    Puzzle(const wordle::Solver& original, const Arguments& a)
        : original_(original), options_{a.threads, a.cache, a.exact, true}, can_expand_(a.answers.empty()) {
        reset();
        for (const auto& item : a.history) {
            const auto [guess, pattern] = observation(item);
            if (!add(guess, pattern)) throw std::invalid_argument("history does not match the word list at " + item);
        }
    }

    const wordle::Solver& solver() const { return expanded() ? *larger_ : original_; }
    const CandidateSet& candidates() const { return states_.back().candidates; }
    const std::vector<Step>& steps() const { return steps_; }
    bool expanded() const { return states_.back().expanded; }
    bool solved() const { return !steps_.empty() && steps_.back().pattern == "22222"; }

    bool add(const std::string& guess, std::uint8_t pattern) {
        if (solved()) return false;
        auto remaining = solver().filter(candidates(), guess, pattern);
        bool use_larger = expanded();
        if (remaining.empty() && can_expand_ && !use_larger) {
            if (!larger_) larger_ = std::make_unique<wordle::Solver>(original_.guesses(), original_.guesses(), options_);
            remaining = larger_->initial_candidates();
            for (const auto& step : steps_) {
                remaining = larger_->filter(remaining, step.guess, wordle::parse_pattern(step.pattern));
                if (remaining.empty()) break;
            }
            remaining = larger_->filter(remaining, guess, pattern);
            use_larger = true;
        }
        if (remaining.empty()) return false;
        steps_.push_back({guess, wordle::format_pattern(pattern), remaining.size()});
        states_.push_back({std::move(remaining), use_larger});
        return true;
    }

    void undo() {
        if (steps_.empty()) return;
        steps_.pop_back();
        states_.pop_back();
    }
    void reset() {
        steps_.clear();
        states_.clear();
        states_.push_back({original_.initial_candidates(), false});
    }

private:
    struct State { CandidateSet candidates; bool expanded; };
    const wordle::Solver& original_;
    wordle::Options options_;
    bool can_expand_;
    std::unique_ptr<wordle::Solver> larger_;
    std::vector<State> states_;
    std::vector<Step> steps_;
};

const char* expanded_notice = "No matches in the original answer list. Using the larger word list.\n";
void print_recommendation(const Recommendation& r) {
    std::cout << "{\"word\":\"" << r.word << "\",\"entropy_bits\":" << r.entropy
              << ",\"expected_remaining\":" << r.expected_remaining
              << ",\"worst_case\":" << r.worst_case
              << ",\"possible_answer\":" << (r.possible_answer ? "true" : "false")
              << ",\"exact\":" << (r.exact ? "true" : "false");
    if (r.exact) std::cout << ",\"expected_guesses\":" << r.expected_guesses;
    std::cout << '}';
}
std::string choose(const wordle::Solver& solver, const Arguments& a, const CandidateSet& candidates,
                   const wordle::OptimalBook* book, bool candidate_only) {
    if (book && !candidate_only) {
        if (const auto move = book->lookup(candidates)) return move->word;
    }
    return solver.best(candidates, a.strategy, candidate_only).word;
}
Recommendation describe_move(const wordle::Solver& solver, const CandidateSet& candidates,
                             const wordle::BookMove& move) {
    Recommendation r;
    r.word = move.word;
    r.exact = true;
    r.expected_guesses = move.expected_guesses;
    std::array<std::size_t, 243> counts{};
    for (const auto id : candidates) {
        ++counts[wordle::feedback(r.word, solver.answers()[id])];
        r.possible_answer = r.possible_answer || r.word == solver.answers()[id];
    }
    const double n = static_cast<double>(candidates.size());
    for (const auto count : counts) if (count) {
        const double p = static_cast<double>(count) / n;
        r.entropy -= p * std::log2(p);
        r.expected_remaining += static_cast<double>(count * count) / n;
        r.worst_case = std::max(r.worst_case, static_cast<std::uint32_t>(count));
    }
    return r;
}
void explain(const wordle::Solver& solver, const Arguments& a, const CandidateSet& candidates,
             const wordle::OptimalBook* book, const std::string& requested = {}, bool expanded = false) {
    const auto next = requested.empty() ? choose(solver, a, candidates, book, a.candidate_only) : word(requested);
    if (!std::binary_search(solver.guesses().begin(), solver.guesses().end(), next))
        throw std::invalid_argument("guess is not in the supplied guess list: " + next);
    std::array<CandidateSet, 243> outcomes;
    std::map<std::string, std::vector<std::string>> families;
    for (const auto id : candidates) {
        const auto& answer = solver.answers()[id];
        outcomes[wordle::feedback(next, answer)].push_back(id);
        for (std::size_t i = 0; i < 5; ++i) {
            auto mask = answer;
            mask[i] = '?';
            families[mask].push_back(answer);
        }
    }
    std::vector<std::pair<std::size_t, std::uint8_t>> buckets;
    double expected = 0, entropy = 0;
    for (std::size_t pattern = 0; pattern < outcomes.size(); ++pattern) if (!outcomes[pattern].empty()) {
        const auto count = outcomes[pattern].size();
        const auto p = static_cast<double>(count) / static_cast<double>(candidates.size());
        expected += p * static_cast<double>(count);
        entropy -= p * std::log2(p);
        buckets.emplace_back(count, static_cast<std::uint8_t>(pattern));
    }
    std::sort(buckets.begin(), buckets.end(), [](const auto& x, const auto& y) { return x.first != y.first ? x.first > y.first : x.second < y.second; });
    std::vector<std::pair<std::string, std::vector<std::string>>> traps;
    for (auto& family : families) if (family.second.size() >= 3) traps.push_back(std::move(family));
    std::sort(traps.begin(), traps.end(), [](const auto& x, const auto& y) { return x.second.size() != y.second.size() ? x.second.size() > y.second.size() : x.first < y.first; });
    if (traps.size() > 3) traps.resize(3);
    if (a.json) {
        std::cout << "{\"word\":\"" << next << "\",\"expanded_dictionary\":" << (expanded ? "true" : "false") << ",\"candidate_count\":" << candidates.size()
                  << ",\"outcome_count\":" << buckets.size() << ",\"entropy_bits\":" << entropy
                  << ",\"expected_remaining\":" << expected << ",\"worst_case\":" << buckets.front().first << ",\"outcomes\":[";
        for (std::size_t i = 0; i < buckets.size(); ++i) {
            if (i) std::cout << ',';
            std::cout << "{\"feedback\":\"" << wordle::format_pattern(buckets[i].second) << "\",\"count\":" << buckets[i].first << '}';
        }
        std::cout << "],\"traps\":[";
        for (std::size_t i = 0; i < traps.size(); ++i) {
            if (i) std::cout << ',';
            std::cout << "{\"mask\":\"" << traps[i].first << "\",\"words\":[";
            for (std::size_t j = 0; j < traps[i].second.size(); ++j) { if (j) std::cout << ','; std::cout << '"' << traps[i].second[j] << '"'; }
            std::cout << "]}";
        }
        std::cout << "]}\n";
    } else {
        std::cout << "\n" << next << " splits " << candidates.size() << " answers into " << buckets.size() << " feedback groups.\n"
                  << "Expected left: " << expected << "  |  worst case: " << buckets.front().first << "  |  information: " << entropy << " bits\n"
                  << "\nThe largest branches:\n";
        for (std::size_t i = 0; i < std::min<std::size_t>(5, buckets.size()); ++i) {
            const auto [count, pattern] = buckets[i];
            std::cout << "  " << tiles(next, wordle::format_pattern(pattern)) << "  " << std::setw(4) << count << " left";
            const auto& examples = outcomes[pattern];
            std::cout << "  (";
            for (std::size_t j = 0; j < std::min<std::size_t>(3, examples.size()); ++j) {
                if (j) std::cout << ", ";
                std::cout << solver.answers()[examples[j]];
            }
            if (examples.size() > 3) std::cout << ", ...";
            std::cout << ")\n";
        }
        if (!traps.empty()) {
            std::cout << "\nOne-letter traps still in play:\n";
            for (const auto& [mask, words] : traps) {
                std::cout << "  " << mask << "  ";
                for (std::size_t i = 0; i < words.size(); ++i) { if (i) std::cout << ' '; std::cout << words[i]; }
                std::cout << '\n';
            }
        }
    }
}
void suggest(const wordle::Solver& solver, const Arguments& a, const CandidateSet& candidates,
             const wordle::OptimalBook* book, bool expanded = false) {
    const auto start = Clock::now();
    const auto ranking = a.top ? solver.rank(candidates, a.top, a.strategy, a.candidate_only) : std::vector<Recommendation>{};
    const auto move = book ? book->lookup(candidates) : std::nullopt;
    Recommendation recommendation;
    if (move) recommendation = describe_move(solver, candidates, *move);
    else if (!ranking.empty() && candidates.size() > std::max<std::size_t>(2, a.exact))
        recommendation = ranking.front();
    else recommendation = solver.best(candidates, a.strategy, a.candidate_only);
    const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    if (a.json) {
        std::cout << "{\"candidate_count\":" << candidates.size() << ",\"expanded_dictionary\":" << (expanded ? "true" : "false") << ",\"policy\":\""
                  << (move ? "optimal-book" : recommendation.exact ? "exact-endgame" : "heuristic") << "\",\"recommendation\":";
        print_recommendation(recommendation);
        std::cout << ",\"ranking\":[";
        for (std::size_t i = 0; i < ranking.size(); ++i) { if (i) std::cout << ','; print_recommendation(ranking[i]); }
        std::cout << "],\"candidates\":[";
        for (std::size_t i = 0; i < candidates.size(); ++i) { if (i) std::cout << ','; std::cout << '"' << solver.answers()[candidates[i]] << '"'; }
        std::cout << "],\"ranking_ms\":" << elapsed << ",\"matrix_ms\":" << solver.matrix_seconds() * 1000
                  << ",\"cache_loaded\":" << (solver.cache_loaded() ? "true" : "false") << "}\n";
    } else {
        if (expanded) std::cout << expanded_notice;
        std::cout << candidates.size() << " possible answers. Play " << recommendation.word;
        if (recommendation.exact) std::cout << " (" << recommendation.expected_guesses << " expected guesses to finish)";
        if (!ranking.empty()) {
            std::cout << "\n\nWORD    EXPECTED LEFT   WORST   ENTROPY\n";
            for (const auto& r : ranking)
                std::cout << r.word << (r.possible_answer ? " *  " : "    ") << std::setw(10) << r.expected_remaining
                          << std::setw(8) << r.worst_case << std::setw(10) << r.entropy << '\n';
            std::cout << "\n* possible answer\n";
        }
        std::cout << "\n" << (move ? "Book" : "Search") << ": " << elapsed << " ms\n";
    }
}
struct Game { std::string answer; bool solved = false; std::vector<Step> steps; };
using Policy = std::map<std::tuple<CandidateSet, bool, bool>, std::string>;
Game play(const wordle::Solver& solver, const Arguments& a, const std::string& answer, Policy& policy,
          const wordle::OptimalBook* book) {
    if (!std::binary_search(solver.answers().begin(), solver.answers().end(), answer) &&
        (!a.answers.empty() || !std::binary_search(solver.guesses().begin(), solver.guesses().end(), answer)))
        throw std::invalid_argument("answer is not in the supplied answer list: " + answer);
    if (a.history.size() > a.max_turns) throw std::invalid_argument("history exceeds the simulated turn limit");
    Puzzle puzzle(solver, a);
    for (const auto& step : puzzle.steps())
        if (wordle::feedback(step.guess, answer) != wordle::parse_pattern(step.pattern))
            throw std::invalid_argument("answer contradicts the supplied history");
    for (std::size_t turn = a.history.size(); turn < a.max_turns && !puzzle.solved(); ++turn) {
        const bool candidate_only = a.candidate_only || turn + 1 == a.max_turns;
        const auto& candidates = puzzle.candidates();
        const auto& current_solver = puzzle.solver();
        std::string guess;
        if (turn == a.history.size() && !a.first.empty()) {
            guess = a.first;
            if (candidate_only && std::none_of(candidates.begin(), candidates.end(), [&](auto id) { return current_solver.answers()[id] == guess; }))
                throw std::invalid_argument("forced guess must be a remaining answer in candidate-only mode or on the final turn");
        } else {
            const auto [it, inserted] = policy.try_emplace({candidates, candidate_only, puzzle.expanded()});
            if (inserted) it->second = choose(current_solver, a, candidates, puzzle.expanded() ? nullptr : book, candidate_only);
            guess = it->second;
        }
        const auto pattern = wordle::feedback(guess, answer);
        if (!puzzle.add(guess, pattern)) throw std::runtime_error("answer does not match the word list");
    }
    return {answer, puzzle.solved(), puzzle.steps()};
}
void print_game(const Game& game, bool json) {
    if (json) {
        std::cout << "{\"answer\":\"" << game.answer << "\",\"solved\":" << (game.solved ? "true" : "false") << ",\"turns\":" << game.steps.size() << ",\"steps\":[";
        for (std::size_t i = 0; i < game.steps.size(); ++i) {
            if (i) std::cout << ',';
            const auto& s = game.steps[i];
            std::cout << "{\"guess\":\"" << s.guess << "\",\"feedback\":\"" << s.pattern << "\",\"remaining\":" << s.remaining << '}';
        }
        std::cout << "]}\n";
    } else {
        for (const auto& s : game.steps) std::cout << tiles(s.guess, s.pattern) << "  " << s.remaining << " remaining\n";
        std::cout << (game.solved ? "Solved" : "Unsolved") << " in " << game.steps.size() << " turns.\n";
    }
}
int evaluate(const wordle::Solver& solver, const Arguments& a, const wordle::OptimalBook* book) {
    if (!a.history.empty()) throw std::invalid_argument("evaluate does not accept --history");
    const auto n = a.limit ? std::min(a.limit, solver.answers().size()) : solver.answers().size();
    Policy policy;
    std::vector<std::size_t> distribution(a.max_turns + 1);
    std::vector<std::string> failures;
    std::size_t total = 0, maximum = 0;
    const auto start = Clock::now();
    for (std::size_t i = 0; i < n; ++i) {
        const auto game = play(solver, a, solver.answers()[i], policy, book);
        if (game.solved) { ++distribution[game.steps.size()]; total += game.steps.size(); maximum = std::max(maximum, game.steps.size()); }
        else failures.push_back(game.answer);
        if (!a.json && (i + 1) % 250 == 0) std::cerr << "Evaluated " << i + 1 << '/' << n << " answers\n";
    }
    const auto seconds = std::chrono::duration<double>(Clock::now() - start).count();
    const auto solved = n - failures.size();
    const double mean = solved ? static_cast<double>(total) / static_cast<double>(solved) : 0;
    if (a.json) {
        std::cout << "{\"answers\":" << n << ",\"solved\":" << solved << ",\"failed\":" << failures.size()
                  << ",\"mean_guesses_solved\":" << mean << ",\"max_guesses_solved\":" << maximum
                  << ",\"seconds\":" << seconds << ",\"policy_states\":" << policy.size() << ",\"distribution\":{";
        for (std::size_t i = 1; i < distribution.size(); ++i) { if (i > 1) std::cout << ','; std::cout << '"' << i << "\":" << distribution[i]; }
        std::cout << "},\"failures\":[";
        for (std::size_t i = 0; i < failures.size(); ++i) { if (i) std::cout << ','; std::cout << '"' << failures[i] << '"'; }
        std::cout << "]}\n";
    } else {
        std::cout << solved << '/' << n << " solved; mean " << mean << ", max " << maximum << " guesses; " << seconds << " s\n";
        for (std::size_t i = 1; i < distribution.size(); ++i) std::cout << i << " guesses: " << distribution[i] << '\n';
        for (const auto& f : failures) std::cout << "Failed: " << f << '\n';
    }
    return failures.empty() ? 0 : 2;
}
void bench(const wordle::Solver& solver, const Arguments& a) {
    (void)solver.rank(solver.initial_candidates(), 1, a.strategy, a.candidate_only);
    if (a.json) std::cout << "{\"answers\":" << solver.answers().size() << ",\"guesses\":" << solver.guesses().size()
                          << ",\"matrix_bytes\":" << solver.matrix_bytes() << ",\"matrix_ms\":" << solver.matrix_seconds() * 1000
                          << ",\"cache_loaded\":" << (solver.cache_loaded() ? "true" : "false") << ",\"iterations\":" << a.iterations << ",\"ranking\":[";
    else std::cout << "Matrix: " << solver.matrix_bytes() << " bytes, " << solver.matrix_seconds() * 1000 << " ms (" << (solver.cache_loaded() ? "disk cache" : "built") << ")\n";
    const std::vector<std::size_t> sizes = {solver.answers().size(), std::min<std::size_t>(100, solver.answers().size()), std::min<std::size_t>(10, solver.answers().size())};
    for (std::size_t k = 0; k < sizes.size(); ++k) {
        CandidateSet candidates;
        for (std::size_t i = 0; i < sizes[k]; ++i) candidates.push_back(static_cast<std::uint32_t>(i * solver.answers().size() / sizes[k]));
        (void)solver.rank(candidates, 1, a.strategy, a.candidate_only);
        std::vector<double> samples;
        std::string best;
        for (std::size_t i = 0; i < a.iterations; ++i) {
            const auto start = Clock::now();
            const auto ranked = solver.rank(candidates, 1, a.strategy, a.candidate_only);
            samples.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
            best = ranked.front().word;
        }
        std::sort(samples.begin(), samples.end());
        const double median = samples[samples.size() / 2], p95 = samples[static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(samples.size()))) - 1];
        if (a.json) {
            if (k) std::cout << ',';
            std::cout << "{\"candidates\":" << sizes[k] << ",\"median_ms\":" << median << ",\"p95_ms\":" << p95 << ",\"best\":\"" << best << "\"}";
        } else std::cout << sizes[k] << " candidates: median " << median << " ms, p95 " << p95 << " ms; " << best << '\n';
    }
    if (a.json) std::cout << "]}\n";
}
void interactive(const wordle::Solver& solver, const Arguments& a, const wordle::OptimalBook* book) {
    Puzzle puzzle(solver, a);
    std::cout << "\nwordle solver\n"
              << "Play the suggested word in Wordle, then enter its tile colors here.\n"
              << "0 gray · 1 yellow · 2 green (B/Y/G works too)\n"
              << "Played another word? Type it first, or enter word + colors.\n"
              << "help · why · list · undo · reset · quit\n";
    if (puzzle.expanded()) std::cout << expanded_notice;
    std::string next;
    while (true) {
        if (puzzle.solved()) { std::cout << "Solved: " << puzzle.steps().back().guess << '\n'; break; }
        const auto& current_solver = puzzle.solver();
        const auto* current_book = puzzle.expanded() ? nullptr : book;
        if (next.empty()) next = choose(current_solver, a, puzzle.candidates(), current_book, a.candidate_only);
        std::cout << "\nPlay " << next << " (" << puzzle.candidates().size() << " left).\n"
                  << "Colors for " << next << ": " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) break;
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos) continue;
        line = line.substr(first, line.find_last_not_of(" \t\r") - first + 1);
        for (auto& c : line) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        if (line == "quit" || line == "q") break;
        if (line == "undo") { puzzle.undo(); next.clear(); continue; }
        if (line == "reset") { puzzle.reset(); next.clear(); continue; }
        if (line == "help" || line == "?" || line == "feedback") {
            std::cout << "Enter five colors for " << next << ": 0 gray, 1 yellow, 2 green.\n"
                      << "Example: 00101 means gray gray yellow gray yellow.\n"
                      << "For a different guess, type the word first or enter word + colors.\n";
            continue;
        }
        if (line == "why") {
            try { explain(current_solver, a, puzzle.candidates(), current_book, next, puzzle.expanded()); }
            catch (const std::invalid_argument&) {
                std::cout << "That word isn't in the bundled guess list. You can still enter its colors or choose another word.\n";
            }
            continue;
        }
        if (line == "list") {
            for (std::size_t i = 0; i < std::min<std::size_t>(40, puzzle.candidates().size()); ++i)
                std::cout << current_solver.answers()[puzzle.candidates()[i]] << ((i + 1) % 8 == 0 ? '\n' : ' ');
            std::cout << "\n" << puzzle.candidates().size() << " possible answers" << (puzzle.candidates().size() > 40 ? " (first 40 shown)" : "") << '\n';
            continue;
        }
        try {
            std::istringstream input(line);
            std::string guess = next, pattern, extra;
            input >> pattern;
            const bool has_word = static_cast<bool>(input >> extra);
            if (has_word) { guess = word(pattern); pattern = extra; if (input >> extra) throw std::invalid_argument("extra input"); }
            std::uint8_t code;
            try { code = wordle::parse_pattern(pattern); }
            catch (const std::invalid_argument&) {
                if (has_word) throw;
                next = word(pattern);
                std::cout << "Using " << next << ". Enter its five colors.\n";
                continue;
            }
            const bool was_expanded = puzzle.expanded();
            if (!puzzle.add(guess, code)) {
                std::cout << "No word matches all these clues. Check these colors or use 'undo' to fix an earlier guess.\n";
                continue;
            }
            if (!was_expanded && puzzle.expanded()) std::cout << expanded_notice;
            std::cout << tiles(guess, wordle::format_pattern(code)) << '\n';
            next.clear();
        } catch (const std::invalid_argument&) {
            std::cout << "Enter five colors for " << next << " (0 gray, 1 yellow, 2 green), or type 'help'.\n";
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        const auto a = parse(argc, argv);
        std::cout << std::fixed << std::setprecision(a.json ? 6 : 2);
        if (a.command == "help") { std::cout << help; return 0; }
        if (a.command == "version") { std::cout << "wordle 1.0.0\n"; return 0; }
        if (a.command == "feedback") {
            const auto pattern = wordle::format_pattern(wordle::feedback(word(a.positional[0]), word(a.positional[1])));
            if (a.json) std::cout << "{\"feedback\":\"" << pattern << "\"}\n";
            else std::cout << pattern << '\n';
            return 0;
        }
        if (a.command != "interactive" && a.command != "suggest" && a.command != "why" && a.command != "solve" && a.command != "evaluate" && a.command != "bench")
            throw std::invalid_argument("unknown command: " + a.command);
        auto answers = a.answers.empty() ? wordle::data::answers() : read_words(a.answers);
        auto guesses = a.guesses.empty() ? (a.answers.empty() ? wordle::data::guesses() : answers) : read_words(a.guesses);
        const wordle::Solver solver(std::move(answers), std::move(guesses), {a.threads, a.cache, a.exact, true});
        const wordle::OptimalBook book(solver.answers(), solver.guesses());
        const auto* active_book = !a.dynamic && !a.candidate_only && a.strategy == wordle::Strategy::Expected && book.compatible() ? &book : nullptr;
        if (!a.first.empty() && !std::binary_search(solver.guesses().begin(), solver.guesses().end(), a.first))
            throw std::invalid_argument("opening guess is not in the supplied guess list");
        if (a.command == "suggest" || a.command == "why") {
            Puzzle puzzle(solver, a);
            const auto* current_book = puzzle.expanded() ? nullptr : active_book;
            if (a.command == "suggest") suggest(puzzle.solver(), a, puzzle.candidates(), current_book, puzzle.expanded());
            else {
                if (puzzle.expanded() && !a.json) std::cout << expanded_notice;
                explain(puzzle.solver(), a, puzzle.candidates(), current_book, a.positional.empty() ? "" : a.positional[0], puzzle.expanded());
            }
        }
        else if (a.command == "solve") { Policy policy; const auto game = play(solver, a, word(a.positional[0]), policy, active_book); print_game(game, a.json); return game.solved ? 0 : 2; }
        else if (a.command == "evaluate") return evaluate(solver, a, active_book);
        else if (a.command == "bench") bench(solver, a);
        else interactive(solver, a, active_book);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "wordle: " << e.what() << '\n';
        return 1;
    }
}
