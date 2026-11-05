#pragma once
// Portable JSON event builder. Failure drops the whole event, never a partial JSON line.
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstddef>
namespace nk {
template <size_t Capacity> class EventLine {
    char line_[Capacity] = "@NK {";
    size_t used_ = 5;
    bool ok_ = true, first_ = true;
    void append(const char *s) {
        const size_t n = std::strlen(s);
        if (n >= sizeof(line_) - used_) { ok_ = false; return; }
        std::memcpy(line_ + used_, s, n + 1); used_ += n;
    }
    void quoted(const char *s) {
        append("\"");
        for (const unsigned char *p = reinterpret_cast<const unsigned char *>(s); *p; ++p) {
            char b[8] = {};
            if (*p == '"' || *p == '\\') { b[0] = '\\'; b[1] = *p; }
            else if (*p < 0x20) std::snprintf(b, sizeof b, "\\u%04x", *p);
            else b[0] = *p;
            append(b);
        }
        append("\"");
    }
    void key(const char *s) { if (!first_) append(","); first_ = false; quoted(s); append(":"); }
public:
    explicit EventLine(const char *ev) { text("ev", ev); }
    EventLine &text(const char *k, const char *v) { key(k); quoted(v ? v : ""); return *this; }
    EventLine &number(const char *k, double v) {
        key(k); char b[40];
        if (std::isfinite(v)) std::snprintf(b, sizeof b, "%.9g", v);
        else std::strcpy(b, "null");
        append(b); return *this;
    }
    EventLine &boolean(const char *k, bool v) { key(k); append(v ? "true" : "false"); return *this; }
    const char *finish() { append("}\n"); return ok_ ? line_ : nullptr; }
    void emit() { const char *s = finish(); if (s) std::printf("%s", s); }
};
using Event = EventLine<1024>;
inline void event_mode(unsigned mode) { EventLine<128>("mode").number("mode", mode).text("name", mode == 1 ? "toy" : "plush").emit(); }
inline void event_state(const char *state, const char *reason) { EventLine<256>("state").text("state", state).text("reason", reason).emit(); }
}
