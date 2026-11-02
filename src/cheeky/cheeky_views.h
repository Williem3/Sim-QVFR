// The DLSS views in a Cheeky runtime snapshot (CheekyRuntime_Snapshot's JSON, "view_details"): one
// entry per game DLSS handle Cheeky has seen, with its evaluation count and sizes. The layer logs how
// often each view is evaluated per xrEndFrame, which says how ACE runs DLSS in each rendering mode
// (two eyes, or four views in views4). Pure: no Windows or Cheeky dependency.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace aceqv {

struct CheekyView {
    std::string id, eye, region, mapping, api;
    uint64_t calls = 0;  // evaluation_calls: DLSS evaluations Cheeky observed on this handle
    int inWidth = 0, inHeight = 0, outWidth = 0, outHeight = 0;
};

namespace cheeky_detail {

// The value of "key" in a flat JSON object `object` (no nesting), as text: a string's contents, or
// a number's digits. Empty when absent.
inline std::string field(const std::string& object, const char* key) {
    const std::string name = std::string("\"") + key + "\":";
    const size_t at = object.find(name);
    if (at == std::string::npos)
        return {};
    size_t i = at + name.size();
    if (i < object.size() && object[i] == '"') {
        const size_t end = object.find('"', i + 1);
        return end == std::string::npos ? std::string() : object.substr(i + 1, end - i - 1);
    }
    const size_t end = object.find_first_of(",}", i);
    return object.substr(i, end == std::string::npos ? std::string::npos : end - i);
}

inline uint64_t number(const std::string& object, const char* key) {
    return std::strtoull(field(object, key).c_str(), nullptr, 10);
}

}  // namespace cheeky_detail

// The entries of "view_details" (empty when the snapshot has none). Each entry is a flat object.
inline std::vector<CheekyView> cheeky_views(const std::string& snapshot) {
    std::vector<CheekyView> views;
    const std::string key = "\"view_details\":[";
    size_t i = snapshot.find(key);
    if (i == std::string::npos)
        return views;
    i += key.size();
    while (i < snapshot.size() && snapshot[i] == '{') {
        const size_t end = snapshot.find('}', i);
        if (end == std::string::npos)
            break;
        const std::string object = snapshot.substr(i, end + 1 - i);
        CheekyView v;
        v.id = cheeky_detail::field(object, "id");
        v.eye = cheeky_detail::field(object, "eye");
        v.region = cheeky_detail::field(object, "region");
        v.mapping = cheeky_detail::field(object, "mapping");
        v.api = cheeky_detail::field(object, "api");
        v.calls = cheeky_detail::number(object, "evaluation_calls");
        v.inWidth = static_cast<int>(cheeky_detail::number(object, "input_width"));
        v.inHeight = static_cast<int>(cheeky_detail::number(object, "input_height"));
        v.outWidth = static_cast<int>(cheeky_detail::number(object, "output_width"));
        v.outHeight = static_cast<int>(cheeky_detail::number(object, "output_height"));
        views.push_back(std::move(v));
        i = end + 1;
        if (i < snapshot.size() && snapshot[i] == ',')
            ++i;
    }
    return views;
}

// One log line per view: its sizes, how Cheeky mapped it, and its evaluations per frame since the
// last report (`before`, matched by id; a new id counts from 0) over `frames` xrEndFrames.
inline std::vector<std::string> cheeky_view_report(const std::vector<CheekyView>& before,
                                                   const std::vector<CheekyView>& now, uint64_t frames) {
    std::vector<std::string> lines;
    for (const auto& v : now) {
        uint64_t previous = 0;
        for (const auto& b : before)
            if (b.id == v.id && b.calls <= v.calls)
                previous = b.calls;
        const uint64_t calls = v.calls - previous;
        char line[512];
        std::snprintf(line, sizeof(line),
                      "cheeky: view %s %s in %dx%d -> out %dx%d, eye %s, region %s, mapping %s: %llu evaluations, "
                      "%.2f per frame",
                      v.id.c_str(), v.api.c_str(), v.inWidth, v.inHeight, v.outWidth, v.outHeight, v.eye.c_str(),
                      v.region.c_str(), v.mapping.c_str(), static_cast<unsigned long long>(calls),
                      frames ? static_cast<double>(calls) / static_cast<double>(frames) : 0.0);
        lines.emplace_back(line);
    }
    return lines;
}

}  // namespace aceqv
