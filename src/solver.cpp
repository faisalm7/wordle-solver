#include "solver.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace wordle {
namespace {

constexpr std::size_t pattern_count = 243;
constexpr std::uint8_t solved_pattern = 242;
constexpr std::size_t matrix_limit = 512ULL * 1024 * 1024;
constexpr std::uint64_t fnv_offset = 14695981039346656037ULL;
constexpr std::uint64_t fnv_prime = 1099511628211ULL;
using Clock = std::chrono::steady_clock;

std::string normalize(std::string_view value) {
    if (value.size() != 5) throw std::invalid_argument("Words must have exactly five ASCII letters");
    std::string result(value);
    for (char& c : result) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c < 'a' || c > 'z') throw std::invalid_argument("Words must have exactly five ASCII letters");
    }
    return result;
}

void normalize_words(std::vector<std::string>& words) {
    for (auto& word : words) word = normalize(word);
    if (!std::is_sorted(words.begin(), words.end())) std::sort(words.begin(), words.end());
    words.erase(std::unique(words.begin(), words.end()), words.end());
}

using Letters = std::array<std::uint8_t, 5>;

Letters letters(std::string_view word) {
    Letters result{};
    for (std::size_t i = 0; i < 5; ++i) result[i] = static_cast<std::uint8_t>(word[i] - 'a');
    return result;
}

std::uint8_t feedback_unchecked(const Letters& guess, const Letters& answer) {
    std::uint8_t remaining[26]{};
    std::uint8_t result[5]{};
    for (std::size_t i = 0; i < 5; ++i) {
        if (guess[i] == answer[i]) result[i] = 2;
        else ++remaining[answer[i]];
    }
    for (std::size_t i = 0; i < 5; ++i) {
        if (result[i] != 2 && remaining[guess[i]] != 0) {
            result[i] = 1;
            --remaining[guess[i]];
        }
    }
    return static_cast<std::uint8_t>(81 * result[0] + 27 * result[1] +
                                      9 * result[2] + 3 * result[3] + result[4]);
}

std::uint64_t hash_bytes(const std::uint8_t* bytes, std::size_t size,
                         std::uint64_t hash = fnv_offset) {
    for (std::size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * fnv_prime;
    return hash;
}

void write_u64(std::ostream& output, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) output.put(static_cast<char>((value >> (i * 8)) & 255));
}

std::uint64_t read_u64(std::istream& input) {
    std::uint64_t result = 0;
    for (unsigned i = 0; i < 8; ++i) {
        const int value = input.get();
        if (value == std::char_traits<char>::eof()) throw std::runtime_error("Truncated matrix cache");
        result |= static_cast<std::uint64_t>(static_cast<unsigned char>(value)) << (8 * i);
    }
    return result;
}

std::size_t thread_count(std::size_t requested, std::size_t tasks) {
    const auto available = requested == 0 ? std::max(1U, std::thread::hardware_concurrency()) : requested;
    return std::max<std::size_t>(1, std::min({available, tasks, std::size_t{64}}));
}

template <class Function>
void parallel_blocks(std::size_t tasks, std::size_t count, Function function) {
    if (count <= 1) { function(0, tasks); return; }
    std::vector<std::jthread> workers;
    workers.reserve(count);
    for (std::size_t worker = 0; worker < count; ++worker) {
        const auto begin = tasks * worker / count;
        const auto end = tasks * (worker + 1) / count;
        workers.emplace_back([=, &function] { function(begin, end); });
    }
}

CandidateSet canonical_candidates(const CandidateSet& input, std::size_t answer_count) {
    CandidateSet result(input);
    for (const auto id : result) {
        if (id >= answer_count) throw std::invalid_argument("Candidate ID is outside the answer dictionary");
    }
    if (!std::is_sorted(result.begin(), result.end())) std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

bool better(const Recommendation& lhs, const Recommendation& rhs, Strategy strategy) {
    switch (strategy) {
    case Strategy::Expected:
        if (lhs.expected_remaining != rhs.expected_remaining) return lhs.expected_remaining < rhs.expected_remaining;
        break;
    case Strategy::Entropy:
        // Ignore rounding noise when two guesses split the words the same way.
        if (const auto left = std::llround(lhs.entropy * 1e12), right = std::llround(rhs.entropy * 1e12);
            left != right) return left > right;
        break;
    case Strategy::Minimax:
        if (lhs.worst_case != rhs.worst_case) return lhs.worst_case < rhs.worst_case;
        break;
    }
    if (lhs.possible_answer != rhs.possible_answer) return lhs.possible_answer;
    return lhs.word < rhs.word;
}

} // namespace

