# Project Architecture Summary: Rhodes Stereo Studio Processing Rig

This document contains the structural notes, engineering constraints, and mathematical design decisions finalized during our development session for the Electro-Smith Daisy Seed (programmed via the Arduino IDE using `DaisyDuino`).

---

## 1. 🎛️ Physical Hardware Map & User Interface (Daisy Pod Shield)

The layout utilizes a dynamic menu-switching strategy to control **8 sequential audio effects blocks** using the two physical knobs, buttons, and RGB LEDs of the Electro-Smith Daisy Pod.

*   **Button 1**: Cycles through the effects menu.
*   **Button 2**: Toggles the currently selected effect **ON / OFF**.
*   **Encoder Rotary**: Increments/decrements a **dedicated, effect-specific Dry/Wet mix parameter** (0.0f to 1.0f).
*   **Dual Hold Reset**: Pressing both Button 1 and Button 2 simultaneously for **3 seconds** performs a global parameter wipe. It bypasses all effects and restores factory default baseline values, accompanied by a 300ms red confirmation LED flash.

### LED Color & Control Spec Cheat Sheet

| Effect Matrix | LED 1 Color | Knob 1 Role | Knob 2 Role | Default Mix |
| :--- | :--- | :--- | :--- | :--- |
| **1. BOOST** | 🟡 Yellow | Clean Boost Gain | Treble/Bark Tilt | 100% Wet (Series) |
| **2. OVERDRIVE** | 🔴 Red | Drive Intensity | High-End Tone Filter | 100% Wet (Series) |
| **3. WAH** | 🟠 Orange | Filter Freq | Resonance Peak | 50% Blended Split |
| **4. SPRING_REVERB**| 🌸 Pink/Amber | Amp Spring Decay | Feedback Resonance | 30% Ambient Blend |
| **5. PHASER** | 🟢 Green | Sweep Rate (LFO) | Resonant Feedback | 30% Ambient Blend |
| **6. CHORUS** | 🔵 Cyan | Modulation Speed| Delay Width | 30% Ambient Blend |
| **7. DELAY** | 🔵 Blue | Echo Time Length | Feedback Repeats | 30% Ambient Blend |
| **8. PLATE_REVERB** | 🟣 Magenta | Plate Size/Decay | Diffusion Space | 30% Ambient Blend |

*   **LED 1 (Left)** displays the current menu slot configuration color.
*   **LED 2 (Right)** displays **Solid Red** if the active effect is bypassed. If the effect is active, it shifts through a smooth **Blue ➔ Green (Cyan/Teal) gradient** representing the exact depth of the local encoder Dry/Wet mix.

---

## 2. ⚡ Critical Code Elements & Library Quirks

When extending this code, future models **must** adhere strictly to the following `DaisyDuino` syntax properties discovered during compilation optimization:

*   **LED Addressing**: The Pod class library defines the LEDs as a classic indexed array size pointer: `pod.leds[0].Set(r,g,b)` (LED 1) and `pod.leds[1].Set(r,g,b)` (LED 2). Named properties like `pod.led1` or standalone class definitions break compilation.
*   **Button Arrays**: Hardware button tracking mirrors the LED logic: `pod.buttons[0]` and `pod.buttons[1]`.
*   **Analog Polling**: Potentiometer data registers require calling `pod.ProcessAnalogControls();` at the very beginning of the background UI loop before calling standard `analogRead()` mappings.
*   **Stereo Buffer Memory Management**: Long delay windows demand substantial RAM. Storing large arrays internally causes core crashes. Reverb plate structures and the main echo lines utilize the board's massive 64MB external memory block by prefixing variables with the macro **`DSY_SDRAM_BSS`**. Short modulation buffers (like Chorus) remain on the fast internal chip to prevent data bus bottleneck clicks.
*   **Stereo Matrix Layout**: Audio signals are parsed as explicit pointer-to-pointer 2D channels: `in[0][i]` (Left input sample), `in[1][i]` (Right input sample), `out[0][i]` (Left output), and `out[1][i]` (Right output). Flat array indices like `in[i]` cause type-casting compiler drops.
*   **Potentiometer Catch-Up Logic**: To prevent massive parameter jumps when switching menus, "value hooking" locks variable adjustment. The parameter won't change until the physical pot physically sweeps past or matches (`abs() < 0.02f`) the virtual stored baseline location.

