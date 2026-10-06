#include "display_attributes.h"
#include "input_timing.h"
#include <wrl/client.h>
#include <new>

namespace nskkzime::windows {
namespace {
TF_DISPLAYATTRIBUTE defaults(bool converted) {
    TF_DISPLAYATTRIBUTE value{};
    value.crText.type = TF_CT_NONE;
    value.crBk.type = TF_CT_NONE;
    value.lsStyle = converted ? TF_LS_SOLID : TF_LS_DOT;
    value.fBoldLine = converted ? TRUE : FALSE;
    value.crLine.type = TF_CT_NONE;
    value.bAttr = converted ? TF_ATTR_TARGET_CONVERTED : TF_ATTR_INPUT;
    return value;
}
class Attribute final : public ITfDisplayAttributeInfo {
public:
    explicit Attribute(bool converted) : converted_(converted), value_(defaults(converted)) { ++module_objects; }
    ~Attribute() { --module_objects; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_ITfDisplayAttributeInfo) return E_NOINTERFACE;
        *out = static_cast<ITfDisplayAttributeInfo*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto refs = --refs_; if (!refs) delete this; return refs; }
    HRESULT STDMETHODCALLTYPE GetGUID(GUID* value) override {
        if (!value) return E_POINTER;
        *value = converted_ ? converted_attribute_guid : input_attribute_guid; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDescription(BSTR* value) override {
        if (!value) return E_POINTER;
        *value = SysAllocString(converted_ ? L"nskkzime selected candidate" : L"nskkzime reading");
        return *value ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetAttributeInfo(TF_DISPLAYATTRIBUTE* value) override {
        if (!value) return E_POINTER;
        *value = value_; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetAttributeInfo(const TF_DISPLAYATTRIBUTE* value) override {
        if (!value) return E_POINTER;
        value_ = *value; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Reset() override { value_ = defaults(converted_); return S_OK; }
private:
    std::atomic<ULONG> refs_{1};
    bool converted_;
    TF_DISPLAYATTRIBUTE value_;
};
class Attributes final : public IEnumTfDisplayAttributeInfo {
public:
    explicit Attributes(ULONG position = 0) : position_(position) { ++module_objects; }
    ~Attributes() { --module_objects; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IEnumTfDisplayAttributeInfo) return E_NOINTERFACE;
        *out = static_cast<IEnumTfDisplayAttributeInfo*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto refs = --refs_; if (!refs) delete this; return refs; }
    HRESULT STDMETHODCALLTYPE Clone(IEnumTfDisplayAttributeInfo** out) override {
        if (!out) return E_POINTER;
        *out = new (std::nothrow) Attributes(position_); return *out ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE Next(ULONG count, ITfDisplayAttributeInfo** out, ULONG* fetched) override {
        if (fetched) *fetched = 0;
        if (!out || (!fetched && count != 1)) return E_POINTER;
        ULONG obtained = 0;
        while (obtained < count && position_ < 2) {
            auto* item = new (std::nothrow) Attribute(position_ == 1);
            if (!item) { if (fetched) *fetched = obtained; return E_OUTOFMEMORY; }
            out[obtained++] = item; ++position_;
        }
        if (fetched) *fetched = obtained;
        return obtained == count ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Reset() override { position_ = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE Skip(ULONG count) override {
        const ULONG remaining = 2 - position_;
        position_ += count < remaining ? count : remaining;
        return count <= remaining ? S_OK : S_FALSE;
    }
private:
    std::atomic<ULONG> refs_{1};
    ULONG position_;
};
}
HRESULT enum_display_attributes(IEnumTfDisplayAttributeInfo** output) noexcept {
    if (!output) return E_POINTER;
    *output = new (std::nothrow) Attributes; return *output ? S_OK : E_OUTOFMEMORY;
}
HRESULT get_display_attribute(REFGUID guid, ITfDisplayAttributeInfo** output) noexcept {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (guid != input_attribute_guid && guid != converted_attribute_guid) return E_INVALIDARG;
    *output = new (std::nothrow) Attribute(guid == converted_attribute_guid);
    return *output ? S_OK : E_OUTOFMEMORY;
}
HRESULT display_attribute_atoms(TfGuidAtom& input, TfGuidAtom& converted) noexcept {
    Microsoft::WRL::ComPtr<ITfCategoryMgr> categories;
    auto hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&categories));
    if (SUCCEEDED(hr)) hr = categories->RegisterGUID(input_attribute_guid, &input);
    if (SUCCEEDED(hr)) hr = categories->RegisterGUID(converted_attribute_guid, &converted);
    return hr;
}
void set_display_attribute(ITfContext* context, TfEditCookie cookie, ITfRange* range, TfGuidAtom atom) noexcept {
    const InputPhaseTiming timing(InputTimingPhase::DisplayAttribute);
    Microsoft::WRL::ComPtr<ITfProperty> property;
    if (!range || !atom || FAILED(context->GetProperty(GUID_PROP_ATTRIBUTE, &property)) || !property) return;
    VARIANT value; VariantInit(&value); value.vt = VT_I4; value.lVal = static_cast<LONG>(atom);
    property->SetValue(cookie, range, &value);
}
void clear_display_attribute(ITfContext* context, TfEditCookie cookie, ITfRange* range) noexcept {
    const InputPhaseTiming timing(InputTimingPhase::DisplayAttribute);
    Microsoft::WRL::ComPtr<ITfProperty> property;
    if (range && SUCCEEDED(context->GetProperty(GUID_PROP_ATTRIBUTE, &property)) && property) property->Clear(cookie, range);
}
}
