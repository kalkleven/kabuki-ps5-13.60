// SPDX-License-Identifier: GPL-3.0-or-later
// Kabuki's own small file format, between the daemon and the ShellUI worker.
//
// The daemon parses everything from outside (JSON from providers) and writes what the worker
// needs as records: one per line, fields separated by tabs, the first field naming the record.
// Tabs, newlines and backslashes inside fields are escaped. The first record is `kabuki 1`.
// Reading needs no allocation-heavy parser inside SceShellUI, and nothing can be malformed in
// a way that matters: an unknown or short record is skipped.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace kabuki {

using Record = std::vector<std::string>;

inline void escape_into(std::string& out, const std::string& field) {
    for (const char c : field) {
        if (c == '\\') out += "\\\\";
        else if (c == '\t') out += "\\t";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out += c;
    }
}

class Records {
    std::string text_ = "kabuki\t1\n";
public:
    Records& add(std::initializer_list<std::string> fields) {
        bool first = true;
        for (const auto& f : fields) {
            if (!first) text_ += '\t';
            escape_into(text_, f);
            first = false;
        }
        text_ += '\n';
        return *this;
    }
    Records& add(const Record& fields) {
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (i) text_ += '\t';
            escape_into(text_, fields[i]);
        }
        text_ += '\n';
        return *this;
    }
    const std::string& text() const { return text_; }
};

// All records of a text; empty if it doesn't start with `kabuki 1`.
inline std::vector<Record> parse_records(const std::string& text) {
    std::vector<Record> out;
    Record record;
    std::string field;
    bool escaped = false;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        const char c = i < text.size() ? text[i] : '\n';
        if (escaped) {
            field += c == 't' ? '\t' : c == 'n' ? '\n' : c == 'r' ? '\r' : c;
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '\t') {
            record.push_back(std::move(field)); field.clear();
        } else if (c == '\n') {
            if (!field.empty() || !record.empty()) { record.push_back(std::move(field)); out.push_back(std::move(record)); }
            field.clear(); record.clear();
        } else {
            field += c;
        }
    }
    if (out.empty() || out[0].size() < 2 || out[0][0] != "kabuki" || out[0][1] != "1") return {};
    out.erase(out.begin());
    return out;
}

inline const std::string& field(const Record& r, std::size_t i) {
    static const std::string none;
    return i < r.size() ? r[i] : none;
}
inline long long number(const Record& r, std::size_t i, long long fallback = 0) {
    const auto& s = field(r, i);
    if (s.empty()) return fallback;
    char* end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    return end && *end == 0 ? v : fallback;
}

// Writes `text` to `path` beside it first, then renames, so a reader never sees half a file.
inline bool write_file_atomic(const std::string& path, const std::string& text) {
    const std::string temp = path + ".tmp";
    FILE* f = std::fopen(temp.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    ok = std::fclose(f) == 0 && ok;
    if (!ok) { std::remove(temp.c_str()); return false; }
#ifdef _WIN32
    std::remove(path.c_str()); // rename doesn't replace on Windows (the PC preview)
#endif
    return std::rename(temp.c_str(), path.c_str()) == 0;
}

inline std::string read_file(const std::string& path, std::size_t limit = 1 << 20) {
    std::string text;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char bytes[4096]; std::size_t n;
        while ((n = std::fread(bytes, 1, sizeof bytes, f)) > 0 && text.size() < limit) text.append(bytes, n);
        std::fclose(f);
    }
    return text;
}

} // namespace kabuki
