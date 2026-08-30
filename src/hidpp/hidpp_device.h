// HID++-2.0-Transport ueber ein Collection-Paar.
//
// Rahmenformat:
//   short (ID 0x10,  7 Byte): 10 | devIdx | featIdx | funcId<<4|swId | p0 p1 p2
//   long  (ID 0x11, 20 Byte): 11 | devIdx | featIdx | funcId<<4|swId | p0 ... p15
//
// Gesendet wird ausschliesslich long -- HID++-2.0-Geraete beantworten jede Funktion auch
// ueber den Long-Kanal. Gelauscht wird auf beiden Collections, weil manche Geraete kurze
// Antworten auf der Short-Collection zustellen.
#pragma once

#include "hidpp/hid_enum.h"

#include <array>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hidpp {

inline constexpr uint8_t kReportShort    = 0x10;
inline constexpr uint8_t kReportLong     = 0x11;
inline constexpr uint8_t kReportVeryLong = 0x12;

// swId 0 ist reserviert; jede andere 4-Bit-Kennung darf die Software frei waehlen.
inline constexpr uint8_t kSwId = 0x0A;

// 0xFF adressiert den Empfaenger selbst bzw. ein direkt angeschlossenes Geraet.
// 1..6 sind die am Empfaenger gepaarten Geraete.
inline constexpr uint8_t kIndexDirect   = 0xFF;
inline constexpr uint8_t kMaxPairedSlot = 6;

// Feature 0x0000 (IRoot) liegt per Definition immer auf Index 0.
inline constexpr uint8_t kRootIndex = 0x00;

enum class Status : uint8_t {
    Ok,
    Timeout,        // Geraet hat nicht geantwortet (haeufig: Funkgeraet schlaeft)
    IoError,
    DeviceError,    // Geraet hat eine Fehlerantwort geschickt, siehe error_code
    NotOpen,
};

struct Reply {
    Status  status      = Status::NotOpen;
    uint8_t error_code  = 0;      // nur bei DeviceError
    bool    legacy_error = false; // Fehlerantwort im HID++-1.0-Format (0x8F)
    std::array<uint8_t, 20> raw{};
    size_t  raw_len = 0;

    const uint8_t* params() const { return raw.data() + 4; }
    size_t param_len() const { return raw_len > 4 ? raw_len - 4 : 0; }
    uint8_t param(size_t i) const { return i < param_len() ? raw[4 + i] : 0; }
    uint16_t param_u16(size_t i) const {
        return static_cast<uint16_t>((param(i) << 8) | param(i + 1));
    }
    explicit operator bool() const { return status == Status::Ok; }
};

const wchar_t* status_text(Status s);
const wchar_t* error_text(uint8_t code, bool legacy);
std::wstring describe(const Reply& r);
std::wstring hex_dump(const uint8_t* data, size_t len);

// Ein offener HID++-Kanal.
//
// Ein eigener Verteiler-Thread liest dauerhaft auf allen offenen Collections und sortiert
// jede eintreffende Meldung ein: passt sie zu einer wartenden Anfrage, bekommt sie der
// Wartende, sonst geht sie an den Meldungs-Handler. Ohne diesen Dauerbetrieb gingen
// unaufgeforderte Meldungen -- G-Tastendruecke, Akkustand -- verloren, weil zwischen zwei
// Anfragen niemand liest.
//
// Die Unterscheidung ist verlaesslich: Antworten tragen swId 0x0A, unaufgeforderte
// Meldungen swId 0.
class Channel {
public:
    // Wird auf dem Verteiler-Thread aufgerufen, nicht auf dem des Aufrufers.
    using NotificationHandler = std::function<void(const uint8_t* data, size_t len)>;

    Channel() = default;
    ~Channel();
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    bool open(const HidppEndpoint& ep, std::wstring* error_out = nullptr);
    void close();
    bool is_open() const { return long_h_ != nullptr; }

    const HidppEndpoint& endpoint() const { return ep_; }

    void set_notification_handler(NotificationHandler fn);

    // Eine Anfrage senden und auf die zugehoerige Antwort warten.
    Reply call(uint8_t dev_index, uint8_t feat_index, uint8_t func_id,
               const uint8_t* params, size_t nparams, unsigned timeout_ms = 600);

    Reply call(uint8_t dev_index, uint8_t feat_index, uint8_t func_id,
               std::initializer_list<uint8_t> params, unsigned timeout_ms = 600) {
        return call(dev_index, feat_index, func_id, params.begin(), params.size(), timeout_ms);
    }

private:
    struct Waiter;
    void dispatch_loop();
    bool deliver(const uint8_t* data, size_t len);   // true, wenn einem Wartenden zugestellt

    HidppEndpoint ep_{};
    void* long_h_      = nullptr;   // HANDLE
    void* short_h_     = nullptr;   // HANDLE, optional
    void* very_long_h_ = nullptr;   // HANDLE, optional
    void* stop_event_  = nullptr;   // HANDLE

    std::thread dispatcher_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::vector<Waiter*> waiters_;
    NotificationHandler on_notification_;
};

} // namespace hidpp
