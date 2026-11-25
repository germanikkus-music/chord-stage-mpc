// script_test.cpp - plays a scripted MIDI performance into the native Chord Stage build,
// writes the whole render to a WAV, and checks that the audio holds the right chords.
//
// It loads the plugin like a host (VSTPluginMain, the SDK's own VST2 ABI header), sets
// parameters by name, sends note on/off at exact sample positions with a transport that
// can be playing, and reads the "Now / Next" display text after each chord.
// Pitch check: Tone 0 makes the E.Piano a pure sine, so a Hann-windowed Goertzel scan of
// every MIDI note finds exactly the notes that sound.
//
// Build + run: test/run.sh   (exit code 0 = every check passed)

#include "mpc_vst2_abi.h"

#include <dlfcn.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

static const double kSR = 44100.0;
static const int kBlock = 128;
static VstTimeInfo gTime;
static bool gPlaying = false;
static double gBpm = 120.0;
static int64_t gSample = 0;

static VstIntPtr host(AEffect*, VstInt32 op, VstInt32, VstIntPtr, void*, float)
{
    switch (op)
    {
        case audioMasterVersion: return 2400;
        case audioMasterGetTime:
            std::memset(&gTime, 0, sizeof(gTime));
            gTime.sampleRate = kSR; gTime.samplePos = (double)gSample; gTime.tempo = gBpm;
            gTime.ppqPos = (double)gSample / kSR * gBpm / 60.0;
            gTime.barStartPos = std::floor(gTime.ppqPos / 4.0) * 4.0;
            gTime.timeSigNumerator = 4; gTime.timeSigDenominator = 4;
            gTime.flags = kVstTempoValid | kVstPpqPosValid | kVstBarsValid | kVstTimeSigValid | (gPlaying ? kVstTransportPlaying : 0);
            return (VstIntPtr)&gTime;
        default: return 0;
    }
}

struct Plug
{
    AEffect* e = nullptr;
    std::vector<std::string> names;
    VstIntPtr d(VstInt32 op, VstInt32 i = 0, VstIntPtr v = 0, void* p = nullptr, float o = 0) { return e->dispatcher(e, op, i, v, p, o); }
    int idx(const char* name)
    {
        for (size_t i = 0; i < names.size(); ++i) if (names[i] == name) return (int)i;
        std::fprintf(stderr, "no parameter %s\n", name); std::exit(1);
    }
    std::string display(const char* name) { char b[256] = {0}; d(effGetParamDisplay, idx(name), 0, b); return b; }
    // Sets a parameter to the first normalised value whose display text equals `text`
    void setText(const char* name, const char* text)
    {
        const int i = idx(name);
        for (int k = 0; k <= 2000; ++k)
        {
            e->setParameter(e, i, (float)k / 2000.0f);
            char b[256] = {0}; d(effGetParamDisplay, i, 0, b);
            if (std::string(b) == text) return;
        }
        // Numbers on a fine (log) scale may never show exactly: take the closest one
        char* end = nullptr; const double want = std::strtod(text, &end);
        if (end != text)
        {
            int best = -1; double bestErr = 1e30;
            for (int k = 0; k <= 20000; ++k)
            {
                e->setParameter(e, i, (float)k / 20000.0f);
                char b[256] = {0}; d(effGetParamDisplay, i, 0, b);
                const double v = std::strtod(b, nullptr), err = std::fabs(v - want);
                if (err < bestErr) { bestErr = err; best = k; }
            }
            if (bestErr <= 0.02 * std::fabs(want) + 0.5) { e->setParameter(e, i, (float)best / 20000.0f); return; }
        }
        std::fprintf(stderr, "could not set %s to %s\n", name, text); std::exit(1);
    }
};

static std::vector<float> L, R;
struct Ev { int64_t at; uint8_t s, d1, d2; };
static std::vector<Ev> queue;

