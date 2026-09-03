// ---------------------------------------------------------------------------
// svg2png — convert SVG to PNG and pack PNGs into an embeddable texture atlas.
//
// Two modes (selected as the first argument):
//
//   svg2png png   --svg-path <f.svg>   --save-path <out.png>
//                   svg2png png         --svg-string "<svg>...</svg>" --save-path <out.png>
//   svg2png texture --output-path <o.cxxpng> --png-path <t1.png> [--png-path ...]
//
// Mode "png": SVG is a *vector* (XML) format, so neither CImg nor lodepng can
// rasterize it on its own.  This mode therefore:
//   * parses the SVG markup and its presentation attributes itself,
//   * rasterizes the shapes into an anti-aliased (supersampled) image using
//     CImg as the image buffer / manipulation primitive,
//   * encodes the resulting pixels as PNG with lodepng.
//   Supported elements: svg, g, defs, symbol, use, rect, circle, ellipse, line,
//   polyline, polygon, path.  Text, embedded <image>, clip paths, gradients,
//   masks and filters are intentionally out of scope.
//
// Mode "texture": packs several already-encoded PNG textures into a single
// in-memory atlas and writes a C++ header (.cxxpng) that embeds the atlas PNG
// bytes together with a std::map<std::string, coordinate_t> giving each
// texture's rectangle inside the atlas.  The header can be #included into C++
// code to embed the textures directly in an application.  lodepng decodes the
// source PNGs, CImg builds the atlas, and lodepng re-encodes it.
// ---------------------------------------------------------------------------

// Headless CImg: no X11 / GDI / SDL display code is pulled in.
#ifndef cimg_display
#define cimg_display 0
#endif
#include "CImg.h"
#include "lodepng.h"

// CImg's classes (CImg<...>, cimg_version, ...) live in the 'cimg_library'
// namespace.  Bringing it into scope is the documented way to use the library
// and lets us write plain CImg<T> / cimg_version below.
using namespace cimg_library;

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr float kPi = 3.14159265358979323846f;

