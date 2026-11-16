/*
 * rtsp_message.cpp
 * Vedi rtsp_message.h.
 */
#include "rtsp_message.h"

#include <cctype>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>

#include "net_util.h"
#include "rust_core.h"

namespace irplay {

    const std::string* RtspRequest::header(std::string_view name) const {
        for (const auto& [k, v] : headers) if (rtsp::iequals(k, name)) return &v;
        return nullptr;
    }

    namespace rtsp {

        namespace {

            constexpr const char* kServerHeader = "AirTunes/220.68";
            constexpr size_t kMaxHeaders = 64;

        } // namespace

        bool iequals(std::string_view a, std::string_view b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i) {
                char x = a[i], y = b[i];
                if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + 32);
                if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + 32);
                if (x != y) return false;
            }
            return true;
        }

        std::string_view trim(std::string_view s) {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
            return s;
        }

        bool isDigits(std::string_view s, size_t maxLen) {
            if (s.empty() || s.size() > maxLen) return false;
            for (char c : s) if (c < '0' || c > '9') return false;
            return true;
        }

        // I byte di rete li legge Rust (rust/src/rtsp.rs), con gli stessi limiti di
        // rtsp_message.h: qui arrivano solo posizioni nel buffer, controllate di nuovo.
        int parseRequest(std::vector<uint8_t>& buf, RtspRequest& req) {
            IrplayRtspHead h{};
            IrplaySpan hs[2 * kMaxHeaders];
            const int r = irplay_rtsp_parse_request(buf.data(), buf.size(), &h, hs, 2 * kMaxHeaders);
            if (r != 1) return r;
            const size_t limit = h.consumed;
            if (limit > buf.size() || h.header_count > kMaxHeaders) return -1;
            bool ok = true;
            auto str = [&](IrplaySpan s) {
                if (s.off > limit || s.len > limit - s.off) { ok = false; return std::string(); }
                return std::string(reinterpret_cast<const char*>(buf.data()) + s.off, s.len);
            };
            req = RtspRequest{};
            req.method = str(h.method);
            req.uri = str(h.uri);
            req.protocol = str(h.protocol);
            for (size_t i = 0; i < h.header_count; ++i) req.headers.emplace_back(str(hs[2 * i]), str(hs[2 * i + 1]));
            const std::string body = str(h.body);
            if (!ok) return -1;
            req.body.assign(body.begin(), body.end());
            buf.erase(buf.begin(), buf.begin() + static_cast<long>(limit));
            return 1;
        }

        int discardResponse(std::vector<uint8_t>& buf) {
            size_t consumed = 0;
            const int r = irplay_rtsp_discard_response(buf.data(), buf.size(), &consumed);
            if (r != 1) return r;
            if (consumed > buf.size()) return -1;
            buf.erase(buf.begin(), buf.begin() + static_cast<long>(consumed));
            return 1;
        }

        bool sendAll(int fd, const void* data, size_t len) {
            const auto* p = static_cast<const uint8_t*>(data);
            while (len > 0) {
                const ssize_t n = ::send(fd, p, len, net::kSendFlags);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    return false;
                }
                p += n;
                len -= static_cast<size_t>(n);
            }
            return true;
        }

        bool sendAllTimed(int fd, const void* data, size_t len, int timeoutMs) {
            using Clock = std::chrono::steady_clock;
            const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
            const auto* p = static_cast<const uint8_t*>(data);
            while (len > 0) {
                const ssize_t n = ::send(fd, p, len, net::kSendFlags | MSG_DONTWAIT);
                if (n >= 0) {
                    p += n;
                    len -= static_cast<size_t>(n);
                    continue;
                }
                if (errno == EINTR) continue;
                if (errno != EAGAIN && errno != EWOULDBLOCK) return false;
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
                if (left <= 0) return false;
                pollfd pf{fd, POLLOUT, 0};
                if (::poll(&pf, 1, static_cast<int>(left)) <= 0) return false;
            }
            return true;
        }

        bool sendResponse(int fd, const RtspRequest* req, int code, const char* reason,
                          const Headers& extra, const std::vector<uint8_t>& body) {
            const std::string proto = req ? req->protocol : "RTSP/1.0";
            std::string out = proto + " " + std::to_string(code) + " " + reason + "\r\n";
            if (req) {
                const std::string* cseq = req->header("CSeq");
                if (cseq && isDigits(*cseq, 10)) out += "CSeq: " + *cseq + "\r\n";
            }
            out += std::string("Server: ") + kServerHeader + "\r\n";
            for (const auto& [k, v] : extra) out += k + ": " + v + "\r\n";
            // 101 (passaggio al canale inverso) non ha corpo né Content-Length.
            if (code != 101) out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
            out += "\r\n";
            if (!sendAll(fd, out.data(), out.size())) return false;
            return body.empty() || sendAll(fd, body.data(), body.size());
        }

    } // namespace rtsp

} // namespace irplay