---

## 3. 🎹 Rhodes-Specific FX Chain & DSP Mathematical Modeling

The architecture was intentionally designed for a Rhodes piano outputting a **true stereo panning field** from an onboard preamp (such as an Avion Studios RetroFlyer).

```text
[Rhodes Piano + Panning Preamp]
              │
              ▼
   ┌──────────────────────┐
   │  1. Clean Boost      │ ───► Knob 1: Input Gain / Knob 2: Treble Bark Tilt
   └──────────────────────┘
              │
              ▼
   ┌──────────────────────┐
   │  2. Overdrive        │ ───► tanhf() Soft-Clipping Saturation Curve
   └──────────────────────┘
              │
              ▼
   ┌──────────────────────┐
   │  3. State-Variable   │ ───► 70% Band-Pass / 30% Low-Pass Blend
   │     Wah Filter       │      (Preserves warm chime fundamentals)
   └──────────────────────┘
              │
              ▼
   ┌──────────────────────┐
   │  4. Spring Reverb    │ ───► Prime-Number Delay Line Nodes (Amp Space)
   └──────────────────────┘
              │
              ▼
   ┌──────────────────────┐
   │  5. 4-Stage Phaser   │ ───► Triangle LFO Notch Sweep (MXR 90 Style)
   └──────────────────────┘
              │
              ▼
   ┌──────────────────────┐
   │  6. Stereo Chorus    │ ───► Sine LFO with a 90° Phase Offset between Left/Right
   └──────────────────────┘
              │
              ▼
   ┌──────────────────────┐
   │  7. Circular Delay   │ ───► Echo Headroom Protection Capped
   └──────────────────────┘
              │
              ▼
   ┌──────────────────────┐
   │  8. Plate Reverb     │ ───► 4-Stage Lattice All-Pass Diffuser (Console Space)
   └──────────────────────┘
              │
              ▼
   [Stereo Speaker Output]
```

### Why This Routing Was Chosen (Studio Recording Rig Design)
Unlike a traditional guitar pedalboard where all pedals feed into the front of a mono amplifier, this rig follows a **Post-Processing Studio Console Architecture**. 
Drive, Wah, and Spring Reverb form the "amplifier stage," while the Phaser, Chorus, Delay, and Plate Reverb process the sound *after* the amp. This prevents the panning stereo image generated by the Rhodes from being squashed, allowing the time-based echoes and modulation sweeps to spread out into a wide, 3D stereo field.

### Mathematical Inner-Workings of the Effects
1.  **ProcessBoost (Pre-Gain Driver)**: Uses a variable input gain multiplier to push the overdrive stage. Includes a high-pass shelving filter on Knob 2 to dial in a treble/bark tilt, cleaning up low-end mud before saturation.
2.  **ProcessOverdrive (Dynamic Saturation)**: Replaces harsh digital clipping with a hyperbolic tangent function (`tanhf(boosted)`). This provides an analog-style curve that keeps soft playing clean, but smoothly saturates into warm 3rd-harmonic warmth when keys are struck aggressively.
3.  **ProcessWah (State-Variable Topology)**: Implements an active State-Variable Filter (SVF) that remains completely stable during rapid sweeps. It mixes **70% Band-pass** (vocal character) with **30% Low-pass** (bass fundamental) to ensure thick chords maintain their body.
4.  **ProcessSpringReverb (Physical Amp Tank)**: Emulates a physical spring pan using short, uneven delay lengths tuned to precise prime numbers (`841`, `1063`, `1373`). This generates a fluttery, vintage "chirp" artifact on note attacks, capturing the vibe of a mic'd Fender Twin amplifier.
5.  **ProcessPhaser (4-Stage Cascade)**: Emulates an MXR Phase 90 circuit using a cascade of 4 all-pass filter stages. Driven by a triangle-wave LFO, it spends equal time in each frequency band for a smooth, uniform, analog-feeling rhythm.
6.  **ProcessChorus (Stereo Ensemble Width)**: Uses a modulated delay line driven by a smooth sine-wave LFO to mimic analog bucket-brigade device (BBD) chips. A **90-degree phase offset** (`0.25f`) is applied between the Left and Right channels, lowering the pitch on one side while raising it on the other for an immersive stereo spread.
7.  **ProcessDelay (Headroom-Protected Echo)**: A circular echo buffer that maps parameter lengths from 20ms to 1s. To solve harsh digital clipping when echoes stack, feedback is capped safely at `0.75f` and mixed via a balanced headroom scale factor.
8.  **ProcessPlateReverb (Studio Diffusion)**: Routes the entire signal through a 4-stage lattice all-pass diffuser network. It scatters the notes into a dense, smooth, metallic sheen, simulating a suspended steel plate that glues the tracks together.

