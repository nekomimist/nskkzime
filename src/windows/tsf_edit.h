#pragma once
#include <msctf.h>
#include <functional>
#include <string>
#include <string_view>

namespace nskkzime::windows {
std::wstring utf16(std::u32string_view);
HRESULT request_edit(ITfContext*, TfClientId, DWORD flags, std::function<HRESULT(TfEditCookie)>);
bool range_matches(ITfRange*, TfEditCookie, std::wstring_view);
bool selection_at_end(ITfContext*, TfEditCookie, ITfRange*);
HRESULT select_end(ITfContext*, TfEditCookie, ITfRange*);
} // namespace nskkzime::windows