std::uint8_t feedback(std::string_view guess, std::string_view answer) {
    return feedback_unchecked(letters(normalize(guess)), letters(normalize(answer)));
}

std::uint8_t parse_pattern(std::string_view pattern) {
    if (pattern.size() != 5) throw std::invalid_argument("Feedback must contain five digits (0/1/2) or letters (B/Y/G)");
    unsigned value = 0;
    for (const char c : pattern) {
        unsigned digit;
        if (c == '0' || c == 'b' || c == 'B' || c == 'x' || c == 'X' || c == '.') digit = 0;
        else if (c == '1' || c == 'y' || c == 'Y') digit = 1;
        else if (c == '2' || c == 'g' || c == 'G') digit = 2;
        else throw std::invalid_argument("Feedback accepts only 0/1/2 or B/Y/G");
        value = value * 3 + digit;
    }
    return static_cast<std::uint8_t>(value);
}

std::string format_pattern(std::uint8_t pattern) {
    if (pattern >= pattern_count) throw std::invalid_argument("Feedback code must be between 0 and 242");
    std::string result(5, '0');
    for (int i = 4; i >= 0; --i) { result[static_cast<std::size_t>(i)] += pattern % 3; pattern /= 3; }
    return result;
}

struct Solver::Impl {
    std::vector<std::string> answer_words;
    std::vector<std::string> guess_words;
    std::vector<Letters> answer_letters;
    std::vector<Letters> guess_letters;
    std::vector<std::uint32_t> answer_to_guess;
    std::unordered_map<std::string, std::uint32_t> guess_ids;
    std::vector<std::uint8_t> matrix;
    std::vector<double> count_log_count;
    Options options;
    std::atomic<bool> loaded{false};
    std::atomic<bool> matrix_ready{false};
    std::atomic<double> build_seconds{0};
    std::once_flag matrix_once;

    void ensure_matrix() {
        std::call_once(matrix_once, [&] {
            const auto start = Clock::now();
            matrix.resize(guess_words.size() * answer_words.size());
            loaded = load_cache();
            if (!loaded) {
                parallel_blocks(guess_words.size(), thread_count(options.threads, guess_words.size()),
                                [&](std::size_t begin, std::size_t end) {
                    for (auto guess = begin; guess < end; ++guess) {
                        auto* values = matrix.data() + guess * answer_words.size();
                        for (std::size_t answer = 0; answer < answer_words.size(); ++answer) {
                            values[answer] = feedback_unchecked(guess_letters[guess], answer_letters[answer]);
                        }
                    }
                });
                save_cache();
            }
            build_seconds = std::chrono::duration<double>(Clock::now() - start).count();
            matrix_ready.store(true, std::memory_order_release);
        });
    }

    std::uint64_t fingerprint() const {
        std::uint64_t result = fnv_offset;
        for (const auto& word : answer_words) result = hash_bytes(reinterpret_cast<const std::uint8_t*>(word.data()), 5, result);
        const std::uint8_t separator = 255;
        result = hash_bytes(&separator, 1, result);
        for (const auto& word : guess_words) result = hash_bytes(reinterpret_cast<const std::uint8_t*>(word.data()), 5, result);
        return result;
    }

    bool load_cache() {
        if (options.cache_path.empty()) return false;
        try {
            std::ifstream input(options.cache_path, std::ios::binary | std::ios::ate);
            if (!input || input.tellg() != static_cast<std::streamoff>(40 + matrix.size())) return false;
            input.seekg(0);
            char magic[8]{};
            input.read(magic, 8);
            if (std::string_view(magic, 8) != "WLDMAT02") return false;
            if (read_u64(input) != guess_words.size() || read_u64(input) != answer_words.size() ||
                read_u64(input) != fingerprint()) return false;
            const auto checksum = read_u64(input);
            input.read(reinterpret_cast<char*>(matrix.data()), static_cast<std::streamsize>(matrix.size()));
            if (!input || hash_bytes(matrix.data(), matrix.size()) != checksum) return false;
            return std::all_of(matrix.begin(), matrix.end(), [](auto p) { return p < pattern_count; });
        } catch (const std::exception&) { return false; }
    }

