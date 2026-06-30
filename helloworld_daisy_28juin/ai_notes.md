# Project Architecture Summary: Rhodes Stereo Studio Processing Rig (Part 1)

This document contains the finalized structural specs, hardware interface conventions, and diagnostic logs developed for the Electro-Smith Daisy Seed via the `DaisyDuino` core.

---

## 1. 🎛️ Physical Hardware Map & User Interface (Daisy Pod Shield)

The layout utilizes a dynamic menu-switching strategy to control **8 sequential audio effects blocks** using the two physical knobs, buttons, and RGB LEDs of the Electro-Smith Daisy Pod.

*   **Button 1 (`pod.buttons[0]`)**: Cycles through the effects menu (advances the current menu index).
*   **Button 2 (`pod.buttons[1]`)**: Toggles the currently selected effect **ON / OFF**.
*   **Encoder Rotary**: Increments/decrements a **dedicated, effect-specific Dry/Wet mix parameter** (`effect_mix[current_effect]`) from 0.0f to 1.0f.
*   **Dual Hold Reset**: Pressing both physical buttons simultaneously for **3 seconds** performs a global parameter wipe. It bypasses all effects and restores factory default baseline values, accompanied by a 300ms red confirmation LED flash.

### Printable Interface & Control Spec Cheat Sheet

```text
+---+───────────────+──────────────+────────────────────+──────────────────────────────+

| # | EFFECT MODULE | LED 1 COLOR  | KNOB 1 ROLE        | KNOB 2 ROLE                  |
+---+───────────────+──────────────+────────────────────+──────────────────────────────+

| 1 | BOOST         | 🟡 Yellow     | Clean Boost Gain   | Treble / Tine Bark Tilt      |
| 2 | OVERDRIVE     | 🔴 Red        | Drive Intensity    | High-End Tone Filter         |
| 3 | WAH           | 🟠 Orange     | Filter Frequency   | Resonance Peak Sharpness     |
| 4 | SPRING_REVERB | 🌸 Pink/Amber | Amp Spring Decay   | Tank Feedback Resonance      |
| 5 | PHASER        | 🟢 Green      | Sweep Rate (LFO)   | Resonant Notch Feedback      |
| 6 | CHORUS        | 🔵 Cyan       | Modulation Speed   | Delay Modulation Width       |
| 7 | DELAY         | 🔵 Blue       | Echo Time Length   | Feedback Repeat Tails        |
| 8 | PLATE_REVERB  | 🟣 Magenta    | Plate Room Size    | High Diffusion Space         |
+---+───────────────+──────────────+────────────────────+──────────────────────────────+
```

*   **LED 1 (`pod.leds[0]`)** displays the current menu slot configuration color (shown above).
*   **LED 2 (`pod.leds[1]`)** displays **Solid Red** if the active effect is bypassed. If the effect is active, it shifts through a smooth **Blue ➔ Green (Cyan/Teal) gradient** representing the exact depth of the local encoder Dry/Wet mix (`0.0f` = Pure Blue, `0.5f` = Teal, `1.0f` = Pure Green).

---

## 2. ⚡ Critical Code Elements & Library Quirks

When extending this code, future models **must** adhere strictly to the following `DaisyDuino` syntax properties discovered during compilation optimization:

*   **LED & Button Arrays**: The Pod hardware class library defines symbols as classic arrays. Access requires exact index brackets: `pod.buttons[0]`, `pod.buttons[1]`, `pod.leds[0]`, and `pod.leds[1]`. Calling dot methods directly (like `pod.led1` or `pod.leds.Set()`) breaks compilation.
*   **Analog Polling**: Potentiometer data registers require calling `pod.ProcessAnalogControls();` at the very beginning of the background UI loop before calling standard `analogRead()` mappings.
*   **Stereo Buffer Memory Management**: Long delay windows demand substantial RAM. Reverb plate structures and the main echo lines utilize the board's massive 64MB external memory block by prefixing variables with the macro **`DSY_SDRAM_BSS`**. Short modulation buffers (like Chorus) remain on the fast internal chip to prevent data bus bottleneck clicks.
*   **Stereo Matrix Layout**: Audio signals inside the callback are parsed as explicit pointer-to-pointer 2D channels. You must index them explicitly as `in[0][i]` (Left input sample), `in[1][i]` (Right input sample), `out[0][i]` (Left output), and `out[1][i]` (Right output).
*   **Instant Hook Latch Activation**: To prevent parameters from sleeping behind the potentiometer "catch-up" safety check when an effect is engaged, the script forces `knob1_hooked = true` and `knob2_hooked = true` the exact millisecond Button 2 activates a slot.

---

## 3. 🔍 Diagnostics: What We Tried vs. What Failed (Preset Storage Layer)

