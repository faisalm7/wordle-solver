#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wordle {

using CandidateSet = std::vector<std::uint32_t>;
enum class Strategy { Expected, Entropy, Minimax };

// 0 = gray, 1 = yellow, 2 = green.
std::uint8_t feedback(std::string_view guess, std::string_view answer);
std::uint8_t parse_pattern(std::string_view pattern);
std::string format_pattern(std::uint8_t pattern);

struct Options {
    std::size_t threads = 0;
    std::string cache_path;
    std::size_t exact_threshold = 8;
    // Build the table only when a position has more than 128 answers.
    bool defer_matrix = false;
};

struct Recommendation {
    std::string word;
    double entropy = 0;
    double expected_remaining = 0;
    std::uint32_t worst_case = 0;
    bool possible_answer = false;
    double expected_guesses = 0;
    bool exact = false;
};

class Solver {
public:
    Solver(std::vector<std::string> answers, std::vector<std::string> guesses,
           Options options = {});
    ~Solver();
    const std::vector<std::string>& answers() const;
    const std::vector<std::string>& guesses() const;
    CandidateSet initial_candidates() const;
    CandidateSet filter(const CandidateSet&, std::string_view guess,
                        std::uint8_t pattern) const;
    std::vector<Recommendation> rank(const CandidateSet&, std::size_t limit = 10,
                                     Strategy strategy = Strategy::Expected,
                                     bool candidate_only = false) const;
    Recommendation best(const CandidateSet&, Strategy strategy = Strategy::Expected,
                        bool candidate_only = false) const;
    bool cache_loaded() const;
    double matrix_seconds() const;
    // Table size, even if it hasn't been built yet.
    std::size_t matrix_bytes() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wordle
