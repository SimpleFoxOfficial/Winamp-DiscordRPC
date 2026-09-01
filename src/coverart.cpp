#include "coverart.h"
#include "util.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <wincrypt.h>
#include <gdiplus.h>

#include <mutex>

namespace coverart {

namespace {

std::mutex  g_gdiMutex;
ULONG_PTR   g_gdiToken = 0;
bool        g_gdiStarted = false;

// Filenames commonly used for cover art kept beside the audio rather than
// embedded in it.
const wchar_t* kAdjacentNames[] = {
    L"cover.jpg",  L"cover.jpeg",  L"cover.png",
    L"folder.jpg", L"folder.jpeg", L"folder.png",
    L"front.jpg",  L"front.jpeg",  L"front.png",
    L"album.jpg",  L"albumart.jpg", L"albumartsmall.jpg",
};

bool GetJpegEncoderClsid(CLSID* clsid) {
    UINT count = 0, bytes = 0;
    Gdiplus::GetImageEncodersSize(&count, &bytes);
    if (bytes == 0) return false;

    std::vector<unsigned char> buffer(bytes);
    Gdiplus::ImageCodecInfo* codecs = (Gdiplus::ImageCodecInfo*)buffer.data();
    if (Gdiplus::GetImageEncoders(count, bytes, codecs) != Gdiplus::Ok) return false;

    for (UINT i = 0; i < count; ++i) {
        if (wcscmp(codecs[i].MimeType, L"image/jpeg") == 0) {
            *clsid = codecs[i].Clsid;
            return true;
        }
    }
    return false;
}

// Scales if needed and writes JPEG bytes into `out`.
bool EncodeToJpeg(Gdiplus::Bitmap* source, int maxEdge, std::vector<unsigned char>* out) {
    if (!source || source->GetLastStatus() != Gdiplus::Ok) return false;

    const UINT w = source->GetWidth();
    const UINT h = source->GetHeight();
    if (w == 0 || h == 0) return false;

    CLSID jpegClsid;
    if (!GetJpegEncoderClsid(&jpegClsid)) return false;

    std::unique_ptr<Gdiplus::Bitmap> scaled;
    Gdiplus::Bitmap* toSave = source;

    if ((int)w > maxEdge || (int)h > maxEdge) {
        const double factor = (double)maxEdge / (double)(w > h ? w : h);
        const int nw = (int)(w * factor + 0.5);
        const int nh = (int)(h * factor + 0.5);

        scaled.reset(new Gdiplus::Bitmap(nw, nh, PixelFormat24bppRGB));
        if (!scaled || scaled->GetLastStatus() != Gdiplus::Ok) return false;

        Gdiplus::Graphics g(scaled.get());
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        // Cover art is opaque; flattening onto black avoids a black-on-black
        // surprise if the source happens to carry an alpha channel.
        g.Clear(Gdiplus::Color(255, 0, 0, 0));
        if (g.DrawImage(source, 0, 0, nw, nh) != Gdiplus::Ok) return false;
        toSave = scaled.get();
    }

    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || !stream) return false;

    ULONG quality = 88;
    Gdiplus::EncoderParameters params;
    params.Count = 1;
    params.Parameter[0].Guid           = Gdiplus::EncoderQuality;
    params.Parameter[0].Type           = Gdiplus::EncoderParameterValueTypeLong;
    params.Parameter[0].NumberOfValues = 1;
    params.Parameter[0].Value          = &quality;

    const bool saved = (toSave->Save(stream, &jpegClsid, &params) == Gdiplus::Ok);

