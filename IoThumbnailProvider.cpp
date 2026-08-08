// IoThumbnailProvider.cpp
// Windows Shell thumbnail handler for .io (BrickLink Studio) files.
//
// Studio .io files are ZIP archives containing thumbnail.png at the archive root.
// This handler extracts that PNG and returns it to Explorer.
//
// Dependencies: miniz.h + miniz.c (place in third_party/miniz/)
//   https://github.com/richgel999/miniz/releases
//
// Build (Developer PowerShell, x64):
//   cl /LD /EHsc /O2 IoThumbnailProvider.cpp third_party/miniz/miniz.c /link /DEF:IoThumbnailProvider.def ole32.lib oleaut32.lib shlwapi.lib gdiplus.lib /OUT:IoThumbnailProvider.dll
//
// Register (elevated):
//   regsvr32 IoThumbnailProvider.dll
//
// Unregister:
//   regsvr32 /u IoThumbnailProvider.dll

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <thumbcache.h>
#include <gdiplus.h>
#include "third_party/miniz/miniz.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

// {E7E4677B-5BBF-4459-BDCF-A97AE3BE21C8}
// Generate your own GUID: https://guidgenerator.com
static const CLSID CLSID_IoThumbnailProvider =
    {0xE7E4677B, 0x5BBF, 0x4459,
     {0xBD, 0xCF, 0xA9, 0x7A, 0xE3, 0xBE, 0x21, 0xC8}};

static LONG      g_cDllRef = 0;
static HINSTANCE g_hInst   = nullptr;

// ---------------------------------------------------------------------------
// GDI+ lifetime, scoped to a single GetThumbnail call.
//
// GdiplusStartup must not be called from DllMain; doing so here hung
// CoCreateInstance indefinitely, before the provider was ever reached.
// Per-call startup/shutdown is one of the three approaches Microsoft
// documents for a DLL that uses GDI+:
//   https://learn.microsoft.com/en-us/windows/win32/api/gdiplusinit/nf-gdiplusinit-gdiplusstartup
// ---------------------------------------------------------------------------
struct GdiplusSession
{
    ULONG_PTR       token;
    Gdiplus::Status status;

    GdiplusSession() : token(0)
    {
        Gdiplus::GdiplusStartupInput si;
        status = Gdiplus::GdiplusStartup(&token, &si, nullptr);
    }

    ~GdiplusSession()
    {
        if (status == Gdiplus::Ok) Gdiplus::GdiplusShutdown(token);
    }
};

// ---------------------------------------------------------------------------
// Read all bytes from an IStream into a heap buffer (CoTaskMemAlloc).
// ---------------------------------------------------------------------------
static HRESULT StreamToBytes(IStream* pStream, BYTE** ppData, DWORD* pSize)
{
    STATSTG stat{};
    HRESULT hr = pStream->Stat(&stat, STATFLAG_NONAME);
    if (FAILED(hr)) return hr;

    DWORD size = static_cast<DWORD>(stat.cbSize.QuadPart);
    BYTE* buf  = static_cast<BYTE*>(CoTaskMemAlloc(size));
    if (!buf) return E_OUTOFMEMORY;

    LARGE_INTEGER li{};
    pStream->Seek(li, STREAM_SEEK_SET, nullptr);

    DWORD read = 0;
    hr = pStream->Read(buf, size, &read);
    if (FAILED(hr) || read != size) {
        CoTaskMemFree(buf);
        return FAILED(hr) ? hr : E_FAIL;
    }

    *ppData = buf;
    *pSize  = size;
    return S_OK;
}

// ---------------------------------------------------------------------------
// Studio writes some .io files as ZipCrypto-encrypted archives, using one
// static password documented as the container's format:
//   https://wiki.ldraw.org/wiki/IO
// miniz has no decryption support, so those entries are decrypted here and
// then handed to miniz's inflate.
// ---------------------------------------------------------------------------
static const char* const IO_ZIP_PASSWORD = "soho0909";