inline float hypot2(float x, float y) { return std::sqrt(x * x + y * y); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---------------------------------------------------------------------------
// Small math helpers
// ---------------------------------------------------------------------------

struct Pt {
    float x = 0.0f, y = 0.0f;
};

// 3x3 affine transform stored as a 2x2 block + translation.
//   x' = a*x + c*y + e
//   y' = b*x + d*y + f
struct Affine {
    float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f, e = 0.0f, f = 0.0f;
    static Affine identity() { return {}; }
};

inline Pt applyPt(const Affine& m, const Pt& p) {
    return Pt{m.a * p.x + m.c * p.y + m.e, m.b * p.x + m.d * p.y + m.f};
}

// `after` is applied first, then `before`  (returns before * after).
inline Affine compose(const Affine& before, const Affine& after) {
    return Affine{
        before.a * after.a + before.c * after.b,
        before.b * after.a + before.d * after.b,
        before.a * after.c + before.c * after.d,
        before.b * after.c + before.d * after.d,
        before.a * after.e + before.c * after.f + before.e,
        before.b * after.e + before.d * after.f + before.f,
    };
}

struct Color {
    int r = 0, g = 0, b = 0;
    float a = 1.0f;
};

// ---------------------------------------------------------------------------
// String helpers
// ---------------------------------------------------------------------------

std::string toLower(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> splitTop(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s) {
        if (ch == sep) { out.push_back(cur); cur.clear(); }
        else cur.push_back(ch);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

void replaceAll(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::string decodeEntities(std::string s) {
    static const std::pair<std::string, std::string> named[] = {
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"},
    };
    for (auto& p : named) replaceAll(s, p.first, p.second);
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '&' && i + 1 < s.size() && s[i + 1] == '#') {
            size_t j = i + 2;
            bool hex = false;
            if (j < s.size() && (s[j] == 'x' || s[j] == 'X')) { hex = true; ++j; }
            size_t start = j, val = 0;
            while (j < s.size() && std::isxdigit(static_cast<unsigned char>(s[j]))) ++j;
            if (j > start) {
                std::string code = s.substr(start, j - start);
                val = std::strtoul(code.c_str(), nullptr, hex ? 16 : 10);
                if (val <= 0x10FFFF) out.push_back(static_cast<char>(val));
                i = j;
                continue;
            }
        }
        out.push_back(s[i]);
        ++i;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Minimal SVG/XML tokenizer -> node tree
// ---------------------------------------------------------------------------

struct Node {
    std::string tag;
    std::map<std::string, std::string> attr;
    std::vector<std::unique_ptr<Node>> children;
    std::string id;
    Node* parent = nullptr;

    void add(std::unique_ptr<Node> n) {
        n->parent = this;
        children.push_back(std::move(n));
    }
};

std::string readQuoted(const std::string& s, size_t& pos) {
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
    if (pos >= s.size()) return "";
    char q = s[pos];
    if (q != '"' && q != '\'') {
        std::string v;
        while (pos < s.size() && !std::isspace(static_cast<unsigned char>(s[pos]))) { v.push_back(s[pos]); ++pos; }
        return v;
    }
    ++pos;
    std::string v;
    while (pos < s.size() && s[pos] != q) { v.push_back(s[pos]); ++pos; }
    if (pos < s.size()) ++pos;
    return v;
}

void parseAttributes(std::map<std::string, std::string>& attr, const std::string& s, size_t pos) {
    while (pos < s.size()) {
        while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
        if (pos >= s.size() || s[pos] == '>' ||
            (s[pos] == '/' && pos + 1 < s.size() && s[pos + 1] == '>'))
            break;
        size_t nameStart = pos;
        while (pos < s.size() && s[pos] != '=' && !std::isspace(static_cast<unsigned char>(s[pos])) &&
               s[pos] != '>' && s[pos] != '/')
            ++pos;
        size_t nameEnd = pos;
        if (nameEnd == nameStart) { ++pos; continue; }
        std::string name = toLower(s.substr(nameStart, nameEnd - nameStart));
        while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
        if (pos < s.size() && s[pos] == '=') {
            ++pos;
            attr[name] = decodeEntities(readQuoted(s, pos));
        } else {
            attr[name] = "";
        }
    }
}

std::string preClean(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '<') {
            if (s.compare(i, 4, "<!--") == 0) {
                size_t end = s.find("-->", i + 4);
                i = (end == std::string::npos) ? s.size() : end + 3;
                continue;
            }
            if (s.compare(i, 2, "<!") == 0) {
                size_t end = s.find('>', i + 2);
                i = (end == std::string::npos) ? s.size() : end + 1;
                continue;
            }
            if (s.compare(i, 2, "<?") == 0) {
                size_t end = s.find("?>", i + 2);
                i = (end == std::string::npos) ? s.size() : end + 2;
                continue;
            }
        }
        out.push_back(s[i]);
        ++i;
    }
    return out;
}

std::unique_ptr<Node> parseNode(const std::string& s, size_t& pos) {
    auto node = std::make_unique<Node>();
    size_t tagStart = pos + 1;
    while (pos < s.size() && s[pos] != '>') ++pos;
    size_t tagEnd = pos;
    bool selfClose = (tagEnd > 0 && s[tagEnd - 1] == '/');

    size_t k = tagStart;
    while (k < tagEnd && !std::isspace(static_cast<unsigned char>(s[k]))) ++k;
    node->tag = toLower(s.substr(tagStart, k - tagStart));
    parseAttributes(node->attr, s, k);
    node->id = node->attr.count("id") ? node->attr.at("id") : "";

    ++pos;
    if (selfClose) return node;

    std::string closeTag = "</" + node->tag;
    while (pos < s.size()) {
        if (s[pos] == '<') {
            if (s.compare(pos, closeTag.size(), closeTag.c_str()) == 0) {
                while (pos < s.size() && s[pos] != '>') ++pos;
                ++pos;
                return node;
            }
            auto child = parseNode(s, pos);
            node->add(std::move(child));
        } else {
            ++pos; // ignore text content
        }
    }
    return node;
}

std::unique_ptr<Node> parseDocument(const std::string& raw) {
    std::string s = preClean(raw);
    auto root = std::make_unique<Node>();
    root->tag = "__root__";
    size_t pos = 0;
    while (pos < s.size()) {
        if (s[pos] == '<') {
            if (s.compare(pos, 2, "</") == 0) { ++pos; continue; }
            auto node = parseNode(s, pos);
            root->add(std::move(node));
        } else {
            ++pos;
        }
    }
    return root;
}

void collectIds(Node* root, std::map<std::string, Node*>& ids) {
    for (auto& c : root->children) {
        if (!c->id.empty()) ids[c->id] = c.get();
        collectIds(c.get(), ids);
    }
}

// ---------------------------------------------------------------------------
// Lengths / units
// ---------------------------------------------------------------------------

float unitToPx(float v, const std::string& unit) {
    if (unit.empty() || unit == "px") return v;
    if (unit == "pt") return v * 96.0f / 72.0f;
    if (unit == "pc") return v * 96.0f / 6.0f;
    if (unit == "in") return v * 96.0f;
    if (unit == "cm") return v * 96.0f / 2.54f;
    if (unit == "mm") return v * 96.0f / 25.4f;
    if (unit == "q") return v * 96.0f / 25.4f / 4.0f;
    return v;
}

bool parseLength(const std::string& s, float& out, std::string& unit) {
    std::string t = trim(s);
    if (t.empty()) return false;
    size_t i = 0;
    while (i < t.size() && (std::isdigit(static_cast<unsigned char>(t[i])) || t[i] == '.' ||
                            t[i] == '-' || t[i] == '+'))
        ++i;
    if (i == 0) return false;
    out = std::atof(t.substr(0, i).c_str());
    unit = toLower(trim(t.substr(i)));
    return true;
}

float parseLengthPx(const std::string& s) {
    float v = 0;
    std::string unit;
    if (!parseLength(s, v, unit)) return 0.0f;
    if (unit == "%") return v;
    return unitToPx(v, unit);
}

// ---------------------------------------------------------------------------
// Color parsing
// ---------------------------------------------------------------------------

Color colorNamed(const std::string& name) {
    std::string n = toLower(trim(name));
    if (n == "white") return {255, 255, 255, 1};
    if (n == "red") return {255, 0, 0, 1};
    if (n == "green") return {0, 128, 0, 1};
    if (n == "lime") return {0, 255, 0, 1};
    if (n == "blue") return {0, 0, 255, 1};
    if (n == "navy") return {0, 0, 128, 1};
    if (n == "aqua" || n == "cyan") return {0, 255, 255, 1};
    if (n == "yellow") return {255, 255, 0, 1};
    if (n == "magenta" || n == "fuchsia") return {255, 0, 255, 1};
    if (n == "purple") return {128, 0, 128, 1};
    if (n == "maroon") return {128, 0, 0, 1};
    if (n == "olive") return {128, 128, 0, 1};
    if (n == "teal") return {0, 128, 128, 1};
    if (n == "silver") return {192, 192, 192, 1};
    if (n == "gray" || n == "grey") return {128, 128, 128, 1};
    if (n == "darkgray" || n == "darkgrey") return {169, 169, 169, 1};
    if (n == "lightgray" || n == "lightgrey") return {211, 211, 211, 1};
    return {0, 0, 0, 1};
}

float alphaComp(const std::string& s) {
    std::string t = trim(s);
    if (t.empty()) return 1.0f;
    if (t.back() == '%') return clampf(std::atof(t.c_str()) / 100.0f, 0, 1);
    return clampf(std::atof(t.c_str()), 0, 1);
}

Color parseColor(const std::string& in, bool& none) {
    none = false;
    std::string s = toLower(trim(in));
    if (s.empty() || s == "none" || s == "transparent") { none = true; return {0, 0, 0, 0}; }
    if (s[0] == '#') {
        std::string h = s.substr(1);
        auto hx = [](char ch) -> int {
            if (ch >= '0' && ch <= '9') return ch - '0';
            return ch - 'a' + 10;
        };
        Color c = {0, 0, 0, 1};
        if (h.size() == 3) {
            c.r = hx(h[0]) * 16 + hx(h[0]);
            c.g = hx(h[1]) * 16 + hx(h[1]);
            c.b = hx(h[2]) * 16 + hx(h[2]);
        } else if (h.size() >= 6) {
            c.r = (hx(h[0]) << 4) | hx(h[1]);
            c.g = (hx(h[2]) << 4) | hx(h[3]);
            c.b = (hx(h[4]) << 4) | hx(h[5]);
            if (h.size() == 8) c.a = (((hx(h[6]) << 4) | hx(h[7])) / 255.0f);
        }
        return c;
    }
    if (s.compare(0, 4, "rgb(") == 0 || s.compare(0, 5, "rgba(") == 0) {
        size_t open = s.find('('), close = s.find(')');
        std::string inner = (open != std::string::npos && close != std::string::npos) ? s.substr(open + 1, close - open - 1) : "";
        std::string norm;
        for (char ch : inner) norm.push_back(std::isspace(static_cast<unsigned char>(ch)) ? ',' : ch);
        std::vector<std::string> parts = splitTop(norm, ',');
        std::vector<std::string> vals;
        for (auto& p : parts) if (!trim(p).empty()) vals.push_back(trim(p));
        auto comp = [](const std::string& p) -> float {
            std::string t = trim(p);
            if (t.back() == '%') return clampf(std::atof(t.c_str()) * 255.0f / 100.0f, 0, 255);
            return clampf(std::atof(t.c_str()), 0, 255);
        };
        Color c;
        c.r = clampi(static_cast<int>(comp(vals.at(0))), 0, 255);
        if (vals.size() > 1) c.g = clampi(static_cast<int>(comp(vals.at(1))), 0, 255);
        if (vals.size() > 2) c.b = clampi(static_cast<int>(comp(vals.at(2))), 0, 255);
        if (vals.size() > 3) c.a = alphaComp(vals.at(3));
        return c;
    }
    return colorNamed(s);
}

// ---------------------------------------------------------------------------
// Path / shape model (geometry stored in *sample* space: device px * S)
// ---------------------------------------------------------------------------

struct Subpath {
    std::vector<Pt> p;
    bool closed = false;
};
struct Path {
    std::vector<Subpath> s;
};

struct Style {
    Color fill{0, 0, 0, 1};
    bool fillNone = false;
    Color stroke{0, 0, 0, 1};
    bool strokeNone = true;
    float strokeWidth = 1.0f;
    float fillOpacity = 1.0f;
    float strokeOpacity = 1.0f;
    int linecap = 0; // 0 butt, 1 round, 2 square
};

void parsePoints(const std::string& s, std::vector<Pt>& out) {
    std::vector<std::string> nums;
    std::string cur;
    for (char ch : s) {
        if (std::isdigit(static_cast<unsigned char>(ch)) || ch == '.' || ch == '-' || ch == '+' ||
            ch == 'e' || ch == 'E')
            cur.push_back(ch);
        else {
            if (!trim(cur).empty()) nums.push_back(trim(cur));
            cur.clear();
        }
    }
    if (!trim(cur).empty()) nums.push_back(trim(cur));
    for (size_t i = 0; i + 1 < nums.size(); i += 2)
        out.push_back(Pt{static_cast<float>(std::atof(nums[i].c_str())),
                         static_cast<float>(std::atof(nums[i + 1].c_str()))});
}

// Read a CSS number starting at s[i]; advances i past it and returns the value.
float readNumber(const std::string& s, size_t& i) {
    size_t start = i;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
    if (i < s.size() && s[i] == '.') {
        ++i;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
    } else {
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
        if (i < s.size() && s[i] == '.') {
            ++i;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
        }
    }
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
    }
    return std::atof(s.substr(start, i - start).c_str());
}

void parsePathD(const std::string& in, Path& path) {
    using Tok = std::pair<char, std::vector<float>>;
    std::vector<Tok> toks;

    auto isNumStart = [](char ch) {
        return std::isdigit(static_cast<unsigned char>(ch)) || ch == '.';
    };
    {
        size_t i = 0;
        while (i < in.size()) {
            char ch = in[i];
            if (std::isalpha(static_cast<unsigned char>(ch))) {
                toks.emplace_back(ch, std::vector<float>{});
                ++i;
                while (i < in.size() && !std::isalpha(static_cast<unsigned char>(in[i]))) {
                    if (isNumStart(in[i]) || in[i] == '+' || in[i] == '-')
                        toks.back().second.push_back(readNumber(in, i));
                    else ++i;
                }
            } else if (isNumStart(ch) || ch == '+' || ch == '-') {
                if (toks.empty()) toks.emplace_back('M', std::vector<float>{});
                toks.back().second.push_back(readNumber(in, i));
            } else {
                ++i;
            }
        }
    }

    auto segLen = [](const Pt& a, const Pt& b) { return hypot2(b.x - a.x, b.y - a.y); };
    auto addSample = [](Subpath& sp, const Pt& p) {
        if (sp.p.empty()) { sp.p.push_back(p); return; }
        Pt last = sp.p.back();
        if (std::fabs(last.x - p.x) > 1e-4f || std::fabs(last.y - p.y) > 1e-4f) sp.p.push_back(p);
    };
    auto cubic = [&](Subpath& sp, const Pt& p0, const Pt& p1, const Pt& p2, const Pt& p3) {
        int n = clampi(static_cast<int>(std::ceil((segLen(p0, p1) + segLen(p1, p2) + segLen(p2, p3)) / 8.0f)), 4, 128);
        for (int k = 1; k <= n; ++k) {
            float t = k / static_cast<float>(n), it = 1 - t;
            addSample(sp, Pt{
                it * it * it * p0.x + 3 * it * it * t * p1.x + 3 * it * t * t * p2.x + t * t * t * p3.x,
                it * it * it * p0.y + 3 * it * it * t * p1.y + 3 * it * t * t * p2.y + t * t * t * p3.y});
        }
    };
    auto quad = [&](Subpath& sp, const Pt& p0, const Pt& p1, const Pt& p2) {
        int n = clampi(static_cast<int>(std::ceil((segLen(p0, p1) + segLen(p1, p2)) / 8.0f)), 4, 128);
        for (int k = 1; k <= n; ++k) {
            float t = k / static_cast<float>(n), it = 1 - t;
            addSample(sp, Pt{it * it * p0.x + 2 * it * t * p1.x + t * t * p2.x,
                             it * it * p0.y + 2 * it * t * p1.y + t * t * p2.y});
        }
    };
    Pt cur{0, 0}, start{0, 0}, prevCtrl{0, 0};
    char prevCmd = 0;
    auto ensure = [&]() { if (path.s.empty()) path.s.push_back(Subpath{{cur}, false}); };

    for (auto& tk : toks) {
        char raw = tk.first;
        char c = static_cast<char>(std::tolower(static_cast<unsigned char>(raw)));
        bool rel = std::islower(raw);
        const std::vector<float>& v = tk.second;
        auto P = [&](float x, float y) -> Pt { return rel ? Pt{cur.x + x, cur.y + y} : Pt{x, y}; };
        size_t i = 0;

        switch (c) {
        case 'm': {
            if (i + 1 < v.size() || (i + 1 == v.size())) {
                float x = v[i++], y = v[i++];
                Pt p = P(x, y);
                cur = p;
                start = p;
                path.s.push_back(Subpath{{p}, false});
                while (i + 1 < v.size()) {
                    float nx = v[i++], ny = v[i++];
                    Pt q = P(nx, ny);
                    cur = q;
                    path.s.back().p.push_back(q);
                }
            }
            prevCmd = 0;
            break;
        }
        case 'l':
            while (i + 1 < v.size()) {
                ensure();
                float nx = v[i++], ny = v[i++];
                Pt p = P(nx, ny);
                path.s.back().p.push_back(p);
                cur = p;
            }
            prevCmd = 'l';
            break;
        case 'h':
            while (i < v.size()) {
                ensure();
                Pt p = rel ? Pt{cur.x + v[i], cur.y} : Pt{v[i], cur.y};
                ++i;
                path.s.back().p.push_back(p);
                cur = p;
            }
            prevCmd = 'h';
            break;
        case 'v':
            while (i < v.size()) {
                ensure();
                Pt p = rel ? Pt{cur.x, cur.y + v[i]} : Pt{cur.x, v[i]};
                ++i;
                path.s.back().p.push_back(p);
                cur = p;
            }
            prevCmd = 'v';
            break;
        case 'c':
            while (i + 5 <= v.size()) {
                ensure();
                Pt p0 = cur;
                float x1 = v[i++], y1 = v[i++];
                float x2 = v[i++], y2 = v[i++];
                float x3 = v[i++], y3 = v[i++];
                Pt p1 = P(x1, y1), p2 = P(x2, y2), p3 = P(x3, y3);
                cubic(path.s.back(), p0, p1, p2, p3);
                prevCtrl = p2;
                cur = p3;
            }
            prevCmd = 'c';
            break;
        case 'q':
            while (i + 3 <= v.size()) {
                ensure();
                Pt p0 = cur;
                float x1 = v[i++], y1 = v[i++];
                float x2 = v[i++], y2 = v[i++];
                Pt p1 = P(x1, y1), p2 = P(x2, y2);
                quad(path.s.back(), p0, p1, p2);
                prevCtrl = p1;
                cur = p2;
            }
            prevCmd = 'q';
            break;
        case 's':
            while (i + 3 <= v.size()) {
                ensure();
                Pt p1 = (prevCmd == 'c' || prevCmd == 's') ? Pt{2 * cur.x - prevCtrl.x, 2 * cur.y - prevCtrl.y} : cur;
                float x2 = v[i++], y2 = v[i++];
                float x3 = v[i++], y3 = v[i++];
                Pt p2 = P(x2, y2), p3 = P(x3, y3);
                cubic(path.s.back(), cur, p1, p2, p3);
                prevCtrl = p2;
                cur = p3;
            }
            prevCmd = 's';
            break;
        case 't':
            while (i + 1 <= v.size()) {
                ensure();
                Pt p1 = (prevCmd == 'q' || prevCmd == 't') ? Pt{2 * cur.x - prevCtrl.x, 2 * cur.y - prevCtrl.y} : cur;
                float x2 = v[i++], y2 = v[i++];
                Pt p2 = P(x2, y2);
                quad(path.s.back(), cur, p1, p2);
                prevCtrl = p1;
                cur = p2;
            }
            prevCmd = 't';
            break;
        case 'z':
            if (!path.s.empty()) { path.s.back().closed = true; cur = start; }
            prevCmd = 0;
            break;
        default:
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Rasterizer
// ---------------------------------------------------------------------------

using Buf = CImg<unsigned char>;

// Composite a straight-color source (R,G,B in 0..255, alpha A in 0..1) over the
// buffer.  The buffer stores premultiplied RGBA.
void composite(Buf& buf, int x, int y, float R, float G, float B, float A) {
    if (A <= 0.0f) return;
    if (x < 0 || y < 0 || x >= buf.width() || y >= buf.height()) return;
    float sa = A * 255.0f, sr = R * A, sg = G * A, sb = B * A;
    float da = buf(x, y, 0, 3), dr = buf(x, y, 0, 0), dg = buf(x, y, 0, 1), db = buf(x, y, 0, 2);
    float na = da + sa * (1.0f - da / 255.0f);
    float nr = dr + sr - 1.0f * dr * sa / 255.0f;
    float ng = dg + sg - 1.0f * dg * sa / 255.0f;
    float nb = db + sb - 1.0f * db * sa / 255.0f;
    buf(x, y, 0, 3) = static_cast<unsigned char>(clampf(na, 0, 255) + 0.5f);
    buf(x, y, 0, 0) = static_cast<unsigned char>(clampf(nr, 0, 255) + 0.5f);
    buf(x, y, 0, 1) = static_cast<unsigned char>(clampf(ng, 0, 255) + 0.5f);
    buf(x, y, 0, 2) = static_cast<unsigned char>(clampf(nb, 0, 255) + 0.5f);
}

inline float distToSeg(const Pt& p, const Pt& a, const Pt& b) {
    float abx = b.x - a.x, aby = b.y - a.y;
    float denom = abx * abx + aby * aby;
    float t = denom > 0 ? clampf(((p.x - a.x) * abx + (p.y - a.y) * aby) / denom, 0, 1) : 0;
    float qx = a.x + t * abx, qy = a.y + t * aby;
    return hypot2(p.x - qx, p.y - qy);
}

bool pointInPath(const Path& path, float x, float y) {
    bool in = false;
    for (auto& sp : path.s) {
        if (!sp.closed || sp.p.size() < 3) continue;
        for (size_t i = 0, n = sp.p.size(); i < n; ++i) {
            const Pt& p = sp.p[i];
            const Pt& q = sp.p[(i + 1) % n];
            if ((p.y > y) != (q.y > y)) {
                float xint = (q.x - p.x) * (y - p.y) / (q.y - p.y) + p.x;
                if (x < xint) in = !in;
            }
        }
    }
    return in;
}

struct RenderParams {
    bool doFill = true;
    Color fillColor{255, 255, 255, 1};
    float fillAlpha = 1.0f;
    bool doStroke = true;
    Color strokeColor{0, 0, 0, 1};
    float strokeAlpha = 1.0f;
    float hw = 0.0f; // half stroke width in sample space
    int cap = 0;
};

void fillPath(Buf& buf, const Path& path, float R, float G, float B, float A) {
    if (A <= 0.0f) return;
    int W = buf.width(), H = buf.height();
    float bx0 = 1e30f, bx1 = -1e30f, by0 = 1e30f, by1 = -1e30f;
    for (auto& sp : path.s)
        if (sp.closed)
            for (auto& p : sp.p) {
                bx0 = std::min(bx0, p.x);
                bx1 = std::max(bx1, p.x);
                by0 = std::min(by0, p.y);
                by1 = std::max(by1, p.y);
            }
    if (bx0 > bx1) return;
    int x0 = clampi(static_cast<int>(std::floor(bx0)), 0, W - 1);
    int x1 = clampi(static_cast<int>(std::ceil(bx1)), 0, W - 1);
    int y0 = clampi(static_cast<int>(std::floor(by0)), 0, H - 1);
    int y1 = clampi(static_cast<int>(std::ceil(by1)), 0, H - 1);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            if (pointInPath(path, x, y)) composite(buf, x, y, R, G, B, A);
}

void strokePath(Buf& buf, const Path& path, const Color& color, float alpha, float hw, int cap) {
    if (hw <= 0.0f || alpha <= 0.0f) return;
    int W = buf.width(), H = buf.height();
    for (auto& sp : path.s) {
        size_t n = sp.p.size();
        if (n < 2) continue;
        size_t edges = sp.closed ? n : n - 1;
        for (size_t e = 0; e < edges; ++e) {
            const Pt& a = sp.p[e];
            const Pt& b = sp.p[(e + 1) % n];
            int xi0 = clampi(static_cast<int>(std::floor(std::min(a.x, b.x) - hw)), 0, W - 1);
            int xi1 = clampi(static_cast<int>(std::ceil(std::max(a.x, b.x) + hw)), 0, W - 1);
            int yi0 = clampi(static_cast<int>(std::floor(std::min(a.y, b.y) - hw)), 0, H - 1);
            int yi1 = clampi(static_cast<int>(std::ceil(std::max(a.y, b.y) + hw)), 0, H - 1);
            for (int y = yi0; y <= yi1; ++y)
                for (int x = xi0; x <= xi1; ++x)
                    if (distToSeg(Pt{static_cast<float>(x), static_cast<float>(y)}, a, b) <= hw) composite(buf, x, y, color.r, color.g, color.b, alpha);
        }
        if (!sp.closed && cap != 0) {
            auto capAt = [&](const Pt& c) {
                int x0 = clampi(static_cast<int>(c.x - hw), 0, W - 1);
                int x1 = clampi(static_cast<int>(c.x + hw), 0, W - 1);
                int y0 = clampi(static_cast<int>(c.y - hw), 0, H - 1);
                int y1 = clampi(static_cast<int>(c.y + hw), 0, H - 1);
                for (int y = y0; y <= y1; ++y)
                    for (int x = x0; x <= x1; ++x)
                        if (hypot2(x - c.x, y - c.y) <= hw) composite(buf, x, y, color.r, color.g, color.b, alpha);
            };
            capAt(sp.p.front());
            capAt(sp.p.back());
        }
    }
}

void renderPath(Buf& buf, const Path& path, const RenderParams& rp) {
    if (rp.doFill) fillPath(buf, path, rp.fillColor.r, rp.fillColor.g, rp.fillColor.b, rp.fillAlpha);
    if (rp.doStroke) strokePath(buf, path, rp.strokeColor, rp.strokeAlpha, rp.hw, rp.cap);
}

// ---------------------------------------------------------------------------
// Style / transform helpers
// ---------------------------------------------------------------------------

void applyStyleKey(Style& s, const std::string& key, const std::string& val) {
    if (key == "fill") { bool none; s.fill = parseColor(val, none); s.fillNone = none; }
    else if (key == "stroke") { bool none; s.stroke = parseColor(val, none); s.strokeNone = none; }
    else if (key == "fill-opacity") s.fillOpacity = alphaComp(val);
    else if (key == "stroke-opacity") s.strokeOpacity = alphaComp(val);
    else if (key == "stroke-width") s.strokeWidth = parseLengthPx(val);
    else if (key == "opacity") s.fillOpacity *= alphaComp(val);
    else if (key == "stroke-linecap") {
        std::string t = toLower(trim(val));
        s.linecap = (t == "round") ? 1 : (t == "square") ? 2 : 0;
    }
}

void applyNodeStyle(Style& s, const Node* node) {
    const auto& a = node->attr;
    if (auto v = a.find("fill"); v != a.end()) { bool none; s.fill = parseColor(v->second, none); s.fillNone = none; }
    if (auto v = a.find("stroke"); v != a.end()) { bool none; s.stroke = parseColor(v->second, none); s.strokeNone = none; }
    if (auto v = a.find("fill-opacity"); v != a.end()) s.fillOpacity = alphaComp(v->second);
    if (auto v = a.find("stroke-opacity"); v != a.end()) s.strokeOpacity = alphaComp(v->second);
    if (auto v = a.find("stroke-width"); v != a.end()) s.strokeWidth = parseLengthPx(v->second);
    if (auto v = a.find("stroke-linecap"); v != a.end()) {
        std::string t = toLower(trim(v->second));
        s.linecap = (t == "round") ? 1 : (t == "square") ? 2 : 0;
    }
    if (auto v = a.find("style"); v != a.end())
        for (auto& pair : splitTop(v->second, ';')) {
            auto c = pair.find(':');
            if (c == std::string::npos) continue;
            applyStyleKey(s, toLower(trim(pair.substr(0, c))), trim(pair.substr(c + 1)));
        }
}

void parseTransform(const std::string& s, Affine& D) {
    Affine M = D;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (i >= s.size()) break;
        size_t nameStart = i;
        while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) ++i;
        std::string name = toLower(s.substr(nameStart, i - nameStart));
        while (i < s.size() && s[i] != '(') { if (i >= s.size()) break; ++i; }
        if (i >= s.size()) break;
        size_t open = i;
        ++i;
        size_t close = s.find(')', i);
        if (close == std::string::npos) close = s.size() - 1;
        std::string argsStr = s.substr(open + 1, close - open - 1);
        i = close + 1;

        std::vector<float> nums;
        {
            size_t j = 0;
            while (j < argsStr.size()) {
                if (std::isdigit(static_cast<unsigned char>(argsStr[j])) || argsStr[j] == '.' ||
                    argsStr[j] == '+' || argsStr[j] == '-')
                    nums.push_back(readNumber(argsStr, j));
                else ++j;
            }
        }
        auto at = [&](int k) { return k < static_cast<int>(nums.size()) ? nums[k] : 0.0f; };
        Affine T = Affine::identity();
        if (name == "translate") { T.e = at(0); T.f = at(1); }
        else if (name == "scale") { T.a = at(0); T.d = at(1); }
        else if (name == "rotate") {
            float a = at(0) * kPi / 180.0f;
            Affine rot{std::cos(a), std::sin(a), -std::sin(a), std::cos(a), 0, 0};
            if (at(1) != 0 || at(2) != 0)
                T = compose(compose({1, 0, 0, 1, at(1), at(2)}, rot), {1, 0, 0, 1, -at(1), -at(2)});
            else T = rot;
        } else if (name == "skewx") { T.c = std::tan(at(0) * kPi / 180.0f); }
        else if (name == "skewy") { T.b = std::tan(at(0) * kPi / 180.0f); }
        else if (name == "matrix") { T = {at(0), at(1), at(2), at(3), at(4), at(5)}; }
        M = compose(M, T);
    }
    D = M;
}

// ---------------------------------------------------------------------------
// Shape construction (user space)
// ---------------------------------------------------------------------------

void buildRectPath(const std::string& xs, const std::string& ys, const std::string& ws, const std::string& hs,
                   const std::string& rxs, const std::string& rys, Path& out) {
    float x0 = parseLengthPx(xs), y0 = parseLengthPx(ys), w = parseLengthPx(ws), h = parseLengthPx(hs);
    float rxv = parseLengthPx(rxs), ryv = parseLengthPx(rys);
    if (rxv <= 0 && ryv <= 0) {
        out.s.push_back(Subpath{{Pt{x0, y0}, Pt{x0 + w, y0}, Pt{x0 + w, y0 + h}, Pt{x0, y0 + h}}, true});
        return;
    }
    if (rxv <= 0) rxv = ryv;
    if (ryv <= 0) ryv = rxv;
    rxv = std::min(rxv, w / 2.0f);
    ryv = std::min(ryv, h / 2.0f);
    Subpath sp;
    sp.closed = true;
    auto arc = [&](float cx, float cy, float a0, float a1) {
        int n = 16;
        for (int i = 0; i <= n; ++i) {
            float a = (a0 + (a1 - a0) * i / n) * kPi / 180.0f;
            sp.p.push_back(Pt{cx + rxv * std::cos(a), cy + ryv * std::sin(a)});
        }
    };
    sp.p.push_back(Pt{x0 + rxv, y0});                       // top-edge start (A)
    arc(x0 + w - rxv, y0 + ryv, 270, 360);                  // top-right corner: B->C
    sp.p.push_back(Pt{x0 + w, y0 + ryv});                   // right-edge start (C)
    arc(x0 + w - rxv, y0 + h - ryv, 0, 90);                 // bottom-right corner: D->E
    sp.p.push_back(Pt{x0 + w - rxv, y0 + h});               // bottom-edge start (E)
    arc(x0 + rxv, y0 + h - ryv, 90, 180);                   // bottom-left corner: F->G
    sp.p.push_back(Pt{x0, y0 + h - ryv});                   // left-edge start (G)
    arc(x0 + rxv, y0 + ryv, 180, 270);                      // top-left corner: H->A
    std::vector<Pt> d;
    for (auto& p : sp.p)
        if (d.empty() || d.back().x != p.x || d.back().y != p.y) d.push_back(p);
    sp.p = std::move(d);
    out.s.push_back(std::move(sp));
}

void buildCirclePath(float cx, float cy, float r, Path& out) {
    Subpath sp;
    sp.closed = true;
    for (int i = 0; i <= 64; ++i) {
        float a = 2 * kPi * i / 64;
        sp.p.push_back(Pt{cx + r * std::cos(a), cy + r * std::sin(a)});
    }
    out.s.push_back(std::move(sp));
}

void buildEllipsePath(float cx, float cy, float rx, float ry, Path& out) {
    Subpath sp;
    sp.closed = true;
    for (int i = 0; i <= 64; ++i) {
        float a = 2 * kPi * i / 64;
        sp.p.push_back(Pt{cx + rx * std::cos(a), cy + ry * std::sin(a)});
    }
    out.s.push_back(std::move(sp));
}

// ---------------------------------------------------------------------------
// Tree walk
// ---------------------------------------------------------------------------

struct Renderer {
    Buf buf;
    int W = 0, H = 0, S = 1;

    Renderer(int w, int h, int s) { W = w; H = h; S = s; buf.assign(w * s, h * s, 1, 4, 0); }

    Path transformPath(const Path& user, const Affine& D) const {
        Path out;
        out.s.reserve(user.s.size());
        for (auto& sp : user.s) {
            Subpath o;
            o.closed = sp.closed;
            o.p.reserve(sp.p.size());
            for (auto& p : sp.p) {
                Pt q = applyPt(D, p);
                o.p.push_back(Pt{q.x * S, q.y * S});
            }
            out.s.push_back(std::move(o));
        }
        return out;
    }

    float deviceScale(const Affine& D) const {
        return (hypot2(D.a, D.b) + hypot2(D.c, D.d)) * 0.5f;
    }
};

void walkNode(Node* node, Affine D, Style style, float groupOpacity, Renderer& R,
              const std::map<std::string, Node*>& ids);

void walkNode(Node* node, Affine D, Style style, float groupOpacity, Renderer& R,
              const std::map<std::string, Node*>& ids) {
    if (!node) return;

    // group opacity (approximated per-element)
    float gop = groupOpacity;
    if (auto op = node->attr.find("opacity"); op != node->attr.end()) gop = groupOpacity * alphaComp(op->second);
    else if (auto st = node->attr.find("style"); st != node->attr.end())
        for (auto& pair : splitTop(st->second, ';')) {
            auto c = pair.find(':');
            if (c != std::string::npos && toLower(trim(pair.substr(0, c))) == "opacity")
                gop = groupOpacity * alphaComp(trim(pair.substr(c + 1)));
        }

    Affine Dhere = D;
    if (auto tf = node->attr.find("transform"); tf != node->attr.end()) parseTransform(tf->second, Dhere);

    Style s = style;
    applyNodeStyle(s, node);

    const std::string& tag = node->tag;

    auto draw = [&](const Path& user) {
        Path sample = R.transformPath(user, Dhere);
        RenderParams rp;
        rp.doFill = !s.fillNone;
        rp.fillColor = s.fill;
        rp.fillAlpha = clampf(s.fillOpacity * gop, 0, 1);
        rp.doStroke = !s.strokeNone && s.strokeWidth > 0;
        rp.strokeColor = s.stroke;
        rp.strokeAlpha = clampf(s.strokeOpacity * gop, 0, 1);
        rp.hw = s.strokeWidth * R.deviceScale(Dhere) * R.S * 0.5f;
        rp.cap = s.linecap;
        renderPath(R.buf, sample, rp);
    };

    if (tag == "rect") {
        Path p;
        buildRectPath(node->attr.count("x") ? node->attr.at("x") : "0",
                      node->attr.count("y") ? node->attr.at("y") : "0",
                      node->attr.count("width") ? node->attr.at("width") : "0",
                      node->attr.count("height") ? node->attr.at("height") : "0",
                      node->attr.count("rx") ? node->attr.at("rx") : "0",
                      node->attr.count("ry") ? node->attr.at("ry") : "0",
                      p);
        draw(p);
    } else if (tag == "circle") {
        Path p;
        buildCirclePath(parseLengthPx(node->attr.count("cx") ? node->attr.at("cx") : "0"),
                        parseLengthPx(node->attr.count("cy") ? node->attr.at("cy") : "0"),
                        parseLengthPx(node->attr.count("r") ? node->attr.at("r") : "0"), p);
        draw(p);
    } else if (tag == "ellipse") {
        Path p;
        buildEllipsePath(parseLengthPx(node->attr.count("cx") ? node->attr.at("cx") : "0"),
                         parseLengthPx(node->attr.count("cy") ? node->attr.at("cy") : "0"),
                         parseLengthPx(node->attr.count("rx") ? node->attr.at("rx") : "0"),
                         parseLengthPx(node->attr.count("ry") ? node->attr.at("ry") : "0"), p);
        draw(p);
    } else if (tag == "line") {
        Path p;
        p.s.push_back(Subpath{{
            Pt{parseLengthPx(node->attr.count("x1") ? node->attr.at("x1") : "0"),
               parseLengthPx(node->attr.count("y1") ? node->attr.at("y1") : "0")},
            Pt{parseLengthPx(node->attr.count("x2") ? node->attr.at("x2") : "0"),
               parseLengthPx(node->attr.count("y2") ? node->attr.at("y2") : "0")},
        }, false});
        draw(p);
    } else if (tag == "polyline" || tag == "polygon") {
        Path p;
        parsePoints(node->attr.count("points") ? node->attr.at("points") : "", p.s.emplace_back(Subpath{{}, false}).p);
        if (tag == "polygon") p.s.back().closed = true;
        draw(p);
    } else if (tag == "path") {
        Path p;
        parsePathD(node->attr.count("d") ? node->attr.at("d") : "", p);
        draw(p);
    } else if (tag == "use") {
        std::string href = node->attr.count("href") ? node->attr.at("href")
                     : node->attr.count("xlink:href") ? node->attr.at("xlink:href") : "";
        if (!href.empty() && href[0] == '#') {
            Node* ref = ids.count(href.substr(1)) ? ids.at(href.substr(1)) : nullptr;
            Affine Duse = Dhere;
            float ux = parseLengthPx(node->attr.count("x") ? node->attr.at("x") : "0");
            float uy = parseLengthPx(node->attr.count("y") ? node->attr.at("y") : "0");
            Duse = compose(Duse, Affine{1, 0, 0, 1, ux, uy});
            walkNode(ref, Duse, style, gop, R, ids);
        }
    } else if (tag == "defs" || tag == "symbol") {
        // not drawn directly; referenced via <use>
    } else {
        for (auto& c : node->children) walkNode(c.get(), Dhere, s, gop, R, ids);
    }
}

// ---------------------------------------------------------------------------
// Driver
// ---------------------------------------------------------------------------

struct Options {
    std::string svgPath;
    std::string svgString;
    std::string savePath;
    int width = 0;
    int height = 0;
    int supersample = 0;
    std::string background = "#ffffff";
    bool transparent = false;
};

void printUsage() {
    std::cout <<
        "svg2png — SVG to PNG conversion and texture-atlas packing.\n"
        "\n"
        "Usage:\n"
        "  svg2png png --svg-path <file.svg> --save-path <out.png>\n"
        "  svg2png png --svg-string \"<svg>...</svg>\" --save-path <out.png>\n"
        "  svg2png texture --output-path <out.cxxpng> --png-path <t1.png> [--png-path ...]\n"
        "\n"
        "Modes:\n"
        "  png       Convert an SVG document to a PNG image.\n"
        "  texture   Pack several PNGs into a single in-memory atlas and write a C++\n"
        "            header (.cxxpng) embedding the atlas PNG bytes plus a\n"
        "            std::map<std::string, coordinate_t> of each texture's rectangle.\n"
        "\n"
        "png options:\n"
        "  --svg-path <p>     Path to the input .svg file.\n"
        "  --svg-string <s>   Raw SVG markup as a string.\n"
        "  --save-path <p>    Output .png path (required).\n"
        "  -w, --width <n>    Force output width in px (default: from SVG).\n"
        "  -h, --height <n>   Force output height in px (default: from SVG).\n"
        "  -S, --supersample  Force supersampling factor (default: auto 1..8).\n"
        "  --background <c>   Background color, e.g. \"#ffffff\" (default: white).\n"
        "  --transparent      Make the background transparent.\n"
        "\n"
        "texture options:\n"
        "  --output-path <p>  Output .cxxpng C++ header (required).\n"
        "  --png-path <p>     A source PNG texture to pack (repeatable, required).\n"
        "  --atlas-path <p>   Also write the raw packed atlas PNG here (optional).\n"
        "  --shelf-width <n>  Max atlas width per shelf (default: 2048, 0 = single row).\n"
        "  --padding <n>      Gap (px) between packed textures (default: 1).\n"
        "\n"
        "  -?, --help         Show this help.\n"
        "  --version          Print version.\n";
}

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int run(const Options& opt) {
    if (opt.svgPath.empty() && opt.svgString.empty()) {
        std::cerr << "error: provide --svg-path or --svg-string\n";
        return 1;
    }
    if (opt.savePath.empty()) {
        std::cerr << "error: --save-path is required\n";
        return 1;
    }
    std::string svg = opt.svgString.empty() ? readFile(opt.svgPath) : opt.svgString;

    auto root = parseDocument(svg);
    std::map<std::string, Node*> ids;
    collectIds(root.get(), ids);

    Node* svgRoot = nullptr;
    for (auto& c : root->children)
        if (c->tag == "svg") { svgRoot = c.get(); break; }
    if (!svgRoot) svgRoot = root.get();

    const auto& a = svgRoot->attr;
    float Wf = 300, Hf = 150;
    bool hasW = a.count("width"), hasH = a.count("height");

    float vx = 0, vy = 0, vw = 0, vh = 0;
    bool hasVB = false;
    if (a.count("viewBox")) {
        std::string vb = a.at("viewBox");
        std::vector<float> nums;
        std::string cur;
        for (char ch : vb) {
            if (std::isdigit(static_cast<unsigned char>(ch)) || ch == '.' || ch == '-' || ch == '+')
                cur.push_back(ch);
            else { if (!trim(cur).empty()) nums.push_back(std::atof(cur.c_str())); cur.clear(); }
        }
        if (!trim(cur).empty()) nums.push_back(std::atof(cur.c_str()));
        if (nums.size() >= 4) { vx = nums[0]; vy = nums[1]; vw = nums[2]; vh = nums[3]; hasVB = true; }
    }

    if (hasW) Wf = parseLengthPx(a.at("width"));
    else if (hasVB) Wf = vw;
    if (hasH) Hf = parseLengthPx(a.at("height"));
    else if (hasVB) Hf = vh;
    if (opt.width > 0) Wf = opt.width;
    if (opt.height > 0) Hf = opt.height;

    int W = clampi(static_cast<int>(std::round(Wf)), 1, 16384);
    int H = clampi(static_cast<int>(std::round(Hf)), 1, 16384);

    Affine global = Affine::identity();
    if (hasVB && vw > 0 && vh > 0) {
        bool slice = a.count("preserveAspectRatio") &&
                     toLower(a.at("preserveAspectRatio")).find("slice") != std::string::npos;
        float s = slice ? std::max(W / vw, H / vh) : std::min(W / vw, H / vh);
        float ox = (W - vw * s) * 0.5f, oy = (H - vh * s) * 0.5f;
        global = {s, 0, 0, s, ox - vx * s, oy - vy * s};
    }

    int S = opt.supersample > 0 ? opt.supersample : 0;
    if (S <= 0) S = clampi(static_cast<int>(std::round(1024.0f / std::max(W, H))), 1, 8);
    const long long maxSamples = 64LL * 1024 * 1024;
    while (S > 1 && static_cast<long long>(W) * S * static_cast<long long>(H) * S > maxSamples) --S;

    Renderer R(W, H, S);

    if (!opt.transparent) {
        bool none;
        Color bg = parseColor(opt.background, none);
        for (int y = 0; y < R.buf.height(); ++y)
            for (int x = 0; x < R.buf.width(); ++x)
                composite(R.buf, x, y, bg.r, bg.g, bg.b, bg.a);
    }

    Style rootStyle;
    rootStyle.fill = {0, 0, 0, 1};
    rootStyle.strokeNone = true;
    walkNode(svgRoot, global, rootStyle, 1.0f, R, ids);

    R.buf.resize(W, H, -100, -100, 2); // box-filter downsample (moving average)

    std::vector<unsigned char> rgba(static_cast<size_t>(W) * H * 4, 0);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            int pa = R.buf(x, y, 0, 3);
            size_t o = (static_cast<size_t>(y) * W + x) * 4;
            if (pa > 0) {
                rgba[o] = static_cast<unsigned char>(clampf(R.buf(x, y, 0, 0) * 255.0f / pa, 0, 255));
                rgba[o + 1] = static_cast<unsigned char>(clampf(R.buf(x, y, 0, 1) * 255.0f / pa, 0, 255));
                rgba[o + 2] = static_cast<unsigned char>(clampf(R.buf(x, y, 0, 2) * 255.0f / pa, 0, 255));
            }
            rgba[o + 3] = static_cast<unsigned char>(pa);
        }

    unsigned err = lodepng::encode(opt.savePath, rgba.data(), W, H, LCT_RGBA, 8);
    if (err) {
        std::cerr << "error: PNG encode failed: " << lodepng_error_text(err) << "\n";
        return 1;
    }
    std::cout << "wrote " << opt.savePath << " (" << W << "x" << H << ", " << S << "x supersample)\n";
    return 0;
}

// ---------------------------------------------------------------------------
// Texture mode: pack several PNG textures into one in-memory atlas and emit a
// C++ header (.cxxpng) that embeds the atlas PNG bytes together with a map of
// texture name -> rectangle (coordinate_t) inside the atlas.  The result can be
// #included into C++ code to embed the textures without shipping extra files.
// ---------------------------------------------------------------------------

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
};