    void save_cache() const {
        if (options.cache_path.empty()) return;
        const std::filesystem::path target(options.cache_path);
        std::error_code error;
        if (target.has_parent_path()) std::filesystem::create_directories(target.parent_path(), error);
        if (error) return;
        static std::atomic<std::uint64_t> sequence{0};
        auto temporary = target;
        temporary += ".tmp." + std::to_string(Clock::now().time_since_epoch().count()) + "." + std::to_string(sequence++);
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) return;
            output.write("WLDMAT02", 8);
            write_u64(output, guess_words.size());
            write_u64(output, answer_words.size());
            write_u64(output, fingerprint());
            write_u64(output, hash_bytes(matrix.data(), matrix.size()));
            output.write(reinterpret_cast<const char*>(matrix.data()), static_cast<std::streamsize>(matrix.size()));
            output.close();
            if (!output) { std::filesystem::remove(temporary, error); return; }
        }
        std::filesystem::rename(temporary, target, error);
        if (error) std::filesystem::remove(temporary, error);
    }

    const std::uint8_t* row(std::uint32_t guess) const { return matrix.data() + static_cast<std::size_t>(guess) * answer_words.size(); }

    Recommendation score(std::uint32_t guess, const CandidateSet& candidates,
                         const std::vector<bool>& possible, double log_n,
                         bool matrix_available) const {
        std::uint32_t histogram[pattern_count]{};
        const auto* values = matrix_available ? row(guess) : nullptr;
        std::uint64_t sum_squares = 0;
        double sum_logs = 0;
        std::uint32_t largest = 0;
        if (candidates.size() <= 128) {
            // Small positions need only the nonempty buckets.
            std::uint8_t touched[128];
            std::size_t touched_count = 0;
            for (const auto answer : candidates) {
                const auto pattern = values ? values[answer] : feedback_unchecked(guess_letters[guess], answer_letters[answer]);
                if (histogram[pattern]++ == 0) touched[touched_count++] = pattern;
            }
            for (std::size_t i = 0; i < touched_count; ++i) {
                const auto count = histogram[touched[i]];
                sum_squares += static_cast<std::uint64_t>(count) * count;
                sum_logs += count_log_count[count];
                largest = std::max(largest, count);
            }
        } else {
            for (const auto answer : candidates) ++histogram[values[answer]];
            for (const auto count : histogram) {
                sum_squares += static_cast<std::uint64_t>(count) * count;
                sum_logs += count_log_count[count];
                largest = std::max(largest, count);
            }
        }
        const double n = static_cast<double>(candidates.size());
        return {guess_words[guess], log_n - sum_logs / n,
                static_cast<double>(sum_squares) / n, largest, possible[guess], 0, false};
    }

    struct StateHash {
        std::size_t operator()(const CandidateSet& candidates) const {
            std::size_t value = candidates.size();
            for (const auto id : candidates) value = value * 1000003U + id + 1;
            return value;
        }
    };

    struct ExactResult { std::uint32_t cost; std::uint32_t guess; };

    ExactResult exact(const CandidateSet& candidates, bool candidate_only, bool matrix_available,
                      std::unordered_map<CandidateSet, ExactResult, StateHash>& memo) const {
        if (candidates.size() == 1) return {1, answer_to_guess[candidates.front()]};
        if (candidates.size() == 2) return {3, answer_to_guess[candidates.front()]};
        if (const auto found = memo.find(candidates); found != memo.end()) return found->second;
        const auto n = static_cast<std::uint32_t>(candidates.size());
        // Start with the cost of trying each answer in order.
        ExactResult result{n * (n + 1) / 2, answer_to_guess[candidates.front()]};
        // Try answers first, then other guesses. This also settles ties.
        std::vector<std::uint32_t> possible;
        for (const auto id : candidates) possible.push_back(answer_to_guess[id]);
        auto is_possible = [&](auto guess) { return std::binary_search(possible.begin(), possible.end(), guess); };
        std::unordered_set<std::string> partitions_seen;
        const auto choice_count = possible.size() + (candidate_only ? 0 : guess_words.size());
        for (std::size_t choice = 0; choice < choice_count; ++choice) {
            const auto guess = choice < possible.size() ? possible[choice]
                : static_cast<std::uint32_t>(choice - possible.size());
            if (choice >= possible.size() && is_possible(guess)) continue;
            const auto* values = matrix_available ? row(guess) : nullptr;
            // Exact search handles at most 12 answers.
            std::array<CandidateSet, 12> buckets;
            std::string partition_key;
            partition_key.reserve(candidates.size() + 1);
            std::array<std::uint8_t, pattern_count> labels{};
            std::uint8_t next_label = 1;
            std::size_t largest = 0;
            for (const auto answer : candidates) {
                const auto pattern = values ? values[answer] : feedback_unchecked(guess_letters[guess], answer_letters[answer]);
                if (pattern == solved_pattern) partition_key.push_back('\0');
                else {
                    if (labels[pattern] == 0) labels[pattern] = next_label++;
                    partition_key.push_back(static_cast<char>(labels[pattern]));
                    auto& bucket = buckets[labels[pattern] - 1];
                    bucket.push_back(answer);
                    largest = std::max(largest, bucket.size());
                }
            }
            if (largest == candidates.size()) continue;
            if (!partitions_seen.insert(partition_key).second) continue;
            std::uint32_t cost = n;
            for (const auto& bucket : buckets) {
                if (bucket.empty()) continue;
                cost += exact(bucket, candidate_only, matrix_available, memo).cost;
                if (cost > result.cost) break;
            }
            const bool prefer = is_possible(guess) != is_possible(result.guess)
                ? is_possible(guess) : guess < result.guess;
            if (cost < result.cost || (cost == result.cost && prefer)) result = {cost, guess};
            // Can't do better than solving one now and the rest next turn.
            if (result.cost == 2 * n - 1) break;
        }
        memo.emplace(candidates, result);
        return result;
    }
};

