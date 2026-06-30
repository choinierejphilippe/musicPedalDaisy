#include "DaisyDuino.h"

DaisyHardware pod;

/*
========================================================================================
                         DAISY POD STUDIO RIG INTERFACE MANUAL
========================================================================================
 [BUTTON 1] Cycle active menu slot     [BUTTON 2] Toggle selected effect ON / OFF
 [ENCODER]  Turn: Adjust Dry/Wet mix   [HOLD BOTH] Hold 1+2 for 3s to factory reset
 [LED 1]    Displays current menu color [LED 2]    RED (Bypassed) or BLUE->GREEN (Mix)
========================================================================================

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

========================================================================================
                         SIGNAL PIPELINE & CONTROLS SPECIFICATION
========================================================================================
 * Signal Flow: Input -> Boost -> Drive -> Wah -> Spring -> Phaser -> Chorus -> Delay -> Plate
 * Initialization: All effects default to OFF on power-up for a transparent base tone.
 * LED 2 Mix Indicator: 0% Wet = Pure Blue | 50% Wet = Cyan/Teal | 100% Wet = Pure Green
========================================================================================
*/

enum EffectType {
    BOOST,
    OVERDRIVE,
    WAH,
    SPRING_REVERB,
    PHASER,
    CHORUS,
    DELAY,
    PLATE_REVERB,
    NUM_EFFECTS
};

int current_effect = BOOST;

bool effect_active[NUM_EFFECTS] = {
    false, false, false, false, 
    false, false, false, false
}; 

float effect_param1[NUM_EFFECTS] = {
    0.4f, 0.3f, 0.4f, 0.5f, 0.2f, 0.3f, 0.35f, 0.5f
}; 

float effect_param2[NUM_EFFECTS] = {
    0.5f, 0.4f, 0.3f, 0.3f, 0.5f, 0.4f, 0.4f, 0.4f
}; 

float effect_mix[NUM_EFFECTS] = {
    1.0f, 1.0f, 0.5f, 0.3f, 0.3f, 0.3f, 0.3f, 0.3f
};

bool knob1_hooked = false;
bool knob2_hooked = false;
float last_knob1_phys = 0.0f;
float last_knob2_phys = 0.0f;

unsigned long buttons_held_start_time = 0;
bool dual_hold_active = false;
unsigned long reset_flash_timer = 0;
bool is_flashing_reset = false;

#define MAX_DELAY_SAMPLES 48000
float DSY_SDRAM_BSS delay_buffer_l[MAX_DELAY_SAMPLES];
float DSY_SDRAM_BSS delay_buffer_r[MAX_DELAY_SAMPLES];
int write_ptr = 0;

#define CHORUS_BUFFER_SIZE 2048
float chorus_buffer_l[CHORUS_BUFFER_SIZE];
float chorus_buffer_r[CHORUS_BUFFER_SIZE];
int chorus_write_ptr = 0;
float chorus_lfo_phase = 0.0f;
float phaser_lfo_phase = 0.0f;

#define NUM_SPRING_LINES 3
#define SPRING_LINE_0_SIZE 841
#define SPRING_LINE_1_SIZE 1063
#define SPRING_LINE_2_SIZE 1373
#define MAX_SPRING_BOUND 1400
// FIX: Added clear maximum bounds column allocations
float spring_buf_l[NUM_SPRING_LINES][MAX_SPRING_BOUND]; 
float spring_buf_r[NUM_SPRING_LINES][MAX_SPRING_BOUND];
int spring_write_ptrs[NUM_SPRING_LINES] = {0, 0, 0};

#define NUM_PLATE_STAGES 4
int plate_sizes[NUM_PLATE_STAGES] = {511, 733, 1109, 1439};
#define MAX_PLATE_BOUND 1450
// FIX: Added clear maximum bounds column allocations
float DSY_SDRAM_BSS plate_buf_l[NUM_PLATE_STAGES][MAX_PLATE_BOUND];
float DSY_SDRAM_BSS plate_buf_r[NUM_PLATE_STAGES][MAX_PLATE_BOUND];
int plate_ptrs[NUM_PLATE_STAGES] = {0, 0, 0, 0};

