#include "input_mode.h"
#include <algorithm>
#include <string_view>
#include <olectl.h>

namespace nskkzime::windows {
using Microsoft::WRL::ComPtr;
namespace {
constexpr std::array<const GUID*, 2> ids{{&GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, &GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION}};
constexpr std::array<Mode, 3> modes{{Mode::Hiragana, Mode::Katakana, Mode::Latin}};
const wchar_t* text(Mode mode) { return mode == Mode::Hiragana ? L"あ" : mode == Mode::Katakana ? L"ア" : L"A"; }
const wchar_t* description(Mode mode) {
    return mode == Mode::Hiragana ? L"nskkzime: ひらがな" : mode == Mode::Katakana ? L"nskkzime: カタカナ" : L"nskkzime: 英数（IMEオフ）";
}
HRESULT string_result(const wchar_t* text, BSTR* out) {
    if (!out) return E_POINTER;
    *out = SysAllocString(text); return *out ? S_OK : E_OUTOFMEMORY;
}
std::optional<LONG> read(ITfCompartment* compartment) {
    if (!compartment) return {};
    VARIANT value; VariantInit(&value);
    const auto hr = compartment->GetValue(&value);
    const auto result = hr == S_OK && value.vt == VT_I4 ? std::optional{value.lVal} : std::nullopt;
    VariantClear(&value); return result;
}
void write(ITfCompartment* compartment, TfClientId client, LONG value) {
    if (!compartment || read(compartment) == value) return;
    VARIANT item; VariantInit(&item); item.vt = VT_I4; item.lVal = value;
    compartment->SetValue(client, &item);
}
}
ModeButton::ModeButton(std::weak_ptr<ModeActions> actions) : actions_(std::move(actions)) { ++module_objects; }
ModeButton::~ModeButton() { --module_objects; }
HRESULT ModeButton::QueryInterface(REFIID iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid == IID_IUnknown || iid == IID_ITfLangBarItem || iid == IID_ITfLangBarItemButton) *out = static_cast<ITfLangBarItemButton*>(this);
    else if (iid == IID_ITfSource) *out = static_cast<ITfSource*>(this);
    else return E_NOINTERFACE;
    AddRef(); return S_OK;
}
ULONG ModeButton::AddRef() { return ++refs_; }
ULONG ModeButton::Release() { const auto refs = --refs_; if (!refs) delete this; return refs; }
HRESULT ModeButton::GetInfo(TF_LANGBARITEMINFO* out) {
    if (!out) return E_POINTER;
    *out = {service_clsid, GUID_LBI_INPUTMODE, TF_LBI_STYLE_BTN_MENU | TF_LBI_STYLE_SHOWNINTRAY | TF_LBI_STYLE_TEXTCOLORICON, 0, {}};
    wcscpy_s(out->szDescription, L"nskkzime 入力モード"); return S_OK;
}
HRESULT ModeButton::GetStatus(DWORD* out) { if (!out) return E_POINTER; *out = status_; return S_OK; }
HRESULT ModeButton::Show(BOOL show) {
    if (show) status_ &= ~TF_LBI_STATUS_HIDDEN; else status_ |= TF_LBI_STATUS_HIDDEN;
    const auto sink = sink_; if (sink) sink->OnUpdate(TF_LBI_STATUS); return S_OK;
}
HRESULT ModeButton::GetTooltipString(BSTR* out) { return string_result(description(mode_), out); }
HRESULT ModeButton::GetText(BSTR* out) { return string_result(text(mode_), out); }
HRESULT ModeButton::select(Mode mode) noexcept {
    if (status_ & TF_LBI_STATUS_DISABLED) return S_FALSE;
    const auto actions = actions_.lock();
    if (!actions || !actions->change) return S_FALSE;
    try { actions->change(mode); return S_OK; } catch (...) { return E_FAIL; }
}
HRESULT ModeButton::OnClick(TfLBIClick click, POINT, const RECT*) {
    return click == TF_LBI_CLK_LEFT ? select(mode_ == Mode::Latin ? Mode::Hiragana : Mode::Latin) : S_OK;
}
HRESULT ModeButton::InitMenu(ITfMenu* menu) {
    if (!menu) return E_POINTER;
    constexpr std::array<const wchar_t*, 3> names{{L"ひらがな", L"カタカナ", L"英数（IMEオフ）"}};
    for (UINT i = 0; i < modes.size(); ++i) {
        const auto hr = menu->AddMenuItem(i + 1, modes[i] == mode_ ? TF_LBMENUF_CHECKED : 0, nullptr, nullptr,
            names[i], static_cast<ULONG>(wcslen(names[i])), nullptr);
        if (FAILED(hr)) return hr;
    }
    auto hr = menu->AddMenuItem(0, TF_LBMENUF_SEPARATOR, nullptr, nullptr, nullptr, 0, nullptr);
    if (FAILED(hr)) return hr;
    const auto actions = actions_.lock();
    constexpr std::array<const wchar_t*, 2> tools{{L"設定…", L"学習辞書の管理…"}};
    for (UINT i = 0; i < tools.size(); ++i) {
        const bool enabled = !(status_ & TF_LBI_STATUS_DISABLED) && actions && actions->open_tool && actions->tools_available[i];
        hr = menu->AddMenuItem(settings_menu_id + i, enabled ? 0 : TF_LBMENUF_GRAYED, nullptr, nullptr,
                              tools[i], static_cast<ULONG>(wcslen(tools[i])), nullptr);
        if (FAILED(hr)) return hr;
    }
    return S_OK;
}
HRESULT ModeButton::OnMenuSelect(UINT id) {
    if (id >= 1 && id <= modes.size()) return select(modes[id - 1]);
    if (id != settings_menu_id && id != dictionary_menu_id) return E_INVALIDARG;
    const auto actions = actions_.lock();
    const auto slot = id - settings_menu_id;
    if ((status_ & TF_LBI_STATUS_DISABLED) || !actions || !actions->open_tool || !actions->tools_available[slot]) return S_FALSE;
    try { return actions->open_tool(static_cast<ImeTool>(slot)); } catch (...) { return E_FAIL; }
}
HRESULT ModeButton::GetIcon(HICON* out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    // Monochrome AND/XOR mask. TSF recolors TEXTCOLORICON for the shell theme.
    const int width = std::max(16, GetSystemMetrics(SM_CXSMICON)), height = std::max(16, GetSystemMetrics(SM_CYSMICON));
    const auto mask = CreateBitmap(width, height * 2, 1, 1, nullptr);
    const auto dc = CreateCompatibleDC(nullptr);
    if (!mask || !dc) { if (mask) DeleteObject(mask); if (dc) DeleteDC(dc); return E_OUTOFMEMORY; }
    const auto old_bitmap = SelectObject(dc, mask);
    PatBlt(dc, 0, 0, width, height, WHITENESS); PatBlt(dc, 0, height, width, height, BLACKNESS);
    const auto font = CreateFontW(-height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, DEFAULT_PITCH, L"Yu Gothic UI");
    const auto old_font = font ? SelectObject(dc, font) : nullptr;
    SetTextColor(dc, RGB(0, 0, 0)); SetBkColor(dc, RGB(255, 255, 255));
    RECT rect{0, 0, width, height}; DrawTextW(dc, text(mode_), -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (font) { SelectObject(dc, old_font); DeleteObject(font); }
    SelectObject(dc, old_bitmap); DeleteDC(dc);
    ICONINFO info{TRUE, 0, 0, mask, nullptr}; *out = CreateIconIndirect(&info); DeleteObject(mask);
    return *out ? S_OK : E_FAIL;
}
HRESULT ModeButton::AdviseSink(REFIID iid, IUnknown* value, DWORD* cookie) {
    if (!cookie || !value) return E_POINTER;
    *cookie = TF_INVALID_COOKIE;
    if (iid != IID_ITfLangBarItemSink) return CONNECT_E_CANNOTCONNECT;
    if (sink_) return CONNECT_E_ADVISELIMIT;
    const auto hr = value->QueryInterface(IID_PPV_ARGS(&sink_)); if (SUCCEEDED(hr)) *cookie = 1; return hr;
}
HRESULT ModeButton::UnadviseSink(DWORD cookie) {
    if (cookie != 1 || !sink_) return CONNECT_E_NOCONNECTION;
    sink_.Reset(); return S_OK;
}
void ModeButton::update(Mode mode) noexcept {
    if (mode_ == mode) return;
    mode_ = mode; const auto sink = sink_; if (sink) sink->OnUpdate(TF_LBI_BTNALL);
}
void ModeButton::close() noexcept {
    actions_.reset(); status_ |= TF_LBI_STATUS_DISABLED | TF_LBI_STATUS_HIDDEN;
    auto sink = std::move(sink_); if (sink) sink->OnUpdate(TF_LBI_STATUS);
}
InputMode::InputMode() { ++module_objects; }
InputMode::~InputMode() { --module_objects; }
HRESULT InputMode::QueryInterface(REFIID iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid != IID_IUnknown && iid != IID_ITfCompartmentEventSink) return E_NOINTERFACE;
    *out = static_cast<ITfCompartmentEventSink*>(this); AddRef(); return S_OK;
}
ULONG InputMode::AddRef() { return ++refs_; }
ULONG InputMode::Release() { const auto refs = --refs_; if (!refs) delete this; return refs; }
void InputMode::connect(ITfThreadMgr* thread, TfClientId client, std::weak_ptr<ModeActions> actions, Mode mode) noexcept {
    disconnect(); actions_ = std::move(actions); client_ = client; mode_ = mode;
    try {
        ComPtr<ITfCompartmentMgr> manager;
        if (thread && SUCCEEDED(thread->QueryInterface(IID_PPV_ARGS(&manager)))) {
            for (std::size_t i = 0; i < ids.size(); ++i) {
                ComPtr<ITfSource> source;
                if (SUCCEEDED(manager->GetCompartment(*ids[i], &compartments_[i])) && compartments_[i] &&
                    SUCCEEDED(compartments_[i].As(&source))) source->AdviseSink(IID_ITfCompartmentEventSink, this, &cookies_[i]);
            }
        }
        button_.Attach(new ModeButton(actions_)); button_->update(mode_);
        if (thread && SUCCEEDED(thread->QueryInterface(IID_PPV_ARGS(&bar_))) && FAILED(bar_->AddItem(button_.Get()))) bar_.Reset();
        // An existing thread value belongs to Windows; preserve its on/off state.
        if (read(compartments_[0].Get())) OnChange(*ids[0]);
        else publish(mode_);
    } catch (...) { disconnect(); }
}
void InputMode::disconnect() noexcept {
    actions_.reset(); client_ = TF_CLIENTID_NULL;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        auto compartment = std::move(compartments_[i]); const auto cookie = cookies_[i]; cookies_[i] = TF_INVALID_COOKIE;
        ComPtr<ITfSource> source;
        if (cookie != TF_INVALID_COOKIE && compartment && SUCCEEDED(compartment.As(&source))) source->UnadviseSink(cookie);
    }
    auto button = std::move(button_); auto bar = std::move(bar_);
    if (button) { button->close(); if (bar) bar->RemoveItem(button.Get()); }
}
HRESULT InputMode::OnChange(REFGUID guid) {
    if (writing_ || client_ == TF_CLIENTID_NULL || (guid != *ids[0] && guid != *ids[1])) return S_OK;
    const auto open = read(compartments_[0].Get()); if (!open) return S_OK;
    // Windows may choose a conversion mode while closed, then open it in a
    // second notification. Keep that value until the open notification arrives.
    if (!*open && guid == *ids[1]) return S_OK;
    auto next = Mode::Latin;
    if (*open) {
        const auto conversion = read(compartments_[1].Get());
        if (guid == *ids[1] && conversion && !(*conversion & TF_CONVERSIONMODE_NATIVE)) next = Mode::Latin;
        else next = conversion && (*conversion & TF_CONVERSIONMODE_KATAKANA) ? Mode::Katakana : Mode::Hiragana;
    }
    if (next == mode_) return S_OK; // Repeated Windows notifications must not cancel an active registration.
    const auto actions = actions_.lock();
    if (actions && actions->change) { try { actions->change(next); } catch (...) { return E_FAIL; } }
    return S_OK;
}
void InputMode::publish(Mode mode) noexcept {
    if (client_ == TF_CLIENTID_NULL) return;
    mode_ = mode;
    writing_ = true;
    write(compartments_[1].Get(), client_, mode == Mode::Latin ? 0 : TF_CONVERSIONMODE_NATIVE | TF_CONVERSIONMODE_FULLSHAPE |
        (mode == Mode::Katakana ? TF_CONVERSIONMODE_KATAKANA : 0));
    write(compartments_[0].Get(), client_, mode != Mode::Latin);
    writing_ = false;
    const auto button = button_; if (button) button->update(mode);
}
} // namespace nskkzime::windows
