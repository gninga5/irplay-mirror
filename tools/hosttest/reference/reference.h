/*
 * Parser C++ di riferimento (versione dell'app prima del passaggio a Rust), per il
 * confronto in tools/hosttest/parser_diff.cpp. Stesse firme delle funzioni dell'app.
 */
#pragma once

#include <optional>

#include "hls_playlist.h"
#include "now_playing.h"
#include "plist_reader.h"
#include "rtsp_message.h"

namespace ref::plist {
    std::optional<irplay::plist::Value> parseBinary(const uint8_t* data, size_t len);
}
namespace ref::nowplaying {
    bool parseDmap(const uint8_t* data, size_t len, irplay::nowplaying::Metadata& out);
    bool parseProgressRtp(const uint8_t* data, size_t len, uint32_t& start, uint32_t& current, uint32_t& end);
    bool progressMs(uint32_t start, uint32_t current, uint32_t end, int64_t& positionMs, int64_t& durationMs);
    bool parseProgress(const uint8_t* data, size_t len, int64_t& positionMs, int64_t& durationMs);
}
namespace ref::hls {
    bool isPlaylist(std::string_view text);
    bool isSafeHttpUrl(std::string_view url);
    std::optional<std::string> remotePrefixOf(std::string_view location);
    std::optional<irplay::hls::Master> rewriteMaster(std::string_view text, std::string_view remotePrefix);
    std::string toLocal(std::string_view remoteUri, std::string_view remotePrefix);
    std::optional<std::string> expandCondensed(std::string_view media);
}
namespace ref::rtsp {
    int parseRequest(std::vector<uint8_t>& buf, irplay::RtspRequest& req);
    int discardResponse(std::vector<uint8_t>& buf);
}