    bool ok = false;
    if (saved) {
        HGLOBAL hg = nullptr;
        if (SUCCEEDED(GetHGlobalFromStream(stream, &hg)) && hg) {
            const SIZE_T size = GlobalSize(hg);
            void* data = GlobalLock(hg);
            if (data && size > 0) {
                out->assign((unsigned char*)data, (unsigned char*)data + size);
                ok = true;
            }
            if (data) GlobalUnlock(hg);
        }
    }
    stream->Release();
    return ok;
}

// Asks the shell for the file's thumbnail, which for audio files is the
// embedded cover art.
bool TryShellThumbnail(const std::wstring& file, int maxEdge, std::vector<unsigned char>* out) {
    IShellItemImageFactory* factory = nullptr;
    if (FAILED(SHCreateItemFromParsingName(file.c_str(), nullptr, IID_PPV_ARGS(&factory))) ||
        !factory) {
        return false;
    }

    // THUMBNAILONLY matters: without it the shell happily returns the generic
    // file-type icon, which we would otherwise publish as "album art".
    SIZE size = { maxEdge, maxEdge };
    HBITMAP bmp = nullptr;
    const HRESULT hr = factory->GetImage(size, SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK, &bmp);
    factory->Release();

    if (FAILED(hr) || !bmp) return false;

    bool ok = false;
    {
        std::unique_ptr<Gdiplus::Bitmap> bitmap(Gdiplus::Bitmap::FromHBITMAP(bmp, nullptr));
        ok = EncodeToJpeg(bitmap.get(), maxEdge, out);
    }
    DeleteObject(bmp);
    return ok;
}

bool TryAdjacentImage(const std::wstring& audioFile, int maxEdge, std::vector<unsigned char>* out) {
    wchar_t dir[MAX_PATH];
    wcscpy_s(dir, _countof(dir), audioFile.c_str());
    if (!PathRemoveFileSpecW(dir)) return false;

    for (const wchar_t* name : kAdjacentNames) {
        wchar_t candidate[MAX_PATH];
        if (!PathCombineW(candidate, dir, name)) continue;
        if (GetFileAttributesW(candidate) == INVALID_FILE_ATTRIBUTES) continue;

        std::unique_ptr<Gdiplus::Bitmap> bitmap(Gdiplus::Bitmap::FromFile(candidate));
        if (EncodeToJpeg(bitmap.get(), maxEdge, out)) {
            util::Log(L"using adjacent cover %s", name);
            return true;
        }
    }
    return false;
}

} // namespace

void Startup() {
    std::lock_guard<std::mutex> lock(g_gdiMutex);
    if (g_gdiStarted) return;
    Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&g_gdiToken, &input, nullptr) == Gdiplus::Ok) {
        g_gdiStarted = true;
    }
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_gdiMutex);
    if (!g_gdiStarted) return;
    Gdiplus::GdiplusShutdown(g_gdiToken);
    g_gdiStarted = false;
    g_gdiToken   = 0;
}

bool ExtractJpeg(const std::wstring& audioFile, int maxEdge, std::vector<unsigned char>* jpeg) {
    jpeg->clear();
    if (audioFile.empty()) return false;
    if (GetFileAttributesW(audioFile.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    if (TryShellThumbnail(audioFile, maxEdge, jpeg)) return true;
    return TryAdjacentImage(audioFile, maxEdge, jpeg);
}

bool LoadImageAsJpeg(const std::wstring& imageFile, int maxEdge, std::vector<unsigned char>* jpeg) {
    jpeg->clear();
    if (imageFile.empty()) return false;
    if (GetFileAttributesW(imageFile.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    std::unique_ptr<Gdiplus::Bitmap> bitmap(Gdiplus::Bitmap::FromFile(imageFile.c_str()));
    return EncodeToJpeg(bitmap.get(), maxEdge, jpeg);
}

std::string HashBytes(const std::vector<unsigned char>& bytes) {
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash     = 0;
    std::string result;

    if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_FULL,
                              CRYPT_VERIFYCONTEXT)) {
        return result;
    }
    if (CryptCreateHash(provider, CALG_SHA1, 0, 0, &hash)) {
        if (CryptHashData(hash, bytes.data(), (DWORD)bytes.size(), 0)) {
            BYTE digest[20] = {0};
            DWORD len = sizeof(digest);
            if (CryptGetHashParam(hash, HP_HASHVAL, digest, &len, 0)) {
                char hex[41];
                for (DWORD i = 0; i < len; ++i) sprintf_s(hex + i * 2, 3, "%02x", digest[i]);
                result.assign(hex, len * 2);
            }
        }
        CryptDestroyHash(hash);
    }
    CryptReleaseContext(provider, 0);
    return result;
}

} // namespace coverart
