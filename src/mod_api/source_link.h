// A mod's "source" link, checked the same way by the launcher (mod_catalog.cpp) and the core (games/games.cpp).
#pragma once

#include <string>

namespace simqvfr::mod_api {

// https, a host and a path (a project, not a site), no spaces, quotes or backslashes, at most 200 characters.
// Third-party mods must have one (and a licence): Sim-QVFR mods are open source.
inline bool valid_source_link(const std::string& url) {
    if (url.size() < 12 || url.size() > 200 || url.compare(0, 8, "https://") != 0)
        return false;
    for (char c : url)
        if (static_cast<unsigned char>(c) <= 0x20 || c == '"' || c == '<' || c == '>' || c == '\\' || c < 0)
            return false;
    const size_t host = url.find('/', 8);
    return host != std::string::npos && host > 8 && host + 1 < url.size();  // a host and a path: a project, not a site
}

}  // namespace simqvfr::mod_api
