// synth.h - the built-in sounds of Chord Stage: a small polyphonic engine.
//
// SPDX-License-Identifier: MIT
//
// Voices (16) each play one note with one of these sounds:
//   E.Piano  2-operator FM (sine modulating sine at 1:1) plus a short "tine" partial at 14:1.
//            Tone sets the FM depth; at Tone 0 it is a pure sine.
//   Pad      two detuned PolyBLEP saws + a sub sine through a 12 dB state-variable lowpass.
//   Strings  three detuned saws with gentle vibrato through the same lowpass.
//   Organ    four sine "drawbars" (1x, 2x, 3x, 4x); Tone pulls the upper ones in.
//   Bass     sine + filtered saw with its own plucky envelope (the left-hand bass zone).
//   Piano    soft piano: four slightly stretched sine partials, the upper ones dying away faster.
//   SynKeys  two detuned saws through a lowpass that snaps shut after the attack (pluck keys).
//   Wurli    FM with a short bark, softly overdriven: the reedy electric piano.
//   BellEP   1:1 FM body plus a ringing bell partial at 4x (glassy 80s keys).
//   SynPad   three wide-detuned saws + sub, its lowpass slowly opening after each note.
// The sum goes through a light stereo chorus (one delay line, two modulated taps).
//
// CPU: sines are table lookups, saws are PolyBLEP, filters are updated once per block.
// No allocation outside prepare().

#ifndef CHORD_STAGE_SYNTH_H
#define CHORD_STAGE_SYNTH_H

#include "dsp/adsr.h"
#include "dsp/math.h"
#include "dsp/oscillator.h"
#include "dsp/svf.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace cs {

enum Sound { kEPiano = 0, kPad, kStrings, kOrgan, kBass, kPiano, kSynKeys, kWurli, kBellEP, kSynPad, kNumSounds };

constexpr int kNumVoices = 16;
constexpr int kOwnerArp = 200;      // voice owner for arpeggiator notes (keys are 0..127)

inline float sineLookup(float ph) { return mpc::dsp::WavetableSine::lookup(ph); }
inline float wrap01(float p) { return p - (float)(int)p; }

inline float polyBlep(float t, float dt)
{
    if (t < dt) { const float x = t / dt; return x + x - x * x - 1.0f; }
    if (t > 1.0f - dt) { const float x = (t - 1.0f) / dt; return x * x + x + x + 1.0f; }
    return 0.0f;
}
inline float sawStep(float& ph, float inc)
{
    const float y = 2.0f * ph - 1.0f - polyBlep(ph, inc);
    ph += inc; if (ph >= 1.0f) ph -= 1.0f;
    return y;
}

struct Voice
{
    bool active = false;
    bool held = false;        // key (or arp gate) still down
    bool sustained = false;   // key up but the sustain pedal holds it
    int note = 60;
    int owner = -1;
    int sound = kEPiano;
    uint32_t age = 0;
    float amp = 0.0f;
    float baseHz = 261.6f;
    float ph[4] = {0, 0, 0, 0};
    float modEnv = 1.0f, tineEnv = 1.0f;
    float part[4] = {1, 1, 1, 1};   // per-partial decay (Piano, BellEP)
    float filtEnv = 0.0f;           // filter envelope (SynKeys falls, SynPad rises), per block
    mpc::dsp::ADSR env;
    mpc::dsp::SVF filt;
};

struct EnvSettings { float a = 2, d = 2500, s = 0.3f, r = 300; };

class Engine
{
public:
    void prepare(double sr)
    {
        sr_ = (float)sr;
        for (Voice& v : voices_) { v.env.setSampleRate(sr); v.filt.setSampleRate(sr); }
        delay_.assign(kDelaySize, 0.0f);
        modDecay_ = std::exp(-1.0f / (0.35f * sr_));
        static const float partSec[4] = {3.2f, 1.7f, 0.9f, 0.45f};
        for (int k = 0; k < 4; ++k) partDecay_[k] = std::exp(-1.0f / (partSec[k] * sr_));
        bellDecay_ = std::exp(-1.0f / (0.9f * sr_));
        tineDecay_ = std::exp(-1.0f / (0.04f * sr_));
        reset();
    }
    void reset()
    {
        for (Voice& v : voices_) { v.active = false; v.env.reset(); v.filt.reset(); }
        for (float& x : delay_) x = 0.0f;
        writePos_ = 0;
    }

