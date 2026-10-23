#pragma once

#include "wordle/solver.hpp"

#include <optional>
#include <string>
#include <vector>

namespace wordle {

struct BookMove {
    std::string word;
    // Mean additional guesses, including this move, under uniform answers.
    double expected_guesses = 0;
};

// Alex Selby's optimal unrestricted policy for the original Wordle word lists.
// Candidate IDs use the sorted, normalized answer dictionary, as in Solver.
// Other dictionaries and states outside the policy safely miss the book.
class OptimalBook {
public:
    OptimalBook(std::vector<std::string> answers, std::vector<std::string> guesses);

    bool compatible() const noexcept;
    std::optional<BookMove> lookup(const CandidateSet& candidates) const;

private:
    bool compatible_ = false;
};

} // namespace wordle
