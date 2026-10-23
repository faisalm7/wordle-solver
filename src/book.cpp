#include "book.hpp"
#include "book_data.hpp"

#include <algorithm>
#include <cstdint>

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
static_assert(initial_node < generated_book::kNodes.size(), "Book is missing the opening move");

BookMove move_for(const generated_book::Node& node) {
    return {std::string(generated_book::kGuesses[node.guess]),
            static_cast<double>(node.total_turns) / static_cast<double>(node.size)};
}

std::optional<BookMove> find_state(const CandidateSet& candidates) {
    if (candidates.empty()) return std::nullopt;
    // One answer left: play it, even if we went off book.
    if (candidates.size() == 1) {
        return BookMove{std::string(generated_book::kAnswers[candidates.front()]), 1.0};
    }
    // All answers means the opening. No need to hash 2,315 IDs.
    if (candidates.size() == generated_book::kAnswers.size()) {
        return move_for(generated_book::kNodes[initial_node]);
    }
    const auto hash = state_hash(candidates);
    const auto& nodes = generated_book::kNodes;
    auto node = std::lower_bound(nodes.begin(), nodes.end(), hash,
                                 [](const auto& entry, std::uint64_t key) { return entry.hash < key; });
    for (; node != nodes.end() && node->hash == hash; ++node) {
        // Check the IDs too, in case two states hash alike.
        if (node->size != candidates.size()) continue;
        const auto ids = generated_book::kCandidateIds.begin() + node->ids_offset;
        if (!std::equal(candidates.begin(), candidates.end(), ids)) continue;
        return move_for(*node);
    }
    return std::nullopt;
}

} // namespace

OptimalBook::OptimalBook(std::vector<std::string> answers, std::vector<std::string> guesses) {
    if (!normalize_words(answers) || !std::ranges::equal(answers, generated_book::kAnswers)) return;
    if (!normalize_words(guesses)) return;
    if (std::ranges::equal(guesses, generated_book::kGuesses)) { compatible_ = true; return; }
    // Solver adds answers to the guess list, so do the same here.
    guesses.insert(guesses.end(), answers.begin(), answers.end());
    std::sort(guesses.begin(), guesses.end());
    guesses.erase(std::unique(guesses.begin(), guesses.end()), guesses.end());
    compatible_ = std::ranges::equal(guesses, generated_book::kGuesses);
}

bool OptimalBook::compatible() const noexcept { return compatible_; }

std::optional<BookMove> OptimalBook::lookup(const CandidateSet& candidates) const {
    if (!compatible_ || candidates.empty()) return std::nullopt;
    bool canonical = true;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (candidates[i] >= generated_book::kAnswers.size()) return std::nullopt;
        if (i != 0 && candidates[i - 1] >= candidates[i]) canonical = false;
    }
    // Solver keeps IDs sorted and unique, so usually we can skip the copy.
    if (canonical) return find_state(candidates);
    auto normalized = candidates;
    std::sort(normalized.begin(), normalized.end());
    normalized.erase(std::unique(normalized.begin(), normalized.end()), normalized.end());
    return find_state(normalized);
}

} // namespace wordle