float global_vol = 0.5f;

float ApplyDryWet(float dry, float wet, float mix) {
    return (dry * (1.0f - mix)) + (wet * mix);
}

float ProcessBoost(float input, int ch, float g, float t) {
    if (!effect_active[BOOST]) return input;
    float gain = 1.0f + (g * 3.0f); 
    float boosted = input * gain;
    static float hp_state[2] = {0.0f, 0.0f};
    hp_state[ch] += 0.15f * (boosted - hp_state[ch]);
    float highs = boosted - hp_state[ch];
    float wet = boosted + (highs * t * 1.5f);
    return ApplyDryWet(input, wet * 0.75f, effect_mix[BOOST]);
}

float ProcessOverdrive(float input, int ch, float drive, float tone) {
    if (!effect_active[OVERDRIVE]) return input;
    float gain = 1.0f + (drive * 14.0f);
    float wet = tanhf(input * gain);
    static float lp_state[2] = {0.0f, 0.0f};
    float cutoff = 0.05f + (tone * 0.95f); 
    lp_state[ch] += cutoff * (wet - lp_state[ch]);
    float makeup_gain = 1.0f / sqrtf(gain);
    return ApplyDryWet(input, lp_state[ch] * makeup_gain, effect_mix[OVERDRIVE]);
}

float ProcessWah(float input, int ch, float freq, float res) {
    if (!effect_active[WAH]) return input;
    static float d1[2] = {0.0f, 0.0f};
    static float d2[2] = {0.0f, 0.0f};
    float target_freq = 200.0f + (freq * 2000.0f);
    float f = 2.0f * sinf(PI * target_freq / 48000.0f);
    f = constrain(f, 0.0f, 1.0f);
    float q = 0.1f + (res * 0.85f);
    float damping = 2.0f * (1.0f - q);
    float hp = input - d2[ch] - (damping * d1[ch]);
    float bp = (f * hp) + d1[ch];
    float lp = (f * bp) + d2[ch];
    d1[ch] = bp; d2[ch] = lp;
    float wet = (bp * 0.7f) + (lp * 0.3f);
    float attenuation = 1.0f - (res * 0.25f);
    return ApplyDryWet(input, wet * attenuation, effect_mix[WAH]);
}

float ProcessSpringReverb(float input, int ch, float size, float mix_ctrl) {
    if (!effect_active[SPRING_REVERB]) return input;
    int s_sizes[NUM_SPRING_LINES] = {SPRING_LINE_0_SIZE, SPRING_LINE_1_SIZE, SPRING_LINE_2_SIZE};
    float decay = 0.45f + (size * 0.35f);
    float s_input = input; float wet_accum = 0.0f;
    for (int i = 0; i < NUM_SPRING_LINES; i++) {
        int sz = s_sizes[i]; int w_ptr = spring_write_ptrs[i];
        int r_ptr = w_ptr - sz; if (r_ptr < 0) r_ptr += sz;
        float delayed = (ch == 0) ? spring_buf_l[i][r_ptr] : spring_buf_r[i][r_ptr];
        float fb_node = s_input + (delayed * decay);
        float ap_out = (-0.62f * fb_node) + delayed;
        if (ch == 0) { spring_buf_l[i][w_ptr] = fb_node - (0.62f * ap_out); } 
        else { spring_buf_r[i][w_ptr] = fb_node - (0.62f * ap_out); }
        if (ch == 1) {
            spring_write_ptrs[i]++;
            if (spring_write_ptrs[i] >= sz) spring_write_ptrs[i] = 0;
        }
        wet_accum += ap_out; s_input = ap_out * 0.5f;
    }
    return ApplyDryWet(input, wet_accum / 3.0f, effect_mix[SPRING_REVERB]);
}

