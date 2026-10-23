#include "solver.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <class Function>
void rejects(Function function, const std::string& message) {
    try { function(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error(message);
}

// Independent reference: consume individual answer positions, rather than counts.
std::string reference_feedback(const std::string& guess, const std::string& answer) {
    std::array<bool, 5> consumed{};
    std::string result(5, '0');
    for (std::size_t i = 0; i < 5; ++i) {
        if (guess[i] == answer[i]) { result[i] = '2'; consumed[i] = true; }
    }
    for (std::size_t i = 0; i < 5; ++i) {
        if (result[i] == '2') continue;
        for (std::size_t j = 0; j < 5; ++j) {
            if (!consumed[j] && guess[i] == answer[j]) {
                result[i] = '1'; consumed[j] = true; break;
            }
        }
    }
    return result;
}

std::vector<std::string> miniature_words() {
    std::vector<std::string> result;
    for (int n = 0; n < 243; ++n) {
        std::string word(5, 'a');
        int digits = n;
        for (auto& c : word) { c += digits % 3; digits /= 3; }
        result.push_back(word);
    }
    return result;
}

void test_feedback() {
    const std::array<std::array<std::string, 3>, 6> fixtures{{
        {{"allee", "apple", "21002"}}, {{"eerie", "serve", "02202"}},
        {{"sassy", "assay", "11202"}}, {{"level", "hello", "12001"}},
        {{"bobby", "abbey", "10202"}}, {{"civic", "civic", "22222"}}
    }};
    for (const auto& test : fixtures) {
        check(wordle::format_pattern(wordle::feedback(test[0], test[1])) == test[2], "Duplicate-letter fixture failed: " + test[0]);
    }
    const auto words = miniature_words();
    for (const auto& guess : words) for (const auto& answer : words) {
        check(wordle::format_pattern(wordle::feedback(guess, answer)) == reference_feedback(guess, answer),
              "Exhaustive duplicate-letter feedback mismatch");
    }
    for (unsigned pattern = 0; pattern < 243; ++pattern) {
        check(wordle::parse_pattern(wordle::format_pattern(static_cast<std::uint8_t>(pattern))) == pattern,
              "Pattern round trip failed");
    }
    check(wordle::parse_pattern("BYGXY") == wordle::parse_pattern("01201"), "Letter pattern parser failed");
    check(wordle::feedback("APPLE", "apple") == 242, "ASCII case normalization failed");
    rejects([] { wordle::feedback("four", "apple"); }, "Short word accepted");
    rejects([] { wordle::feedback("app!e", "apple"); }, "Nonletter word accepted");
    rejects([] { wordle::parse_pattern("0123x"); }, "Invalid pattern accepted");
    rejects([] { wordle::parse_pattern("012"); }, "Short pattern accepted");
    rejects([] { wordle::format_pattern(255); }, "Out-of-range pattern accepted");
}

void test_filter_and_ranking() {
    const auto words = miniature_words();
    wordle::Solver solver(words, words, {.threads = 4, .exact_threshold = 0});
    const auto all = solver.initial_candidates();
    for (const auto& guess : words) {
        std::array<wordle::CandidateSet, 243> expected;
        for (std::uint32_t i = 0; i < solver.answers().size(); ++i) {
            const auto pattern = wordle::parse_pattern(reference_feedback(guess, solver.answers()[i]));
            expected[pattern].push_back(i);
        }
        for (unsigned pattern = 0; pattern < 243; ++pattern) {
            check(solver.filter(all, guess, static_cast<std::uint8_t>(pattern)) == expected[pattern],
                  "Stored matrix disagrees with independent scorer");
        }
    }
    for (const auto& answer : solver.answers()) {
        const auto candidates = solver.filter(all, "abcde", wordle::feedback("abcde", answer));
        check(std::find_if(candidates.begin(), candidates.end(), [&](auto id) { return solver.answers()[id] == answer; }) != candidates.end(),
              "True answer was eliminated by an out-of-dictionary guess");
    }
    const wordle::CandidateSet state{0, 1, 2, 10, 30, 70};
    const auto rankings = solver.rank(state, solver.guesses().size());
    check(rankings.size() == solver.guesses().size(), "Ranking omitted guesses");
    for (const auto& recommendation : rankings) {
        std::map<std::string, unsigned> counts;
        for (const auto id : state) ++counts[reference_feedback(recommendation.word, solver.answers()[id])];
        double entropy = 0, squares = 0;
        unsigned largest = 0;
        for (const auto& [pattern, count] : counts) {
            (void)pattern;
            const double probability = static_cast<double>(count) / static_cast<double>(state.size());
            entropy -= probability * std::log2(probability);
            squares += count * count;
            largest = std::max(largest, count);
        }
        check(std::abs(entropy - recommendation.entropy) < 1e-12, "Entropy mismatch");
        check(std::abs(squares / static_cast<double>(state.size()) - recommendation.expected_remaining) < 1e-12, "Expected remaining mismatch");
        check(largest == recommendation.worst_case, "Worst-case bucket mismatch");
    }
    const auto candidate_rankings = solver.rank(state, 999, wordle::Strategy::Entropy, true);
    check(candidate_rankings.size() == state.size(), "Candidate-only count mismatch");
    for (const auto& recommendation : candidate_rankings) check(recommendation.possible_answer, "Candidate-only returned a probe");
    for (const auto strategy : {wordle::Strategy::Expected, wordle::Strategy::Entropy, wordle::Strategy::Minimax}) {
        check(solver.best({17}, strategy).word == solver.answers()[17], "One-candidate policy guessed a probe");
        check(solver.rank(state, 0, strategy).empty(), "Zero limit returned rankings");
    }
    check(solver.filter({2, 1, 1, 0}, "aaaaa", 242) == wordle::CandidateSet{0}, "Candidate canonicalization failed");
    rejects([&] { solver.best({}); }, "Empty candidate set accepted");
    rejects([&] { solver.rank({999}); }, "Invalid candidate ID accepted");
    rejects([&] { solver.filter(all, "aaaaa", 255); }, "Invalid feedback code accepted");
    wordle::Solver sorted({"APPLE", "apple", "abbey"}, {"CIGAR"}, {.threads = 1});
    check(sorted.answers() == std::vector<std::string>{"abbey", "apple"}, "Dictionary normalization failed");
    check(sorted.guesses() == std::vector<std::string>{"abbey", "apple", "cigar"}, "Answers not merged into guesses");
    rejects([] { wordle::Solver({}, {}); }, "Empty answer dictionary accepted");
    rejects([] { wordle::Solver({"12345"}, {}); }, "Invalid answer dictionary accepted");
}

struct BruteResult { unsigned cost; std::string guess; };

BruteResult brute_policy(const std::vector<std::string>& state, const std::vector<std::string>& allowed,
                         bool candidate_only, std::map<std::vector<std::string>, BruteResult>& memo) {
    if (state.size() == 1) return {1, state.front()};
    if (memo.contains(state)) return memo.at(state);
    BruteResult best{static_cast<unsigned>(state.size() * (state.size() + 1) / 2), state.front()};
    const auto& choices = candidate_only ? state : allowed;
    for (const auto& guess : choices) {
        std::map<std::string, std::vector<std::string>> buckets;
        for (const auto& answer : state) {
            const auto pattern = reference_feedback(guess, answer);
            if (pattern != "22222") buckets[pattern].push_back(answer);
        }
        if (buckets.size() == 1 && buckets.begin()->second.size() == state.size()) continue;
        unsigned cost = static_cast<unsigned>(state.size());
        for (const auto& [pattern, bucket] : buckets) {
            (void)pattern;
            cost += brute_policy(bucket, allowed, candidate_only, memo).cost;
        }
        const bool possible = std::find(state.begin(), state.end(), guess) != state.end();
        const bool best_possible = std::find(state.begin(), state.end(), best.guess) != state.end();
        if (cost < best.cost || (cost == best.cost && (possible != best_possible ? possible : guess < best.guess))) best = {cost, guess};
    }
    memo.emplace(state, best);
    return best;
}

void test_exact_search() {
    const std::vector<std::string> answers{"cigar", "cider", "civic", "crane", "caper", "cater", "catch", "cacao"};
    const std::vector<std::string> guesses{"arise", "slate", "clamp", "match", "trial", "cigar", "cider", "civic", "crane", "caper", "cater", "catch", "cacao"};
    wordle::Solver solver(answers, guesses, {.threads = 1, .exact_threshold = 8});
    wordle::Solver direct(answers, guesses, {.threads = 1, .exact_threshold = 8, .defer_matrix = true});
    for (const bool candidate_only : {false, true}) {
        std::map<std::vector<std::string>, BruteResult> memo;
        for (unsigned mask = 1; mask < (1U << answers.size()); ++mask) {
            wordle::CandidateSet candidates;
            std::vector<std::string> state;
            for (std::uint32_t i = 0; i < solver.answers().size(); ++i) if ((mask & (1U << i)) != 0) {
                candidates.push_back(i); state.push_back(solver.answers()[i]);
            }
            const auto expected = brute_policy(state, solver.guesses(), candidate_only, memo);
            for (const auto* engine : {&solver, &direct}) {
                const auto actual = engine->best(candidates, wordle::Strategy::Expected, candidate_only);
                check(actual.exact, "Small-state recommendation not marked exact");
                check(actual.word == expected.guess, "Exact policy chose a different guess from brute reference");
                check(std::abs(actual.expected_guesses - static_cast<double>(expected.cost) / static_cast<double>(state.size())) < 1e-12,
                      "Exact expected guess count differs from brute reference");
            }
        }
    }
    check(direct.matrix_seconds() == 0, "Small-state exact search built the full matrix");
}

void test_cache() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("wordle-solver-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto path = directory / "nested" / "patterns.bin";
    const wordle::Options options{.threads = 2, .cache_path = path.string()};
    const std::vector<std::string> words{"apple", "abbey", "cigar", "cider"};
    {
        wordle::Solver first(words, {}, options);
        check(!first.cache_loaded(), "Initial cache unexpectedly loaded");
        check(first.matrix_bytes() == 16, "Matrix byte count mismatch");
        wordle::Solver second(words, {}, options);
        check(second.cache_loaded(), "Valid matrix cache not loaded");
        check(first.rank(first.initial_candidates(), 4).front().word == second.rank(second.initial_candidates(), 4).front().word,
              "Cached recommendation differs");
    }
    {
        std::fstream corrupt(path, std::ios::in | std::ios::out | std::ios::binary);
        corrupt.seekp(40); corrupt.put(static_cast<char>(255));
    }
    wordle::Solver repaired(words, {}, options);
    check(!repaired.cache_loaded(), "Corrupt cache accepted");
    wordle::Solver valid_again(words, {}, options);
    check(valid_again.cache_loaded(), "Rebuilt cache is invalid");
    wordle::Solver different({"apple", "abbey", "cigar", "civic"}, {}, options);
    check(!different.cache_loaded(), "Wrong dictionary cache accepted");
    {
        std::ofstream truncated(path, std::ios::binary | std::ios::trunc);
        truncated << "WLDMAT02";
    }
    wordle::Solver repaired_truncation(words, {}, options);
    check(!repaired_truncation.cache_loaded(), "Truncated cache accepted");
    const auto lazy_path = directory / "lazy.bin";
    wordle::Solver lazy(words, {}, {.threads = 2, .cache_path = lazy_path.string(), .defer_matrix = true});
    check(!lazy.cache_loaded() && lazy.matrix_seconds() == 0 && !std::filesystem::exists(lazy_path),
          "Deferred constructor built a matrix");
    check(lazy.best({0}).word == lazy.answers()[0] && lazy.matrix_seconds() == 0 && !lazy.cache_loaded(),
          "Deferred singleton best initialized the matrix");
    check(lazy.best({0, 1}).expected_guesses == 1.5 && lazy.matrix_seconds() == 0 && !lazy.cache_loaded(),
          "Deferred two-candidate best initialized the matrix");
    const auto filtered = lazy.filter(lazy.initial_candidates(), "apple", wordle::feedback("apple", "abbey"));
    check(!filtered.empty() && !std::filesystem::exists(lazy_path) && lazy.matrix_seconds() == 0,
          "Deferred filtering initialized the matrix");
    std::array<std::string, 2> concurrent_results;
    {
        std::jthread first([&] { concurrent_results[0] = lazy.rank(lazy.initial_candidates(), 1).front().word; });
        std::jthread second([&] { concurrent_results[1] = lazy.rank(lazy.initial_candidates(), 1).front().word; });
    }
    check(concurrent_results[0] == concurrent_results[1] && !std::filesystem::exists(lazy_path) && lazy.matrix_seconds() == 0,
          "Small deferred ranking built a full matrix");
    check(concurrent_results[0] == repaired_truncation.rank(repaired_truncation.initial_candidates(), 1).front().word,
          "Direct feedback ranking differs from stored-matrix ranking");
    const auto mini = miniature_words();
    wordle::Solver direct(mini, mini, {.threads = 2, .defer_matrix = true});
    wordle::Solver stored(mini, mini, {.threads = 2});
    for (const auto count : {3U, 8U, 10U, 100U, 128U}) {
        wordle::CandidateSet state(count);
        std::iota(state.begin(), state.end(), 0U);
        for (const auto strategy : {wordle::Strategy::Expected, wordle::Strategy::Entropy, wordle::Strategy::Minimax}) {
            const auto expected = stored.rank(state, 30, strategy);
            const auto actual = direct.rank(state, 30, strategy);
            check(expected.size() == actual.size(), "Direct rank result size mismatch");
            for (std::size_t i = 0; i < expected.size(); ++i) {
                check(expected[i].word == actual[i].word && expected[i].expected_remaining == actual[i].expected_remaining &&
                      std::abs(expected[i].entropy - actual[i].entropy) < 1e-12 && expected[i].worst_case == actual[i].worst_case,
                      "Direct rank scores differ from stored-matrix ranking");
            }
        }
    }
    check(direct.matrix_seconds() == 0, "A state of at most 128 answers built a full matrix");
    {
        std::jthread first([&] { concurrent_results[0] = direct.rank(direct.initial_candidates(), 1).front().word; });
        std::jthread second([&] { concurrent_results[1] = direct.rank(direct.initial_candidates(), 1).front().word; });
    }
    check(concurrent_results[0] == concurrent_results[1] && direct.matrix_seconds() > 0,
          "Concurrent deferred matrix initialization failed");
    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

void test_memory_guard() {
    std::vector<std::string> words;
    words.reserve(23200);
    for (unsigned value = 0; value < 23200; ++value) {
        unsigned digits = value;
        std::string word(5, 'a');
        for (auto& c : word) { c += digits % 26; digits /= 26; }
        words.push_back(word);
    }
    try { wordle::Solver oversized(words, {}); }
    catch (const std::length_error&) { return; }
    throw std::runtime_error("Matrix larger than 512 MiB was accepted");
}

} // namespace

int main() {
    try {
        test_feedback();
        test_filter_and_ranking();
        test_exact_search();
        test_cache();
        test_memory_guard();
        std::cout << "All solver tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Solver test failure: " << error.what() << '\n';
        return 1;
    }
}