static void renderN(Plug& p, int64_t frames)
{
    const int64_t end = gSample + frames;
    float bl[kBlock], br[kBlock];
    while (gSample < end)
    {
        const int n = (int)std::min<int64_t>(kBlock, end - gSample);
        std::vector<VstMidiEvent> mev; std::vector<Ev> keep;
        for (const Ev& q : queue)
        {
            if (q.at < gSample + n)
            {
                VstMidiEvent m; std::memset(&m, 0, sizeof(m));
                m.type = kVstMidiType; m.byteSize = sizeof(m); m.deltaFrames = (VstInt32)std::max<int64_t>(0, q.at - gSample);
                m.midiData[0] = (char)q.s; m.midiData[1] = (char)q.d1; m.midiData[2] = (char)q.d2;
                mev.push_back(m);
            }
            else keep.push_back(q);
        }
        queue = keep;
        std::vector<char> buf(sizeof(VstEvents) + sizeof(VstEvent*) * (mev.size() + 2));
        VstEvents* evs = (VstEvents*)buf.data(); evs->numEvents = (VstInt32)mev.size();
        for (size_t i = 0; i < mev.size(); ++i) evs->events[i] = (VstEvent*)&mev[i];
        p.d(effProcessEvents, 0, 0, evs);
        std::memset(bl, 0, sizeof(bl)); std::memset(br, 0, sizeof(br));
        float* outs[2] = {bl, br};
        p.e->processReplacing(p.e, nullptr, outs, n);
        for (int i = 0; i < n; ++i) { L.push_back(bl[i]); R.push_back(br[i]); }
        gSample += n;
    }
}
static void render(Plug& p, double seconds) { renderN(p, (int64_t)std::llround(seconds * kSR)); }
static void noteOn(int note, double inSec = 0) { queue.push_back({gSample + (int64_t)(inSec * kSR), 0x90, (uint8_t)note, 100}); }
static void noteOff(int note, double inSec = 0) { queue.push_back({gSample + (int64_t)(inSec * kSR), 0x80, (uint8_t)note, 0}); }

