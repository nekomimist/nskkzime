#include "context_text.h"
#include <initguid.h>
#include <inputscope.h>
#include <wrl/client.h>
#include <array>
#include <optional>
#include <string_view>
#include <utility>

namespace nskkzime::windows {
using Microsoft::WRL::ComPtr;
InputPermission input_permission(ITfContext* context, ITfRange* range, TfEditCookie cookie) {
    if (!context || !range) return InputPermission::Unknown;
    ComPtr<ITfReadOnlyProperty> property;
    if (FAILED(context->GetAppProperty(GUID_PROP_INPUTSCOPE, &property)) || !property) return InputPermission::Unknown;
    VARIANT value; VariantInit(&value);
    const auto hr = property->GetValue(cookie, range, &value);
    auto result = InputPermission::Unknown;
    if (hr == S_OK && value.vt == VT_UNKNOWN && value.punkVal) {
        ComPtr<ITfInputScope> scope;
        if (SUCCEEDED(value.punkVal->QueryInterface(IID_PPV_ARGS(&scope)))) {
            InputScope* values = nullptr; UINT count = 0;
            if (scope->GetInputScopes(&values, &count) == S_OK && count && values) {
                result = InputPermission::Allowed;
                for (UINT i = 0; i < count; ++i) {
                    if (values[i] == IS_PASSWORD || values[i] == IS_PRIVATE || values[i] == IS_NUMERIC_PASSWORD ||
                        values[i] == IS_NUMERIC_PIN || values[i] == IS_ALPHANUMERIC_PIN || values[i] == IS_ALPHANUMERIC_PIN_SET)
                        result = InputPermission::Restricted;
                }
            }
            CoTaskMemFree(values);
        }
    }
    VariantClear(&value); return result;
}
namespace {
constexpr std::size_t limit = 40;
constexpr LONG units = static_cast<LONG>(limit * 2 + 1);
bool high(wchar_t ch) { return ch >= 0xd800 && ch <= 0xdbff; }
bool low(wchar_t ch) { return ch >= 0xdc00 && ch <= 0xdfff; }
std::optional<std::u32string> decode(std::wstring_view text, bool before, LONG shifted) {
    // The bounded window may bisect a surrogate pair at its far edge only.
    if (shifted == units && !text.empty()) {
        if (before && low(text.front())) text.remove_prefix(1);
        if (!before && high(text.back())) text.remove_suffix(1);
    }
    std::u32string result;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char32_t ch = text[i];
        if (high(text[i])) {
            if (i + 1 == text.size() || !low(text[i + 1])) return {};
            ch = 0x10000 + ((ch - 0xd800) << 10) + (text[++i] - 0xdc00);
        } else if (low(text[i])) return {};
        // Embedded objects / NUL are not usable model context.
        if (!ch || ch == 0xfffc) return {};
        result += ch;
    }
    if (result.size() > limit) {
        if (before) result.erase(0, result.size() - limit);
        else result.resize(limit);
    }
    return result;
}
struct Preview { bool enabled = false; const void* owner = nullptr; SurroundingText text; };
thread_local Preview preview;
std::wstring display(std::u32string_view text) {
    std::wstring result;
    for (auto ch : text) {
        if (ch == U'\r') result += L"\\r";
        else if (ch == U'\n') result += L"\\n";
        else if (ch == U'\t') result += L"\\t";
        else if (ch <= 0xffff) result += static_cast<wchar_t>(ch);
        else { ch -= 0x10000; result += static_cast<wchar_t>(0xd800 + (ch >> 10)); result += static_cast<wchar_t>(0xdc00 + (ch & 0x3ff)); }
    }
    return result;
}
}
SurroundingText surrounding_text(ITfContext* context, ITfRange* target, TfEditCookie cookie) {
    SurroundingText result;
    result.permission = input_permission(context, target, cookie);
    if (result.permission != InputPermission::Allowed) return result;
    std::array<ComPtr<ITfRange>, 2> ranges;
    std::array<LONG, 2> lengths{};
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        auto& range = ranges[i];
        if (FAILED(target->Clone(&range)) || !range ||
            FAILED(range->Collapse(cookie, i == 0 ? TF_ANCHOR_START : TF_ANCHOR_END))) return result;
        LONG shifted = 0;
        const auto hr = i == 0 ? range->ShiftStart(cookie, -units, &shifted, nullptr) : range->ShiftEnd(cookie, units, &shifted, nullptr);
        if (FAILED(hr) || (i == 0 && (shifted > 0 || shifted < -units)) || (i == 1 && (shifted < 0 || shifted > units))) return result;
        lengths[i] = i == 0 ? -shifted : shifted;
        // Empty ranges at document/region boundaries need no text access.
        if (lengths[i] && input_permission(context, range.Get(), cookie) != InputPermission::Allowed) return result;
    }
    std::array<std::u32string, 2> text;
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        if (!lengths[i]) continue;
        std::array<wchar_t, static_cast<std::size_t>(units)> buffer{}; ULONG count = 0;
        if (ranges[i]->GetText(cookie, 0, buffer.data(), static_cast<ULONG>(lengths[i]), &count) != S_OK ||
            count != static_cast<ULONG>(lengths[i])) return result;
        auto decoded = decode({buffer.data(), count}, i == 0, lengths[i]);
        if (!decoded) return result;
        text[i] = std::move(*decoded);
    }
    result.before = std::move(text[0]); result.after = std::move(text[1]); result.available = true;
    return result;
}
void enable_context_preview(bool enable) { preview = {}; preview.enabled = enable; }
bool context_preview_enabled() { return preview.enabled; }
void publish_context_preview(const void* owner, SurroundingText text) {
    if (preview.enabled) { preview.owner = owner; preview.text = std::move(text); }
}
void clear_context_preview(const void* owner) {
    if (preview.owner == owner) { preview.owner = nullptr; preview.text = {}; }
}
std::wstring context_preview_status() {
    if (!preview.enabled) return L"文脈確認: オフ（本文を取得しません）";
    if (!preview.owner) return L"文脈確認: オン。入力欄でSpace変換すると前後の文脈を表示します。";
    const auto& text = preview.text;
    if (text.permission == InputPermission::Restricted) return L"文脈確認: 保護された入力欄のため取得しません。";
    if (text.permission == InputPermission::Unknown) return L"文脈確認: 入力属性が不明なため取得しません。";
    if (!text.available) return L"文脈確認: 周辺範囲を安全に取得できません。文脈なしで動作します。";
    return L"文脈確認: 前 " + std::to_wstring(text.before.size()) + L"文字／後 " + std::to_wstring(text.after.size()) +
        L"文字\r\n前: 「" + display(text.before) + L"」\r\n後: 「" + display(text.after) + L"」";
}
}
