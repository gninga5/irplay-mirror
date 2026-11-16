/*
 * Versione C++ di riferimento del buffer di riordino audio, com'era in media_streams.cpp
 * prima del passaggio a Rust (fase 6): stessa logica, con le azioni (riproduci, chiedi di
 * nuovo) registrate in una lista invece che eseguite. Solo per parser_diff.cpp.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ref::audio {

    struct Action {
        int kind;          // 1 riproduci, 2 chiedi di nuovo
        uint32_t a, b;     // rtp | primo, quanti
        std::vector<uint8_t> data;
        bool operator==(const Action&) const = default;
    };

    class Reorder {
    public:
        void insert(uint16_t seq, uint32_t rtp, const uint8_t* payload, size_t len);
        void drain(int64_t now, std::vector<Action>& out);
        void flush(int nextSeq);
        bool acceptLive(uint16_t seq);
        size_t buffered() const { return buffered_; }
        uint64_t lost() const { return statLost_; }
        uint64_t resendAsked() const { return statResendAsked_; }

    private:
        struct Slot { bool used = false; uint16_t seq = 0; uint32_t rtp = 0; std::vector<uint8_t> data; };
        static constexpr size_t kSlots = 256;
        void resetBuffer();
        bool haveSeq_ = false;
        uint16_t lastSeq_ = 0, expected_ = 0;
        std::array<Slot, kSlots> slots_;
        size_t buffered_ = 0;
        int64_t gapSinceMs_ = 0;
        uint64_t statLost_ = 0, statResendAsked_ = 0;
    };

    bool parseRtp(const uint8_t* buf, size_t len, uint16_t& seq, uint32_t& rtp);
    int classifyControl(const uint8_t* buf, size_t n, uint32_t& rtp);   // 0 altro, 1 ritrasmissione, 2 sync

} // namespace ref::audio
