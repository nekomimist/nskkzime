#pragma once
#include "module.h"

namespace nskkzime::windows {
inline constexpr GUID input_attribute_guid = {0x2fbe38c2, 0x3e73, 0x43fa, {0x92, 0x0c, 0xce, 0xbe, 0x30, 0xdb, 0xe3, 0xa4}};
inline constexpr GUID converted_attribute_guid = {0x61b73655, 0x32ad, 0x4d1a, {0x87, 0xac, 0xf4, 0xab, 0xb7, 0x1f, 0xd1, 0x65}};
HRESULT enum_display_attributes(IEnumTfDisplayAttributeInfo** output) noexcept;
HRESULT get_display_attribute(REFGUID guid, ITfDisplayAttributeInfo** output) noexcept;
HRESULT display_attribute_atoms(TfGuidAtom& input, TfGuidAtom& converted) noexcept;
void set_display_attribute(ITfContext* context, TfEditCookie cookie, ITfRange* range, TfGuidAtom atom) noexcept;
void clear_display_attribute(ITfContext* context, TfEditCookie cookie, ITfRange* range) noexcept;
}
