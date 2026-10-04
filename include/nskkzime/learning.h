#pragma once
#include "engine.h"
#include <string_view>

namespace nskkzime {
inline constexpr std::size_t learning_max_bytes = 16 * 1024 * 1024;
enum class DictionaryAction { Learn, Forget };
// Binary envelope with length-prefixed UTF-8 strings; shared by disk and IPC.
struct LearningRequest {
    std::string client; // 32 hexadecimal digits, unique to a producer lifetime.
    std::uint64_t sequence = 0;
    LearningEvent event;
    DictionaryAction action = DictionaryAction::Learn;
};
void validate_learning(const LearningRequest&);
std::string encode_learning(const LearningRequest&);
LearningRequest decode_learning(std::string_view);
class UserDictionary final : public Dictionary {
public:
    bool apply(const LearningRequest&); // false for an already acknowledged sequence
    std::vector<Candidate> lookup(const LookupKey&, std::u32string_view okuri = {}) const override;
    std::string serialize() const;
    std::vector<LearningEvent> entries() const; // Key order, then candidate priority.
    std::string export_skk() const; // UTF-8; throws if a key cannot be represented losslessly.
    static UserDictionary parse(std::string_view);
    std::uint64_t revision() const { return revision_; }
private:
    struct Key {
        LookupKey lookup;
        std::u32string okuri;
        auto operator<=>(const Key&) const = default;
    };
    std::map<Key, std::vector<Candidate>> entries_;
    std::map<std::string, std::uint64_t> clients_;
    std::uint64_t revision_ = 0;
};
} // namespace nskkzime