    // Settings read once per block
    float tone = 0.5f, chorus = 0.3f, bend = 1.0f;
    EnvSettings envSet;

    void start(int note, float amp, int sound, int owner)
    {
        Voice& v = voices_[findVoice()];
        v.active = true; v.held = true; v.sustained = false;
        v.note = note; v.owner = owner; v.sound = sound; v.amp = amp; v.age = ++clock_;
        v.baseHz = 440.0f * std::pow(2.0f, (float)(note - 69) / 12.0f);
        for (float& p : v.ph) p = 0.0f;
        v.modEnv = 1.0f; v.tineEnv = 1.0f;
        for (float& a : v.part) a = 1.0f;
        v.filtEnv = sound == kSynPad ? 0.0f : 1.0f;
        v.filt.reset();
        applyEnv(v);
        v.env.reset();
        v.env.noteOn();
    }
    // Release every held voice of an owner. With the pedal down they are only marked sustained.
    void releaseOwner(int owner, bool pedal)
    {
        for (Voice& v : voices_)
            if (v.active && v.held && v.owner == owner)
            {
                v.held = false;
                if (pedal && owner != kOwnerArp) v.sustained = true; else v.env.noteOff();
            }
    }
    void releaseOwnerNote(int owner, int note)
    {
        for (Voice& v : voices_)
            if (v.active && v.held && v.owner == owner && v.note == note) { v.held = false; v.env.noteOff(); }
    }
    void pedalUp()
    {
        for (Voice& v : voices_) if (v.active && v.sustained) { v.sustained = false; v.env.noteOff(); }
    }
    void allOff() { for (Voice& v : voices_) if (v.active) { v.held = false; v.sustained = false; v.env.noteOff(); } }

    void envChanged() { for (Voice& v : voices_) if (v.active) applyEnv(v); }

    int activeCount() const { int n = 0; for (const Voice& v : voices_) n += v.active ? 1 : 0; return n; }

