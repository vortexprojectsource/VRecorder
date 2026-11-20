#include "EncoderManager.hpp"

#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <algorithm>
#include <set>

using Microsoft::WRL::ComPtr;

namespace vrec {

namespace {

std::wstring CleanEncoderDisplayName(const std::wstring& raw, bool isHw, bool isHevc) {
    if (raw.find(L"NVIDIA") != std::wstring::npos) {
        return isHevc ? L"NVIDIA NVENC HEVC (H.265)" : L"NVIDIA NVENC H.264";
    }
    if (raw.find(L"Intel") != std::wstring::npos) {
        return isHevc ? L"Intel Quick Sync HEVC (H.265)" : L"Intel Quick Sync H.264";
    }
    if (raw.find(L"AMD") != std::wstring::npos || raw.find(L"Radeon") != std::wstring::npos) {
        return isHevc ? L"AMD AMF HEVC (H.265)" : L"AMD AMF H.264";
    }
    if (raw == L"H264 Encoder MFT" || !isHw) {
        return isHevc ? L"Software CPU HEVC" : L"Microsoft CPU H.264";
    }

    std::wstring cleaned;
    cleaned.reserve(raw.size());
    for (wchar_t ch : raw) {
        if (ch != L'\u00AE' && ch != L'\u2122') cleaned.push_back(ch);
    }
    return cleaned;
}

}

std::vector<DetectedEncoder> EncoderManager::EnumerateAvailableEncoders() {
    std::vector<DetectedEncoder> out;

    DetectedEncoder autoEntry{};
    autoEntry.id = L"auto";
    autoEntry.friendlyName = L"Auto (Best Available)";
    autoEntry.badge = L"AUTO";
    out.push_back(std::move(autoEntry));

    IMFActivate** ppActivate = nullptr;
    UINT32 count = 0;
    const UINT32 flags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT |
                         MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_LOCALMFT |
                         MFT_ENUM_FLAG_SORTANDFILTER;

    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, nullptr, nullptr, &ppActivate, &count)) || !ppActivate) {
        return out;
    }

    std::set<std::wstring> seenClsids;
    for (UINT32 i = 0; i < count; ++i) {
        if (!ppActivate[i]) continue;

        CLSID clsid = GUID_NULL;
        if (FAILED(ppActivate[i]->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &clsid))) {
            ppActivate[i]->Release();
            continue;
        }

        wchar_t guidStr[64]{};
        StringFromGUID2(clsid, guidStr, 64);
        std::wstring idStr(guidStr);
        if (seenClsids.count(idStr) > 0) {
            ppActivate[i]->Release();
            continue;
        }

        UINT8* blob = nullptr;
        UINT32 bSize = 0;
        bool supportsH264 = false;
        bool supportsHevc = false;
        if (SUCCEEDED(ppActivate[i]->GetAllocatedBlob(MFT_OUTPUT_TYPES_Attributes, &blob, &bSize)) &&
            blob && bSize >= sizeof(MFT_REGISTER_TYPE_INFO)) {
            const auto* infos = reinterpret_cast<const MFT_REGISTER_TYPE_INFO*>(blob);
            const UINT32 nTypes = bSize / sizeof(MFT_REGISTER_TYPE_INFO);
            for (UINT32 t = 0; t < nTypes; ++t) {
                if (IsEqualGUID(infos[t].guidSubtype, MFVideoFormat_H264)) supportsH264 = true;
                if (IsEqualGUID(infos[t].guidSubtype, MFVideoFormat_HEVC)) supportsHevc = true;
            }
        }
        if (blob) CoTaskMemFree(blob);

        if (!supportsH264 && !supportsHevc) {
            ppActivate[i]->Release();
            continue;
        }

        wchar_t rawName[256]{};
        UINT32 nLen = 0;
        ppActivate[i]->GetString(MFT_FRIENDLY_NAME_Attribute, rawName, 256, &nLen);

        UINT32 mftFlags = 0;
        ppActivate[i]->GetUINT32(MF_TRANSFORM_FLAGS_Attribute, &mftFlags);
        const bool isHw = (mftFlags & MFT_ENUM_FLAG_HARDWARE) != 0;

        UINT64 luidVal = 0;
        const bool hasLuid = SUCCEEDED(ppActivate[i]->GetUINT64(MFT_ENUM_ADAPTER_LUID, &luidVal));

        seenClsids.insert(idStr);

        DetectedEncoder de{};
        de.id = idStr;
        de.clsid = clsid;
        de.isHardware = isHw;
        de.isHevc = !supportsH264 && supportsHevc;
        de.subtype = de.isHevc ? MFVideoFormat_HEVC : MFVideoFormat_H264;
        de.rawName = rawName;
        de.friendlyName = CleanEncoderDisplayName(de.rawName, isHw, de.isHevc);
        de.badge = std::wstring(isHw ? L"GPU · " : L"CPU · ") + (de.isHevc ? L"HEVC" : L"H.264");
        de.hasLuid = hasLuid;
        de.adapterLuid = luidVal;
        out.push_back(std::move(de));

        ppActivate[i]->Release();
    }
    CoTaskMemFree(ppActivate);

    auto vendorRank = [](const DetectedEncoder& e) -> int {
        const std::wstring& s = e.rawName.empty() ? e.friendlyName : e.rawName;
        if (s.find(L"NVIDIA") != std::wstring::npos || e.friendlyName.find(L"NVIDIA") != std::wstring::npos) return 0;
        if (s.find(L"AMD") != std::wstring::npos || s.find(L"Radeon") != std::wstring::npos ||
            e.friendlyName.find(L"AMD") != std::wstring::npos) return 1;
        if (s.find(L"Intel") != std::wstring::npos || e.friendlyName.find(L"Intel") != std::wstring::npos) return 2;
        return 3;
    };
    if (out.size() > 2) {
        std::sort(out.begin() + 1, out.end(), [&](const DetectedEncoder& a, const DetectedEncoder& b) {
            if (a.isHevc != b.isHevc) return !a.isHevc;
            if (a.isHardware != b.isHardware) return a.isHardware;
            const int ra = vendorRank(a), rb = vendorRank(b);
            if (ra != rb) return ra < rb;
            return a.friendlyName < b.friendlyName;
        });
    }
    return out;
}

