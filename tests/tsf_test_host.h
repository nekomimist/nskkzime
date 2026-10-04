#pragma once
// Deterministic COM host for the TSF adapter contract. It is not a substitute
// for testing Windows' text stores, native key routing, or application UIs.
#include <windows.h>
#include <msctf.h>
#include <wrl/client.h>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace test_host {
using Microsoft::WRL::ComPtr;
#define TEST_REFS(Type) \
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; } \
    ULONG STDMETHODCALLTYPE Release() override { auto n = --refs_; if (!n) delete this; return n; } \
    ULONG refs_ = 1
#define UNUSED_METHOD(name, ...) HRESULT STDMETHODCALLTYPE name(__VA_ARGS__) override { return E_NOTIMPL; }

class Manager final : public ITfThreadMgr, public ITfKeystrokeMgr, public ITfSource {
public:
    bool reject_sink = false;
    ComPtr<ITfKeyEventSink> keys;
    ComPtr<ITfThreadMgrEventSink> events;
    TEST_REFS(Manager);
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_ITfThreadMgr) *out = static_cast<ITfThreadMgr*>(this);
        else if (iid == IID_ITfKeystrokeMgr) *out = static_cast<ITfKeystrokeMgr*>(this);
        else if (iid == IID_ITfSource) *out = static_cast<ITfSource*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE AdviseKeyEventSink(TfClientId, ITfKeyEventSink* sink, BOOL) override { keys = sink; return S_OK; }
    HRESULT STDMETHODCALLTYPE UnadviseKeyEventSink(TfClientId) override { keys.Reset(); return S_OK; }
    HRESULT STDMETHODCALLTYPE AdviseSink(REFIID iid, IUnknown* sink, DWORD* cookie) override {
        if (reject_sink) return E_FAIL;
        if (iid != IID_ITfThreadMgrEventSink) return E_NOINTERFACE;
        *cookie = 42; return sink->QueryInterface(IID_PPV_ARGS(&events));
    }
    HRESULT STDMETHODCALLTYPE UnadviseSink(DWORD) override { events.Reset(); return S_OK; }
    UNUSED_METHOD(Activate, TfClientId*)
    UNUSED_METHOD(Deactivate)
    UNUSED_METHOD(CreateDocumentMgr, ITfDocumentMgr**)
    UNUSED_METHOD(EnumDocumentMgrs, IEnumTfDocumentMgrs**)
    UNUSED_METHOD(GetFocus, ITfDocumentMgr**)
    UNUSED_METHOD(SetFocus, ITfDocumentMgr*)
    UNUSED_METHOD(AssociateFocus, HWND, ITfDocumentMgr*, ITfDocumentMgr**)
    UNUSED_METHOD(IsThreadFocus, BOOL*)
    UNUSED_METHOD(GetFunctionProvider, REFCLSID, ITfFunctionProvider**)
    UNUSED_METHOD(EnumFunctionProviders, IEnumTfFunctionProviders**)
    UNUSED_METHOD(GetGlobalCompartment, ITfCompartmentMgr**)
    UNUSED_METHOD(GetForeground, CLSID*)
    UNUSED_METHOD(TestKeyDown, WPARAM, LPARAM, BOOL*)
    UNUSED_METHOD(TestKeyUp, WPARAM, LPARAM, BOOL*)
    UNUSED_METHOD(KeyDown, WPARAM, LPARAM, BOOL*)
    UNUSED_METHOD(KeyUp, WPARAM, LPARAM, BOOL*)
    UNUSED_METHOD(GetPreservedKey, ITfContext*, const TF_PRESERVEDKEY*, GUID*)
    UNUSED_METHOD(IsPreservedKey, REFGUID, const TF_PRESERVEDKEY*, BOOL*)
    UNUSED_METHOD(PreserveKey, TfClientId, REFGUID, const TF_PRESERVEDKEY*, const WCHAR*, ULONG)
    UNUSED_METHOD(UnpreserveKey, REFGUID, const TF_PRESERVEDKEY*)
    UNUSED_METHOD(SetPreservedKeyDescription, REFGUID, const WCHAR*, ULONG)
    UNUSED_METHOD(GetPreservedKeyDescription, REFGUID, BSTR*)
    UNUSED_METHOD(SimulatePreservedKey, ITfContext*, REFGUID, BOOL*)
};

