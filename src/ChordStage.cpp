// Chord Stage - an MPC instrument plugin by Germanikkus
//
// The MPC version of the Chord Stage web app: play "not-so-precise" fingering on a
// keyboard and hear the right chords, with a built-in sound.
//
// Signal flow
//   MIDI from the track (any channel)
//     -> zones: [Bass zone] below Bass Split | [Chord zone] | [Upper zone] at/above Split
//     -> key + scale correction (wrong notes snap to the nearest note in the key)
//     -> chord choice:
//          Chords  one key = one chord: the diatonic triad / 7th on that key's degree
//          Sloppy  a key near the next chord of the Progression plays that exact chord
//          Lock    only the next chord's root key plays (that chord); every other key is silent
//     -> voicing: Smooth (voice leading, like the web app) or root position, + root bass
//     -> Strum (notes rolled at a tempo-synced spacing) or Arp (tempo-synced, follows the
//        MPC grid while the transport plays)
//     -> Engine (synth.h): E.Piano / Pad / Strings / Organ voices, Bass voice, chorus
//     -> stereo out
//   The read-only "Now / Next" parameter shows the chord you played and the one to play next.
//   "Prog File" picks an MPC .progression file from Content/Progressions instead of a preset;
//   its chords play with the file's exact notes, and Key / Scale follow the file.
//
// MIDI: notes, sustain pedal (CC64), pitch bend (+-2), all notes off (CC120/123).
// The transport restarting from the top of the song sends the progression back to step 1.

#include "mpc_plugin.h"
#include "dsp/tempo.h"
#include "theory.h"
#include "synth.h"
#include "progfile.h"

#include <atomic>
#include <climits>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace cs;

// Parameter order is permanent: append new ones at the end.
enum P
{
    kVolume = 0,   // master volume, dB (parameter 0: harmless when the MPC nudges it)
    kKey,          // C..B
    kScale,        // Major, Minor, ...
    kMode,         // Chords / Sloppy / Lock
    kProg,         // preset progression
    kStep,         // progression position (turn it to jump; it follows as you play)
    kNowNext,      // read-only display: "Now > Next" chord names
    kSevenths,     // diatonic chords as 7ths
    kSmooth,       // voice leading
    kRootBass,     // add the root an octave or so below each chord
    kChordOct,     // shift chords by octaves
    kSplit,        // keys at/above this play the Upper zone (Off = all chords)
    kUpper,        // Upper zone: Melody (single notes in key) or Chords
    kBassSplit,    // keys below this are the bass zone (Off = none)
    kBassStyle,    // Single / Octave / Fifth
    kBassSound,    // Bass (own synth bass) or Main (the chord sound)
    kBassLevel,    // %
    kStrum,        // Off or a note division between strummed notes
    kStrumDir,     // Up / Down / Alt
    kArp,          // Off / Up / Down / UpDown / Random
    kArpRate,      // note division
    kArpOct,       // 1..3 octaves
    kArpGate,      // % of a step
    kVelSens,      // velocity sensitivity %
    kArpLatch,     // arp keeps going after you let go, until the next chord
    kSound,        // E.Piano / Pad / Strings / Organ / Piano / Syn Keys / Wurli / Bell EP / Syn Pad
    kTone,         // brightness %
    kChorus,       // %
    kAttack, kDecay, kSustain, kRelease,   // amp envelope of the chord sound
    kFile,         // 0 = none, n = the n-th .progression file in Content/Progressions (overrides Progression)
    kNow,          // read-only display: the chord you just played (big on the PLAY page)
    kNext,         // read-only display: the chord to play next (the PLAY page's centerpiece)
    kNumParams
};

static float strumBeats(int idx)                         // Off, 1/128, 1/64, 1/32T, 1/32, 1/16T, 1/16
{
    static const float b[] = {0.0f, 0.03125f, 0.0625f, 0.08333f, 0.125f, 0.16667f, 0.25f};
    return b[mpc::clamp(idx, 0, 6)];
}
static float arpBeats(int idx)                           // 1/4, 1/8, 1/8T, 1/16, 1/16T, 1/32
{
    static const float b[] = {1.0f, 0.5f, 0.33333f, 0.25f, 0.16667f, 0.125f};
    return b[mpc::clamp(idx, 0, 5)];
}

constexpr int kSplitBase = 35;      // Split choice i (i >= 1) = MIDI note 35 + i  (C2 = 36 .. C6 = 84)
constexpr int kMaxPending = 64;
constexpr int kMaxArp = 24;

static std::string noteLabel(int n) { return std::string(kSharp[mod12(n)]) + std::to_string(n / 12 - 1); }

