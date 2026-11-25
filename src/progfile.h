// progfile.h - loads MPC .progression files from the plugin's Content/Progressions folder.
//
// SPDX-License-Identifier: MIT
//
// An MPC .progression file is JSON:
//   { "progression": { "name": "...", "rootNote": "D", "scale": "Major",
//                      "chords": [ { "name": "Gmaj7", "notes": [43, 59, 62, 66] }, ... ] } }
// Each chord keeps its exact notes (played as written, like the web app does) plus a root and
// quality worked out from its name, so Sloppy and Lock know which key plays it.
// Loading reads files and allocates: constructor only, never on the audio thread.

#ifndef CHORD_STAGE_PROGFILE_H
#define CHORD_STAGE_PROGFILE_H

#include "theory.h"
#include "mpc_resources.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

#if !defined(_WIN32)
  #include <dirent.h>
#endif

namespace cs {

constexpr int kMaxFiles = 64;          // "Prog File" slots 1..64
constexpr int kMaxFileSteps = 256;
constexpr int kMaxVoicing = 10;

struct FileChord
{
    Chord chord;
    std::string name;                   // shown on screen, spelled for the file's key
    int notes[kMaxVoicing] = {0};
    int count = 0;
};
struct ProgFile
{
    std::string title;                  // file name without .progression
    int keyRoot = 0, scale = 0;
    std::vector<FileChord> chords;
};

// ---------------------------------------------------------------------------
// A small JSON reader: just enough for .progression files (objects, arrays,
// strings, numbers, true/false/null). Unknown fields are skipped.
// ---------------------------------------------------------------------------
struct Json
{
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    double num = 0; bool b = false; std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;
    const Json* get(const char* k) const { for (auto& kv : obj) if (kv.first == k) return &kv.second; return nullptr; }
};
struct JsonParser
{
    const std::string& s; size_t i = 0; int depth = 0;
    explicit JsonParser(const std::string& src) : s(src) {}
    void ws() { while (i < s.size() && std::isspace((unsigned char)s[i])) ++i; }
    bool str(std::string& out)
    {
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        while (i < s.size() && s[i] != '"')
        {
            char c = s[i++];
            if (c == '\\' && i < s.size())
            {
                char e = s[i++];
                if (e == 'n') c = '\n'; else if (e == 't') c = '\t';
                else if (e == 'u') { i += 4; c = '?'; }   // non-ASCII escapes: not needed here
                else c = e;
            }
            out += c;
        }
        if (i >= s.size()) return false;
        ++i; return true;
    }
    bool value(Json& v)
    {
        if (++depth > 64) return false;
        ws();
        if (i >= s.size()) return false;
        const char c = s[i];
        bool ok = true;
        if (c == '{')
        {
            v.type = Json::Obj; ++i; ws();
            if (i < s.size() && s[i] == '}') { ++i; }
            else for (;;)
            {
                ws(); std::string k; if (!str(k)) { ok = false; break; }
                ws(); if (i >= s.size() || s[i] != ':') { ok = false; break; } ++i;
                Json child; if (!value(child)) { ok = false; break; }
                v.obj.emplace_back(std::move(k), std::move(child));
                ws(); if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; break; }
                ok = false; break;
            }
        }
        else if (c == '[')
        {
            v.type = Json::Arr; ++i; ws();
            if (i < s.size() && s[i] == ']') { ++i; }
            else for (;;)
            {
                Json child; if (!value(child)) { ok = false; break; }
                v.arr.push_back(std::move(child));
                ws(); if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; break; }
                ok = false; break;
            }
        }
        else if (c == '"') { v.type = Json::Str; ok = str(v.str); }
        else if (c == 't' && s.compare(i, 4, "true") == 0) { v.type = Json::Bool; v.b = true; i += 4; }
        else if (c == 'f' && s.compare(i, 5, "false") == 0) { v.type = Json::Bool; i += 5; }
        else if (c == 'n' && s.compare(i, 4, "null") == 0) { i += 4; }
        else
        {
            char* end = nullptr; v.num = std::strtod(s.c_str() + i, &end);
            if (end == s.c_str() + i) ok = false;
            v.type = Json::Num; i = (size_t)(end - s.c_str());
        }
        --depth;
        return ok;
    }
};

