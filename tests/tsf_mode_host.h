#pragma once
#include "tsf_test_host.h"
#include "input_mode.h"
namespace test_host {
#define MODE_REFS(Type) \
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; } \
    ULONG STDMETHODCALLTYPE Release() override { const auto n = --references; if (!n) delete this; return n; } \
    ULONG references = 1
#define MODE_UNUSED(name, ...) HRESULT STDMETHODCALLTYPE name(__VA_ARGS__) override { return E_NOTIMPL; }
class Compartment final : public ITfCompartment, public ITfSource {
public:
    explicit Compartment(GUID id) : id(id) {}
    GUID id;
    std::optional<LONG> value;
    ComPtr<ITfCompartmentEventSink> sink;
    bool reject_write = false, reject_sink = false;
    unsigned writes = 0;
    MODE_REFS(Compartment);
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_ITfCompartment) *out = static_cast<ITfCompartment*>(this);
        else if (iid == IID_ITfSource) *out = static_cast<ITfSource*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetValue(VARIANT* out) override {
        VariantInit(out); if (!value) return S_FALSE;
        out->vt = VT_I4; out->lVal = *value; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetValue(TfClientId, const VARIANT* item) override {
        if (reject_write) return E_FAIL;
        if (item->vt != VT_I4) return E_INVALIDARG;
        value = item->lVal; ++writes; const auto notify = sink; return notify ? notify->OnChange(id) : S_OK;
    }
    HRESULT STDMETHODCALLTYPE AdviseSink(REFIID iid, IUnknown* item, DWORD* cookie) override {
        if (reject_sink || sink || iid != IID_ITfCompartmentEventSink) return E_FAIL;
        *cookie = 1; return item->QueryInterface(IID_PPV_ARGS(&sink));
    }
    HRESULT STDMETHODCALLTYPE UnadviseSink(DWORD cookie) override { if (cookie != 1) return E_INVALIDARG; sink.Reset(); return S_OK; }
    void set(LONG next) { VARIANT item{}; item.vt = VT_I4; item.lVal = next; SetValue(1, &item); }
};
class ModeManager final : public Manager, public ITfCompartmentMgr, public ITfLangBarItemMgr {
public:
    ModeManager() {
        open.Attach(new Compartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE));
        conversion.Attach(new Compartment(GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION));
    }
    ComPtr<Compartment> open, conversion;
    ComPtr<ITfLangBarItem> button;
    bool reject_bar = false;
    std::vector<GUID> preserved;
    MODE_REFS(ModeManager);
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == IID_ITfCompartmentMgr) *out = static_cast<ITfCompartmentMgr*>(this);
        else if (iid == IID_ITfLangBarItemMgr) *out = static_cast<ITfLangBarItemMgr*>(this);
        else return Manager::QueryInterface(iid, out);
        AddRef(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCompartment(REFGUID id, ITfCompartment** out) override {
        if (id == open->id) return open.CopyTo(out);
        if (id == conversion->id) return conversion.CopyTo(out);
        *out = nullptr; return E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE AddItem(ITfLangBarItem* item) override { if (reject_bar || button) return E_FAIL; button = item; return S_OK; }
    HRESULT STDMETHODCALLTYPE RemoveItem(ITfLangBarItem* item) override { if (button.Get() != item) return E_INVALIDARG; button.Reset(); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetItem(REFGUID id, ITfLangBarItem** out) override {
        if (id != GUID_LBI_INPUTMODE) { *out = nullptr; return E_FAIL; } return button.CopyTo(out);
    }
    HRESULT STDMETHODCALLTYPE PreserveKey(TfClientId, REFGUID id, const TF_PRESERVEDKEY*, const WCHAR*, ULONG) override { preserved.push_back(id); return S_OK; }
    HRESULT STDMETHODCALLTYPE UnpreserveKey(REFGUID id, const TF_PRESERVEDKEY*) override { std::erase(preserved, id); return S_OK; }
    MODE_UNUSED(ClearCompartment, TfClientId, REFGUID)
    MODE_UNUSED(EnumCompartments, IEnumGUID**)
    MODE_UNUSED(EnumItems, IEnumTfLangBarItems**)
    MODE_UNUSED(AdviseItemSink, ITfLangBarItemSink*, DWORD*, REFGUID)
    MODE_UNUSED(UnadviseItemSink, DWORD)
    MODE_UNUSED(GetItemFloatingRect, DWORD, REFGUID, RECT*)
    MODE_UNUSED(GetItemsStatus, ULONG, const GUID*, DWORD*)
    MODE_UNUSED(GetItemNum, ULONG*)
    MODE_UNUSED(GetItems, ULONG, ITfLangBarItem**, TF_LANGBARITEMINFO*, DWORD*, ULONG*)
    MODE_UNUSED(AdviseItemsSink, ULONG, ITfLangBarItemSink**, const GUID*, DWORD*)
    MODE_UNUSED(UnadviseItemsSink, ULONG, DWORD*)
};
#undef MODE_REFS
#undef MODE_UNUSED
} // namespace test_host
