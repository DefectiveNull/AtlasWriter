// AtlasWriterProps.dll: a Windows property handler for Markdown files.
//
// Explorer (and Windows Search) load it for .md and .markdown files and ask
// for their properties. It reads the ```meta block at the top of the file
// (see markdown/Meta.h) and reports:
//
//   title        -> Title             (System.Title)
//   tags         -> Tags              (System.Keywords)
//   author       -> Authors           (System.Author)
//   description  -> Comments          (System.Comment)
//   created/date -> Content created   (System.Document.DateCreated)
//   updated      -> Date last saved   (System.Document.DateSaved)
//
// Read-only: Explorer can show and search these, not edit them. Registered
// machine-wide by install/explorer.ps1 (Windows only reads property handlers
// from HKLM).

#include <windows.h>

// propsys.h defines PROPERTYKEY, which propkey.h needs.
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shlwapi.h>

#include "markdown/Meta.h"

#include <atomic>
#include <new>
#include <string>
#include <vector>

namespace {

// {B7A1B14C-EA09-4CBC-9930-2E4F1B4D7524}; install/explorer.ps1 registers the same.
constexpr CLSID kClsid = {0xb7a1b14c, 0xea09, 0x4cbc, {0x99, 0x30, 0x2e, 0x4f, 0x1b, 0x4d, 0x75, 0x24}};

constexpr ULONG kMaxRead = 64 * 1024; // the meta block sits at the top

std::atomic<long> g_objects{0};
std::atomic<long> g_locks{0};

std::wstring widen(const std::string& utf8)
{
    if (utf8.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

// "2026-11-02", optionally followed by "T14:30" or " 14:30[:05]", as local
// time (a date alone is local midnight, so Explorer shows the same day).
bool parseDate(const std::string& s, FILETIME& out)
{
    const auto digits = [&](size_t at, size_t count, WORD& value) {
        if (at + count > s.size())
            return false;
        value = 0;
        for (size_t i = at; i < at + count; ++i) {
            if (s[i] < '0' || s[i] > '9')
                return false;
            value = static_cast<WORD>(value * 10 + (s[i] - '0'));
        }
        return true;
    };
    SYSTEMTIME local{};
    if (!digits(0, 4, local.wYear) || s.size() < 10 || s[4] != '-' || !digits(5, 2, local.wMonth) || s[7] != '-'
        || !digits(8, 2, local.wDay))
        return false;
    if (s.size() >= 16 && (s[10] == 'T' || s[10] == ' ') && s[13] == ':') {
        if (!digits(11, 2, local.wHour) || !digits(14, 2, local.wMinute))
            return false;
        if (s.size() >= 19 && s[16] == ':')
            digits(17, 2, local.wSecond);
    }
    SYSTEMTIME utc{};
    return TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc) && SystemTimeToFileTime(&utc, &out);
}

class PropertyHandler final : public IInitializeWithStream, public IPropertyStore, public IPropertyStoreCapabilities
{
public:
    PropertyHandler() { ++g_objects; }

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        static const QITAB table[] = {
            QITABENT(PropertyHandler, IInitializeWithStream),
            QITABENT(PropertyHandler, IPropertyStore),
            QITABENT(PropertyHandler, IPropertyStoreCapabilities),
            {},
        };
        return QISearch(this, table, riid, ppv);
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(++refs_); }
    IFACEMETHODIMP_(ULONG) Release() override
    {
        const long left = --refs_;
        if (left == 0)
            delete this;
        return static_cast<ULONG>(left);
    }

    // IInitializeWithStream
    IFACEMETHODIMP Initialize(IStream* stream, DWORD) override
    {
        if (store_)
            return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        HRESULT hr = PSCreateMemoryPropertyStore(IID_PPV_ARGS(&store_));
        if (FAILED(hr))
            return hr;

        std::string text(kMaxRead, '\0');
        ULONG total = 0;
        while (total < kMaxRead) {
            ULONG got = 0;
            hr = stream->Read(text.data() + total, kMaxRead - total, &got);
            if (FAILED(hr) || got == 0)
                break;
            total += got;
        }
        text.resize(total);
        if (text.starts_with("\xEF\xBB\xBF"))
            text.erase(0, 3);
        std::erase(text, '\r');

        const md::Meta meta = md::parseMeta(text);
        if (meta.present && meta.error.empty())
            fill(meta);
        return S_OK; // a file without metadata simply has none
    }

    // IPropertyStore: everything is answered by the in-memory store.
    IFACEMETHODIMP GetCount(DWORD* count) override
    {
        if (!store_)
            return E_UNEXPECTED;
        return store_->GetCount(count);
    }
    IFACEMETHODIMP GetAt(DWORD index, PROPERTYKEY* key) override
    {
        if (!store_)
            return E_UNEXPECTED;
        return store_->GetAt(index, key);
    }
    IFACEMETHODIMP GetValue(REFPROPERTYKEY key, PROPVARIANT* value) override
    {
        if (!store_)
            return E_UNEXPECTED;
        return store_->GetValue(key, value);
    }
    IFACEMETHODIMP SetValue(REFPROPERTYKEY, REFPROPVARIANT) override { return STG_E_ACCESSDENIED; }
    IFACEMETHODIMP Commit() override { return STG_E_ACCESSDENIED; }

    // IPropertyStoreCapabilities
    IFACEMETHODIMP IsPropertyWritable(REFPROPERTYKEY) override { return S_FALSE; }

private:
    ~PropertyHandler()
    {
        if (store_)
            store_->Release();
        --g_objects;
    }

    void setString(REFPROPERTYKEY key, const std::string& value)
    {
        if (value.empty())
            return;
        PROPVARIANT v;
        if (SUCCEEDED(InitPropVariantFromString(widen(value).c_str(), &v))) {
            store_->SetValue(key, v);
            PropVariantClear(&v);
        }
    }

    void setStrings(REFPROPERTYKEY key, const std::vector<std::string>& values)
    {
        if (values.empty())
            return;
        std::vector<std::wstring> wide;
        std::vector<PCWSTR> pointers;
        for (const std::string& s : values)
            wide.push_back(widen(s));
        for (const std::wstring& w : wide)
            pointers.push_back(w.c_str());
        PROPVARIANT v;
        if (SUCCEEDED(InitPropVariantFromStringVector(pointers.data(), static_cast<ULONG>(pointers.size()), &v))) {
            store_->SetValue(key, v);
            PropVariantClear(&v);
        }
    }

    void setDate(REFPROPERTYKEY key, const std::string& value)
    {
        FILETIME time;
        if (value.empty() || !parseDate(value, time))
            return;
        PROPVARIANT v;
        if (SUCCEEDED(InitPropVariantFromFileTime(&time, &v))) {
            store_->SetValue(key, v);
            PropVariantClear(&v);
        }
    }

    void fill(const md::Meta& meta)
    {
        setString(PKEY_Title, meta.title);
        setStrings(PKEY_Keywords, meta.tags);
        if (!meta.author.empty())
            setStrings(PKEY_Author, {meta.author});
        setString(PKEY_Comment, meta.description);
        setDate(PKEY_Document_DateCreated, !meta.created.empty() ? meta.created : meta.date);
        setDate(PKEY_Document_DateSaved, meta.updated);
    }

    std::atomic<long> refs_{1};
    IPropertyStore* store_ = nullptr;
};

class ClassFactory final : public IClassFactory
{
public:
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        static const QITAB table[] = {QITABENT(ClassFactory, IClassFactory), {}};
        return QISearch(this, table, riid, ppv);
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return 2; } // static object
    IFACEMETHODIMP_(ULONG) Release() override { return 1; }

    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override
    {
        *ppv = nullptr;
        if (outer)
            return CLASS_E_NOAGGREGATION;
        PropertyHandler* handler = new (std::nothrow) PropertyHandler();
        if (!handler)
            return E_OUTOFMEMORY;
        const HRESULT hr = handler->QueryInterface(riid, ppv);
        handler->Release();
        return hr;
    }
    IFACEMETHODIMP LockServer(BOOL lock) override
    {
        lock ? ++g_locks : --g_locks;
        return S_OK;
    }
};

ClassFactory g_factory;

} // namespace

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv)
{
    *ppv = nullptr;
    if (clsid != kClsid)
        return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow()
{
    return g_objects == 0 && g_locks == 0 ? S_OK : S_FALSE;
}