    // Renders numFrames into L/R (added). Call once per segment.
    void render(float* L, float* R, int numFrames, float gain)
    {
        // Per-block voice setup: pitch (bend, vibrato), filter cutoff
        vibPhase_ = wrap01(vibPhase_ + 5.5f * (float)numFrames / sr_);
        const float vib = 1.0f + 0.0025f * sineLookup(vibPhase_);
        const float t2 = tone * tone;
        const float blockFall = std::exp(-(float)numFrames / (0.18f * sr_));   // SynKeys filter: ~180 ms
        const float blockRise = 1.0f - std::exp(-(float)numFrames / (1.2f * sr_));   // SynPad filter: ~1.2 s
        float inc[kNumVoices][4];
        for (int k = 0; k < kNumVoices; ++k)
        {
            Voice& v = voices_[k];
            if (!v.active) continue;
            float hz = v.baseHz * bend;
            if (v.sound == kStrings) hz *= vib;
            const float i0 = hz / sr_;
            switch (v.sound)
            {
                case kPad:     inc[k][0] = i0 * 1.0035f; inc[k][1] = i0 * 0.9965f; inc[k][2] = i0 * 0.5f; inc[k][3] = 0;
                               v.filt.set(clampHz(250.0f + t2 * 6000.0f + hz * 1.5f), 0.12f); break;
                case kStrings: inc[k][0] = i0; inc[k][1] = i0 * 1.006f; inc[k][2] = i0 * 0.994f; inc[k][3] = 0;
                               v.filt.set(clampHz(700.0f + t2 * 7000.0f + hz * 2.0f), 0.08f); break;
                case kOrgan:   inc[k][0] = i0; inc[k][1] = i0 * 2.0f; inc[k][2] = i0 * 3.0f; inc[k][3] = i0 * 4.0f; break;
                case kBass:    inc[k][0] = i0; inc[k][1] = i0; inc[k][2] = 0; inc[k][3] = 0;
                               v.filt.set(clampHz(180.0f + t2 * 1800.0f + hz * 2.0f), 0.2f); break;
                case kPiano:   inc[k][0] = i0; inc[k][1] = i0 * 2.0008f; inc[k][2] = i0 * 3.0024f; inc[k][3] = i0 * 4.005f; break;
                case kSynKeys: inc[k][0] = i0; inc[k][1] = i0 * 1.0045f; inc[k][2] = i0 * 0.5f; inc[k][3] = 0;
                               v.filtEnv *= blockFall;
                               v.filt.set(clampHz(250.0f + hz * 1.5f + (600.0f + 5000.0f * t2) * (0.25f + v.filtEnv)), 0.25f); break;
                case kWurli:   inc[k][0] = i0; inc[k][1] = i0; inc[k][2] = 0; inc[k][3] = 0; break;
                case kBellEP:  inc[k][0] = i0; inc[k][1] = i0; inc[k][2] = i0 * 4.0f; inc[k][3] = i0 * 4.0f; break;
                case kSynPad:  inc[k][0] = i0 * 1.008f; inc[k][1] = i0 * 0.992f; inc[k][2] = i0; inc[k][3] = i0 * 0.5f;
                               v.filtEnv += (1.0f - v.filtEnv) * blockRise;
                               v.filt.set(clampHz(250.0f + hz + (500.0f + 6000.0f * t2) * v.filtEnv), 0.22f); break;
                case kEPiano:
                default:       inc[k][0] = i0; inc[k][1] = i0; inc[k][2] = i0 * 14.0f; inc[k][3] = 0; break;
            }
            if (inc[k][2] > 0.45f) inc[k][2] = 0.0f;   // keep partials under Nyquist
            if (inc[k][3] > 0.45f) inc[k][3] = 0.0f;
        }
        const float fmDepth = t2 * 2.2f, fmFloor = tone * 0.12f, tineAmt = tone * 0.12f;
        const float organ3 = 0.08f + 0.35f * tone, organ4 = 0.3f * t2;
        const float pw1 = 0.3f + 0.35f * tone, pw2 = 0.12f + 0.3f * tone, pw3 = 0.05f + 0.25f * t2;   // piano partials
        const float wIdx = 0.5f + 1.6f * tone, wDrive = 1.2f + 1.3f * tone;
        const float bellAmt = 0.15f + 0.45f * tone;

        for (int i = 0; i < numFrames; ++i)
        {
            float mono = 0.0f;
            for (int k = 0; k < kNumVoices; ++k)
            {
                Voice& v = voices_[k];
                if (!v.active) continue;
                const float e = v.env.process();
                if (!v.env.isActive()) { v.active = false; continue; }
                float y;
                switch (v.sound)
                {
                    case kPad:
                    {
                        float s = sawStep(v.ph[0], inc[k][0]) + sawStep(v.ph[1], inc[k][1]);
                        s += 0.6f * sineLookup(v.ph[2]); v.ph[2] = wrap01(v.ph[2] + inc[k][2]);
                        y = 0.45f * v.filt.process(s, mpc::dsp::SVF::Mode::Lowpass);
                        break;
                    }
                    case kStrings:
                    {
                        const float s = sawStep(v.ph[0], inc[k][0]) + sawStep(v.ph[1], inc[k][1]) + sawStep(v.ph[2], inc[k][2]);
                        y = 0.35f * v.filt.process(s, mpc::dsp::SVF::Mode::Lowpass);
                        break;
                    }
                    case kOrgan:
                    {
                        y = 0.55f * sineLookup(v.ph[0]) + 0.3f * sineLookup(v.ph[1]);
                        if (inc[k][2] > 0.0f) y += organ3 * 0.5f * sineLookup(v.ph[2]);
                        if (inc[k][3] > 0.0f) y += organ4 * 0.4f * sineLookup(v.ph[3]);
                        for (int p = 0; p < 4; ++p) v.ph[p] = wrap01(v.ph[p] + inc[k][p]);
                        break;
                    }
                    case kBass:
                    {
                        const float sine = sineLookup(v.ph[0]); v.ph[0] = wrap01(v.ph[0] + inc[k][0]);
                        const float saw = sawStep(v.ph[1], inc[k][1]);
                        y = 0.8f * sine + 0.5f * tone * v.filt.process(saw, mpc::dsp::SVF::Mode::Lowpass);
                        break;
                    }
                    case kPiano:
                    {
                        y = v.part[0] * sineLookup(v.ph[0]) + pw1 * v.part[1] * sineLookup(v.ph[1]);
                        if (inc[k][2] > 0.0f) y += pw2 * v.part[2] * sineLookup(v.ph[2]);
                        if (inc[k][3] > 0.0f) y += pw3 * v.part[3] * sineLookup(v.ph[3]);
                        for (int p = 0; p < 4; ++p) { v.ph[p] = wrap01(v.ph[p] + inc[k][p]); v.part[p] *= partDecay_[p]; }
                        y *= 0.62f;
                        break;
                    }
                    case kSynKeys:
                    {
                        float s = sawStep(v.ph[0], inc[k][0]) + sawStep(v.ph[1], inc[k][1]);
                        s += 0.5f * sineLookup(v.ph[2]); v.ph[2] = wrap01(v.ph[2] + inc[k][2]);
                        y = 0.65f * v.filt.process(s, mpc::dsp::SVF::Mode::Lowpass);
                        break;
                    }
                    case kWurli:
                    {
                        const float idx = (wIdx * v.modEnv + 0.25f * tone) * (0.5f + 0.5f * v.amp);
                        const float c = sineLookup(v.ph[0] + idx * sineLookup(v.ph[1]) * 0.159f);
                        y = 0.7f * mpc::dsp::fastTanh(c * wDrive * (0.6f + 0.4f * v.amp));
                        v.ph[0] = wrap01(v.ph[0] + inc[k][0]); v.ph[1] = wrap01(v.ph[1] + inc[k][1]);
                        v.modEnv *= modDecay_;
                        break;
                    }
                    case kBellEP:
                    {
                        y = 0.75f * sineLookup(v.ph[0] + 0.25f * tone * v.modEnv * sineLookup(v.ph[1]) * 0.159f);
                        if (inc[k][2] > 0.0f)
                            y += bellAmt * v.part[0] * sineLookup(v.ph[2] + 0.6f * v.part[0] * sineLookup(v.ph[3]) * 0.159f);
                        for (int p = 0; p < 4; ++p) v.ph[p] = wrap01(v.ph[p] + inc[k][p]);
                        v.modEnv *= modDecay_; v.part[0] *= bellDecay_;
                        break;
                    }
                    case kSynPad:
                    {
                        float s = sawStep(v.ph[0], inc[k][0]) + sawStep(v.ph[1], inc[k][1]) + sawStep(v.ph[2], inc[k][2]);
                        s += 0.5f * sineLookup(v.ph[3]); v.ph[3] = wrap01(v.ph[3] + inc[k][3]);
                        y = 0.3f * v.filt.process(s, mpc::dsp::SVF::Mode::Lowpass);
                        break;
                    }
                    case kEPiano:
                    default:
                    {
                        const float m = sineLookup(v.ph[1]);
                        const float idx = (fmDepth * v.modEnv + fmFloor) * (0.4f + 0.6f * v.amp);
                        y = sineLookup(v.ph[0] + idx * m * 0.159f);
                        if (inc[k][2] > 0.0f) y += tineAmt * v.tineEnv * sineLookup(v.ph[2]);
                        v.ph[0] = wrap01(v.ph[0] + inc[k][0]); v.ph[1] = wrap01(v.ph[1] + inc[k][1]); v.ph[2] = wrap01(v.ph[2] + inc[k][2]);
                        v.modEnv *= modDecay_; v.tineEnv *= tineDecay_;
                        y *= 0.8f;
                        break;
                    }
                }
                mono += y * e * v.amp;
            }
            mono *= gain;

            // Chorus: two taps around 9 ms swinging +-2.5 ms in opposite directions
            delay_[(size_t)writePos_] = mono;
            float l = mono, r = mono;
            if (chorus > 0.001f)
            {
                chPhase_ += 0.55f / sr_; if (chPhase_ >= 1.0f) chPhase_ -= 1.0f;
                const float m = sineLookup(chPhase_);
                const float dl = (0.009f + 0.0025f * m) * sr_, dr = (0.009f - 0.0025f * m) * sr_;
                const float wl = tap(dl), wr = tap(dr);
                const float w = chorus * 0.7f;
                l = mono * (1.0f - 0.3f * chorus) + w * wl;
                r = mono * (1.0f - 0.3f * chorus) + w * wr;
            }
            writePos_ = (writePos_ + 1) & (kDelaySize - 1);
            L[i] += mpc::dsp::fastTanh(l);
            R[i] += mpc::dsp::fastTanh(r);
        }
    }

private:
    static constexpr int kDelaySize = 2048;   // 46 ms at 44.1 kHz, power of two