struct TextureOptions {
    std::string outputPath;          // .cxxpng header to generate (required)
    std::vector<std::string> pngPaths; // source PNGs to pack (repeatable)
    std::string atlasPath;           // optional: also write the raw atlas PNG here
    int shelfWidth = 2048;           // max atlas width per shelf (0 = single row)
    int padding = 1;                 // gap (px) between packed textures
};

std::string fileStem(const std::string& path) {
    std::filesystem::path p(path);
    std::string stem = p.stem().string();
    return stem.empty() ? p.filename().string() : stem;
}

std::string escapeCppString(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\' || c == '"') { out.push_back('\\'); out.push_back(c); }
        else out.push_back(c);
    }
    return out;
}

// FNV-1a hash used to build a unique include guard for the generated header.
std::string hashGuard(const std::string& s) {
    std::uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(h));
    return buf;
}

// Decode a PNG into a 4-channel (RGBA) CImg buffer.  lodepng owns the decoding.
bool decodePngToCImg(const std::string& path, CImg<unsigned char>& img, std::string& err) {
    std::vector<unsigned char> bytes;
    unsigned w = 0, h = 0;
    unsigned code = lodepng::decode(bytes, w, h, path, LCT_RGBA, 8);
    if (code) { err = lodepng_error_text(code); return false; }
    if (w == 0 || h == 0) { err = "empty image"; return false; }
    img.assign(w, h, 1, 4, 0);
    const size_t wh = static_cast<size_t>(w) * h;
    if (bytes.size() < wh * 4) { err = "truncated png"; return false; }
    // lodepng delivers interleaved RGBA; CImg stores channels block-planar, so
    // de-interleave while copying.
    for (size_t i = 0; i < wh; ++i) {
        img.data()[i]       = bytes[i * 4 + 0];
        img.data()[i + wh]  = bytes[i * 4 + 1];
        img.data()[i + 2 * wh] = bytes[i * 4 + 2];
        img.data()[i + 3 * wh] = bytes[i * 4 + 3];
    }
    return true;
}