Solver::Solver(std::vector<std::string> answers, std::vector<std::string> guesses, Options options)
    : impl_(std::make_unique<Impl>()) {
    normalize_words(answers);
    if (answers.empty()) throw std::invalid_argument("Answer dictionary cannot be empty");
    guesses.insert(guesses.end(), answers.begin(), answers.end());
    normalize_words(guesses);
    if (guesses.size() > matrix_limit / answers.size()) {
        throw std::length_error("Feedback matrix exceeds the 512 MiB memory limit");
    }
    if (options.exact_threshold > 12) throw std::invalid_argument("Exact search threshold must not exceed 12 candidates");
    auto& data = *impl_;
    data.answer_words = std::move(answers);
    data.guess_words = std::move(guesses);
    data.options = std::move(options);
    for (const auto& word : data.answer_words) data.answer_letters.push_back(letters(word));
    for (std::uint32_t id = 0; id < data.guess_words.size(); ++id) {
        data.guess_letters.push_back(letters(data.guess_words[id]));
        data.guess_ids.emplace(data.guess_words[id], id);
    }
    for (const auto& word : data.answer_words) data.answer_to_guess.push_back(data.guess_ids.at(word));
    data.count_log_count.resize(data.answer_words.size() + 1, 0);
    for (std::size_t i = 1; i < data.count_log_count.size(); ++i) data.count_log_count[i] = static_cast<double>(i) * std::log2(static_cast<double>(i));
    if (!data.options.defer_matrix) data.ensure_matrix();
}

Solver::~Solver() = default;

const std::vector<std::string>& Solver::answers() const { return impl_->answer_words; }
const std::vector<std::string>& Solver::guesses() const { return impl_->guess_words; }

CandidateSet Solver::initial_candidates() const {
    CandidateSet result(answers().size());
    std::iota(result.begin(), result.end(), 0U);
    return result;
}

