#pragma once

#include "solver.hpp"

#include <optional>
#include <string>
#include <vector>

namespace wordle {

struct BookMove {
    std::string word;
    // Average guesses left, including this one. Each answer is equally likely.
    double expected_guesses = 0;
};

// Alex Selby's strategy for the original word lists. IDs match Solver's sorted
// answers. A different dictionary or an off-book state returns no move.
class OptimalBook {
public:
    OptimalBook(std::vector<std::string> answers, std::vector<std::string> guesses);

    bool compatible() const noexcept;
    std::optional<BookMove> lookup(const CandidateSet& candidates) const;

private:
    bool compatible_ = false;
};

} // namespace wordle