// ---------------------------------------------------------------------------
// Chord names: "F#m7/E" -> root F#, quality m7 (the bass note comes from the voicing)
// ---------------------------------------------------------------------------
// MPC sometimes writes the real sharp and flat signs (UTF-8 "♯" / "♭"): turn them into # and b
inline std::string asciiAccidentals(std::string s)
{
    static const char* const sharp = "\xE2\x99\xAF";
    static const char* const flat = "\xE2\x99\xAD";
    for (size_t at; (at = s.find(sharp)) != std::string::npos;) s.replace(at, 3, "#");
    for (size_t at; (at = s.find(flat)) != std::string::npos;) s.replace(at, 3, "b");
    return s;
}

inline int letterPc(char c) { static const int pc[7] = {9, 11, 0, 2, 4, 5, 7}; return (c >= 'A' && c <= 'G') ? pc[c - 'A'] : -1; }

// Root pitch class and length of the root part ("C#" -> 1, 2), or -1
inline int parseRoot(const std::string& n, size_t& len)
{
    len = 0;
    if (n.empty()) return -1;
    int pc = letterPc(n[0]); if (pc < 0) return -1;
    len = 1;
    if (n.size() > 1 && n[1] == '#') { ++pc; len = 2; }
    else if (n.size() > 1 && n[1] == 'b') { --pc; len = 2; }
    return mod12(pc);
}

inline int qualityFromSuffix(std::string suf, const int* notes, int count, int rootPc)
{
    const size_t slash = suf.find('/');
    if (slash != std::string::npos) suf = suf.substr(0, slash);
    if (suf.compare(0, 3, "min") == 0 && suf.compare(0, 4, "minM") != 0) suf = "m" + suf.substr(3);   // MPC writes Cmin9
    static const struct { const char* s; int q; } map[] = {
        {"", qMaj}, {"maj", qMaj}, {"M", qMaj}, {"m", qMin}, {"-", qMin}, {"dim", qDim}, {"o", qDim}, {"aug", qAug}, {"+", qAug},
        {"maj7", qMaj7}, {"M7", qMaj7}, {"7", q7}, {"m7", qMin7}, {"m7b5", qM7b5}, {"dim7", qDim7}, {"o7", qDim7},
        {"mMaj7", qMinMaj7}, {"mmaj7", qMinMaj7}, {"sus2", qSus2}, {"sus4", qSus4}, {"sus", qSus4}, {"6", q6}, {"m6", qMin6},
        {"9", q9}, {"7(9)", q9}, {"m9", qMin9}, {"maj9", qMaj9}, {"M9", qMaj9}, {"add9", qAdd9}, {"(9)", qAdd9}, {"add2", qAdd9},
    };
    for (auto& m : map) if (suf == m.s) return m.q;
    // Unknown symbol: name it from the voicing, minor if it has a minor 3rd and no major 3rd
    bool min3 = false, maj3 = false;
    for (int i = 0; i < count; ++i) { const int iv = mod12(notes[i] - rootPc); if (iv == 3) min3 = true; if (iv == 4) maj3 = true; }
    return (min3 && !maj3) ? qMin : qMaj;
}