namespace zipcrypto {

static mz_uint32 Crc32Update(mz_uint32 crc, BYTE b)
{
    return mz_crc32(crc ^ 0xFFFFFFFFu, &b, 1) ^ 0xFFFFFFFFu;
}

struct Keys
{
    mz_uint32 k0, k1, k2;

    explicit Keys(const char* password) : k0(305419896u), k1(591751049u), k2(878082192u)
    {
        for (const char* p = password; *p; ++p)
            Update(static_cast<BYTE>(*p));
    }

    void Update(BYTE b)
    {
        k0 = Crc32Update(k0, b);
        k1 = k1 + (k0 & 0xFFu);
        k1 = k1 * 134775813u + 1u;
        k2 = Crc32Update(k2, static_cast<BYTE>(k1 >> 24));
    }

    BYTE StreamByte() const
    {
        mz_uint32 temp = (k2 & 0xFFFFu) | 2u;
        return static_cast<BYTE>(((temp * (temp ^ 1u)) >> 8) & 0xFFu);
    }

    BYTE Decrypt(BYTE c)
    {
        BYTE plain = static_cast<BYTE>(c ^ StreamByte());
        Update(plain);
        return plain;
    }
};

} // namespace zipcrypto

// Decrypt and inflate one encrypted entry. Returns a buffer to free with mz_free.
static HRESULT ExtractEncryptedEntry(const BYTE* zipData, DWORD zipLen,
                                      const mz_zip_archive_file_stat& st,
                                      void** ppOut, size_t* pOutLen)
{
    // Local header: 30 fixed bytes, then filename and extra field.
    const mz_uint64 lho = st.m_local_header_ofs;
    if (lho + 30 > zipLen) return E_FAIL;

    const BYTE* lh = zipData + lho;
    const mz_uint16 nameLen  = static_cast<mz_uint16>(lh[26] | (lh[27] << 8));
    const mz_uint16 extraLen = static_cast<mz_uint16>(lh[28] | (lh[29] << 8));

    const mz_uint64 dataOfs = lho + 30 + nameLen + extraLen;
    const mz_uint64 compLen = st.m_comp_size;
    if (compLen < 12 || dataOfs + compLen > zipLen) return E_FAIL;

    // First 12 bytes are the encryption header, not part of the payload.
    const BYTE*  cipher     = zipData + dataOfs;
    const size_t payloadLen = static_cast<size_t>(compLen) - 12;

    BYTE* plain = static_cast<BYTE*>(CoTaskMemAlloc(payloadLen));
    if (!plain) return E_OUTOFMEMORY;

    zipcrypto::Keys keys(IO_ZIP_PASSWORD);
    for (int i = 0; i < 12; ++i) keys.Decrypt(cipher[i]);
    for (size_t i = 0; i < payloadLen; ++i) plain[i] = keys.Decrypt(cipher[12 + i]);

    void*  out    = nullptr;
    size_t outLen = 0;

    if (st.m_method == 0) {
        out = MZ_MALLOC(payloadLen ? payloadLen : 1);
        if (out) { memcpy(out, plain, payloadLen); outLen = payloadLen; }
    } else {
        out = tinfl_decompress_mem_to_heap(plain, payloadLen, &outLen, 0);
    }

    CoTaskMemFree(plain);
    if (!out) return E_FAIL;

    *ppOut   = out;
    *pOutLen = outLen;
    return S_OK;
}

// ---------------------------------------------------------------------------
// Extract thumbnail.png from an in-memory ZIP, encrypted or not.
// Returns a buffer allocated by miniz (free with mz_free).
// ---------------------------------------------------------------------------
static HRESULT ExtractThumbnailPng(const BYTE* zipData, DWORD zipLen,
                                    void** ppPng, size_t* pPngLen)
{
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, zipData, zipLen, 0))
        return E_FAIL;

    HRESULT hr = E_FAIL;
    int index  = mz_zip_reader_locate_file(&zip, "thumbnail.png", nullptr, 0);

    if (index >= 0) {
        mz_zip_archive_file_stat st{};
        if (mz_zip_reader_file_stat(&zip, static_cast<mz_uint>(index), &st)) {
            if (st.m_bit_flag & 1) {
                hr = ExtractEncryptedEntry(zipData, zipLen, st, ppPng, pPngLen);
            } else {
                void* pngData = mz_zip_reader_extract_to_heap(
                    &zip, static_cast<mz_uint>(index), pPngLen, 0);
                if (pngData) { *ppPng = pngData; hr = S_OK; }
            }
        }
    }

    mz_zip_reader_end(&zip);
    return hr;
}

