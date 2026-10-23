#include "book.hpp"
#include "book_data.hpp"

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace wordle {
namespace {

constexpr std::uint64_t fnv_offset = 14695981039346656037ULL;
constexpr std::uint64_t fnv_prime = 1099511628211ULL;

bool normalize_words(std::vector<std::string>& words) {
    for (auto& word : words) {
        if (word.size() != 5) return false;
        for (char& c : word) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (c < 'a' || c > 'z') return false;
        }
    }
    if (!std::is_sorted(words.begin(), words.end())) std::sort(words.begin(), words.end());
    words.erase(std::unique(words.begin(), words.end()), words.end());
    return true;
}

template <class Dictionary>
bool same_dictionary(const std::vector<std::string>& words, const Dictionary& reference) {
    return words.size() == reference.size() &&
           std::equal(words.begin(), words.end(), reference.begin());
}

std::uint64_t state_hash(const CandidateSet& candidates) {
    std::uint64_t hash = fnv_offset;
    for (const auto id : candidates) {
        hash = (hash ^ (id & 255U)) * fnv_prime;
        hash = (hash ^ (id >> 8U)) * fnv_prime;
    }
    return hash;
}

constexpr std::size_t root_index() {
    for (std::size_t i = 0; i < generated_book::kNodes.size(); ++i) {
        if (generated_book::kNodes[i].size == generated_book::kAnswers.size()) return i;
    }
    return generated_book::kNodes.size();
}

constexpr auto initial_node = root_index();
static_assert(initial_node < generated_book::kNodes.size(), "Optimal book must contain the initial state");

BookMove move_for(const generated_book::Node& node) {
    return {std::string(generated_book::kGuesses[node.guess]),
            static_cast<double>(node.total_turns) / static_cast<double>(node.size)};
}

std::optional<BookMove> find_state(const CandidateSet& candidates) {
    if (candidates.empty()) return std::nullopt;
    // Every singleton has the same optimal action, even after an off-book move.
    if (candidates.size() == 1) {
        return BookMove{std::string(generated_book::kAnswers[candidates.front()]), 1.0};
    }
    // A canonical, valid state with all IDs is necessarily the initial state.
    // Skip serial hashing of 2,315 IDs for this common opening lookup.
    if (candidates.size() == generated_book::kAnswers.size()) {
        return move_for(generated_book::kNodes[initial_node]);
    }
    const auto hash = state_hash(candidates);
    const auto& nodes = generated_book::kNodes;
    auto node = std::lower_bound(nodes.begin(), nodes.end(), hash,
                                 [](const auto& entry, std::uint64_t key) { return entry.hash < key; });
    for (; node != nodes.end() && node->hash == hash; ++node) {
        // The complete key comparison makes hash collisions harmless.
        if (node->size != candidates.size()) continue;
        const auto ids = generated_book::kCandidateIds.begin() + node->ids_offset;
        if (!std::equal(candidates.begin(), candidates.end(), ids)) continue;
        return move_for(*node);
    }
    return std::nullopt;
}

} // namespace

OptimalBook::OptimalBook(std::vector<std::string> answers, std::vector<std::string> guesses) {
    if (!normalize_words(answers) || !same_dictionary(answers, generated_book::kAnswers)) return;
    if (!normalize_words(guesses)) return;
    if (same_dictionary(guesses, generated_book::kGuesses)) { compatible_ = true; return; }
    // Solver always makes all answers legal guesses, including custom input.
    guesses.insert(guesses.end(), answers.begin(), answers.end());
    if (!normalize_words(guesses)) return;
    compatible_ = same_dictionary(guesses, generated_book::kGuesses);
}

bool OptimalBook::compatible() const noexcept { return compatible_; }

std::optional<BookMove> OptimalBook::lookup(const CandidateSet& candidates) const {
    if (!compatible_ || candidates.empty()) return std::nullopt;
    bool canonical = true;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (candidates[i] >= generated_book::kAnswers.size()) return std::nullopt;
        if (i != 0 && candidates[i - 1] >= candidates[i]) canonical = false;
    }
    // Normal solver states take this allocation-free path.
    if (canonical) return find_state(candidates);
    auto normalized = candidates;
    std::sort(normalized.begin(), normalized.end());
    normalized.erase(std::unique(normalized.begin(), normalized.end()), normalized.end());
    return find_state(normalized);
}

} // namespace wordle