CandidateSet Solver::filter(const CandidateSet& input, std::string_view guess, std::uint8_t pattern) const {
    if (pattern >= pattern_count) throw std::invalid_argument("Feedback code must be between 0 and 242");
    const auto candidates = canonical_candidates(input, answers().size());
    const auto normalized = normalize(guess);
    CandidateSet result;
    if (const auto found = impl_->guess_ids.find(normalized);
        found != impl_->guess_ids.end() && impl_->matrix_ready.load(std::memory_order_acquire)) {
        const auto* row = impl_->row(found->second);
        for (const auto id : candidates) if (row[id] == pattern) result.push_back(id);
    } else {
        const auto packed = letters(normalized);
        for (const auto id : candidates) if (feedback_unchecked(packed, impl_->answer_letters[id]) == pattern) result.push_back(id);
    }
    return result;
}

std::vector<Recommendation> Solver::rank(const CandidateSet& input, std::size_t limit,
                                         Strategy strategy, bool candidate_only) const {
    const auto candidates = canonical_candidates(input, answers().size());
    if (candidates.empty()) throw std::invalid_argument("Cannot rank an empty candidate set");
    if (limit == 0) return {};
    // Small positions don't need the full table.
    if (candidates.size() > 128) impl_->ensure_matrix();
    const bool matrix_available = impl_->matrix_ready.load(std::memory_order_acquire);
    std::vector<bool> possible(guesses().size(), false);
    for (const auto id : candidates) possible[impl_->answer_to_guess[id]] = true;
    std::vector<std::uint32_t> choices;
    if (candidate_only) for (const auto id : candidates) choices.push_back(impl_->answer_to_guess[id]);
    else { choices.resize(guesses().size()); std::iota(choices.begin(), choices.end(), 0U); }
    std::vector<Recommendation> results(choices.size());
    const auto work = choices.size() * candidates.size();
    // Scoring without the table is more work, so start workers sooner.
    const std::size_t parallel_threshold = matrix_available ? 1000000 : 100000;
    const auto threads = work >= parallel_threshold ? thread_count(impl_->options.threads, choices.size()) : 1;
    const double log_n = std::log2(static_cast<double>(candidates.size()));
    parallel_blocks(choices.size(), threads, [&](std::size_t begin, std::size_t end) {
        for (auto i = begin; i < end; ++i) results[i] = impl_->score(choices[i], candidates, possible, log_n, matrix_available);
    });
    limit = std::min(limit, results.size());
    std::partial_sort(results.begin(), results.begin() + static_cast<std::ptrdiff_t>(limit), results.end(),
                      [strategy](const auto& a, const auto& b) { return better(a, b, strategy); });
    results.resize(limit);
    return results;
}

Recommendation Solver::best(const CandidateSet& input, Strategy strategy, bool candidate_only) const {
    const auto candidates = canonical_candidates(input, answers().size());
    if (candidates.empty()) throw std::invalid_argument("Cannot choose from an empty candidate set");
    if (candidates.size() <= 2) {
        // With one or two answers left, just guess one.
        return {answers()[candidates.front()], candidates.size() == 1 ? 0.0 : 1.0,
                1, 1, true, candidates.size() == 1 ? 1.0 : 1.5, true};
    }
    if (impl_->options.exact_threshold != 0 && candidates.size() <= impl_->options.exact_threshold) {
        const bool matrix_available = impl_->matrix_ready.load(std::memory_order_acquire);
        std::unordered_map<CandidateSet, Impl::ExactResult, Impl::StateHash> memo;
        const auto result = impl_->exact(candidates, candidate_only, matrix_available, memo);
        std::vector<bool> possible(guesses().size(), false);
        for (const auto id : candidates) possible[impl_->answer_to_guess[id]] = true;
        auto recommendation = impl_->score(result.guess, candidates, possible, std::log2(static_cast<double>(candidates.size())), matrix_available);
        recommendation.expected_guesses = static_cast<double>(result.cost) / static_cast<double>(candidates.size());
        recommendation.exact = true;
        return recommendation;
    }
    return rank(candidates, 1, strategy, candidate_only).front();
}

bool Solver::cache_loaded() const { return impl_->loaded.load(); }
double Solver::matrix_seconds() const { return impl_->build_seconds.load(); }
std::size_t Solver::matrix_bytes() const { return answers().size() * guesses().size(); }

} // namespace wordle