// Shelf (row) packer: place textures left-to-right, wrapping to a new shelf
// when one does not fit.  Produces a tight bounding box.  `padding` is the gap
// between neighbouring textures.
void packShelves(const std::vector<Rect>& texs, int shelfWidth, int padding,
                 std::vector<Rect>& out, int& aw, int& ah) {
    int shelfX = 0, shelfY = 0, rightmost = 0, bottommost = 0;
    for (const auto& t : texs) {
        if (shelfX != 0 && shelfWidth > 0 && shelfX + t.w > shelfWidth) {
            shelfY = bottommost;
            shelfX = 0;
        }
        out.push_back(Rect{shelfX, shelfY, t.w, t.h});
        rightmost = std::max(rightmost, shelfX + t.w);
        bottommost = std::max(bottommost, shelfY + t.h);
        shelfX += t.w + padding;
    }
    aw = rightmost;
    ah = bottommost;
}

// Emit a C++ byte-initialiser list, 16 values per line.
void emitByteList(std::ostream& os, const std::vector<unsigned char>& bytes) {
    os << "{\n    ";
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i != 0 && i % 16 == 0) os << "\n    ";
        char buf[8];
        std::snprintf(buf, sizeof(buf), "0x%02x", bytes[i]);
        os << buf;
        if (i + 1 < bytes.size()) os << ", ";
    }
    if (!bytes.empty()) os << "\n    ";
    os << "}";
}

