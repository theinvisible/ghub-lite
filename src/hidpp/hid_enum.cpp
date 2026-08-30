#include "hidpp/hid_enum.h"

// INITGUID vor devpkey.h: sonst sind die DEVPROPKEYs nur deklariert, nicht definiert
// (LNK2019 auf DEVPKEY_Device_InstanceId).
#define INITGUID
#include <initguid.h>
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devpkey.h>

extern "C" {
#include <hidsdi.h>
}

#include <algorithm>
#include <map>

namespace hidpp {
namespace {

// RAII fuer HDEVINFO und HANDLE, damit die Fehlerpfade unten geradeaus bleiben.
struct DevInfoGuard {
    HDEVINFO h;
    ~DevInfoGuard() { if (h != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(h); }
};
struct HandleGuard {
    HANDLE h;
    ~HandleGuard() { if (h != INVALID_HANDLE_VALUE && h != nullptr) CloseHandle(h); }
};

// Instanz-ID des USB-Interface-Knotens ueber dem HID-Interface. Beide Collections
// (Col01/Col02) haengen am selben Elternknoten -- das ist der einzige verlaessliche
// Weg, sie einander zuzuordnen. Ein Vergleich der Pfad-Praefixe waere Raterei.
std::wstring parent_instance_id(const std::wstring& interface_path) {
    DEVPROPTYPE type = 0;
    ULONG size = 0;
    CM_Get_Device_Interface_PropertyW(interface_path.c_str(), &DEVPKEY_Device_InstanceId,
                                      &type, nullptr, &size, 0);
    if (size == 0) return {};

    std::wstring instance_id(size / sizeof(wchar_t), L'\0');
    if (CM_Get_Device_Interface_PropertyW(interface_path.c_str(), &DEVPKEY_Device_InstanceId,
                                          &type, reinterpret_cast<PBYTE>(instance_id.data()),
                                          &size, 0) != CR_SUCCESS) {
        return {};
    }
    instance_id.resize(wcslen(instance_id.c_str()));

    DEVINST devinst = 0;
    if (CM_Locate_DevNodeW(&devinst, instance_id.data(), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
        return {};

    DEVINST parent = 0;
    if (CM_Get_Parent(&parent, devinst, 0) != CR_SUCCESS) return {};

    ULONG len = 0;
    if (CM_Get_Device_ID_Size(&len, parent, 0) != CR_SUCCESS) return {};

    std::wstring parent_id(len + 1, L'\0');
    if (CM_Get_Device_IDW(parent, parent_id.data(), len + 1, 0) != CR_SUCCESS) return {};
    parent_id.resize(wcslen(parent_id.c_str()));
    return parent_id;
}

} // namespace

std::vector<HidInterface> enumerate(uint16_t vid_filter) {
    std::vector<HidInterface> out;

    GUID hid_guid{};
    HidD_GetHidGuid(&hid_guid);

    DevInfoGuard set{SetupDiGetClassDevsW(&hid_guid, nullptr, nullptr,
                                          DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)};
    if (set.h == INVALID_HANDLE_VALUE) return out;

    SP_DEVICE_INTERFACE_DATA iface{};
    iface.cbSize = sizeof(iface);

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set.h, nullptr, &hid_guid, i, &iface); ++i) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(set.h, &iface, nullptr, 0, &needed, nullptr);
        if (needed == 0) continue;

        std::vector<uint8_t> buf(needed);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set.h, &iface, detail, needed, nullptr, nullptr))
            continue;

        // Nur lesende Abfrage der Eigenschaften: Zugriffsmaske 0. Damit laesst sich auch
        // ein exklusiv belegtes Geraet (System-Maus, oder eines, das G HUB haelt) noch
        // befragen -- CreateFile mit GENERIC_READ wuerde hier scheitern.
        HandleGuard dev{CreateFileW(detail->DevicePath, 0,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, 0, nullptr)};
        if (dev.h == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attrs{};
        attrs.Size = sizeof(attrs);
        if (!HidD_GetAttributes(dev.h, &attrs)) continue;
        if (vid_filter != 0 && attrs.VendorID != vid_filter) continue;

        PHIDP_PREPARSED_DATA prep = nullptr;
        if (!HidD_GetPreparsedData(dev.h, &prep)) continue;

        HIDP_CAPS caps{};
        const bool caps_ok = HidP_GetCaps(prep, &caps) == HIDP_STATUS_SUCCESS;
        HidD_FreePreparsedData(prep);
        if (!caps_ok) continue;

        HidInterface hi;
        hi.path       = detail->DevicePath;
        hi.vendor_id  = attrs.VendorID;
        hi.product_id = attrs.ProductID;
        hi.usage_page = caps.UsagePage;
        hi.usage      = caps.Usage;
        hi.input_len  = caps.InputReportByteLength;
        hi.output_len = caps.OutputReportByteLength;

        wchar_t product[128] = {};
        if (HidD_GetProductString(dev.h, product, sizeof(product)))
            hi.product = product;

        hi.parent_id = parent_instance_id(hi.path);
        out.push_back(std::move(hi));
    }

    return out;
}

std::vector<HidppEndpoint> find_endpoints(uint16_t vid_filter) {
    std::map<std::wstring, HidppEndpoint> grouped;

    for (auto& hi : enumerate(vid_filter)) {
        if (hi.usage_page < kUsagePageVendorFirst) continue;
        if (hi.parent_id.empty()) continue;

        // Nach Reportlaenge einsortieren, nicht nach Usage: die Usage-Nummern
        // unterscheiden sich zwischen 0xFF00 (Maus) und 0xFF43 (Tastatur), die Laengen
        // nicht. Collections mit anderer Laenge sind kein HID++ und fallen durch.
        auto& ep = grouped[hi.parent_id];
        switch (hi.input_len) {
            case kLenShort:    ep.short_col     = hi; break;
            case kLenLong:     ep.long_col      = hi; break;
            case kLenVeryLong: ep.very_long_col = hi; break;
            default: break;
        }
    }

    std::vector<HidppEndpoint> out;
    for (auto& [parent, ep] : grouped) {
        if (ep.long_col.path.empty()) continue;   // ohne Long-Collection kein HID++ 2.0
        out.push_back(ep);
    }
    return out;
}

} // namespace hidpp