    float clampHz(float hz) const { return hz > 0.45f * sr_ ? 0.45f * sr_ : hz; }
    float tap(float delaySamples) const
    {
        float pos = (float)writePos_ - delaySamples;
        while (pos < 0.0f) pos += (float)kDelaySize;
        const int i0 = (int)pos; const float f = pos - (float)i0;
        const float a = delay_[(size_t)(i0 & (kDelaySize - 1))], b = delay_[(size_t)((i0 + 1) & (kDelaySize - 1))];
        return a + f * (b - a);
    }
    void applyEnv(Voice& v)
    {
        switch (v.sound)
        {
            case kBass:    v.env.set(3.0f, 700.0f, 0.55f, 120.0f); break;
            case kPad:     v.env.set(envSet.a + 200.0f, envSet.d, envSet.s, envSet.r * 1.5f); break;
            case kStrings: v.env.set(envSet.a + 80.0f, envSet.d, envSet.s, envSet.r); break;
            case kOrgan:   v.env.set(envSet.a + 3.0f, envSet.d, envSet.s, envSet.r); break;
            case kSynPad:  v.env.set(envSet.a + 300.0f, envSet.d, envSet.s, envSet.r * 1.5f); break;
            default:       v.env.set(envSet.a, envSet.d, envSet.s, envSet.r); break;
        }
    }
    int findVoice()
    {
        int best = -1; uint32_t bestAge = 0xffffffffu;
        for (int k = 0; k < kNumVoices; ++k) if (!voices_[k].active) return k;
        // steal the oldest released voice, then the oldest held one
        for (int pass = 0; pass < 2 && best < 0; ++pass)
            for (int k = 0; k < kNumVoices; ++k)
            {
                const Voice& v = voices_[k];
                const bool releasing = !v.held && !v.sustained;
                if ((pass == 0 && !releasing) || v.age >= bestAge) continue;
                best = k; bestAge = v.age;
            }
        return best < 0 ? 0 : best;
    }

    Voice voices_[kNumVoices];
    std::vector<float> delay_;
    int writePos_ = 0;
    float sr_ = 44100.0f, chPhase_ = 0.0f, vibPhase_ = 0.0f;
    float modDecay_ = 0.9999f, tineDecay_ = 0.999f, bellDecay_ = 0.9999f;
    float partDecay_[4] = {0.9999f, 0.9999f, 0.9999f, 0.9999f};
    uint32_t clock_ = 0;
};

} // namespace cs

#endif // CHORD_STAGE_SYNTH_H