// Notes sounding in [start, start + N) samples
static std::set<int> detect(int64_t start, int N, float rel = 0.3f)
{
    std::vector<double> amp(128, 0.0);
    double mx = 0;
    for (int note = 28; note <= 100; ++note)
    {
        const double f = 440.0 * std::pow(2.0, (note - 69) / 12.0);
        const double w = 2.0 * std::cos(2.0 * M_PI * f / kSR);
        double s1 = 0, s2 = 0;
        for (int i = 0; i < N; ++i)
        {
            const double hann = 0.5 - 0.5 * std::cos(2.0 * M_PI * i / (N - 1));
            const int64_t k = start + i;
            const double x = k < (int64_t)L.size() ? 0.5 * (L[(size_t)k] + R[(size_t)k]) : 0.0;
            const double s = x * hann + w * s1 - s2; s2 = s1; s1 = s;
        }
        amp[(size_t)note] = std::sqrt(s1 * s1 + s2 * s2 - w * s1 * s2);
        if (amp[(size_t)note] > mx) mx = amp[(size_t)note];
    }
    std::set<int> out;
    if (mx < 1e-3 * N * 0.01) return out;   // silence
    // a note counts when it is a local peak (short windows smear a tone into its neighbours)
    for (int note = 28; note <= 100; ++note)
        if (amp[(size_t)note] > rel * mx && (N >= 32768 || (amp[(size_t)note] >= amp[(size_t)note - 1] && amp[(size_t)note] >= amp[(size_t)note + 1]))) out.insert(note);
    return out;
}
static std::string fmt(const std::set<int>& s)
{
    static const char* nm[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    std::string o; for (int n : s) { if (!o.empty()) o += " "; o += nm[n % 12] + std::to_string(n / 12 - 1); }
    return o.empty() ? "(silence)" : o;
}

static void writeWav(const char* path, size_t from, size_t to)
{
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    const uint32_t frames = (uint32_t)(to - from), dataBytes = frames * 4, riff = 36 + dataBytes, fmtLen = 16, sr = (uint32_t)kSR, br = sr * 4;
    const uint16_t pcm = 1, ch = 2, ba = 4, bits = 16;
    std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
    std::fwrite(&fmtLen, 4, 1, f); std::fwrite(&pcm, 2, 1, f); std::fwrite(&ch, 2, 1, f); std::fwrite(&sr, 4, 1, f);
    std::fwrite(&br, 4, 1, f); std::fwrite(&ba, 2, 1, f); std::fwrite(&bits, 2, 1, f);
    std::fwrite("data", 1, 4, f); std::fwrite(&dataBytes, 4, 1, f);
    for (size_t i = from; i < to; ++i)
    {
        int16_t v[2] = {(int16_t)std::lround(std::fmax(-1.0, std::fmin(1.0, L[i])) * 32767.0),
                        (int16_t)std::lround(std::fmax(-1.0, std::fmin(1.0, R[i])) * 32767.0)};
        std::fwrite(v, 2, 2, f);
    }
    std::fclose(f);
    std::printf("wrote %s (%.1f s)\n", path, frames / kSR);
}

static int failures = 0, checks = 0;
static size_t soundsFrom = 0, soundsTo = 0;
static void expectNotes(const char* what, const std::set<int>& got, const std::set<int>& want)
{
    ++checks;
    const bool ok = got == want;
    if (!ok) ++failures;
    std::printf("  %-4s %-34s got %-26s want %s\n", ok ? "PASS" : "FAIL", what, fmt(got).c_str(), fmt(want).c_str());
}
static void expectText(const char* what, const std::string& got, const std::string& want)
{
    ++checks;
    const bool ok = got == want;
    if (!ok) ++failures;
    std::printf("  %-4s %-34s shows \"%s\"%s\n", ok ? "PASS" : "FAIL", what, got.c_str(), ok ? "" : (" want \"" + want + "\"").c_str());
}

// Hold `key` for 1 s, check the chord in the steady part, then release and wait
static void chord(Plug& p, int key, const std::set<int>& want, const char* label, const char* nowNext = nullptr)
{
    const int64_t t0 = gSample;
    noteOn(key);
    render(p, 1.0);
    if (nowNext) expectText((std::string(label) + " Now / Next").c_str(), p.display("Now / Next"), nowNext);
    noteOff(key);
    render(p, 0.15);
    expectNotes(label, detect(t0 + (int64_t)(0.2 * kSR), 32768), want);
}

int main(int argc, char** argv)
{
    if (argc < 3) { std::fprintf(stderr, "usage: script_test <plugin.dylib> <out.wav>\n"); return 1; }
    void* lib = dlopen(argv[1], RTLD_NOW);
    if (!lib) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    typedef AEffect* (*Entry)(audioMasterCallback);
    Entry entry = (Entry)dlsym(lib, "VSTPluginMain");
    Plug p; p.e = entry(host);
    p.d(effSetSampleRate, 0, 0, nullptr, (float)kSR); p.d(effSetBlockSize, 0, kBlock); p.d(effOpen);
    for (int i = 0; i < p.e->numParams; ++i) { char b[64] = {0}; p.d(effGetParamName, i, 0, b); p.names.push_back(b); }
    p.d(effSetProgram, 0, 0);
    p.d(effMainsChanged, 0, 1); p.d(effStartProcess);

    // Test sound: pure sine E.Piano, no chorus, flat envelope, quick release
    p.setText("Tone", "0"); p.setText("Chorus", "0"); p.setText("Vel Sens", "0");
    p.setText("Attack", "1"); p.setText("Decay", "10000"); p.setText("Sustain", "100"); p.setText("Release", "10");
    render(p, 0.1);

    std::printf("A. Chords mode, C major, root position (Smooth off)\n");
    p.setText("Smooth", "Off");
    chord(p, 60, {48, 60, 64, 67}, "C4 key -> C", "C > F");
    chord(p, 62, {50, 62, 65, 69}, "D4 key -> Dm", "Dm > G");
    chord(p, 61, {48, 60, 64, 67}, "C#4 (wrong key) -> C");
    chord(p, 67, {55, 67, 71, 74}, "G4 key -> G");
    chord(p, 71, {59, 71, 74, 77}, "B4 key -> Bdim");
    p.setText("7ths", "On");
    chord(p, 69, {57, 69, 72, 76, 79}, "7ths: A4 key -> Am7", "Am7 > Fmaj7");
    p.setText("7ths", "Off");

    std::printf("B. Sloppy mode, A minor, Andalusian (Am G F E)\n");
    p.setText("Key", "A"); p.setText("Scale", "Minor"); p.setText("Progression", "Andalusian"); p.setText("Mode", "Sloppy");
    render(p, 0.05);
    expectText("before playing", p.display("Now / Next"), "- > Am");
    chord(p, 57, {45, 57, 60, 64}, "A3 -> Am", "Am > G");
    chord(p, 55, {43, 55, 59, 62}, "G3 -> G", "G > F");
    chord(p, 54, {41, 53, 57, 60}, "F#3 (sloppy) -> F", "F > E");
    chord(p, 52, {40, 52, 56, 59}, "E3 -> E major (from the prog)", "E > Am");
    expectText("Step after 4 chords", p.display("Step"), "1/4");
    chord(p, 62, {50, 62, 65, 69}, "D4 (not next) -> Dm in key", "Dm > Am");

    std::printf("C. Chord lock, C major, Jazz ii-V (Dm7 G7 Cmaj7 Cmaj7)\n");
    p.setText("Key", "C"); p.setText("Scale", "Major"); p.setText("Progression", "Jazz ii-V"); p.setText("Mode", "Lock");
    render(p, 0.05);
    chord(p, 60, {}, "C4 while Dm7 is next -> silent");
    chord(p, 62, {50, 62, 65, 69, 72}, "D4 -> Dm7", "Dm7 > G7");
    chord(p, 67, {55, 67, 71, 74, 77}, "G4 -> G7", "G7 > Cmaj7");
    chord(p, 72, {60, 72, 76, 79, 83}, "C5 -> Cmaj7");
    chord(p, 61, {48, 60, 64, 67, 71}, "C#4 (snaps to C) -> Cmaj7", "Cmaj7 > Dm7");

    std::printf("D. Smooth voicing, Sloppy, Pop (C G Am F)\n");
    p.setText("Progression", "Pop"); p.setText("Mode", "Sloppy"); p.setText("Smooth", "On");
    render(p, 0.05);
    // leadVoice: C starts near G3; then each chord takes the closest inversion
    chord(p, 60, {36, 55, 60, 64}, "C -> C2 + G3 C4 E4");
    chord(p, 67, {43, 55, 59, 62}, "G -> G2 + G3 B3 D4");
    chord(p, 69, {45, 57, 60, 64}, "Am -> A2 + A3 C4 E4");
    chord(p, 65, {41, 57, 60, 65}, "F -> F2 + A3 C4 F4", "F > C");

    std::printf("E. Bass zone below C3, Fifth style, C major\n");
    p.setText("Progression", "None"); p.setText("Mode", "Chords"); p.setText("Smooth", "Off");
    p.setText("Bass Split", "C3"); p.setText("Bass Style", "Single");
    render(p, 0.05);
    chord(p, 40, {40}, "E2 -> E2 bass");
    chord(p, 42, {41}, "F#2 (wrong) -> F2 bass");
    p.setText("Bass Style", "Fifth");
    chord(p, 43, {43, 50}, "G2 fifth -> G2 + D3");
    chord(p, 60, {48, 60, 64, 67}, "C4 above the zone -> C chord");
    p.setText("Bass Split", "Off");

    std::printf("F. Strum 1/16 Up at 120 BPM (one note every 125 ms)\n");
    p.setText("Strum", "1/16");
    {
        const int64_t t0 = gSample;
        noteOn(60); render(p, 0.8); noteOff(60); render(p, 0.15);
        const std::set<int> w[4] = {{48}, {48, 60}, {48, 60, 64}, {48, 60, 64, 67}};
        for (int k = 0; k < 4; ++k)
        {
            char lbl[64]; std::snprintf(lbl, sizeof lbl, "strum %d-%d ms", 125 * k + 20, 125 * k + 113);
            expectNotes(lbl, detect(t0 + (int64_t)((0.125 * k + 0.02) * kSR), 4096), w[k]);
        }
    }
    p.setText("Strum", "Off");

    std::printf("G. Arp Up 1/8, transport playing at 120 BPM (steps on the MPC grid)\n");
    p.setText("Arp", "Up"); p.setText("Arp Rate", "1/8"); p.setText("Arp Gate", "100");
    {
        // Start the transport on a beat boundary, then press 0.1 s (= 0.2 beats) late
        const int64_t beat = (int64_t)(kSR / 2.0);
        renderN(p, (beat - gSample % beat) % beat);
        gPlaying = true;
        render(p, 0.1);
        const int64_t gridStart = gSample - (int64_t)(0.1 * kSR);   // ppq of a 1/8 line
        noteOn(60); render(p, 1.6); noteOff(60); render(p, 0.2);
        gPlaying = false;
        // First note plays at once; the next ones land on the 1/8 lines (every 0.25 s)
        expectNotes("arp first step (at the press)", detect(gridStart + (int64_t)(0.12 * kSR), 4096, 0.45f), {48, 60});
        const int seq[5] = {64, 67, 60, 64, 67};
        for (int k = 0; k < 5; ++k)
        {
            char lbl[64]; std::snprintf(lbl, sizeof lbl, "arp step on 1/8 line %d", k + 1);
            expectNotes(lbl, detect(gridStart + (int64_t)((0.25 * (k + 1) + 0.03) * kSR), 4096, 0.45f), {48, seq[k]});
        }
    }

    std::printf("H. Progression files from Content/Progressions (exact MPC voicings)\n");
    p.setText("Arp", "Off"); p.setText("Progression", "None"); p.setText("Mode", "Sloppy");
    p.setText("Key", "C"); p.setText("Scale", "Major");
    expectText("Prog File slot 1", (p.e->setParameter(p.e, p.idx("Prog File"), 1.0f / 100.0f), p.display("Prog File")), "Gymnopedie No 1");
    p.setText("Prog File", "Test Eb");
    render(p, 0.05);
    expectText("picking the file sets Key", p.display("Key"), "Eb");
    expectText("and Scale (Chromatic -> best fit)", p.display("Scale"), "Major");
    expectText("before playing", p.display("Now / Next"), "- > Ebmaj7");
    chord(p, 63, {39, 55, 58, 62}, "Eb4 -> D#maj7 as written", "Ebmaj7 > Cmin9");
    expectText("big Now display", p.display("Now"), "Ebmaj7");
    expectText("big Next display", p.display("Next"), "Cmin9");
    chord(p, 61, {36, 58, 62, 63, 67}, "C#4 (snaps to C) -> Cmin9", "Cmin9 > Ab/C");
    chord(p, 56, {36, 56, 60, 63}, "Ab3 -> G#/C as Ab/C", "Ab/C > Bb7");
    expectText("Step", p.display("Step"), "4/4");
    p.setText("Mode", "Lock");
    chord(p, 63, {}, "Lock: Eb while Bb7 is next -> silent");
    chord(p, 58, {46, 56, 58, 62, 65}, "Lock: Bb3 -> A#7 as Bb7", "Bb7 > Ebmaj7");
    // D is in Eb major so it does not snap; a half step from Eb still counts
    chord(p, 62, {39, 55, 58, 62}, "Lock: D4 (half step off) -> Ebmaj7", "Ebmaj7 > Cmin9");
    chord(p, 66, {}, "Lock: F#4 (3 steps off) -> silent");
    p.setText("Mode", "Sloppy");
    chord(p, 61, {36, 58, 62, 63, 67}, "Sloppy: C#4 -> Cmin9 (exact first)", "Cmin9 > Ab/C");
    chord(p, 57, {36, 56, 60, 63}, "Sloppy: A3 (half step off) -> Ab/C", "Ab/C > Bb7");
    p.setText("Mode", "Lock");
    {
        // A saved project remembers the file by name
        void* chunk = nullptr; const VstIntPtr n = p.d(effGetChunk, 0, 0, &chunk);
        std::vector<char> saved((char*)chunk, (char*)chunk + n);
        p.setText("Prog File", "None");
        p.d(effSetChunk, 0, (VstIntPtr)saved.size(), saved.data());
        expectText("project reload keeps the file", p.display("Prog File"), "Test Eb");
        render(p, 0.05);
    }
    p.setText("Prog File", "Gymnopedie No 1");
    render(p, 0.05);
    expectText("Gymnopedie sets Key", p.display("Key"), "D");
    chord(p, 67, {43, 59, 62, 66}, "Gymnopedie G3 -> Gmaj7 as written", "Gmaj7 > Dmaj7");

    std::printf("I. Every sound: C Am F G with the normal settings (also written to the sounds demo WAV)\n");
    p.setText("Prog File", "None"); p.setText("Mode", "Chords"); p.setText("Key", "C"); p.setText("Scale", "Major");
    p.setText("Tone", "50"); p.setText("Chorus", "30"); p.setText("Vel Sens", "70");
    p.setText("Attack", "2"); p.setText("Decay", "2500"); p.setText("Sustain", "30"); p.setText("Release", "350");   // the defaults
    render(p, 0.3);
    const size_t soundsFrom0 = L.size();
    double epRms = 0;
    const char* sounds[] = {"E.Piano", "Pad", "Strings", "Organ", "Piano", "Syn Keys", "Wurli", "Bell EP", "Syn Pad"};
    for (const char* snd : sounds)
    {
        p.setText("Sound", snd);
        const size_t from = L.size();
        for (int key : {60, 69, 65, 67}) { noteOn(key); render(p, 0.7); noteOff(key); render(p, 0.05); }
        render(p, 0.6);
        // Level of the first chord (C) once it has settled in
        double sum = 0, peak = 0; size_t n = 0;
        for (size_t i = from; i < L.size(); ++i) peak = std::fmax(peak, std::fabs(L[i]) + 0.0);
        for (size_t i = from + (size_t)(0.35 * kSR); i < from + (size_t)(0.7 * kSR); ++i) { sum += L[i] * L[i]; ++n; }
        const double rms = std::sqrt(sum / (double)n);
        if (std::string(snd) == "E.Piano") epRms = rms;
        const double rel = 20.0 * std::log10(rms / epRms), pk = 20.0 * std::log10(peak);
        ++checks;
        const bool ok = rms > 1e-3 && peak < 0.99 && std::fabs(rel) <= 9.0;
        if (!ok) ++failures;
        std::printf("  %-4s %-10s level %+5.1f dB vs E.Piano, peak %5.1f dBFS\n", ok ? "PASS" : "FAIL", snd, rel, pk);
    }
    soundsFrom = soundsFrom0; soundsTo = L.size();

    p.d(effStopProcess); p.d(effMainsChanged, 0, 0); p.d(effClose);

    writeWav(argv[2], 0, L.size());
    if (argc > 3) writeWav(argv[3], soundsFrom, soundsTo);
    std::printf("%s: %d checks, %d failed\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