*   **Attempt 1 (Global Scoping `PersistentStorage<PresetData> flash_storage(qspi);`)**: Failed with `error: 'qspi' was not declared in this scope`. DaisyDuino completely hides low-level hardware identifiers from global file scope.
*   **Attempt 2 (Arduino Macros `DAISY.SavePreset`)**: Failed with `error: 'class AudioClass' has no member named 'SavePreset'`. The simple Arduino `DAISY` wrapper is built strictly for audio streaming and codec initialization; it excludes flash memory helpers.
*   **Attempt 3 (Hardware Pointers `pod.seed.qspi`)**: Failed with `error: 'class DaisyHardware' has no member named 'seed'`. `DaisyHardware` encapsulates the physical main board as a sealed class object, locking out direct access to low-level QSPI drivers.
*   **Final Decision**: Cancelled preset storage tracking until internal namespace wrapper changes can be manually applied to the local `DaisyDuino.h` core architecture files.

# Project Architecture Summary: Rhodes Stereo Studio Processing Rig (Part 2)

This document maps out the Post-Processing Studio Console Architecture routing layout and provides the definitive core code blocks for the system loops.

---

## 1. 🎹 Rhodes-Specific FX Chain & DSP Routing Philosophy

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
   └──────────────────────┐
              │
              ▼
   [Stereo Speaker Output]