class ChordStage : public mpc::Plugin
{
public:
    ChordStage() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        std::vector<std::string> keys(kKeyNames, kKeyNames + 12);
        std::vector<std::string> scales; for (int i = 0; i < kNumScales; ++i) scales.push_back(kScales[i].name);
        std::vector<std::string> progs; for (int i = 0; i < kNumProgPresets; ++i) progs.push_back(kProgPresets[i].name);
        std::vector<std::string> split{"Off"}; for (int n = 36; n <= 84; ++n) split.push_back(noteLabel(n));
        std::vector<std::string> bsplit{"Off"}; for (int n = 36; n <= 72; ++n) bsplit.push_back(noteLabel(n));

        addParam(floatParam("volume", "Volume", -40.0f, 6.0f, -6.0f, "dB", 1));
        addParam(choiceParam("key", "Key", keys, 0));
        addParam(choiceParam("scale", "Scale", scales, 0));
        addParam(choiceParam("mode", "Mode", {"Chords", "Sloppy", "Lock"}, 0));
        addParam(choiceParam("prog", "Progression", progs, 0));
        addParam(intParam("step", "Step", 1, kMaxSteps, 1));
        Param nn = intParam("nownext", "Now / Next", 0, 1000, 0); nn.automatable = false; addParam(nn);
        addParam(boolParam("sevenths", "7ths", false));
        addParam(boolParam("smooth", "Smooth", true));
        addParam(boolParam("rootbass", "Root Bass", true));
        addParam(intParam("chordoct", "Chord Oct", -2, 2, 0));
        addParam(choiceParam("split", "Split", split, 0));
        addParam(choiceParam("upper", "Upper", {"Melody", "Chords"}, 0));
        addParam(choiceParam("basssplit", "Bass Split", bsplit, 0));
        addParam(choiceParam("bassstyle", "Bass Style", {"Single", "Octave", "Fifth"}, 0));
        addParam(choiceParam("basssound", "Bass Sound", {"Bass", "Main"}, 0));
        addParam(floatParam("basslevel", "Bass Level", 0.0f, 100.0f, 80.0f, "%", 0));
        addParam(choiceParam("strum", "Strum", {"Off", "1/128", "1/64", "1/32T", "1/32", "1/16T", "1/16"}, 0));
        addParam(choiceParam("strumdir", "Strum Dir", {"Up", "Down", "Alt"}, 0));
        addParam(choiceParam("arp", "Arp", {"Off", "Up", "Down", "UpDown", "Random"}, 0));
        addParam(choiceParam("arprate", "Arp Rate", {"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"}, 3));
        addParam(intParam("arpoct", "Arp Oct", 1, 3, 1));
        addParam(floatParam("arpgate", "Arp Gate", 10.0f, 100.0f, 80.0f, "%", 0));
        addParam(floatParam("velsens", "Vel Sens", 0.0f, 100.0f, 70.0f, "%", 0));
        addParam(boolParam("arplatch", "Arp Latch", false));
        // New sounds go at the end of the list so saved projects keep theirs
        addParam(choiceParam("sound", "Sound", {"E.Piano", "Pad", "Strings", "Organ", "Piano", "Syn Keys", "Wurli", "Bell EP", "Syn Pad"}, 0));
        addParam(floatParam("tone", "Tone", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(floatParam("chorus", "Chorus", 0.0f, 100.0f, 30.0f, "%", 0));
        addParam(logParam("attack", "Attack", 1.0f, 3000.0f, 2.0f, "ms", 0));
        addParam(logParam("decay", "Decay", 20.0f, 10000.0f, 2500.0f, "ms", 0));
        addParam(floatParam("sustain", "Sustain", 0.0f, 100.0f, 30.0f, "%", 0));
        addParam(logParam("release", "Release", 10.0f, 5000.0f, 350.0f, "ms", 0));
        // A plain 0..64 slider (not a choice): the files are only known on the MPC, so the
        // skin must not bake in a list of names. The value text shows the file name.
        addParam(floatParam("progfile", "Prog File", 0.0f, (float)kMaxFiles, 0.0f, "", 0));
        // Display only: their value is a counter bumped whenever the text changes (see publishDisplay)
        Param pn = intParam("now", "Now", 0, 1000, 0); pn.automatable = false; addParam(pn);
        Param px = intParam("next", "Next", 0, 1000, 0); px.automatable = false; addParam(px);

        files_ = loadProgFiles();

        for (int i = 0; i < kNumProgPresets; ++i) progs_[i] = parseProgression(kProgPresets[i].text);
        mpc::dsp::WavetableSine warm; (void)warm;   // builds the shared sine table off the audio thread

        addPrograms();
    }

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Chord Stage";
        i.vendor = "Germanikkus";
        i.uniqueId = MPC_FOURCC('G','w','z','6');
        i.version = 1300;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    void prepare(double sr, int) override { engine_.prepare(sr); }

    void reset() override
    {
        engine_.reset();
        for (auto& h : keyDown_) h = false;
        npending_ = 0; arp_.on = false; pedal_ = false;
        prevUpper_.count = 0; progPos_ = 0; nowChord_ = Chord(); lastDegree_ = -1;
        lastStepSeen_ = getInt(kStep);
        cacheKey_ = -1; cacheProg_ = -1; cacheFile_ = -2; justReset_ = true;
        displayDirty_ = true;
        quietPublish_ = true;   // after a load/resume the host re-reads every value text itself
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        if (index == kNowNext)
        {
            Key k; k.root = keyA_.load(std::memory_order_relaxed); k.scale = scaleA_.load(std::memory_order_relaxed);
            const Chord now = Chord::unpack(nowA_.load(std::memory_order_relaxed));
            const Chord next = Chord::unpack(nextA_.load(std::memory_order_relaxed));
            out = chordName(k, now) + " > " + chordName(k, next);
            return true;
        }
        if (index == kNow || index == kNext)
        {
            Key k; k.root = keyA_.load(std::memory_order_relaxed); k.scale = scaleA_.load(std::memory_order_relaxed);
            out = chordName(k, Chord::unpack((index == kNow ? nowA_ : nextA_).load(std::memory_order_relaxed)));
            return true;
        }
        if (index == kFile)
        {
            const int i = (int)std::lround(plain);
            if (i <= 0) out = "None";
            else if (i <= (int)files_.size()) { out = files_[(size_t)i - 1].title; if (out.size() > 20) out = out.substr(0, 20); }
            else out = "Empty " + std::to_string(i);   // distinct text: keeps the build tools from treating it as a list
            return true;
        }
        if (index == kStep)
        {
            const int len = lenA_.load(std::memory_order_relaxed);
            if (len <= 0) { out = "-"; return true; }
            out = std::to_string(posA_.load(std::memory_order_relaxed) + 1) + "/" + std::to_string(len);
            return true;
        }
        return false;
    }

    // The selected file is saved by name, so adding files later does not change which one a project uses
    void saveExtraState(std::vector<uint8_t>& out) override
    {
        const int i = (int)std::lround(get(kFile));
        if (i >= 1 && i <= (int)files_.size()) out.assign(files_[(size_t)i - 1].title.begin(), files_[(size_t)i - 1].title.end());
    }
    void loadExtraState(const uint8_t* data, size_t size) override
    {
        const std::string title(reinterpret_cast<const char*>(data), size);
        if (title.empty()) return;
        int found = 0;
        for (size_t i = 0; i < files_.size(); ++i) if (files_[i].title == title) found = (int)i + 1;
        if (found != (int)std::lround(get(kFile))) set(kFile, (float)found);   // a file that is gone falls back to None
    }

    void process(mpc::ProcessContext& ctx) override
    {
        readSettings(ctx);
        float* L = ctx.outputs[0];
        float* R = ctx.numOutputs > 1 ? ctx.outputs[1] : ctx.outputs[0];
        const float gain = 0.2f * std::pow(10.0f, get(kVolume) / 20.0f);

        // Transport: from-the-top restart sends the progression back to step 1
        const mpc::Transport& t = ctx.transport;
        const bool playing = t.valid && t.playing;
        if (playing && !wasPlaying_ && t.ppqValid && t.ppqPos < 0.25) { progPos_ = 0; displayDirty_ = true; }
        wasPlaying_ = playing;
        bpm_ = mpc::dsp::safeBpm(t.tempoValid ? t.bpm : 120.0);
        synced_ = playing && t.ppqValid;
        ppq0_ = t.ppqPos;
        beatsPerSample_ = bpm_ / 60.0 / ctx.sampleRate;

        // Split the block at MIDI events, strum notes and arp steps
        int ev = 0, pos = 0;
        const int n = ctx.numFrames;
        while (pos < n)
        {
            curPos_ = pos;
            while (ev < ctx.numMidiEvents && ctx.midi[ev].sampleOffset <= pos) handleMidi(ctx.midi[ev++]);
            runTimers(pos);
            int end = n;
            if (ev < ctx.numMidiEvents && ctx.midi[ev].sampleOffset < end) end = ctx.midi[ev].sampleOffset;
            const int nextT = nextTimer(pos);
            if (nextT < end) end = nextT;
            if (end <= pos) end = pos + 1;
            engine_.render(L + pos, R + pos, end - pos, gain);
            pos = end;
        }
        clock_ += n;
        publishDisplay();
    }

private:
    // ------------------------------------------------------------------ settings
    void readSettings(mpc::ProcessContext& ctx)
    {
        (void)ctx;
        key_.root = getInt(kKey); key_.scale = mpc::clamp(getInt(kScale), 0, kNumScales - 1);
        int prog = mpc::clamp(getInt(kProg), 0, kNumProgPresets - 1);
        // A progression file overrides the preset. Picking one sets Key and Scale to the file's
        // (not after a project load: the saved Key / Scale win then).
        const int fi = (int)std::lround(get(kFile)) - 1;
        const int file = (fi >= 0 && fi < (int)files_.size()) ? fi : -1;
        if (file != cacheFile_)
        {
            if (file >= 0 && !justReset_)
            {
                const ProgFile& f = files_[(size_t)file];
                values_[kKey]->store(param(kKey).toNormalised((float)f.keyRoot), std::memory_order_relaxed);
                values_[kScale]->store(param(kScale).toNormalised((float)f.scale), std::memory_order_relaxed);
                notifyHostParameterChanged(kKey); notifyHostParameterChanged(kScale);
                key_.root = f.keyRoot; key_.scale = f.scale;
            }
            cacheFile_ = file; cacheProg_ = -1;   // forces the reset below
        }
        justReset_ = false;
        file_ = file >= 0 ? &files_[(size_t)file] : nullptr;
        if (file_) prog = 1000 + file;
        if (key_.root != cacheKey_ || key_.scale != cacheScale_ || prog != cacheProg_)
        {
            if (prog != cacheProg_ || key_.root != cacheKey_) { progPos_ = 0; nowChord_ = Chord(); lastDegree_ = -1; }
            cacheKey_ = key_.root; cacheScale_ = key_.scale; cacheProg_ = prog;
            prevUpper_.count = 0; displayDirty_ = true;
        }
        prog_ = &progs_[file_ ? 0 : prog];
        if (progPos_ >= progLen()) progPos_ = 0;
        // Step turned by the user: jump there
        const int st = getInt(kStep);
        if (st != lastStepSeen_)
        {
            lastStepSeen_ = st;
            if (progLen() > 0) { progPos_ = (st - 1) % progLen(); displayDirty_ = true; }
        }
        const bool sev = getBool(kSevenths);
        if (sev != cacheSev_) { cacheSev_ = sev; displayDirty_ = true; }

        engine_.tone = get(kTone) * 0.01f;
        engine_.chorus = get(kChorus) * 0.01f;
        static const int kSoundMap[] = {kEPiano, kPad, kStrings, kOrgan, kPiano, kSynKeys, kWurli, kBellEP, kSynPad};
        mainSound_ = kSoundMap[mpc::clamp(getInt(kSound), 0, 8)];
        EnvSettings e; e.a = get(kAttack); e.d = get(kDecay); e.s = get(kSustain) * 0.01f; e.r = get(kRelease);
        if (e.a != engine_.envSet.a || e.d != engine_.envSet.d || e.s != engine_.envSet.s || e.r != engine_.envSet.r)
        { engine_.envSet = e; engine_.envChanged(); }
    }

    // ------------------------------------------------------------------ MIDI
    void handleMidi(const mpc::MidiEvent& e)
    {
        if (e.isNoteOn()) keyDown(e.note(), e.velocity());
        else if (e.isNoteOff()) keyUp(e.note());
        else if (e.isAllNotesOff()) panic();
        else if (e.isController() && e.controller() == 64)
        {
            const bool down = e.controllerValue() >= 64;
            if (!down && pedal_) engine_.pedalUp();
            pedal_ = down;
        }
        else if (e.isPitchBend()) engine_.bend = std::pow(2.0f, (float)e.pitchBend() / 8192.0f * 2.0f / 12.0f);
    }

    float velAmp(int vel) const
    {
        const float s = get(kVelSens) * 0.01f;
        return (1.0f - s) + s * (float)vel / 127.0f;
    }

    void keyDown(int note, int vel)
    {
        keyUp(note);   // a re-press replaces what the key was playing
        keyDown_[note] = true;
        currentKey_ = note;
        const float amp = velAmp(vel);
        const int bassSplit = getInt(kBassSplit), split = getInt(kSplit);

        // Bass zone
        if (bassSplit > 0 && note < kSplitBase + bassSplit)
        {
            const int top = kSplitBase + bassSplit - 1;
            int shift = 0; while (top - shift > 52) shift += 12;   // keep the zone in bass range (top key <= E3)
            const int b = key_.snapNearest(note) - shift;
            const int style = getInt(kBassStyle);
            const bool own = getInt(kBassSound) == 0;
            const float a = amp * get(kBassLevel) * 0.0125f;
            const int snd = own ? kBass : mainSound_;
            if (style == 1) engine_.start(b - 12, a * 0.8f, snd, note);
            engine_.start(b, a, snd, note);
            if (style == 2) engine_.start(key_.snapNearest(b + 7), a * 0.8f, snd, note);
            return;
        }
        // Upper zone, melody
        if (split > 0 && note >= kSplitBase + split && getInt(kUpper) == 0)
        {
            engine_.start(key_.snapNearest(note), amp, mainSound_, note);
            return;
        }

        // Chord zone
        const int mode = getInt(kMode);
        const bool sev = getBool(kSevenths);
        const int len = progLen();
        Notes exact;
        const int pcs[2] = {mod12(note), mod12(key_.snapNearest(note))};
        auto matches = [&pcs](const Chord& c) { return c.rootPc == pcs[0] || c.rootPc == pcs[1]; };
        // A near miss: the key is a half step from the root. Roots outside the key (Abmin7 in
        // F major) can't be reached by snapping, because the keys around them are in the key.
        auto near = [note](const Chord& c) { const int d = mod12(note - c.rootPc); return d == 1 || d == 11; };

        if (mode == 2 && len > 0)   // Lock
        {
            const Chord c = progChord(progPos_, sev, &exact);
            const int64_t now = clock_ + curPos_;
            const bool sameHand = now - lockAt_ < (int64_t)(0.15 * sampleRate());   // keys landing right after it are the same hand
            if (!(matches(c) || near(c)) || sameHand) return;   // silent
            lockAt_ = now;
            advance(1);
            playChord(c, note, amp, &exact);
            return;
        }
        if (mode == 1 && len > 0)   // Sloppy
        {
            // Exact hits first (this chord or the one after), then near misses
            for (int pass = 0; pass < 2; ++pass)
                for (int k = 0; k < (len < 2 ? len : 2); ++k)
                {
                    const Chord c = progChord((progPos_ + k) % len, sev, &exact);
                    if (pass == 0 ? !matches(c) : !near(c)) continue;
                    advance(k + 1);
                    playChord(c, note, amp, &exact);
                    return;
                }
        }
        // One key = one chord
        const Chord c = key_.diatonic(key_.degreeOf(note), sev);
        if (len > 0)   // playing the expected chord (or the one after it) moves the progression on
            for (int k = 0; k < (len < 2 ? len : 2); ++k)
                if (progChord((progPos_ + k) % len, sev, nullptr).sameAs(c)) { advance(k + 1); break; }
        playChord(c, key_.snapNearest(note), amp);
    }

    void advance(int k)
    {
        if (progLen() <= 0) return;
        progPos_ = (progPos_ + k) % progLen();
        stepMoved_ = true;
        displayDirty_ = true;
    }

    int progLen() const { return file_ ? (int)file_->chords.size() : prog_->n; }

    // Chord i of the current progression; for a file, `exact` gets the file's notes
    Chord progChord(int i, bool sev, Notes* exact) const
    {
        if (file_)
        {
            const FileChord& fc = file_->chords[(size_t)i];
            Chord c = fc.chord;
            c.fileRef = (int16_t)((cacheFile_ << 8) | i);
            if (exact) { exact->count = 0; for (int k = 0; k < fc.count; ++k) exact->push(fc.notes[k]); }
            return c;
        }
        if (exact) exact->count = 0;
        return stepChord(prog_->steps[i], key_, sev);
    }

    std::string chordName(const Key& k, const Chord& c) const
    {
        if (c.valid() && c.fileRef >= 0)
        {
            const size_t f = (size_t)(c.fileRef >> 8), st = (size_t)(c.fileRef & 255);
            if (f < files_.size() && st < files_[f].chords.size()) return files_[f].chords[st].name;
            return "?";
        }
        return k.name(c);
    }

    void playChord(const Chord& c, int near, float amp, const Notes* exact = nullptr)
    {
        Notes v;
        if (exact && exact->count > 0)
        {
            // The file's own voicing, as written. Smooth voicing carries on from its upper notes.
            v = *exact;
            prevUpper_.count = 0;
            const int first = (v.count > 2 && v.n[1] - v.n[0] >= 7) ? 1 : 0;
            for (int i = first; i < v.count && prevUpper_.count < 5; ++i) prevUpper_.push(v.n[i]);
        }
        else v = getBool(kSmooth) ? leadVoice(c, prevUpper_, getBool(kRootBass)) : plainVoice(c, near, getBool(kRootBass));
        const int oct = 12 * getInt(kChordOct);
        for (int i = 0; i < v.count; ++i) v.n[i] = mpc::clamp(v.n[i] + oct, 12, 120);
        nowChord_ = c; if (c.degree >= 0) lastDegree_ = c.degree;
        displayDirty_ = true;

        const int key = currentKey_;
        // Arp
        if (getInt(kArp) != 0 && v.count > 1) { arpStart(v, amp, key); return; }
        // Strum
        const float sb = strumBeats(getInt(kStrum));
        if (sb > 0.0f && v.count > 1)
        {
            const int dirSet = getInt(kStrumDir);
            bool down = dirSet == 1;
            if (dirSet == 2) { down = strumAlt_; strumAlt_ = !strumAlt_; }
            const double gap = sb * 60.0 / bpm_ * sampleRate();
            for (int i = 0; i < v.count; ++i)
            {
                const int idx = down ? v.count - 1 - i : i;
                const float a = amp * (1.0f - 0.03f * (float)i);
                if (i == 0) engine_.start(v.n[idx], a, mainSound_, key);
                else schedule(clock_ + curPos_ + (int64_t)(gap * i), v.n[idx], a, key);
            }
            return;
        }
        for (int i = 0; i < v.count; ++i) engine_.start(v.n[i], amp, mainSound_, key);
    }

    void keyUp(int note)
    {
        if (!keyDown_[note]) return;
        keyDown_[note] = false;
        for (int i = 0; i < npending_; ++i) if (pending_[i].owner == note) pending_[i].owner = -1;   // cancel strum notes
        engine_.releaseOwner(note, pedal_);
        if (arp_.on && arp_.owner == note && !getBool(kArpLatch)) arpStop();
    }

    void panic()
    {
        engine_.allOff();
        for (auto& h : keyDown_) h = false;
        npending_ = 0; arpStop(); pedal_ = false;
    }

    // ------------------------------------------------------------------ strum queue
    struct Pending { int64_t at; int note; float amp; int owner; };
    void schedule(int64_t at, int note, float amp, int owner)
    {
        if (npending_ < kMaxPending) pending_[npending_++] = {at, note, amp, owner};
    }

    // ------------------------------------------------------------------ arpeggiator
    struct Arp
    {
        bool on = false;
        int seq[kMaxArp]; int len = 0; int idx = 0;
        int bass = -1; int cur = -1; int owner = -1;
        float amp = 1.0f;
        int64_t nextAt = 0, gateAt = 0;
        long long lastGrid = 0;
    } arp_;

    void arpStart(const Notes& v, float amp, int owner)
    {
        arpStop();
        int sorted[kMaxChordNotes]; int n = v.count;
        for (int i = 0; i < n; ++i) sorted[i] = v.n[i];
        for (int i = 1; i < n; ++i) { int x = sorted[i], j = i - 1; while (j >= 0 && sorted[j] > x) { sorted[j + 1] = sorted[j]; --j; } sorted[j + 1] = x; }
        int first = 0;
        arp_.bass = -1;
        if (n > 2 && sorted[1] - sorted[0] >= 7) { arp_.bass = sorted[0]; first = 1; }   // low root holds underneath
        int up[kMaxArp]; int m = 0;
        const int octs = getInt(kArpOct);
        for (int o = 0; o < octs; ++o) for (int i = first; i < n && m < kMaxArp; ++i) up[m++] = sorted[i] + 12 * o;
        const int mode = getInt(kArp);
        arp_.len = 0;
        if (mode == 2) for (int i = m - 1; i >= 0; --i) arp_.seq[arp_.len++] = up[i];
        else
        {
            for (int i = 0; i < m; ++i) arp_.seq[arp_.len++] = up[i];
            if (mode == 3 && m > 2) for (int i = m - 2; i >= 1 && arp_.len < kMaxArp; --i) arp_.seq[arp_.len++] = up[i];
        }
        arp_.on = true; arp_.idx = 0; arp_.owner = owner; arp_.amp = amp; arp_.cur = -1;
        if (arp_.bass >= 0) engine_.start(arp_.bass, amp, mainSound_, kOwnerArp);
        const int64_t now = clock_ + curPos_;
        arp_.lastGrid = gridAt(curPos_);
        arpStep(now);
    }
    void arpStop()
    {
        if (!arp_.on) return;
        arp_.on = false;
        engine_.releaseOwner(kOwnerArp, false);
    }
    double stepSamples() const { return arpBeats(getInt(kArpRate)) * 60.0 / bpm_ * sampleRate(); }
    long long gridAt(int pos) const
    {
        const double ppq = ppq0_ + pos * beatsPerSample_;
        return (long long)std::floor(ppq / arpBeats(getInt(kArpRate)) + 1e-9);
    }
    void arpStep(int64_t now)
    {
        if (arp_.cur >= 0) engine_.releaseOwnerNote(kOwnerArp, arp_.cur);
        if (arp_.len <= 0) return;
        const int n = getInt(kArp) == 4 ? arp_.seq[(int)(rng() % (uint32_t)arp_.len)] : arp_.seq[arp_.idx++ % arp_.len];
        arp_.cur = n;
        engine_.start(n, arp_.amp, mainSound_, kOwnerArp);
        const double ss = stepSamples();
        arp_.nextAt = now + (int64_t)ss;
        arp_.gateAt = now + (int64_t)(ss * get(kArpGate) * 0.01);
    }

    // Fires strum notes, arp steps and gates that are due at `pos`
    void runTimers(int pos)
    {
        curPos_ = pos;
        const int64_t now = clock_ + pos;
        for (int i = 0; i < npending_;)
        {
            if (pending_[i].at <= now)
            {
                if (pending_[i].owner >= 0 && keyDown_[pending_[i].owner])
                    engine_.start(pending_[i].note, pending_[i].amp, mainSound_, pending_[i].owner);
                pending_[i] = pending_[--npending_];
            }
            else ++i;
        }
        if (!arp_.on) return;
        if (arp_.cur >= 0 && now >= arp_.gateAt) { engine_.releaseOwnerNote(kOwnerArp, arp_.cur); arp_.cur = -1; }
        if (synced_)
        {
            const long long g = gridAt(pos);
            if (g != arp_.lastGrid) { arp_.lastGrid = g; arpStep(now); }
        }
        else if (now >= arp_.nextAt) arpStep(now);
    }
    // The next sample position (in this block) where a timer is due
    int nextTimer(int pos) const
    {
        int64_t best = INT64_MAX;
        const int64_t now = clock_ + pos;
        for (int i = 0; i < npending_; ++i) if (pending_[i].at < best) best = pending_[i].at;
        if (arp_.on)
        {
            if (arp_.cur >= 0 && arp_.gateAt < best) best = arp_.gateAt;
            if (synced_)
            {
                const double div = arpBeats(getInt(kArpRate));
                const double ppq = ppq0_ + pos * beatsPerSample_;
                const double nextLine = (std::floor(ppq / div + 1e-9) + 1.0) * div;
                const int64_t at = now + (int64_t)std::ceil((nextLine - ppq) / beatsPerSample_);
                if (at < best) best = at;
            }
            else if (arp_.nextAt < best) best = arp_.nextAt;
        }
        if (best == INT64_MAX) return INT32_MAX;
        const int64_t rel = best - clock_;
        return rel <= pos ? pos + 1 : (rel > INT32_MAX ? INT32_MAX : (int)rel);
    }

    uint32_t rng() { rng_ = rng_ * 1664525u + 1013904223u; return rng_ >> 8; }

    // ------------------------------------------------------------------ display
    // Publishes the chord names for the UI thread and tells the MPC to re-read them.
    void publishDisplay()
    {
        if (!displayDirty_) return;
        const int64_t minGap = (int64_t)(0.03 * sampleRate());
        if (!quietPublish_ && clock_ - lastNotify_ < minGap) return;
        displayDirty_ = false; lastNotify_ = clock_;

        const bool sev = getBool(kSevenths);
        Chord next;
        if (progLen() > 0) next = progChord(progPos_, sev, nullptr);
        else if (lastDegree_ >= 0) next = key_.diatonic(kNextDegree[lastDegree_], sev);
        const bool changed = keyA_.load() != key_.root || scaleA_.load() != key_.scale || nowA_.load() != nowChord_.pack()
                          || nextA_.load() != next.pack() || lenA_.load() != progLen() || posA_.load() != progPos_;
        keyA_.store(key_.root); scaleA_.store(key_.scale);
        nowA_.store(nowChord_.pack()); nextA_.store(next.pack());
        lenA_.store(progLen()); posA_.store(progPos_);
        if (quietPublish_) { quietPublish_ = false; stepMoved_ = false; return; }
        if (!changed && !stepMoved_) return;

        // Bump the display parameter so the host re-reads its text
        nowNextCounter_ = (nowNextCounter_ + 1) % 1000;
        for (int d : {(int)kNowNext, (int)kNow, (int)kNext})
        {
            values_[d]->store(param(d).toNormalised((float)nowNextCounter_), std::memory_order_relaxed);
            notifyHostParameterChanged(d);
        }
        if (stepMoved_ && progLen() > 0)
        {
            stepMoved_ = false;
            const int shown = progPos_ + 1 > kMaxSteps ? kMaxSteps : progPos_ + 1;   // long files: the knob tops out at 32
            values_[kStep]->store(param(kStep).toNormalised((float)shown), std::memory_order_relaxed);
            lastStepSeen_ = shown;
            notifyHostParameterChanged(kStep);
        }
    }

    // ------------------------------------------------------------------ programs
    void addPrograms()
    {
        std::vector<float> d(kNumParams);
        for (int i = 0; i < kNumParams; ++i) d[(size_t)i] = param(i).def;
        auto with = [&d](std::initializer_list<std::pair<int, float>> kv) { std::vector<float> v = d; for (auto& p : kv) v[(size_t)p.first] = p.second; return v; };
        addProgram("EP Chords", d);
        addProgram("Sloppy Pop", with({{kMode, 1}, {kProg, 1}, {kChorus, 40}}));
        addProgram("Neo Soul Lock", with({{kMode, 2}, {kProg, 7}, {kSevenths, 1}, {kChorus, 50}, {kTone, 40}, {kStrum, 2}}));
        addProgram("Pad Arp", with({{kSound, 1}, {kArp, 1}, {kArpRate, 3}, {kArpOct, 2}, {kArpLatch, 1}, {kAttack, 30}, {kDecay, 1500},
                                     {kSustain, 85}, {kRelease, 900}, {kChorus, 60}, {kSevenths, 1}}));
        addProgram("Strings Strum", with({{kSound, 2}, {kMode, 1}, {kProg, 4}, {kStrum, 4}, {kAttack, 60}, {kDecay, 1200},
                                           {kSustain, 85}, {kRelease, 700}, {kChorus, 45}}));
        addProgram("Gospel Organ", with({{kSound, 3}, {kMode, 1}, {kProg, 10}, {kBassSplit, 13}, {kAttack, 2}, {kDecay, 200},
                                          {kSustain, 100}, {kRelease, 90}, {kChorus, 55}, {kTone, 55}}));
        addProgram("Blues Lock", with({{kMode, 2}, {kProg, 11}, {kBassSplit, 13}, {kBassStyle, 1}, {kTone, 60}}));
        addProgram("Minor Pad", with({{kSound, 1}, {kKey, 9}, {kScale, 1}, {kMode, 1}, {kProg, 12}, {kAttack, 120}, {kDecay, 2000},
                                       {kSustain, 90}, {kRelease, 1500}, {kChorus, 70}, {kTone, 35}}));
        // One for each of the sounds added in 1.3.0 (appended: program numbers are saved in projects)
        addProgram("Soft Piano", with({{kSound, 4}, {kDecay, 5000}, {kSustain, 15}, {kRelease, 500}, {kChorus, 10}, {kTone, 45}}));
        addProgram("Synth Keys Arp", with({{kSound, 5}, {kArp, 1}, {kArpRate, 3}, {kArpOct, 2}, {kArpLatch, 1}, {kDecay, 600},
                                            {kSustain, 45}, {kRelease, 250}, {kChorus, 45}, {kSevenths, 1}}));
        addProgram("Wurli Soul", with({{kSound, 6}, {kMode, 1}, {kProg, 7}, {kSevenths, 1}, {kDecay, 3000}, {kSustain, 35},
                                        {kRelease, 300}, {kChorus, 35}, {kTone, 55}}));
        addProgram("Bell Ballad", with({{kSound, 7}, {kMode, 1}, {kProg, 9}, {kStrum, 3}, {kDecay, 4000}, {kSustain, 25},
                                         {kRelease, 900}, {kChorus, 55}, {kTone, 50}}));
        addProgram("Syn Pad Lock", with({{kSound, 8}, {kMode, 2}, {kProg, 8}, {kAttack, 40}, {kDecay, 3000}, {kSustain, 90},
                                          {kRelease, 1600}, {kChorus, 60}, {kTone, 45}}));
    }

    // ------------------------------------------------------------------ state
    Engine engine_;
    Progression progs_[kNumProgPresets];
    const Progression* prog_ = &progs_[0];
    std::vector<ProgFile> files_;            // loaded once in the constructor, read-only afterwards
    const ProgFile* file_ = nullptr;
    int cacheFile_ = -2; bool justReset_ = true;
    Key key_;
    int cacheKey_ = -1, cacheScale_ = -1, cacheProg_ = -1; bool cacheSev_ = false;
    int progPos_ = 0, lastStepSeen_ = 1, lastDegree_ = -1, mainSound_ = 0;
    bool quietPublish_ = false;
    bool stepMoved_ = false, displayDirty_ = true, pedal_ = false, wasPlaying_ = false, strumAlt_ = false, synced_ = false;
    Chord nowChord_;
    Notes prevUpper_;
    bool keyDown_[128] = {};
    Pending pending_[kMaxPending]; int npending_ = 0;
    int64_t clock_ = 0, lockAt_ = -1000000000, lastNotify_ = -1000000000;
    int curPos_ = 0;
    int currentKey_ = 0;   // the physical key a chord belongs to (its note-off releases it)
    double bpm_ = 120.0, ppq0_ = 0.0, beatsPerSample_ = 0.0;
    int nowNextCounter_ = 0;
    uint32_t rng_ = 12345u;

    // Read by the UI thread in formatValue()
    std::atomic<int> keyA_{0}, scaleA_{0}, lenA_{0}, posA_{0};
    std::atomic<uint32_t> nowA_{0}, nextA_{0};
};

} // namespace

MPC_PLUGIN_ENTRY(ChordStage)
