// theory.h - keys, scales, chords, preset progressions and voicing for Chord Stage.
//
// SPDX-License-Identifier: MIT
//
// A port of the chord logic in the Chord Stage web app (chord_stage/index.html):
//   * SCALES / inScale / snapNearest / degreeOf  -> key + scale correction
//   * keyChords                                   -> the 7 diatonic triads or 7ths of the key
//   * leadVoice / plainVoice                      -> Smooth voicing (voice leading) or root position
//   * preset progressions written as Roman numerals (counted from the major scale, so
//     minor progressions use flats: "i bVI bIII bVII")
// Everything here is plain data and small loops. Nothing allocates after construction,
// so it is safe to call from the audio thread.

#ifndef CHORD_STAGE_THEORY_H
#define CHORD_STAGE_THEORY_H

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace cs {

inline int mod12(int n) { return ((n % 12) + 12) % 12; }

// ---------------------------------------------------------------------------
// Note names
// ---------------------------------------------------------------------------
static const char* const kSharp[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
static const char* const kFlat[12]  = {"C","Db","D","Eb","E","F","Gb","G","Ab","A","Bb","B"};
static const char* const kKeyNames[12] = {"C","Db","D","Eb","E","F","F#","G","Ab","A","Bb","B"};
inline bool flatMajor(int pc) { pc = mod12(pc); return pc == 5 || pc == 10 || pc == 3 || pc == 8 || pc == 1 || pc == 6; }

// ---------------------------------------------------------------------------
// Scales. notes = what the key allows; chord = the 7-note scale chords are built from;
// parent = offset to the related major key (decides sharps or flats).
// ---------------------------------------------------------------------------
struct Scale { const char* name; int notes[7]; int numNotes; int chord[7]; int parent; };
static const Scale kScales[] = {
    {"Major",    {0,2,4,5,7,9,11}, 7, {0,2,4,5,7,9,11}, 0},
    {"Minor",    {0,2,3,5,7,8,10}, 7, {0,2,3,5,7,8,10}, 3},
    {"Harm Min", {0,2,3,5,7,8,11}, 7, {0,2,3,5,7,8,11}, 3},
    {"Dorian",   {0,2,3,5,7,9,10}, 7, {0,2,3,5,7,9,10}, 10},
    {"Mixolyd",  {0,2,4,5,7,9,10}, 7, {0,2,4,5,7,9,10}, 5},
    {"Maj Pent", {0,2,4,7,9},      5, {0,2,4,5,7,9,11}, 0},
    {"Min Pent", {0,3,5,7,10},     5, {0,2,3,5,7,8,10}, 3},
    {"Blues",    {0,3,5,6,7,10},   6, {0,2,3,5,7,8,10}, 3},
};
constexpr int kNumScales = (int)(sizeof(kScales) / sizeof(kScales[0]));
static const int kMajor[7] = {0,2,4,5,7,9,11};

// ---------------------------------------------------------------------------
// Chord qualities: intervals above the root and the symbol shown on screen.
// ---------------------------------------------------------------------------
struct Quality { const char* sym; int iv[4]; int n; };
enum Q { qMaj = 0, qMin, qDim, qAug, qMaj7, q7, qMin7, qM7b5, qDim7, qMinMaj7, qMaj7s5,
         qSus2, qSus4, q6, qMin6, q9, qMin9, qMaj9, qAdd9, kNumQualities };
static const Quality kQualities[kNumQualities] = {
    {"",     {4,7},      2}, {"m",     {3,7},      2}, {"dim",  {3,6},    2}, {"aug",    {4,8},    2},
    {"maj7", {4,7,11},   3}, {"7",     {4,7,10},   3}, {"m7",   {3,7,10}, 3}, {"m7b5",   {3,6,10}, 3},
    {"dim7", {3,6,9},    3}, {"mMaj7", {3,7,11},   3}, {"maj7#5",{4,8,11},3},
    {"sus2", {2,7},      2}, {"sus4",  {5,7},      2}, {"6",    {4,7,9},  3}, {"m6",     {3,7,9},  3},
    {"9",    {4,7,10,14},4}, {"m9",    {3,7,10,14},4}, {"maj9", {4,7,11,14},4}, {"add9", {4,7,14}, 3},
};

// Spelling of a chord's root on screen
enum Spell : uint8_t { kSpellKey = 0, kSpellFlat = 1, kSpellSharp = 2 };

struct Chord
{
    int8_t rootPc = -1;     // -1 = no chord
    int8_t q = qMaj;
    int8_t degree = -1;     // 0..6 inside the key, -1 outside
    uint8_t spell = kSpellKey;
    int16_t fileRef = -1;   // chord from a .progression file: (file << 8) | step, else -1
    bool valid() const { return rootPc >= 0; }
    // "id" used to match a played chord against a progression step: root + 3rd + 5th
    bool sameAs(const Chord& o) const
    {
        return rootPc == o.rootPc && kQualities[q].iv[0] == kQualities[o.q].iv[0] && kQualities[q].iv[1] == kQualities[o.q].iv[1];
    }
    // Packed into 16 bits so the UI thread can read it from an atomic.
    uint32_t pack() const
    {
        if (!valid()) return 0u;
        if (fileRef >= 0) return (1u << 15) | (1u << 14) | (uint32_t)fileRef;
        return (1u << 15) | ((uint32_t)rootPc) | ((uint32_t)q << 4) | ((uint32_t)spell << 9);
    }
    static Chord unpack(uint32_t v)
    {
        Chord c; if (!(v & (1u << 15))) return c;
        if (v & (1u << 14)) { c.rootPc = 0; c.fileRef = (int16_t)(v & 0x3FFF); return c; }
        c.rootPc = (int8_t)(v & 15); c.q = (int8_t)((v >> 4) & 31); c.spell = (uint8_t)((v >> 9) & 3); return c;
    }
};

// ---------------------------------------------------------------------------
// The key
// ---------------------------------------------------------------------------
struct Key
{
    int root = 0;      // 0..11
    int scale = 0;     // index into kScales

    const Scale& sc() const { return kScales[scale]; }
    bool inScale(int n) const
    {
        const int rel = mod12(n - root);
        for (int i = 0; i < sc().numNotes; ++i) if (sc().notes[i] == rel) return true;
        return false;
    }
    bool inChordScale(int n) const
    {
        const int rel = mod12(n - root);
        for (int i = 0; i < 7; ++i) if (sc().chord[i] == rel) return true;
        return false;
    }
    // Nearest note in the key; ties go down
    int snapNearest(int n) const
    {
        if (inScale(n)) return n;
        for (int d = 1; d < 7; ++d) { if (inScale(n - d)) return n - d; if (inScale(n + d)) return n + d; }
        return n;
    }
    // Scale degree (0..6) of a key, using the 7-note chord scale
    int degreeOf(int n) const
    {
        const int rel = mod12(n - root);
        int best = 0, bestDist = 99;
        for (int d = 0; d < 7; ++d)
        {
            const int iv = sc().chord[d];
            const int a = mod12(rel - iv), b = mod12(iv - rel), dist = a < b ? a : b;
            if (dist < bestDist || (dist == bestDist && iv < rel)) { best = d; bestDist = dist; }
        }
        return best;
    }
    // The chord on scale degree d (triad or 7th)
    Chord diatonic(int d, bool sevenths) const
    {
        const int* c = sc().chord;
        const int base = c[d];
        int tones[3]; const int steps = sevenths ? 3 : 2;
        for (int s = 0; s < steps; ++s) { const int i = d + 2 * (s + 1); tones[s] = c[i % 7] + 12 * (i / 7) - base; }
        Chord ch; ch.rootPc = (int8_t)mod12(root + base); ch.degree = (int8_t)d; ch.q = qMaj;
        for (int q = 0; q < kNumQualities; ++q)
        {
            const Quality& Q = kQualities[q];
            if (Q.n != steps) continue;
            bool ok = true; for (int s = 0; s < steps; ++s) if (Q.iv[s] != tones[s]) { ok = false; break; }
            if (ok) { ch.q = (int8_t)q; break; }
        }
        return ch;
    }
    // Flats or sharps for this key (the web app's spelling())
    bool usesFlats() const { return flatMajor(root + sc().parent); }

    std::string name(const Chord& c) const
    {
        if (!c.valid()) return "-";
        const bool flats = c.spell == kSpellFlat ? true : c.spell == kSpellSharp ? false : usesFlats();
        return std::string(flats ? kFlat[c.rootPc] : kSharp[c.rootPc]) + kQualities[c.q].sym;
    }
};

// ---------------------------------------------------------------------------
// Preset progressions (Roman numerals; upper case = major, lower case = minor;
// b / # count from the major scale; suffixes 7, maj7, 9, maj9, dim, ø, +).
// A plain numeral that matches the key's own triad follows the 7ths switch.
// ---------------------------------------------------------------------------
struct ProgPreset { const char* name; const char* text; };
static const ProgPreset kProgPresets[] = {
    {"None",       ""},
    {"Pop",        "I V vi IV"},
    {"Doo-Wop",    "I vi IV V"},
    {"Sad Pop",    "vi IV I V"},
    {"Canon",      "I V vi iii IV I IV V"},
    {"Jazz ii-V",  "ii7 V7 Imaj7 Imaj7"},
    {"Turnaround", "Imaj7 vi7 ii7 V7"},
    {"Neo Soul",   "IVmaj7 iii7 ii7 Imaj7"},
    {"R&B 9ths",   "ii9 V9 Imaj9 vi9"},
    {"Royal Road", "IVmaj7 V7 iii7 vi7"},
    {"Gospel",     "I I7 IV iv I vi ii7 V7"},
    {"Blues 12",   "I7 I7 I7 I7 IV7 IV7 I7 I7 V7 IV7 I7 V7"},
    {"Minor Pop",  "i bVI bIII bVII"},
    {"Andalusian", "i bVII bVI V"},
    {"Dark Minor", "i iv bVI V"},
    {"Rock bVII",  "I bVII IV I"},
};
constexpr int kNumProgPresets = (int)(sizeof(kProgPresets) / sizeof(kProgPresets[0]));
constexpr int kMaxSteps = 32;

// One step of a progression, relative to the key root
struct ProgStep
{
    int rel = 0;          // semitones above the key root
    int q = qMaj;
    bool plain = false;   // no suffix: follows the 7ths switch when it is the key's own chord
    uint8_t spell = kSpellKey;
};
struct Progression { ProgStep steps[kMaxSteps]; int n = 0; };

// Parses "ii7 V7 Imaj7" (constructor time only).
inline Progression parseProgression(const char* text)
{
    Progression p;
    static const char* const romans[7] = {"VII","VI","IV","V","III","II","I"};   // longest first
    static const int romanDeg[7] = {6, 5, 3, 4, 2, 1, 0};
    const char* s = text;
    while (*s && p.n < kMaxSteps)
    {
        while (*s == ' ') ++s;
        if (!*s) break;
        const char* e = s; while (*e && *e != ' ') ++e;
        std::string tok(s, (size_t)(e - s)); s = e;
        size_t i = 0; int acc = 0;
        if (tok[i] == 'b') { acc = -1; ++i; } else if (tok[i] == '#') { acc = 1; ++i; }
        int deg = -1; bool lower = false;
        for (int r = 0; r < 7 && deg < 0; ++r)
        {
            const size_t len = std::strlen(romans[r]);
            if (tok.size() - i < len) continue;
            bool up = true, lo = true;
            for (size_t k = 0; k < len; ++k)
            {
                if (tok[i + k] != romans[r][k]) up = false;
                if (tok[i + k] != (char)(romans[r][k] - 'A' + 'a')) lo = false;
            }
            if (up || lo) { deg = romanDeg[r]; lower = lo; i += len; }
        }
        if (deg < 0) continue;
        const std::string suf = tok.substr(i);
        ProgStep st; st.rel = mod12(kMajor[deg] + acc);
        st.spell = acc < 0 ? kSpellFlat : acc > 0 ? kSpellSharp : kSpellKey;
        if (suf.empty())            { st.q = lower ? qMin : qMaj; st.plain = true; }
        else if (suf == "7")        st.q = lower ? qMin7 : q7;
        else if (suf == "maj7")     st.q = lower ? qMinMaj7 : qMaj7;
        else if (suf == "9")        st.q = lower ? qMin9 : q9;
        else if (suf == "maj9")     st.q = qMaj9;
        else if (suf == "dim")      st.q = qDim;
        else if (suf == "dim7")     st.q = qDim7;
        else if (suf == "\xc3\xb8" || suf == "o7") st.q = qM7b5;   // "ø"
        else if (suf == "+")        st.q = qAug;
        else                        st.q = lower ? qMin : qMaj;
        p.steps[p.n++] = st;
    }
    return p;
}

// The chord a step means in the current key
inline Chord stepChord(const ProgStep& st, const Key& key, bool sevenths)
{
    Chord c; c.rootPc = (int8_t)mod12(key.root + st.rel); c.q = (int8_t)st.q; c.spell = st.spell; c.degree = -1;
    for (int d = 0; d < 7; ++d)
    {
        if (key.sc().chord[d] != st.rel) continue;
        const Chord tri = key.diatonic(d, false);
        if (st.plain && tri.sameAs(c)) { Chord dc = key.diatonic(d, sevenths); dc.spell = st.spell; return dc; }
        // Explicit chords that fit the key still get their degree
        bool fits = true;
        for (int k = 0; k < kQualities[c.q].n; ++k) if (!key.inChordScale(c.rootPc + kQualities[c.q].iv[k])) fits = false;
        if (fits) c.degree = (int8_t)d;
    }
    return c;
}

// Common next moves by degree (strongest first), for the "Next" hint without a progression
static const int kNextDegree[7] = {3, 4, 5, 4, 0, 3, 0};

// ---------------------------------------------------------------------------
// Voicing
// ---------------------------------------------------------------------------
constexpr int kMaxChordNotes = 10;
struct Notes { int n[kMaxChordNotes]; int count = 0; void push(int v) { if (count < kMaxChordNotes) n[count++] = v; } };

// Pitch classes of a chord, root first, no duplicates
inline int chordPcs(const Chord& c, int* out)
{
    int k = 0; out[k++] = c.rootPc;
    const Quality& Q = kQualities[c.q];
    for (int i = 0; i < Q.n; ++i)
    {
        const int pc = mod12(c.rootPc + Q.iv[i]);
        bool dup = false; for (int j = 0; j < k; ++j) if (out[j] == pc) dup = true;
        if (!dup) out[k++] = pc;
    }
    return k;
}

// Smooth voicing: the inversion closest to the previous chord, bottom note between E3 and E4
// (MIDI 52..64), and the bass note in C2..B2 at least a 5th under it. `prev` is updated.
inline Notes leadVoice(const Chord& c, Notes& prev, bool rootBass)
{
    int pcs[5]; const int n = chordPcs(c, pcs);
    auto cost = [&prev](const int* v, int cnt) {
        if (prev.count == 0) return (float)std::abs(v[0] - 55);
        float a = 0.0f;
        for (int i = 0; i < cnt; ++i) { int m = 999; for (int j = 0; j < prev.count; ++j) { int d = std::abs(prev.n[j] - v[i]); if (d < m) m = d; } a += (float)m; }
        for (int j = 0; j < prev.count; ++j) { int m = 999; for (int i = 0; i < cnt; ++i) { int d = std::abs(prev.n[j] - v[i]); if (d < m) m = d; } a += (float)m; }
        return a + (float)std::abs(v[0] - 58) * 0.1f;
    };
    int best[5] = {0}; float bestCost = 1e9f;
    for (int k = 0; k < n; ++k)
    {
        int order[5]; for (int i = 0; i < n; ++i) order[i] = pcs[(k + i) % n];
        for (int low = 52; low <= 64; ++low)
        {
            if (mod12(low) != order[0]) continue;
            int v[5]; v[0] = low;
            for (int i = 1; i < n; ++i) { int m = v[i - 1] + 1; while (mod12(m) != order[i]) ++m; v[i] = m; }
            const float cst = cost(v, n);
            if (cst < bestCost) { bestCost = cst; for (int i = 0; i < n; ++i) best[i] = v[i]; }
        }
    }
    prev.count = 0; for (int i = 0; i < n; ++i) prev.push(best[i]);
    Notes out;
    if (rootBass) { int b = 36 + c.rootPc; if (b > best[0] - 7) b -= 12; out.push(b); }
    for (int i = 0; i < n; ++i) out.push(best[i]);
    return out;
}

// Root position with the root at or just below `near` (never under E2), bass an octave down
inline Notes plainVoice(const Chord& c, int near, bool rootBass)
{
    int root = near - mod12(near - c.rootPc);
    if (root < 40) root += 12;
    Notes out;
    if (rootBass) out.push(root - 12);
    out.push(root);
    const Quality& Q = kQualities[c.q];
    for (int i = 0; i < Q.n; ++i) out.push(root + Q.iv[i]);
    return out;
}

} // namespace cs

#endif // CHORD_STAGE_THEORY_H