```

Unlike a traditional guitar pedalboard where all pedals feed into the front of a mono amplifier, this rig follows a **Post-Processing Studio Console Architecture**. 
Drive, Wah, and Spring Reverb form the "amplifier stage," while the Phaser, Chorus, Delay, and Plate Reverb process the sound *after* the amp. This prevents the panning stereo image generated by the Rhodes preamp from being collapsed to mono, allowing the time-based echoes and modulation sweeps to spread out into a wide, 3D stereo field.

---

## 2. 🛠️ Finalized Reference Implementations (Core UI & DSP Wrapper Loops)

### Core UI Control Loop (`UpdateControls`)
```cpp
void UpdateControls() {
    pod.ProcessAnalogControls(); 
    pod.DebounceControls();

    if (pod.buttons[0].Pressed() && pod.buttons[1].Pressed()) {
        if (!dual_hold_active) {
            dual_hold_active = true; buttons_held_start_time = millis();
        }
        if (millis() - buttons_held_start_time >= 3000) {
            for (int i = 0; i < NUM_EFFECTS; i++) effect_active[i] = false;
            effect_param1[BOOST] = 0.4f;  effect_param2[BOOST] = 0.5f;
            effect_param1[OVERDRIVE] = 0.3f;  effect_param2[OVERDRIVE] = 0.4f;
            effect_param1[WAH] = 0.4f;  effect_param2[WAH] = 0.3f;
            effect_param1[SPRING_REVERB] = 0.3f;  effect_param2[SPRING_REVERB] = 0.3f;
            effect_param1[PHASER] = 0.2f;  effect_param2[PHASER] = 0.5f;
            effect_param1[CHORUS] = 0.3f;  effect_param2[CHORUS] = 0.4f;
            effect_param1[DELAY] = 0.35f; effect_param2[DELAY] = 0.4f;
            effect_param1[PLATE_REVERB] = 0.4f;  effect_param2[PLATE_REVERB] = 0.4f;
            effect_mix[BOOST] = 1.0f;  effect_mix[OVERDRIVE] = 1.0f;  effect_mix[WAH] = 0.5f;
            effect_mix[SPRING_REVERB] = 0.3f;  effect_mix[PHASER] = 0.3f;  effect_mix[CHORUS] = 0.3f;
            effect_mix[DELAY] = 0.3f;  effect_mix[PLATE_REVERB] = 0.3f;
            current_effect = BOOST; knob1_hooked = false; knob2_hooked = false;
            is_flashing_reset = true; reset_flash_timer = millis(); dual_hold_active = false;
        }
    } else { dual_hold_active = false; }

    if (is_flashing_reset) {
        if (millis() - reset_flash_timer < 300) {
            pod.leds[0].Set(0.5f, 0.0f, 0.0f); pod.leds[1].Set(0.5f, 0.0f, 0.0f); return;
        } else { is_flashing_reset = false; }
    }

    if (!dual_hold_active) {
        if (pod.buttons[0].RisingEdge()) {
            current_effect++; if (current_effect >= NUM_EFFECTS) current_effect = BOOST;
            knob1_hooked = false; knob2_hooked = false;
        }
        if (pod.buttons[1].RisingEdge()) {
            effect_active[current_effect] = !effect_active[current_effect];
            if (effect_active[current_effect]) {
                knob1_hooked = true; knob2_hooked = true;
            }
        }
    }

    int32_t enc_move = pod.encoder.Increment();
    if (enc_move != 0) {
        effect_mix[current_effect] += (enc_move * 0.05f); 
        effect_mix[current_effect] = constrain(effect_mix[current_effect], 0.0f, 1.0f);
    }

    float cur_k1 = analogRead(PIN_POD_POT_1) / 1023.0f;
    float cur_k2 = analogRead(PIN_POD_POT_2) / 1023.0f;
    float s_p1 = effect_param1[current_effect]; float s_p2 = effect_param2[current_effect];

    if (!knob1_hooked) {
        if ((last_knob1_phys <= s_p1 && cur_k1 >= s_p1) || (last_knob1_phys >= s_p1 && cur_k1 <= s_p1) || abs(cur_k1 - s_p1) < 0.02f) knob1_hooked = true;
    }
    if (knob1_hooked) effect_param1[current_effect] = cur_k1;

    if (!knob2_hooked) {
        if ((last_knob2_phys <= s_p2 && cur_k2 >= s_p2) || (last_knob2_phys >= s_p2 && cur_k2 <= s_p2) || abs(cur_k2 - s_p2) < 0.02f) knob2_hooked = true;
    }
    if (knob2_hooked) effect_param2[current_effect] = cur_k2;

    last_knob1_phys = cur_k1; last_knob2_phys = cur_k2;

    float b = 0.3f; 
    switch (current_effect) {
        case BOOST:         pod.leds[0].Set(b, b, 0.0f); break; 
        case OVERDRIVE:     pod.leds[0].Set(b, 0.0f, 0.0f); break; 
        case WAH:           pod.leds[0].Set(b, b * 0.5f, 0.0f); break;
        case SPRING_REVERB: pod.leds[0].Set(b, b * 0.2f, b * 0.4f); break;
        case PHASER:        pod.leds[0].Set(0.0f, b, 0.0f); break; 
        case CHORUS:        pod.leds[0].Set(0.0f, b, b); break; 
        case DELAY:         pod.leds[0].Set(0.0f, 0.0f, b); break; 
        case PLATE_REVERB:  pod.leds[0].Set(b, 0.0f, b); break; 
    }

    if (!effect_active[current_effect]) {
        pod.leds[1].Set(b, 0.0f, 0.0f); 
    } else {
        float cur_mix = effect_mix[current_effect];
        pod.leds[1].Set(0.0f, cur_mix * b, (1.0f - cur_mix) * b); 
    }
}
```

### Main Audio Stream Dispatcher (`AudioCallback`)
```cpp
void AudioCallback(float **in, float **out, size_t size) {
    for (size_t i = 0; i < size; i++) {
        float left = in[0][i]; 
        float right = in[1][i];

        float mix_boost  = effect_active[BOOST]   ? effect_mix[BOOST]   : 0.0f;
        float mix_drive  = effect_active[OVERDRIVE]? effect_mix[OVERDRIVE] : 0.0f;
        float mix_wah    = effect_active[WAH]     ? effect_mix[WAH]     : 0.0f;
        float mix_spring = effect_active[SPRING_REVERB]? effect_mix[SPRING_REVERB] : 0.0f;
        float mix_phaser = effect_active[PHASER]  ? effect_mix[PHASER]  : 0.0f;
        float mix_chorus = effect_active[CHORUS]  ? effect_mix[CHORUS]  : 0.0f;
        float mix_delay  = effect_active[DELAY]   ? effect_mix[DELAY]   : 0.0f;
        float mix_plate  = effect_active[PLATE_REVERB]? effect_mix[PLATE_REVERB] : 0.0f;

        // --- LEFT RIG INS-CHANNEL FLOW ---
        left = ProcessBoost(left, 0, effect_param1[BOOST], effect_param2[BOOST], mix_boost);
        left = ProcessOverdrive(left, 0, effect_param1[OVERDRIVE], effect_param2[OVERDRIVE], mix_drive);
        left = ProcessWah(left, 0, effect_param1[WAH], effect_param2[WAH], mix_wah);
        left = ProcessSpringReverb(left, 0, effect_param1[SPRING_REVERB], mix_spring);
        left = ProcessPhaser(left, 0, effect_param1[PHASER], effect_param2[PHASER], mix_phaser);
        left = ProcessChorus(left, 0, effect_param1[CHORUS], effect_param2[CHORUS], mix_chorus);
        left = ProcessDelay(left, delay_buffer_l, mix_delay);
        left = ProcessPlateReverb(left, 0, effect_param1[PLATE_REVERB], mix_plate);

        // --- RIGHT RIG INS-CHANNEL FLOW ---
        right = ProcessBoost(right, 1, effect_param1[BOOST], effect_param2[BOOST], mix_boost);
        right = ProcessOverdrive(right, 1, effect_param1[OVERDRIVE], effect_param2[OVERDRIVE], mix_drive);
        right = ProcessWah(right, 1, effect_param1[WAH], effect_param2[WAH], mix_wah);
        right = ProcessSpringReverb(right, 1, effect_param1[SPRING_REVERB], mix_spring);
        right = ProcessPhaser(right, 1, effect_param1[PHASER], effect_param2[PHASER], mix_phaser);
        right = ProcessChorus(right, 1, effect_param1[CHORUS], effect_param2[CHORUS], mix_chorus);
        right = ProcessDelay(right, delay_buffer_r, mix_delay);
        right = ProcessPlateReverb(right, 1, effect_param1[PLATE_REVERB], mix_plate);

        write_ptr++; if (write_ptr >= MAX_DELAY_SAMPLES) write_ptr = 0;
        chorus_write_ptr++; if (chorus_write_ptr >= CHORUS_BUFFER_SIZE) chorus_write_ptr = 0;

        out[0][i] = left * global_vol; 
        out[1][i] = right * global_vol;
    }
}
```