int runTexture(const TextureOptions& opt) {
    if (opt.outputPath.empty()) {
        std::cerr << "error: --output-path is required\n";
        return 1;
    }
    if (opt.pngPaths.empty()) {
        std::cerr << "error: at least one --png-path is required\n";
        return 1;
    }

    // 1) Decode every source PNG into an RGBA buffer (lodepng).
    std::vector<Rect> sizes;
    std::vector<std::string> names;
    std::vector<CImg<unsigned char>> imgs;
    sizes.reserve(opt.pngPaths.size());
    names.reserve(opt.pngPaths.size());
    imgs.reserve(opt.pngPaths.size());
    for (const auto& p : opt.pngPaths) {
        CImg<unsigned char> img;
        std::string err;
        if (!decodePngToCImg(p, img, err)) {
            std::cerr << "error: cannot read '" << p << "': " << err << "\n";
            return 1;
        }
        sizes.push_back(Rect{0, 0, img.width(), img.height()});
        names.push_back(fileStem(p));
        imgs.push_back(std::move(img));
    }

    // 2) Pack the textures into a single atlas (CImg).
    std::vector<Rect> placed;
    int aw = 0, ah = 0;
    packShelves(sizes, opt.shelfWidth, opt.padding, placed, aw, ah);

    CImg<unsigned char> atlas(aw, ah, 1, 4, 0); // fully transparent background
    for (size_t i = 0; i < imgs.size(); ++i) {
        const Rect& r = placed[i];
        for (int ty = 0; ty < r.h; ++ty)
            for (int tx = 0; tx < r.w; ++tx) {
                // Channel-by-channel: CImg operator() addresses a single channel.
                atlas(r.x + tx, r.y + ty, 0, 0) = imgs[i](tx, ty, 0, 0);
                atlas(r.x + tx, r.y + ty, 0, 1) = imgs[i](tx, ty, 0, 1);
                atlas(r.x + tx, r.y + ty, 0, 2) = imgs[i](tx, ty, 0, 2);
                atlas(r.x + tx, r.y + ty, 0, 3) = imgs[i](tx, ty, 0, 3);
            }
    }

    // 3) Encode the atlas PNG.  The bytes are read back so they can be embedded.
    std::string atlasFile = opt.atlasPath.empty() ? (opt.outputPath + ".atlas.png") : opt.atlasPath;
    const size_t wh = static_cast<size_t>(aw) * ah;
    // CImg stores channels block-planar; interleave into RGBA for lodepng.
    std::vector<unsigned char> interleaved(wh * 4);
    for (size_t i = 0; i < wh; ++i) {
        interleaved[i * 4 + 0] = atlas.data()[i];
        interleaved[i * 4 + 1] = atlas.data()[i + wh];
        interleaved[i * 4 + 2] = atlas.data()[i + 2 * wh];
        interleaved[i * 4 + 3] = atlas.data()[i + 3 * wh];
    }
    unsigned e = lodepng::encode(atlasFile, interleaved, aw, ah, LCT_RGBA, 8);
    if (e) {
        std::cerr << "error: atlas encode failed: " << lodepng_error_text(e) << "\n";
        return 1;
    }
    std::ifstream f(atlasFile, std::ios::binary);
    std::vector<unsigned char> pngBytes((std::istreambuf_iterator<char>(f)),
                                        std::istreambuf_iterator<char>());
    if (opt.atlasPath.empty()) std::remove(atlasFile.c_str()); // keep only the header

    // 4) Emit the C++ header: embedded PNG bytes + name -> coordinate map.
    std::ofstream hdr(opt.outputPath);
    if (!hdr) {
        std::cerr << "error: cannot open output '" << opt.outputPath << "'\n";
        return 1;
    }
    const std::string guard = "SVG2PNG_ATLAS_" + hashGuard(opt.outputPath) + "_H_";
    hdr << "// ---------------------------------------------------------------------------\n";
    hdr << "// This file was GENERATED by svg2png (texture mode). Do not edit by hand.\n";
    hdr << "//\n";
    hdr << "// It embeds a single packed texture atlas (as raw PNG bytes) together with a\n";
    hdr << "// std::map<std::string, coordinate_t> giving each texture's rectangle inside\n";
    hdr << "// the atlas.  #include it to embed the textures directly in your application.\n";
    hdr << "//\n";
    hdr << "// Example:\n";
    hdr << "//   unsigned w, h;\n";
    hdr << "//   std::vector<unsigned char> px;\n";
    hdr << "//   lodepng::decode(texture_atlas::atlas_png.data(), w, h, LCT_RGBA,\n";
    hdr << "//                 texture_atlas::atlas_png.size());\n";
    hdr << "//   for (const auto& kv : texture_atlas::textures())\n";
    hdr << "//     { const auto& c = kv.second; /* blit px at (c.x,c.y,c.width,c.height) */; }\n";
    hdr << "// ---------------------------------------------------------------------------\n";
    hdr << "#ifndef " << guard << "\n#define " << guard << "\n\n";
    hdr << "#include <array>\n#include <cstdint>\n#include <map>\n#include <string>\n\n";
    hdr << "namespace texture_atlas {\n\n";
    hdr << "  // Rectangle of a texture inside the atlas (top-left origin, pixels).\n";
    hdr << "  struct coordinate_t {\n";
    hdr << "      std::uint32_t x = 0, y = 0, width = 0, height = 0;\n";
    hdr << "  };\n\n";
    hdr << "  // Raw PNG bytes of the packed atlas (decode with lodepng).\n";
    hdr << "  static const std::array<std::uint8_t, " << pngBytes.size() << "> atlas_png = ";
    emitByteList(hdr, pngBytes);
    hdr << ";\n\n";
    hdr << "  // Name -> rectangle map for every packed texture.\n";
    hdr << "  inline const std::map<std::string, coordinate_t>& textures() {\n";
    hdr << "      static const std::map<std::string, coordinate_t> m = {\n";
    for (size_t i = 0; i < placed.size(); ++i) {
        hdr << "          {\"" << escapeCppString(names[i]) << "\", {"
            << placed[i].x << ", " << placed[i].y << ", "
            << placed[i].w << ", " << placed[i].h << "}},\n";
    }
    hdr << "      };\n";
    hdr << "      return m;\n";
    hdr << "  }\n\n";
    hdr << "} // namespace texture_atlas\n\n";
    hdr << "#endif // " << guard << "\n";

    std::cout << "wrote " << opt.outputPath << " (" << placed.size() << " textures, "
              << aw << "x" << ah << " atlas, " << pngBytes.size() << " PNG bytes)\n";
    return 0;
}

} // namespace