float ProcessPhaser(float input, int ch, float rate, float feedback) {
    if (!effect_active[PHASER]) return input;
    static float ap_state[2][4] = {{0,0,0,0}, {0,0,0,0}};
    static float last_output[2] = {0.0f, 0.0f};
    if (ch == 0) {
        phaser_lfo_phase += (0.00001f + (rate * 0.0015f));
        if (phaser_lfo_phase >= 1.0f) phaser_lfo_phase -= 1.0f;
    }
    float tri_lfo = phaser_lfo_phase * 2.0f;
    if (tri_lfo > 1.0f) tri_lfo = 2.0f - tri_lfo;
    float g = 0.2f + (tri_lfo * 0.65f); 
    float f_input = input + (last_output[ch] * (feedback * 0.85f));
    float current = f_input;
    for (int stage = 0; stage < 4; stage++) {
        float ap_out = (g * current) + ap_state[ch][stage];
        ap_state[ch][stage] = current - (g * ap_out);
        current = ap_out;
    }
    last_output[ch] = current;
    return ApplyDryWet(input, current, effect_mix[PHASER]);
}

float ProcessChorus(float input, int ch, float rate, float depth) {
    if (!effect_active[CHORUS]) return input;
    float* buffer = (ch == 0) ? chorus_buffer_l : chorus_buffer_r;
    if (ch == 0) {
        chorus_lfo_phase += (0.00001f + (rate * 0.0012f));
        if (chorus_lfo_phase >= 1.0f) chorus_lfo_phase -= 1.0f;
    }
    float f_phase = chorus_lfo_phase + ((ch == 1) ? 0.25f : 0.0f);
    if (f_phase >= 1.0f) f_phase -= 1.0f;
    float sine_lfo = sinf(f_phase * 2.0f * PI);
    float cur_delay = 576.0f + (sine_lfo * (depth * 336.0f));
    float r_index = (float)chorus_write_ptr - cur_delay;
    if (r_index < 0.0f) r_index += (float)CHORUS_BUFFER_SIZE;
    int idx_a = (int)r_index;
    int idx_b = (idx_a + 1) >= CHORUS_BUFFER_SIZE ? 0 : idx_a + 1;
    float wet = buffer[idx_a] + (r_index - (float)idx_a) * (buffer[idx_b] - buffer[idx_a]);
    buffer[chorus_write_ptr] = input;
    return ApplyDryWet(input, wet, effect_mix[CHORUS]);
}

float ProcessDelay(float input, float *buffer) {
    if (!effect_active[DELAY]) return input;
    int d_samples = 1000 + (int)(effect_param1[DELAY] * (MAX_DELAY_SAMPLES - 1001));
    float feedback = effect_param2[DELAY] * 0.75f;
    int r_ptr = write_ptr - d_samples;
    if (r_ptr < 0) r_ptr += MAX_DELAY_SAMPLES;
    float delayed = buffer[r_ptr];
    buffer[write_ptr] = input + (delayed * feedback);
    return ApplyDryWet(input, delayed, effect_mix[DELAY]);
}

float ProcessPlateReverb(float input, int ch, float size, float mix_ctrl) {
    if (!effect_active[PLATE_REVERB]) return input;
    float decay = 0.35f + (size * 0.55f);
    float current = input; float accumulator = 0.0f;
    for (int stage = 0; stage < NUM_PLATE_STAGES; stage++) {
        int sz = plate_sizes[stage]; int w_ptr = plate_ptrs[stage];
        int r_ptr = w_ptr - sz; if (r_ptr < 0) r_ptr += sz;
        float d_out = (ch == 0) ? plate_buf_l[stage][r_ptr] : plate_buf_r[stage][r_ptr];
        float node = current + (d_out * decay);
        float ap_out = (-0.55f * node) + d_out;
        if (ch == 0) { plate_buf_l[stage][w_ptr] = node - (0.55f * ap_out); } 
        else { plate_buf_r[stage][w_ptr] = node - (0.55f * ap_out); }
        if (ch == 1) {
            plate_ptrs[stage]++;
            if (plate_ptrs[stage] >= sz) plate_ptrs[stage] = 0;
        }
        accumulator += ap_out; current = ap_out;
    }
    return ApplyDryWet(input, accumulator / 4.0f, effect_mix[PLATE_REVERB]);
}