// ---------------------------------------------------------------------------
// COM object
// ---------------------------------------------------------------------------
class IoThumbnailProvider : public IInitializeWithStream, public IThumbnailProvider
{
public:
    IoThumbnailProvider() : m_cRef(1), m_pStream(nullptr)
    {
        InterlockedIncrement(&g_cDllRef);
    }

    ~IoThumbnailProvider()
    {
        if (m_pStream) m_pStream->Release();
        InterlockedDecrement(&g_cDllRef);
    }

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (IsEqualIID(riid, IID_IUnknown) ||
            IsEqualIID(riid, IID_IInitializeWithStream))
        {
            *ppv = static_cast<IInitializeWithStream*>(this);
        }
        else if (IsEqualIID(riid, IID_IThumbnailProvider))
        {
            *ppv = static_cast<IThumbnailProvider*>(this);
        }
        else
        {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_cRef); }
    IFACEMETHODIMP_(ULONG) Release() override
    {
        LONG c = InterlockedDecrement(&m_cRef);
        if (c == 0) delete this;
        return c;
    }

    // IInitializeWithStream
    IFACEMETHODIMP Initialize(IStream* pStream, DWORD) override
    {
        if (m_pStream) return E_UNEXPECTED;
        m_pStream = pStream;
        m_pStream->AddRef();
        return S_OK;
    }

    // IThumbnailProvider
    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* pdwAlpha) override
    {
        *phbmp    = nullptr;
        *pdwAlpha = WTSAT_UNKNOWN;

        GdiplusSession gdiplus;
        if (gdiplus.status != Gdiplus::Ok) return E_FAIL;

        BYTE* zipData = nullptr;
        DWORD zipLen  = 0;
        HRESULT hr = StreamToBytes(m_pStream, &zipData, &zipLen);
        if (FAILED(hr)) return hr;

        void*  pngData = nullptr;
        size_t pngLen  = 0;
        hr = ExtractThumbnailPng(zipData, zipLen, &pngData, &pngLen);
        CoTaskMemFree(zipData);
        if (FAILED(hr)) return hr;

        IStream* pngStream = SHCreateMemStream(
            static_cast<const BYTE*>(pngData), static_cast<UINT>(pngLen));
        mz_free(pngData);
        if (!pngStream) return E_OUTOFMEMORY;

        Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromStream(pngStream);
        pngStream->Release();

        if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) {
            delete bmp;
            return E_FAIL;
        }

        UINT w = bmp->GetWidth();
        UINT h = bmp->GetHeight();
        UINT newW, newH;
        if (w >= h) {
            newW = cx;
            newH = (h * cx) / max(w, 1u);
        } else {
            newH = cx;
            newW = (w * cx) / max(h, 1u);
        }
        if (newW == 0) newW = 1;
        if (newH == 0) newH = 1;

        Gdiplus::Bitmap* scaled = new Gdiplus::Bitmap(newW, newH, PixelFormat32bppARGB);
        if (scaled->GetLastStatus() != Gdiplus::Ok) {
            delete scaled;
            delete bmp;
            return E_OUTOFMEMORY;
        }

        Gdiplus::Graphics g(scaled);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.DrawImage(bmp, 0, 0, static_cast<INT>(newW), static_cast<INT>(newH));
        delete bmp;

        scaled->GetHBITMAP(Gdiplus::Color(0, 0, 0, 0), phbmp);
        delete scaled;

        *pdwAlpha = WTSAT_ARGB;
        return *phbmp ? S_OK : E_FAIL;
    }

private:
    LONG     m_cRef;
    IStream* m_pStream;
};