inline bool loadProgFile(const std::string& path, const std::string& title, ProgFile& out)
{
    std::string text;
    if (!mpc::readTextFile(path, text)) return false;
    Json root; JsonParser p(text);
    if (!p.value(root)) return false;
    const Json* pr = root.get("progression");
    if (!pr || pr->type != Json::Obj) return false;
    const Json* chords = pr->get("chords");
    if (!chords || chords->type != Json::Arr || chords->arr.empty()) return false;

    out.title = title;
    out.keyRoot = 0;
    if (const Json* r = pr->get("rootNote")) if (r->type == Json::Str) { size_t l; const int pc = parseRoot(asciiAccidentals(r->str), l); if (pc >= 0) out.keyRoot = pc; }

    // Scale: MPC names -> ours. "Chromatic" (or unknown): major or minor, whichever fits more chord notes
    std::string sc; if (const Json* s = pr->get("scale")) if (s->type == Json::Str) sc = s->str;
    for (char& c : sc) c = (char)std::tolower((unsigned char)c);
    static const struct { const char* n; int idx; } scales[] = {
        {"major", 0}, {"minor", 1}, {"natural minor", 1}, {"harmonic minor", 2}, {"dorian", 3}, {"mixolydian", 4},
        {"major pentatonic", 5}, {"minor pentatonic", 6}, {"blues", 7}};
    out.scale = -1;
    for (auto& s : scales) if (sc == s.n) out.scale = s.idx;

    for (const Json& c : chords->arr)
    {
        if ((int)out.chords.size() >= kMaxFileSteps) break;
        if (c.type != Json::Obj) continue;
        FileChord fc;
        if (const Json* ns = c.get("notes")) if (ns->type == Json::Arr)
            for (const Json& n : ns->arr)
                if (n.type == Json::Num && n.num >= 0 && n.num < 128 && fc.count < kMaxVoicing) fc.notes[fc.count++] = (int)n.num;
        if (fc.count == 0) continue;
        std::sort(fc.notes, fc.notes + fc.count);
        std::string name; if (const Json* nm = c.get("name")) if (nm->type == Json::Str) name = nm->str;
        std::string clean; for (char ch : asciiAccidentals(name)) if (ch != ' ') clean += ch;
        size_t rl = 0; int rootPc = parseRoot(clean, rl);
        if (rootPc < 0) { rootPc = mod12(fc.notes[0]); clean.clear(); rl = 0; }
        fc.chord.rootPc = (int8_t)rootPc;
        fc.chord.q = (int8_t)qualityFromSuffix(rl ? clean.substr(rl) : std::string(), fc.notes, fc.count, rootPc);
        fc.chord.degree = -1;
        fc.name = clean;   // respelled once the key is known (below)
        out.chords.push_back(fc);
    }
    if (out.chords.empty()) return false;

    if (out.scale < 0)
    {
        int fitMaj = 0, fitMin = 0;
        Key kMaj; kMaj.root = out.keyRoot; kMaj.scale = 0;
        Key kMin; kMin.root = out.keyRoot; kMin.scale = 1;
        for (auto& fc : out.chords) for (int i = 0; i < fc.count; ++i) { fitMaj += kMaj.inScale(fc.notes[i]); fitMin += kMin.inScale(fc.notes[i]); }
        out.scale = fitMin > fitMaj ? 1 : 0;
    }
    // MPC always writes sharps: respell roots and bass notes the way the key does (D#maj7/A# in Eb -> Ebmaj7/Bb)
    Key k; k.root = out.keyRoot; k.scale = out.scale;
    const char* const* names = k.usesFlats() ? kFlat : kSharp;
    for (auto& fc : out.chords)
    {
        std::string& n = fc.name;
        if (n.empty())
        {
            n = std::string(names[fc.chord.rootPc]) + kQualities[fc.chord.q].sym;
            if (mod12(fc.notes[0]) != fc.chord.rootPc) n += std::string("/") + names[mod12(fc.notes[0])];
            continue;
        }
        size_t rl; parseRoot(n, rl);
        n = std::string(names[fc.chord.rootPc]) + n.substr(rl);
        const size_t slash = n.find('/');
        if (slash != std::string::npos)
        {
            size_t bl; const int b = parseRoot(n.substr(slash + 1), bl);
            if (b >= 0) n = n.substr(0, slash + 1) + names[b] + n.substr(slash + 1 + bl);
        }
        fc.chord.spell = k.usesFlats() ? kSpellFlat : kSpellSharp;
    }
    return true;
}

// Every *.progression in Content/Progressions, sorted by name (case-insensitive)
inline std::vector<ProgFile> loadProgFiles()
{
    std::vector<ProgFile> files;
#if !defined(_WIN32)
    const std::string dir = mpc::contentPath("Progressions");
    DIR* d = opendir(dir.c_str());
    if (!d) return files;
    std::vector<std::string> names;
    while (dirent* e = readdir(d))
    {
        const std::string n = e->d_name;
        if (n.size() <= 12 || n[0] == '.') continue;   // skips "._" files macOS leaves on SD cards
        std::string ext = n.substr(n.size() - 12);
        for (char& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext == ".progression") names.push_back(n);
    }
    closedir(d);
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        std::string x = a, y = b;
        for (char& c : x) c = (char)std::tolower((unsigned char)c);
        for (char& c : y) c = (char)std::tolower((unsigned char)c);
        return x < y;
    });
    for (const std::string& n : names)
    {
        if ((int)files.size() >= kMaxFiles) break;
        ProgFile f;
        if (loadProgFile(dir + "/" + n, n.substr(0, n.size() - 12), f)) files.push_back(std::move(f));
    }
#endif
    return files;
}

} // namespace cs

#endif // CHORD_STAGE_PROGFILE_H
