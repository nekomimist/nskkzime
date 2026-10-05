#include "nskkzime/compiled_dictionary.h"
#include <functional>
#include <iostream>
#include <random>
#include <stdexcept>
using namespace nskkzime;
namespace {
void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
template <class F> void rejects(F run) {
    bool failed = false;
    try {
        run();
    } catch (const std::exception &) {
        failed = true;
    }
    check(failed, "invalid image accepted");
}
void same(const std::vector<Candidate> &left, const std::vector<Candidate> &right) {
    check(left.size() == right.size(), "candidate count");
    for (std::size_t i = 0; i < left.size(); ++i)
        check(left[i].text == right[i].text && left[i].annotation == right[i].annotation && left[i].id == right[i].id &&
                  left[i].source == right[i].source && left[i].learning == right[i].learning,
              "candidate metadata/order");
}
auto open(std::string value) {
    auto owner = std::make_shared<std::string>(std::move(value));
    return std::make_shared<CompiledDictionary>(*owner, owner);
}
void put(std::string &value, std::size_t offset, std::uint32_t number) {
    for (unsigned i = 0; i < 4; ++i)
        value[offset + i] = static_cast<char>(number >> (i * 8));
}
std::uint32_t word(const std::string &value, std::size_t offset) {
    std::uint32_t result = 0;
    for (unsigned i = 0; i < 4; ++i)
        result |= static_cast<std::uint32_t>(static_cast<unsigned char>(value[offset + i])) << (i * 8);
    return result;
}
} // namespace
int main() {
    int failed = 0;
    const auto source = SkkDictionary::parse(
        U";; okuri-ari entries.\nかk /書;文章/描/[く/描;絵/]/[け/欠/]/\n;; okuri-nasi entries.\nかな "
        U"/仮名;注釈/かな/\nかな /カナ/\nio /(concat \"I\\057O\")/\nよし /𠮷/吉/\nbroken\nempty /(eval x)/\n");
    const auto image = CompiledDictionary::compile(*source);
    const std::vector<std::pair<const char *, std::function<void()>>> cases = {
        {"candidate-order-okuri-id-annotation-unicode",
         [&] {
             const auto packed = open(image);
             source->visit_scopes([&](const auto &key, auto suffix, const auto &) {
                 same(source->lookup(key, suffix), packed->lookup(key, suffix));
                 same(source->lookup(key, U"こ"), packed->lookup(key, U"こ"));
             });
             for (const LookupKey key :
                  {LookupKey{U""}, LookupKey{U"not-present"}, LookupKey{U"か"}, LookupKey{U"かな", 'k'}})
                 same(source->lookup(key), packed->lookup(key));
             check(packed->report().lines == source->report().lines &&
                       packed->report().entries == source->report().entries &&
                       packed->report().candidates == source->report().candidates &&
                       packed->report().malformed_lines == source->report().malformed_lines &&
                       packed->report().unsupported_candidates == source->report().unsupported_candidates &&
                       packed->report().diagnostics == source->report().diagnostics,
                   "diagnostics changed");
         }},
        {"empty-and-owner-lifetime",
         [] {
             const auto packed = open(CompiledDictionary::compile(*SkkDictionary::parse(U";; empty\n")));
             check(packed->lookup({U"かな"}).empty(), "empty image lookup");
             auto owner =
                 std::make_shared<std::string>(CompiledDictionary::compile(*SkkDictionary::parse(U"かな /仮名/\n")));
             std::weak_ptr<std::string> weak = owner;
             {
                 CompiledDictionary dictionary(*owner, owner);
                 owner.reset();
                 check(!weak.expired() && dictionary.lookup({U"かな"})[0].text == U"仮名", "owner not retained");
             }
             check(weak.expired(), "owner leaked");
         }},
        {"reject-truncated-header-count-offset-order-unicode",
         [&] {
             for (const std::size_t size : {0u, 7u, 47u})
                 rejects([&] { open(image.substr(0, size)); });
             for (const std::size_t offset :
                  {8u, 12u, 16u, 20u, 24u, 28u, 36u, 48u, 52u, 56u, 60u, 64u, 68u, 72u, 76u}) {
                 auto damaged = image;
                 put(damaged, offset, UINT32_MAX);
                 rejects([&] { open(damaged); });
             }
             auto surrogate = image;
             put(surrogate, word(image, 48), 0xd800);
             rejects([&] { open(surrogate); });
             auto duplicate = image;
             duplicate.replace(48 + 32, 32, image.substr(48, 32));
             rejects([&] { open(duplicate); });
             rejects([&] { CompiledDictionary dictionary(image, {}); });
         }},
        {"bounded-mutation-fuzz",
         [&] {
             std::mt19937 random(20261006);
             for (int trial = 0; trial < 2000; ++trial) {
                 auto mutated = image;
                 const auto index = random() % mutated.size();
                 mutated[index] ^= static_cast<char>(1u << (random() % 8));
                 try {
                     const auto packed = open(std::move(mutated));
                     for (const auto &key : {LookupKey{U"かな"}, LookupKey{U"か", 'k'}, LookupKey{U"よし"}}) {
                         const auto list = packed->lookup(key, U"く");
                         check(list.size() <= source->report().candidates, "unbounded candidate result");
                         for (const auto &item : list)
                             check(from_utf8(utf8(item.text)) == item.text, "invalid result Unicode");
                     }
                 } catch (const std::invalid_argument &) {
                 }
             }
         }},
        {"byte-parser-keeps-encoding-detection", [] {
             const auto value = SkkDictionary::parse_bytes("\xef\xbb\xbf" + utf8(U"かな /仮名/\n"));
             const auto packed = open(CompiledDictionary::compile(*value));
             same(value->lookup({U"かな"}), packed->lookup({U"かな"}));
             check(packed->report().encoding == DictionaryEncoding::Utf8, "encoding lost");
         }}};
    for (const auto &[name, run] : cases)
        try {
            run();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception &error) {
            ++failed;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    return failed ? 1 : 0;
}