// ---------------------------------------------------------------------------
// Class factory
// ---------------------------------------------------------------------------
class IoClassFactory : public IClassFactory
{
public:
    IoClassFactory() : m_cRef(1) {}

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_cRef); }
    IFACEMETHODIMP_(ULONG) Release() override
    {
        LONG c = InterlockedDecrement(&m_cRef);
        if (c == 0) delete this;
        return c;
    }

    IFACEMETHODIMP CreateInstance(IUnknown* pOuter, REFIID riid, void** ppv) override
    {
        *ppv = nullptr;
        if (pOuter) return CLASS_E_NOAGGREGATION;
        auto* p = new IoThumbnailProvider();
        HRESULT hr = p->QueryInterface(riid, ppv);
        p->Release();
        return hr;
    }

    IFACEMETHODIMP LockServer(BOOL lock) override
    {
        if (lock) InterlockedIncrement(&g_cDllRef);
        else      InterlockedDecrement(&g_cDllRef);
        return S_OK;
    }

private:
    LONG m_cRef;
};

// ---------------------------------------------------------------------------
// DLL entry points
// ---------------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInst = hMod;
        DisableThreadLibraryCalls(hMod);
    }
    return TRUE;
}

STDAPI DllCanUnloadNow()
{
    return g_cDllRef == 0 ? S_OK : S_FALSE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv)
{
    *ppv = nullptr;
    if (!IsEqualCLSID(rclsid, CLSID_IoThumbnailProvider))
        return CLASS_E_CLASSNOTAVAILABLE;
    auto* cf = new IoClassFactory();
    HRESULT hr = cf->QueryInterface(riid, ppv);
    cf->Release();
    return hr;
}

// ---------------------------------------------------------------------------
// Self-registration
// ---------------------------------------------------------------------------
STDAPI DllRegisterServer()
{
    wchar_t clsidStr[64];
    wchar_t dllPath[MAX_PATH];
    StringFromGUID2(CLSID_IoThumbnailProvider, clsidStr, 64);
    GetModuleFileNameW(g_hInst, dllPath, MAX_PATH);

    wchar_t key[256];
    HKEY hKey;

    swprintf_s(key, L"CLSID\\%s", clsidStr);
    RegCreateKeyExW(HKEY_CLASSES_ROOT, key, 0, nullptr, 0,
                    KEY_WRITE, nullptr, &hKey, nullptr);
    RegSetValueExW(hKey, nullptr, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(L"Studio IO Thumbnail Provider"),
                   static_cast<DWORD>((wcslen(L"Studio IO Thumbnail Provider") + 1) * 2));
    RegCloseKey(hKey);

    swprintf_s(key, L"CLSID\\%s\\InProcServer32", clsidStr);
    RegCreateKeyExW(HKEY_CLASSES_ROOT, key, 0, nullptr, 0,
                    KEY_WRITE, nullptr, &hKey, nullptr);
    RegSetValueExW(hKey, nullptr, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(dllPath),
                   static_cast<DWORD>((wcslen(dllPath) + 1) * 2));
    RegSetValueExW(hKey, L"ThreadingModel", 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(L"Apartment"),
                   static_cast<DWORD>((wcslen(L"Apartment") + 1) * 2));
    RegCloseKey(hKey);

    RegCreateKeyExW(HKEY_CLASSES_ROOT,
                    L".io\\shellex\\{e357fccd-a995-4576-b01f-234630154e96}",
                    0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr);
    RegSetValueExW(hKey, nullptr, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(clsidStr),
                   static_cast<DWORD>((wcslen(clsidStr) + 1) * 2));
    RegCloseKey(hKey);

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

STDAPI DllUnregisterServer()
{
    wchar_t clsidStr[64];
    StringFromGUID2(CLSID_IoThumbnailProvider, clsidStr, 64);

    wchar_t key[256];
    swprintf_s(key, L"CLSID\\%s", clsidStr);
    SHDeleteKeyW(HKEY_CLASSES_ROOT, key);
    SHDeleteKeyW(HKEY_CLASSES_ROOT,
                 L".io\\shellex\\{e357fccd-a995-4576-b01f-234630154e96}");

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}