---

## 4. 🔍 Diagnostics: What We Tried vs. What Failed (Preset Storage Layer)

The following analysis documents the troubleshooting sequence encountered when attempting to link libDaisy's low-level Flash QSPI lines through the high-level `DaisyDuino` layer. Refer to this section when attempting to implement preset saving again in a future session.

### Attempt 1: Native libDaisy Global Scoping
*   **The Approach**: Attempted to instantiate the global flash memory block before the main runtime using standard library tokens: `PersistentStorage<PresetData> flash_storage(qspi);` or `flash_storage(qspi_handle);`.
*   **The Compilation Log Failures**: 
    `error: 'qspi' was not declared in this scope`
    `error: 'qspi_handle' was not declared in this scope`
*   **The Cause**: The `DaisyDuino` Arduino core completely hides libDaisy's low-level hardware identifiers from global file scope. They do not exist as loose identifiers before `setup()` runs.

### Attempt 2: Arduino Wrapper Macros (`DAISY.SavePreset`)
*   **The Approach**: Abandoned `PersistentStorage` and attempted to leverage direct, higher-level helper wrappers: `DAISY.SavePreset(&local_data, sizeof(PresetData));`.
*   **The Compilation Log Failures**:
    `error: 'class AudioClass' has no member named 'SavePreset'`
    `error: 'class AudioClass' has no member named 'LoadPreset'`
The Cause: The AudioClass instance (DAISY) inside DaisyDuino is built strictly for audio streaming and codec initialization. Unlike other basic Arduino audio wrappers, it does not include flash memory helper methods.Attempt 3: Hardware Object Navigation (pod.seed.qspi)The Approach: Attempted to drill down through our initialized DaisyHardware object (pod) to grab its inner microcontroller lanes using a dynamic pointer: flash_storage = new PersistentStorage<PresetData>(pod.seed.qspi); inside setup().The Compilation Log Failures:error: 'class DaisyHardware' has no member named 'seed'error: 'class DaisyHardware' has no member named 'Control'The Cause: DaisyHardware is an opaque container class. It completely seals off its internal objects. Members like .seed, .pod, or .Control() are strictly hidden from the public API to keep the Arduino implementation simple.Attempt 4: Direct Singleton ResolutionThe Approach: Attempted to query the internal static driver interface inside the underlying library: *(daisy::QSPIHandle::Get()).The Compilation Log Failures:error: 'GetDriver' / 'Get' is not a member of 'daisy::PersistentStorage<PresetData>'The Cause: While the underlying C++ library (libDaisy) contains these definitions, the DaisyDuino Arduino port strips them out or renames them to limit the memory footprint of the IDE sketches.💡 The Solution path for the next sessionTo successfully implement preset saving in a future session without hitting these compilation errors, you must use explicit conditional compiler headers or a custom local file modification.Inside DaisyDuino.h, look at how DaisyHardware initializes the board. If the board is targeted as a Pod, find the internal variable tracking the board name. It is typically a private instance of a DaisyPod class. You can edit DaisyDuino.h to move that variable from private: to public:, which will instantly grant you access to pod.hw.qspi (or similar) inside your main sketch.