void AudioCallback(float **in, float **out, size_t size) {
    for (size_t i = 0; i < size; i++) {
        float left = in[0][i]; float right = in[1][i];
        left = ProcessBoost(left, 0, effect_param1[BOOST], effect_param2[BOOST]);
        left = ProcessOverdrive(left, 0, effect_param1[OVERDRIVE], effect_param2[OVERDRIVE]);
        left = ProcessWah(left, 0, effect_param1[WAH], effect_param2[WAH]);
        left = ProcessSpringReverb(left, 0, effect_param1[SPRING_REVERB], effect_param2[SPRING_REVERB]);
        left = ProcessPhaser(left, 0, effect_param1[PHASER], effect_param2[PHASER]);
        left = ProcessChorus(left, 0, effect_param1[CHORUS], effect_param2[CHORUS]);
        left = ProcessDelay(left, delay_buffer_l);
        left = ProcessPlateReverb(left, 0, effect_param1[PLATE_REVERB], effect_param2[PLATE_REVERB]);
        right = ProcessBoost(right, 1, effect_param1[BOOST], effect_param2[BOOST]);
        right = ProcessOverdrive(right, 1, effect_param1[OVERDRIVE], effect_param2[OVERDRIVE]);
        right = ProcessWah(right, 1, effect_param1[WAH], effect_param2[WAH]);
        right = ProcessSpringReverb(right, 1, effect_param1[SPRING_REVERB], effect_param2[SPRING_REVERB]);
        right = ProcessPhaser(right, 1, effect_param1[PHASER], effect_param2[PHASER]);
        right = ProcessChorus(right, 1, effect_param1[CHORUS], effect_param2[CHORUS]);
        right = ProcessDelay(right, delay_buffer_r);
        right = ProcessPlateReverb(right, 1, effect_param1[PLATE_REVERB], effect_param2[PLATE_REVERB]);
        write_ptr++; if (write_ptr >= MAX_DELAY_SAMPLES) write_ptr = 0;
        chorus_write_ptr++; if (chorus_write_ptr >= CHORUS_BUFFER_SIZE) chorus_write_ptr = 0;
        out[0][i] = left * global_vol; out[1][i] = right * global_vol;
    }
}

void setup() {
    pod = DAISY.init(DAISY_POD, AUDIO_SR_48K);
    for (int i = 0; i < NUM_SPRING_LINES; i++) {
        for (int j = 0; j < MAX_SPRING_BOUND; j++) {
            spring_buf_l[i][j] = 0.0f; spring_buf_r[i][j] = 0.0f;
        }
    }
    for (int i = 0; i < NUM_PLATE_STAGES; i++) {
        for (int j = 0; j < MAX_PLATE_BOUND; j++) {
            plate_buf_l[i][j] = 0.0f; plate_buf_r[i][j] = 0.0f;
        }
    }
    for (int i = 0; i < MAX_DELAY_SAMPLES; i++) {
        delay_buffer_l[i] = 0.0f; delay_buffer_r[i] = 0.0f;
    }
    
    DAISY.begin(AudioCallback);
}

void UpdateControls() {
    pod.ProcessAnalogControls(); 
    pod.DebounceControls();

    // 3-Second Dual Hold Master Reset
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
        if (pod.buttons[1].RisingEdge()) effect_active[current_effect] = !effect_active[current_effect];
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

void loop() {
    UpdateControls();
    delay(5); 
}
