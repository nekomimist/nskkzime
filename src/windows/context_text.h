#pragma once
#include <windows.h>
#include <msctf.h>
#include <string>

namespace nskkzime::windows {
enum class InputPermission { Allowed, Restricted, Unknown };
// Caller holds a TSF read lock. Mixed or missing scopes are Unknown.
InputPermission input_permission(ITfContext*, ITfRange*, TfEditCookie);
struct SurroundingText {
    InputPermission permission = InputPermission::Unknown;
    bool available = false;
    bool contains_unknown_scope = false; // Never grants permission to persist text.
    std::u32string before, after;
    bool operator==(const SurroundingText&) const = default;
};
// Excludes the target range. At most 40 Unicode scalar values on either side.
// Check target and both surrounding ranges before GetText. Explicit opt-in
// permits missing scopes, never a known protected scope. Retain actual scope.
SurroundingText surrounding_text(ITfContext*, ITfRange* target, TfEditCookie, bool allow_unknown_scope = false);

// Opt-in, thread-local development preview; no files or logs. Cleared on
// composition end, deactivation, or disabling the preview.
void enable_context_preview(bool);
bool context_preview_enabled();
void publish_context_preview(const void* owner, SurroundingText);
void clear_context_preview(const void* owner);
std::wstring context_preview_status();
}
