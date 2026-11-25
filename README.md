# Chord Stage for MPC

The MPC version of [Chord Stage](https://germanikkus-music.github.io/chord-stage). It's an instrument plugin for the MPC Live III (it also runs on 32-bit MPCs) that turns loose, "not-so-precise" fingering on a keyboard like the Akai MPK mini Plus II into the right chords, played with its own built-in sound.

- **Key + scale correction.** Wrong notes snap to the nearest note in the key (ties go down).
- **One key = one chord.** Each key plays the diatonic triad (or 7th) on its scale degree.
- **Progressions.** Pick one of 15 presets. **Sloppy** mode plays the progression's exact chord when you land near the next chord's root. **Lock** mode makes only that root key sound; every other key stays silent.
- **Smooth voicing.** Each chord takes the inversion closest to the last one, like the web app's voice leading.
- **Strum and Arp** follow the MPC tempo. While the transport plays, arp steps land on the MPC's grid.
- **Bass zone** on the far left (single notes, octaves or root + 5th), kept in key.
- **Sounds:** E.Piano, Pad, Strings and Organ, plus a synth bass for the bass zone and a light chorus.
- **Now / Next** on the PLAY page shows the chord you played and the one to play next, e.g. `Am > G`.

## Install

1. Put the MPC's SD card (or a USB drive) in your computer.
2. Make a folder called `VST2 Plugins` at the top of the card if it isn't there yet.
3. Copy the whole folder `build/Germanikkus - VST - Chord Stage` into `VST2 Plugins`. You get `VST2 Plugins/Germanikkus - VST - Chord Stage/`, which holds `plugin.json`, `version.xml`, `VST/` and `Plugin Skins/`.
4. Eject the card and put it in the MPC.
5. The MPC needs **MPC OS 3.10.1 or later** and **Preferences → Enhancements (Beta) → Enable Enhancements and External Plugin Support** turned on. Restart the MPC after you turn it on.
6. Make a **Plugin** track and choose **Germanikkus → Chord Stage** as its instrument. Set the track's MIDI input to your keyboard (or All).

## Playing it

The plugin is an instrument, so it plays its own sound. Plugins on the MPC can't send MIDI on to other tracks or programs. If you want the chords in another sound, record the plugin track's audio or resample it.

| Page | Top row | Next rows |
|---|---|---|
| **PLAY** | Now / Next · Step · Progression · Mode | Key · Scale · 7ths · Smooth / Sound · Tone · Chorus · Volume |
| **CHORDS** | Split · Upper · Root Bass · Chord Oct | Bass Split · Bass Style · Bass Sound · Bass Level |
| **FEEL** | Strum · Strum Dir · Arp · Arp Rate | Arp Oct · Arp Gate · Arp Latch · Vel Sens |
| **SOUND** | Attack · Decay · Sustain · Release | Sound · Tone · Chorus · Volume |

- **Mode.** **Chords** means every key plays its key's chord. If a progression is loaded and you play its next chord (or the one after), Step moves on. **Sloppy** means a key whose note, or the in-key note it snaps to, is the root of the next (or next-but-one) progression chord plays that chord, including chords from outside the key such as the E major in the Andalusian progression. Any other key plays its own chord in the key. **Lock** means only the next chord's root key plays (that chord). Keys pressed within 150 ms after it count as the same hand and stay silent.
- **Step** shows where you are (`3/8`). Turn it to jump. Pressing Play from the start of the sequence sends the progression back to step 1, and so does changing the Progression or the Key.
- **Split / Upper.** Keys at or above Split play the Upper zone: single notes in key (**Melody**) or chords. Off means the whole keyboard plays chords.
- **Bass Split.** Keys below it play bass notes in key, moved down so the zone sits in bass range. **Bass Sound** is the built-in synth bass, or **Main** for the chord sound.
- **Root Bass** adds the chord root under every chord. **Chord Oct** moves chords up or down by octaves.
- **Strum** rolls each chord with a note gap set as a tempo division (`1/32` at 120 BPM is about 62 ms). **Dir** is Up, Down or Alt (alternating).
- **Arp** plays the held chord one note at a time at **Arp Rate**. A low root holds underneath as a drone. **Latch** keeps the arp going after you let go, until the next chord.
- Sustain pedal (CC64), pitch bend (±2 semitones) and All Notes Off work.
- Note names follow the web app: MIDI 60 = **C4** (the MPC itself may label it C3).

### Built-in programs

EP Chords · Sloppy Pop · Neo Soul Lock · Pad Arp · Strings Strum · Gospel Organ · Blues Lock · Minor Pad

### Progressions

| Name | Chords (numerals counted from the major scale) |
|---|---|
| Pop | I V vi IV |
| Doo-Wop | I vi IV V |
| Sad Pop | vi IV I V |
| Canon | I V vi iii IV I IV V |
| Jazz ii-V | ii7 V7 Imaj7 Imaj7 |
| Turnaround | Imaj7 vi7 ii7 V7 |
| Neo Soul | IVmaj7 iii7 ii7 Imaj7 |
| R&B 9ths | ii9 V9 Imaj9 vi9 |
| Royal Road | IVmaj7 V7 iii7 vi7 |
| Gospel | I I7 IV iv I vi ii7 V7 |
| Blues 12 | I7 ×4, IV7 ×2, I7 ×2, V7 IV7 I7 V7 |
| Minor Pop | i bVI bIII bVII |
| Andalusian | i bVII bVI V |
| Dark Minor | i iv bVI V |
| Rock bVII | I bVII IV I |

Plain numerals (no 7 or 9) become 7th chords when **7ths** is on and the chord belongs to the key. Pick Key and Scale yourself; the minor progressions sound right with Scale set to Minor, though they play correctly in any scale.

## Build and test

The plugin builds with the [MPC Plugin SDK](../mpc-plugin-builder-sdk) next to this folder:

```bash
python3 ../mpc-plugin-builder-sdk/tools/build.py .
```

That builds `VST/chord-stage-arm32.so` and `VST/chord-stage-arm64.so` into `build/Germanikkus - VST - Chord Stage/`, plus a native copy for testing. Two tests:

```bash
../mpc-plugin-builder-sdk/.sdk-tools/host_sim build/native/chord-stage.dylib test --notes 60 --wav build/test.wav
```

```bash
test/run.sh
```

`test/run.sh` plays a scripted performance and writes `build/script.wav`. It covers Chords, Sloppy and Lock modes, Smooth voicing, the bass zone, strum timing, and arp steps on the grid while the transport plays. It checks every chord's pitches in the audio and the Now / Next text, 48 checks in all.

## Honest notes

- **Now / Next refresh.** To get the MPC to redraw it, the plugin bumps that parameter's value and tells the host it changed (`audioMasterAutomate`), at most every 30 ms and only when the text changes. This works in the simulator. Whether the MPC screen redraws right away needs checking on the hardware.
- **Not ported from the web app:** typing your own progressions (the MPC has no text entry), importing MPC `.progression` files, song form, the drum beat, loop recording, melody-over, the wind controller and MIDI out to other tracks.
- **No reverb**, to keep CPU low. Put the MPC's own reverb on the track.
- **CPU:** up to 16 voices. E.Piano and Organ are table-lookup sines. Pad and Strings use 2–3 anti-aliased saws and a filter per voice, so they cost the most. In the simulator the plugin uses well under 1% of a desktop core; expect a few percent on the Live III.
- Parameter ids and order are fixed now that projects can be saved. New parameters will only be added at the end.

## License

MIT © Germanikkus. Built on the MPC Plugin SDK framework (MIT).
