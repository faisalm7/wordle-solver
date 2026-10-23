#include "book.hpp"
#include "words.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct PolicyResult {
    std::size_t answers = 0;
    std::uint32_t total_turns = 0;
    std::size_t maximum_turns = 0;
};

// Check every answer path without relying on the tree generator.
PolicyResult validate_policy(const wordle::OptimalBook& book,
                             const std::vector<std::string>& answers,
                             const std::vector<std::string>& guesses,
                             const wordle::CandidateSet& state,
                             std::size_t depth = 1) {
    check(depth <= 6, "Policy needs more than six guesses");
    const auto move = book.lookup(state);
    check(move.has_value(), "Reachable policy state is absent from the book");
    check(std::binary_search(guesses.begin(), guesses.end(), move->word), "Book played an illegal guess");
    std::array<wordle::CandidateSet, 243> partitions;
    for (const auto id : state) {
        const auto pattern = wordle::feedback(move->word, answers[id]);
        partitions[pattern].push_back(id);
    }
    PolicyResult result{partitions[242].size(), static_cast<std::uint32_t>(state.size()), 1};
    check(partitions[242].size() <= 1, "More than one answer produced all-green feedback");
    if (!partitions[242].empty()) {
        check(answers[partitions[242].front()] == move->word, "Solved branch contains the wrong answer");
    }
    for (std::size_t pattern = 0; pattern < 242; ++pattern) {
        const auto& child = partitions[pattern];
        if (child.empty()) continue;
        check(child.size() < state.size(), "Book guess makes no progress");
        const auto subtree = validate_policy(book, answers, guesses, child, depth + 1);
        result.answers += subtree.answers;
        result.total_turns += subtree.total_turns;
        result.maximum_turns = std::max(result.maximum_turns, subtree.maximum_turns + 1);
    }
    check(result.answers == state.size(), "Policy lost an answer");
    check(std::abs(move->expected_guesses - static_cast<double>(result.total_turns) /
                   static_cast<double>(state.size())) < 1e-12,
          "Book expected guess count disagrees with reconstructed policy");
    return result;
}

void test_policy() {
    auto answers = wordle::data::answers();
    auto guesses = wordle::data::guesses();
    std::sort(answers.begin(), answers.end());
    std::sort(guesses.begin(), guesses.end());
    const wordle::OptimalBook book(answers, guesses);
    check(book.compatible(), "Original dictionaries did not enable the book");
    wordle::CandidateSet initial(answers.size());
    std::iota(initial.begin(), initial.end(), 0U);
    const auto opening = book.lookup(initial);
    check(opening.has_value() && opening->word == "salet", "Optimal opening is not salet");
    const auto result = validate_policy(book, answers, guesses, initial);
    check(result.answers == 2315, "Policy did not solve all 2,315 original answers");
    check(result.total_turns == 7920, "Optimal policy total is not 7,920 guesses");
    check(result.maximum_turns == 5, "Historical strategy maximum is not five guesses");
    check(!book.lookup({}), "Empty candidate set produced a recommendation");
    check(!book.lookup({static_cast<std::uint32_t>(answers.size())}), "Invalid candidate ID produced a recommendation");
    check(!book.lookup({0, 1, 999999}), "Invalid candidate ID among valid IDs was accepted");
    for (std::uint32_t id = 0; id < answers.size(); ++id) {
        const auto single = book.lookup({id});
        check(single.has_value() && single->word == answers[id] && single->expected_guesses == 1,
              "Singleton state did not recommend its answer");
    }
    std::reverse(initial.begin(), initial.end());
    initial.push_back(initial.front());
    const auto reordered = book.lookup(initial);
    check(reordered.has_value() && reordered->word == opening->word &&
          reordered->expected_guesses == opening->expected_guesses,
          "Candidate canonicalization changed the recommendation");
}

void test_dictionary_guard() {
    const auto answers = wordle::data::answers();
    const auto guesses = wordle::data::guesses();
    auto missing = answers;
    missing.pop_back();
    const wordle::OptimalBook missing_book(missing, guesses);
    check(!missing_book.compatible() && !missing_book.lookup({0}), "Incomplete answer corpus enabled the book");
    auto different = answers;
    different.front() = "zzzzz";
    check(!wordle::OptimalBook(different, guesses).compatible(), "Changed answer corpus enabled the book");
    auto expanded = guesses;
    expanded.push_back("zzzzz");
    check(!wordle::OptimalBook(answers, expanded).compatible(), "Expanded guess corpus enabled the book");
    auto invalid = guesses;
    invalid.front() = "a!ple";
    check(!wordle::OptimalBook(answers, invalid).compatible(), "Malformed guess corpus enabled the book");
    check(!wordle::OptimalBook({}, {}).compatible(), "Empty dictionaries enabled the book");
    auto normalized_answers = answers;
    auto normalized_guesses = guesses;
    std::reverse(normalized_answers.begin(), normalized_answers.end());
    for (auto& word : normalized_answers) for (auto& c : word) c = static_cast<char>(c - 'a' + 'A');
    normalized_answers.push_back(answers.front());
    normalized_guesses.push_back(guesses.front());
    check(wordle::OptimalBook(normalized_answers, normalized_guesses).compatible(),
          "Equivalent normalized dictionaries disabled the book");
    auto probes_only = guesses;
    probes_only.erase(std::remove_if(probes_only.begin(), probes_only.end(), [&](const auto& word) {
        return std::binary_search(answers.begin(), answers.end(), word);
    }), probes_only.end());
    check(wordle::OptimalBook(answers, probes_only).compatible(), "Automatic answer merging disagrees with Solver");
}

} // namespace

int main() {
    try {
        test_policy();
        test_dictionary_guard();
        std::cout << "Optimal book: all 2,315 answers solved in 7,920 total guesses; every policy state verified\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Book test failure: " << error.what() << '\n';
        return 1;
    }
}