// Parse the options that follow a `png` subcommand (or bare options, kept for
// backward compatibility).  Returns false on a usage error, true on success
// (including when --help was printed, which already returned 0 by the caller).
bool parsePngArgs(int argc, char** argv, Options& opt) {
    for (int i = 0; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&](std::string& out) -> bool {
            if (i + 1 < argc) { out = argv[++i]; return true; }
            std::cerr << "error: option '" << arg << "' expects a value\n";
            return false;
        };
        if (arg == "--svg-path" || arg == "--input") { std::string v; if (!next(v)) return false; opt.svgPath = v; }
        else if (arg == "--svg-string") { std::string v; if (!next(v)) return false; opt.svgString = v; }
        else if (arg == "--save-path" || arg == "--output" || arg == "-o") { std::string v; if (!next(v)) return false; opt.savePath = v; }
        else if (arg == "-w" || arg == "--width") { std::string v; if (!next(v)) return false; opt.width = std::atoi(v.c_str()); }
        else if (arg == "-h" || arg == "--height") { std::string v; if (!next(v)) return false; opt.height = std::atoi(v.c_str()); }
        else if (arg == "-S" || arg == "--supersample") { std::string v; if (!next(v)) return false; opt.supersample = std::atoi(v.c_str()); }
        else if (arg == "--background") { std::string v; if (!next(v)) return false; opt.background = v; }
        else if (arg == "--transparent") { opt.transparent = true; }
        else if (arg == "--help" || arg == "-?") { printUsage(); return true; }
        else { std::cerr << "error: unknown option '" << arg << "'\n"; return false; }
    }
    return true;
}