DetectedEncoder EncoderManager::ResolveSelection(const std::vector<DetectedEncoder>& list, const std::wstring& encoderId) {
    if (!encoderId.empty() && _wcsicmp(encoderId.c_str(), L"auto") != 0) {
        for (const auto& e : list) {
            if (_wcsicmp(e.id.c_str(), encoderId.c_str()) == 0) {
                return e;
            }
        }
    }

    for (const auto& e : list) {
        if (e.id != L"auto" && e.isHardware && !e.isHevc &&
            e.friendlyName.find(L"NVIDIA") != std::wstring::npos) return e;
    }
    for (const auto& e : list) {
        if (e.id != L"auto" && e.isHardware && !e.isHevc) return e;
    }
    for (const auto& e : list) {
        if (e.id != L"auto" && !e.isHevc) return e;
    }
    if (list.size() > 1) return list[1];
    return list.empty() ? DetectedEncoder{} : list.front();
}

std::vector<EncoderPreset> EncoderManager::GetPresetsForEncoder(const DetectedEncoder& enc) {
    std::wstring vendor = L"CPU";
    const std::wstring& ref = enc.rawName.empty() ? enc.friendlyName : enc.rawName;
    if (ref.find(L"NVIDIA") != std::wstring::npos || enc.friendlyName.find(L"NVIDIA") != std::wstring::npos) {
        vendor = L"NVIDIA";
    } else if (ref.find(L"Intel") != std::wstring::npos || enc.friendlyName.find(L"Intel") != std::wstring::npos) {
        vendor = L"Intel QSV";
    } else if (ref.find(L"AMD") != std::wstring::npos || ref.find(L"Radeon") != std::wstring::npos ||
               enc.friendlyName.find(L"AMD") != std::wstring::npos) {
        vendor = L"AMD AMF";
    }

    if (vendor == L"NVIDIA") {
        if (enc.isHevc) {
            return {
                {L"fastest",  L"NVENC HEVC \u00B7 High-FPS Capture (0 B-Frame)", L"120Hz Fast", 10, 76, 0, true,  0, 0.90},
                {L"balanced", L"NVENC HEVC \u00B7 Smart Compression (CBR)",       L"Efficient",  55, 86, 0, false, 0, 1.05},
                {L"quality",  L"NVENC HEVC \u00B7 Cinema Sharp (HQ)",             L"Cinema HQ",  85, 92, 2, false, 2, 1.35},
                {L"studio",   L"NVENC HEVC \u00B7 Archival Master (P7)",          L"Archival",  100, 97, 2, false, 2, 1.70}
            };
        }
        return {
            {L"fastest",  L"NVENC H.264 \u00B7 P1 Ultra-Low Latency", L"P1 Turbo", 0,  76, 0, true,  0, 1.00},
            {L"balanced", L"NVENC H.264 \u00B7 P4 Balanced (CBR)",    L"P4 Bal",   50, 85, 0, false, 0, 1.10},
            {L"quality",  L"NVENC H.264 \u00B7 P6 Quality (2-B VBR)", L"P6 Qual",  85, 92, 2, false, 2, 1.35},
            {L"studio",   L"NVENC H.264 \u00B7 P7 Studio Master",     L"P7 Master",100,97, 2, false, 2, 1.75}
        };
    }
    if (vendor == L"Intel QSV") {
        if (enc.isHevc) {
            return {
                {L"fastest",  L"Intel QSV HEVC \u00B7 Fast Media Engine",  L"QSV Fast",  10, 76, 0, true,  0, 0.90},
                {L"balanced", L"Intel QSV HEVC \u00B7 Balanced Sharp",     L"QSV Bal",   55, 86, 0, false, 0, 1.05},
                {L"quality",  L"Intel QSV HEVC \u00B7 High Efficiency",    L"QSV Eff",   85, 92, 2, false, 2, 1.30},
                {L"studio",   L"Intel QSV HEVC \u00B7 Archival HEVC",      L"QSV HQ",   100, 96, 2, false, 2, 1.60}
            };
        }
        return {
            {L"fastest",  L"Intel QSV AVC \u00B7 TU7 Speed (Low Lag)", L"TU7 Speed", 5,  74, 0, true,  0, 1.00},
            {L"balanced", L"Intel QSV AVC \u00B7 TU4 Balanced (CBR)",  L"TU4 Bal",   50, 84, 0, false, 0, 1.10},
            {L"quality",  L"Intel QSV AVC \u00B7 TU1 High Quality",    L"TU1 Qual",  85, 91, 2, false, 2, 1.35},
            {L"studio",   L"Intel QSV AVC \u00B7 Studio Mastering",    L"TU1 Master",100,96, 2, false, 2, 1.65}
        };
    }
    if (vendor == L"AMD AMF") {
        if (enc.isHevc) {
            return {
                {L"fastest",  L"AMD AMF HEVC \u00B7 Low Latency Speed", L"AMF Fast", 10, 76, 0, true,  0, 0.90},
                {L"balanced", L"AMD AMF HEVC \u00B7 Balanced Crisp",    L"AMF Bal",  55, 86, 0, false, 0, 1.05},
                {L"quality",  L"AMD AMF HEVC \u00B7 Cinema Quality",    L"AMF HQ",   88, 92, 2, false, 2, 1.30},
                {L"studio",   L"AMD AMF HEVC \u00B7 Studio Master",     L"AMF Max", 100, 96, 2, false, 2, 1.60}
            };
        }
        return {
            {L"fastest",  L"AMD AMF AVC \u00B7 Gaming Speed",     L"AMF Speed",  10, 74, 0, true,  0, 1.00},
            {L"balanced", L"AMD AMF AVC \u00B7 Balanced (CBR)",   L"AMF Bal",    50, 84, 0, false, 0, 1.10},
            {L"quality",  L"AMD AMF AVC \u00B7 Visual Quality",   L"AMF Qual",   85, 91, 2, false, 2, 1.35},
            {L"studio",   L"AMD AMF AVC \u00B7 Studio High VBR",  L"AMF Studio",100, 96, 2, false, 2, 1.65}
        };
    }
    return {
        {L"fastest",  L"CPU \u00B7 Ultrafast (0-Delay)",   L"Ultrafast", 0,  72, 0, true,  0, 1.00},
        {L"balanced", L"CPU \u00B7 Standard Balanced",     L"Balanced",  50, 84, 0, false, 0, 1.10},
        {L"quality",  L"CPU \u00B7 High Detail (Multi-T)", L"Quality",   88, 92, 1, false, 2, 1.35}
    };
}

EncoderPreset EncoderManager::ResolvePreset(const DetectedEncoder& enc, const std::wstring& presetId) {
    const auto presets = GetPresetsForEncoder(enc);
    for (const auto& p : presets) {
        if (!presetId.empty() && _wcsicmp(p.id.c_str(), presetId.c_str()) == 0) {
            return p;
        }
    }
    for (const auto& p : presets) {
        if (p.id == L"balanced") return p;
    }
    return presets.empty() ? EncoderPreset{} : presets.front();
}

}
