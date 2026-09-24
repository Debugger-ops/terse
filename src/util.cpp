// util.cpp — strings, files, small algorithms.
#include "tersec.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace terse {

std::string trim(std::string_view s) {
    std::size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) b++;
    std::size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) e--;
    return std::string(s.substr(b, e - b));
}

std::string strip_quotes(std::string s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return s.substr(1, s.size() - 2);
    return s;
}

std::optional<std::string> read_file(const std::string &path) {
    const bool is_stdin = path == "-";
    std::FILE *f = is_stdin ? stdin : std::fopen(path.c_str(), "rb");
    if (!f) return std::nullopt;
    std::string data;
    std::array<char, 1 << 16> chunk;
    std::size_t n;
    while ((n = std::fread(chunk.data(), 1, chunk.size(), f)) > 0) data.append(chunk.data(), n);
    if (!is_stdin) std::fclose(f);
    return data;
}

bool file_exists(const std::string &path) {
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

int edit_distance(std::string_view a, std::string_view b) {
    constexpr std::size_t kMax = 32;
    if (a.size() > kMax || b.size() > kMax) return 99;
    // Two rolling rows instead of a full matrix.
    std::array<int, kMax + 1> prev{}, cur{};
    for (std::size_t j = 0; j <= b.size(); j++) prev[j] = int(j);
    for (std::size_t i = 1; i <= a.size(); i++) {
        cur[0] = int(i);
        for (std::size_t j = 1; j <= b.size(); j++) {
            int cost = ascii::to_lower(a[i - 1]) != ascii::to_lower(b[j - 1]);
            int m = prev[j] + 1;
            if (cur[j - 1] + 1 < m) m = cur[j - 1] + 1;
            if (prev[j - 1] + cost < m) m = prev[j - 1] + cost;
            cur[j] = m;
        }
        prev = cur;
    }
    return prev[b.size()];
}

int count_substr(std::string_view s, std::string_view sub) {
    int n = 0;
    for (std::size_t p = s.find(sub); p != std::string_view::npos; p = s.find(sub, p + sub.size())) n++;
    return n;
}

bool istarts_with(std::string_view s, std::size_t pos, std::string_view prefix) {
    if (pos > s.size() || s.size() - pos < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); i++)
        if (ascii::to_lower(s[pos + i]) != ascii::to_lower(prefix[i])) return false;
    return true;
}

int to_int(std::string_view digits) {
    std::string tmp(digits);
    return static_cast<int>(std::strtol(tmp.c_str(), nullptr, 10));
}

std::string json_quote(std::string_view s) {
    std::string b = "\"";
    for (char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
        case '"': b += "\\\""; break;
        case '\\': b += "\\\\"; break;
        case '\n': b += "\\n"; break;
        case '\r': b += "\\r"; break;
        case '\t': b += "\\t"; break;
        default:
            if (c < 0x20) {
                char esc[8];
                std::snprintf(esc, sizeof esc, "\\u%04x", c);
                b += esc;
            } else {
                b += ch;
            }
        }
    }
    b += '"';
    return b;
}

SourceText split_source(std::string_view raw) {
    SourceText t;
    if (raw.size() >= 3 && raw[0] == '\xEF' && raw[1] == '\xBB' && raw[2] == '\xBF') {
        t.bom = std::string(raw.substr(0, 3));
        raw.remove_prefix(3);
    }
    for (std::size_t pos = 0; pos < raw.size();) {
        std::size_t e = raw.find('\n', pos);
        std::size_t l = e == std::string_view::npos ? raw.size() - pos : e - pos;
        std::size_t ll = l;
        bool cr = ll > 0 && raw[pos + ll - 1] == '\r';
        if (cr) ll--;
        t.lines.emplace_back(raw.substr(pos, ll));
        t.eols.emplace_back(e == std::string_view::npos ? (cr ? "\r" : "") : (cr ? "\r\n" : "\n"));
        pos += l;
        if (pos < raw.size()) pos++;
    }
    return t;
}

std::string SourceText::join() const {
    std::string s = bom;
    for (std::size_t i = 0; i < lines.size(); i++) s += lines[i] + eols[i];
    return s;
}

bool write_file(const std::string &path, std::string_view data) {
    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    return std::fclose(f) == 0 && ok;
}

void KeyRegistry::add(std::string name) {
    if (!contains(name)) keys_.push_back(std::move(name));
}

bool KeyRegistry::contains(std::string_view name) const {
    for (const auto &k : keys_)
        if (k == name) return true;
    return false;
}

} // namespace terse