// Parse the options that follow a `texture` subcommand.  `--png-path` is
// repeatable so several textures can be packed in one invocation.
bool parseTextureArgs(int argc, char** argv, TextureOptions& opt) {
    for (int i = 0; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&](std::string& out) -> bool {
            if (i + 1 < argc) { out = argv[++i]; return true; }
            std::cerr << "error: option '" << arg << "' expects a value\n";
            return false;
        };
        if (arg == "--output-path") { std::string v; if (!next(v)) return false; opt.outputPath = v; }
        else if (arg == "--png-path") { std::string v; if (!next(v)) return false; opt.pngPaths.push_back(v); }
        else if (arg == "--atlas-path") { std::string v; if (!next(v)) return false; opt.atlasPath = v; }
        else if (arg == "--shelf-width") { std::string v; if (!next(v)) return false; opt.shelfWidth = std::atoi(v.c_str()); }
        else if (arg == "--padding") { std::string v; if (!next(v)) return false; opt.padding = std::max(0, std::atoi(v.c_str())); }
        else if (arg == "--help" || arg == "-?") { printUsage(); return true; }
        else { std::cerr << "error: unknown option '" << arg << "' for texture mode\n"; return false; }
    }
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) { printUsage(); return 1; }
    const std::string mode = argv[1];
    const bool hasModeKeyword = (mode == "png" || mode == "texture" || mode == "help" ||
        mode == "--help" || mode == "version" || mode == "--version");
    const int start = hasModeKeyword ? 2 : 1;

    // Global queries first, before the mode keyword is otherwise interpreted
    // (otherwise a bare-option fallback such as "--version" would look like a
    // png option).
    if (mode == "version" || mode == "--version") {
        std::cout << "svg2png 1.1 (CImg " << cimg_version << " / lodepng " << LODEPNG_VERSION_STRING << ")\n";
        return 0;
    }
    if (mode == "help" || mode == "--help") {
        printUsage(); return 0;
    }

    if (mode == "texture") {
        TextureOptions opt;
        if (!parseTextureArgs(argc - start, argv + start, opt)) return 1;
        try { return runTexture(opt); }
        catch (const std::exception& e) { std::cerr << "error: " << e.what() << "\n"; return 1; }
    } else if (mode == "png" || (mode[0] == '-')) {
        // Bare options with no subcommand are treated as the default "png" mode.
        Options opt;
        if (!parsePngArgs(argc - start, argv + start, opt)) return 1;
        try { return run(opt); }
        catch (const std::exception& e) { std::cerr << "error: " << e.what() << "\n"; return 1; }
    }

    std::cerr << "error: unknown mode '" << mode << "' (expected 'png' or 'texture')\n";
    printUsage();
    return 1;
}