class Range;
struct Document {
    std::wstring text;
    LONG caret = 0;
    LONG selection_end = -1; // -1 means a collapsed selection at caret.
    bool reject_write = false;
    bool reject_shift = false;
    bool reject_read = false;
    std::vector<Range*> ranges;
    void replace(LONG begin, LONG finish, std::wstring_view value);
};
class Range final : public ITfRange {
public:
    Range(std::shared_ptr<Document> document, LONG begin, LONG finish) : doc(std::move(document)), start(begin), end(finish) { doc->ranges.push_back(this); }
    ~Range() { std::erase(doc->ranges, this); }
    std::shared_ptr<Document> doc;
    LONG start;
    LONG end;
    TEST_REFS(Range);
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_ITfRange) return E_NOINTERFACE;
        *out = static_cast<ITfRange*>(this); AddRef(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetText(TfEditCookie, DWORD, const WCHAR* text, LONG length) override {
        if (doc->reject_write) return E_FAIL;
        const auto begin = start;
        doc->replace(start, end, std::wstring_view(text, static_cast<std::size_t>(length)));
        start = begin; end = start + length;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(ITfRange** out) override { *out = new Range(doc, start, end); return S_OK; }
    HRESULT STDMETHODCALLTYPE Collapse(TfEditCookie, TfAnchor anchor) override {
        if (anchor == TF_ANCHOR_START) end = start; else start = end;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ShiftStart(TfEditCookie, LONG count, LONG* shifted, const TF_HALTCOND*) override {
        if (doc->reject_shift) return E_FAIL;
        const auto next = std::clamp(start + count, 0L, end);
        *shifted = next - start; start = next; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetText(TfEditCookie, DWORD flags, WCHAR* text, ULONG maximum, ULONG* fetched) override {
        if (doc->reject_read) return E_FAIL;
        *fetched = (std::min)(maximum, static_cast<ULONG>(end - start));
        std::copy_n(doc->text.data() + start, *fetched, text);
        if (flags & TF_TF_MOVESTART) start += static_cast<LONG>(*fetched);
        return S_OK;
    }
    UNUSED_METHOD(GetFormattedText, TfEditCookie, IDataObject**)
    UNUSED_METHOD(GetEmbedded, TfEditCookie, REFGUID, REFIID, IUnknown**)
    UNUSED_METHOD(InsertEmbedded, TfEditCookie, DWORD, IDataObject*)
    UNUSED_METHOD(ShiftEnd, TfEditCookie, LONG, LONG*, const TF_HALTCOND*)
    UNUSED_METHOD(ShiftStartToRange, TfEditCookie, ITfRange*, TfAnchor)
    UNUSED_METHOD(ShiftEndToRange, TfEditCookie, ITfRange*, TfAnchor)
    UNUSED_METHOD(ShiftStartRegion, TfEditCookie, TfShiftDir, BOOL*)
    UNUSED_METHOD(ShiftEndRegion, TfEditCookie, TfShiftDir, BOOL*)
    HRESULT STDMETHODCALLTYPE IsEmpty(TfEditCookie, BOOL* empty) override { *empty = start == end; return S_OK; }
    UNUSED_METHOD(IsEqualStart, TfEditCookie, ITfRange*, TfAnchor, BOOL*)
    UNUSED_METHOD(IsEqualEnd, TfEditCookie, ITfRange*, TfAnchor, BOOL*)
    HRESULT STDMETHODCALLTYPE CompareStart(TfEditCookie, ITfRange* other, TfAnchor anchor, LONG* result) override {
        const auto* range = static_cast<Range*>(other);
        if (doc != range->doc) return E_INVALIDARG;
        const auto position = anchor == TF_ANCHOR_START ? range->start : range->end;
        *result = start < position ? -1 : start > position ? 1 : 0; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CompareEnd(TfEditCookie, ITfRange* other, TfAnchor anchor, LONG* result) override {
        const auto* range = static_cast<Range*>(other);
        if (doc != range->doc) return E_INVALIDARG;
        const auto position = anchor == TF_ANCHOR_START ? range->start : range->end;
        *result = end < position ? -1 : end > position ? 1 : 0; return S_OK;
    }
    UNUSED_METHOD(AdjustForInsert, TfEditCookie, ULONG, BOOL*)
    UNUSED_METHOD(GetGravity, TfGravity*, TfGravity*)
    UNUSED_METHOD(SetGravity, TfEditCookie, TfGravity, TfGravity)
    UNUSED_METHOD(GetContext, ITfContext**)
};
inline void Document::replace(LONG begin, LONG finish, std::wstring_view value) {
    const auto length = static_cast<LONG>(value.size());
    const auto delta = length - (finish - begin);
    text.replace(static_cast<std::size_t>(begin), static_cast<std::size_t>(finish - begin), value);
    auto position = [&](LONG old, bool forward) {
        if (old < begin) return old;
        if (old > finish) return old + delta;
        if (old == finish && finish != begin) return begin + length;
        return forward ? begin + length : begin;
    };
    for (auto* range : ranges) {
        range->start = position(range->start, false);
        range->end = position(range->end, true);
    }
    caret = position(caret, true);
    if (selection_end >= 0) selection_end = position(selection_end, true);
}

class Composition final : public ITfComposition {
public:
    ComPtr<Range> range;
    ComPtr<ITfCompositionSink> sink;
    bool ended = false;
    TEST_REFS(Composition);
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_ITfComposition) return E_NOINTERFACE;
        *out = static_cast<ITfComposition*>(this); AddRef(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRange(ITfRange** out) override { return range.CopyTo(out); }
    HRESULT STDMETHODCALLTYPE ShiftStart(TfEditCookie, ITfRange* value) override { range->start = static_cast<Range*>(value)->start; return S_OK; }
    HRESULT STDMETHODCALLTYPE EndComposition(TfEditCookie) override { ended = true; sink.Reset(); return S_OK; }
    void terminate() { auto keep_sink = sink; if (keep_sink) keep_sink->OnCompositionTerminated(1, this); ended = true; sink.Reset(); }
    UNUSED_METHOD(ShiftEnd, TfEditCookie, ITfRange*)
};

class Context final : public ITfContext, public ITfContextComposition, public ITfInsertAtSelection, public ITfSource {
public:
    std::shared_ptr<Document> doc = std::make_shared<Document>();
    bool reject_session = false;
    bool reject_composition = false;
    bool readonly = false;
    bool reject_edit_sink = false;
    ComPtr<ITfTextEditSink> edits;
    ComPtr<Composition> composition;
    std::vector<ComPtr<ITfEditSession>> queued;
    TEST_REFS(Context);
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_ITfContext) *out = static_cast<ITfContext*>(this);
        else if (iid == IID_ITfContextComposition) *out = static_cast<ITfContextComposition*>(this);
        else if (iid == IID_ITfInsertAtSelection) *out = static_cast<ITfInsertAtSelection*>(this);
        else if (iid == IID_ITfSource) *out = static_cast<ITfSource*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RequestEditSession(TfClientId, ITfEditSession* edit, DWORD flags, HRESULT* status) override {
        if (reject_session) { *status = TF_E_SYNCHRONOUS; return S_OK; }
        if (flags & TF_ES_ASYNC) { queued.emplace_back(edit); *status = TF_S_ASYNC; }
        else { *status = edit->DoEditSession(1); notify(); }
        return S_OK;
    }
    void notify() { auto sink = edits; if (sink) sink->OnEndEdit(this, 1, nullptr); }
    void move_selection(LONG start, LONG end) { doc->caret = start; doc->selection_end = end; notify(); }
    void host_replace(LONG start, LONG end, std::wstring_view value) { doc->replace(start, end, value); notify(); }
    void pump() { auto work = std::move(queued); queued.clear(); for (const auto& item : work) { item->DoEditSession(1); notify(); } }
    HRESULT STDMETHODCALLTYPE AdviseSink(REFIID iid, IUnknown* sink, DWORD* cookie) override {
        if (reject_edit_sink) return E_FAIL;
        if (iid != IID_ITfTextEditSink || edits) return E_INVALIDARG;
        const auto hr = sink->QueryInterface(IID_PPV_ARGS(&edits));
        if (SUCCEEDED(hr)) *cookie = 43;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE UnadviseSink(DWORD cookie) override {
        if (cookie != 43) return E_INVALIDARG;
        edits.Reset(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSelection(TfEditCookie, ULONG, ULONG, TF_SELECTION* selection, ULONG* fetched) override {
        *selection = {new Range(doc, doc->caret, doc->selection_end < 0 ? doc->caret : doc->selection_end), {TF_AE_NONE, FALSE}};
        *fetched = 1; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSelection(TfEditCookie, ULONG, const TF_SELECTION* selection) override {
        doc->caret = static_cast<Range*>(selection->range)->start;
        doc->selection_end = static_cast<Range*>(selection->range)->end;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStatus(TF_STATUS* status) override { *status = {readonly ? static_cast<DWORD>(TF_SD_READONLY) : 0UL, 0}; return S_OK; }
    HRESULT STDMETHODCALLTYPE StartComposition(TfEditCookie, ITfRange* input, ITfCompositionSink* sink, ITfComposition** out) override {
        *out = nullptr;
        if (reject_composition) return S_OK;
        if (composition && !composition->ended) return E_UNEXPECTED;
        composition.Attach(new Composition);
        composition->range = static_cast<Range*>(input);
        composition->sink = sink;
        return composition.CopyTo(out);
    }
    HRESULT STDMETHODCALLTYPE InsertTextAtSelection(TfEditCookie, DWORD, const WCHAR* text, LONG length, ITfRange** out) override {
        ComPtr<Range> range;
        range.Attach(new Range(doc, doc->caret, doc->selection_end < 0 ? doc->caret : doc->selection_end));
        auto hr = range->SetText(1, 0, text, length);
        if (FAILED(hr)) return hr;
        return range.CopyTo(out);
    }
    UNUSED_METHOD(InWriteSession, TfClientId, BOOL*)
    UNUSED_METHOD(GetStart, TfEditCookie, ITfRange**)
    UNUSED_METHOD(GetEnd, TfEditCookie, ITfRange**)
    UNUSED_METHOD(GetActiveView, ITfContextView**)
    UNUSED_METHOD(EnumViews, IEnumTfContextViews**)
    UNUSED_METHOD(GetProperty, REFGUID, ITfProperty**)
    UNUSED_METHOD(GetAppProperty, REFGUID, ITfReadOnlyProperty**)
    UNUSED_METHOD(TrackProperties, const GUID**, ULONG, const GUID**, ULONG, ITfReadOnlyProperty**)
    UNUSED_METHOD(EnumProperties, IEnumTfProperties**)
    UNUSED_METHOD(GetDocumentMgr, ITfDocumentMgr**)
    UNUSED_METHOD(CreateRangeBackup, TfEditCookie, ITfRange*, ITfRangeBackup**)
    UNUSED_METHOD(EnumCompositions, IEnumITfCompositionView**)
    UNUSED_METHOD(FindComposition, TfEditCookie, ITfRange*, IEnumITfCompositionView**)
    UNUSED_METHOD(TakeOwnership, TfEditCookie, ITfCompositionView*, ITfCompositionSink*, ITfComposition**)
    UNUSED_METHOD(InsertEmbeddedAtSelection, TfEditCookie, DWORD, IDataObject*, ITfRange**)
};
#undef TEST_REFS
#undef UNUSED_METHOD
} // namespace test_host
