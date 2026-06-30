#include "DaisyDuino.h"

DaisyHardware pod;

// ==========================================
// EFFECTS CONFIGURATION & MENU NAVIGATION
// ==========================================
enum EffectType {
    OVERDRIVE,
    WAH,
    TREMOLO,
    PHASER,
    CHORUS,
    DELAY,
    REVERB,
    NUM_EFFECTS
};

// Tracks which effect is currently selected for editing via Button 1
int current_effect = OVERDRIVE;

// Arrays keeping track of state for each individual effect
bool effect_active[NUM_EFFECTS] = {true, true, true, true, true, true, true}; // All default ON
float effect_param1[NUM_EFFECTS] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f}; // Defaults for Knob 1
float effect_param2[NUM_EFFECTS] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f}; // Defaults for Knob 2


// Tracks if the physical knob has crossed ("hooked") the saved parameter value
bool knob1_hooked = false;
bool knob2_hooked = false;

// Stores the physical position of the knobs from the previous frame to detect movement direction
float last_knob1_phys = 0.0f;
float last_knob2_phys = 0.0f;

// Global LFO phase variable for the Phaser (0.0 to 1.0)
float phaser_lfo_phase = 0.0f;

// ==========================================
// DELAY ENGINE MEMORY 
// ==========================================
#define MAX_DELAY_SAMPLES 48000
// NEW EXTERNAL SDRAM ALLOCATION
float DSY_SDRAM_BSS delay_buffer_l[MAX_DELAY_SAMPLES];
float DSY_SDRAM_BSS delay_buffer_r[MAX_DELAY_SAMPLES];

int write_ptr = 0;

float global_vol = 0.5f;

// ==========================================
// Chorus Engine Memory (2048 samples is ~42ms at 48kHz, perfect for chorus)
// ==========================================
#define CHORUS_BUFFER_SIZE 2048
float chorus_buffer_l[CHORUS_BUFFER_SIZE];
float chorus_buffer_r[CHORUS_BUFFER_SIZE];
int chorus_write_ptr = 0;

// Global LFO phase variable for the Chorus
float chorus_lfo_phase = 0.0f;


// ==========================================
// INDIVIDUAL EFFECTS BLOCKS
// ==========================================

float ProcessOverdrive(float input, int channel, float drive, float tone) {
    // If bypassed, pass the audio through untouched
    if (!effect_active[OVERDRIVE]) return input;

    // Knob 1 (Drive): Map from 1.0x (clean clean) up to 15.0x (warm saturation)
    float gain = 1.0f + (drive * 14.0f);
    float boosted = input * gain;

    // Soft-clipping using the hyperbolic tangent function
    // This gently compresses peaks above 1.0f rather than hard-chopping them
    float saturated = tanhf(boosted);

    // Knob 2 (Tone): Simple low-pass filter to tame high-end harshness
    // We store the previous sample per channel to create a basic RC filter
    static float lp_state[2] = {0.0f, 0.0f};
    
    // Map tone knob to filter coefficient (higher knob = more highs allowed through)
    float cutoff = 0.05f + (tone * 0.95f); 
    
    // Apply the low-pass filter formula
    lp_state[channel] = lp_state[channel] + cutoff * (saturated - lp_state[channel]);

    // Apply a simple makeup gain so turning up the drive doesn't cause a massive volume blowout
    // This scales the output back down slightly as gain increases
    float makeup_gain = 1.0f / sqrtf(gain);

    return lp_state[channel] * makeup_gain;
}


float ProcessWah(float input, int channel, float freq, float res) {
    if (!effect_active[WAH]) return input;

    // --- 1. FILTER MEMORY STORAGE ---
    // Each stereo channel needs two independent feedback state storage variables
    static float d1 = {0.0f, 0.0f};
    static float d2 = {0.0f, 0.0f};

    // --- 2. PARAMETER MAPPING (RHODES TUNING) ---
    // Knob 1 (Freq): Map to the sweet-spot vowel range of a Rhodes (200 Hz to 2200 Hz)
    float target_freq = 200.0f + (freq * 2000.0f);
    
    // Convert target frequency to the SVF step coefficient 'f'
    // Formula approximation for 48kHz sample rate: 2 * sin(pi * F / Fs)
    float f = 2.0f * sinf(PI * target_freq / 48000.0f);
    f = constrain(f, 0.0f, 1.0f); // Ensure mathematical stability

    // Knob 2 (Resonance): Map to filter sharpness (Q factor)
    // Capped safely to prevent wild, ear-piercing self-oscillation
    float q = 0.1f + (res * 0.85f);
    float damping = 2.0f * (1.0f - q);

    // --- 3. FILTER CODE EQUATIONS (SVF) ---
    // Calculate the multi-format filter outputs simultaneously
    float hp = input - d2[channel] - (damping * d1[channel]);
    float bp = (f * hp) + d1[channel];
    float lp = (f * bp) + d2[channel];

    // Update internal feedback delay states for the next frame
    d1[channel] = bp;
    d2[channel] = lp;

    // --- 4. MIX FOR RHODES VOICE ---
    // Mixing 70% Band-pass (vocal wah texture) with 30% Low-pass (warm fundamental body)
    // This blend ensures your chords stay thick while notes maintain an expressive "quack"
    float wah_mix = (bp * 0.7f) + (lp * 0.3f);

    // Subtle makeup attenuation to compensate for the resonance volume bump
    float output_attenuation = 1.0f - (res * 0.25f);

    return wah_mix * output_attenuation;
}

float ProcessTremolo(float input, int channel, float rate, float depth) {
    if (!effect_active[TREMOLO]) return input;
    return input;
}

float ProcessPhaser(float input, int channel, float rate, float feedback) {
    if (!effect_active[PHASER]) return input;

    // --- 1. MEMORY STORAGE FOR CHANNELS ---
    // A 4-stage phaser needs 4 independent all-pass filter memory states per channel.
    // Indexing: [channel 0 or 1][filter stage 0 to 3]
    static float ap_state[2][4] = {{0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}};
    static float last_output[2] = {0.0f, 0.0f};

    // --- 2. LFO CALCULATION (Executed only once per full stereo sample frame) ---
    if (channel == 0) {
        // Knob 1 (Rate): Map parameter to LFO frequency (approx 0.05 Hz to 5.0 Hz)
        float lfo_speed = 0.00001f + (rate * 0.0015f); 
        phaser_lfo_phase += lfo_speed;
        if (phaser_lfo_phase >= 1.0f) {
            phaser_lfo_phase -= 1.0f;
        }
    }

    // Generate a smooth triangle wave from our raw LFO phase counter
    float triangle_lfo = phaser_lfo_phase * 2.0f;
    if (triangle_lfo > 1.0f) {
        triangle_lfo = 2.0f - triangle_lfo; // Mirrors the ramp into a triangle shape
    }

    // --- 3. FILTER COEFFICIENT CALCULATIONS ---
    // Map the LFO shape to an all-pass filter coefficient 'g'. 
    // This shifts the notch sweep smoothly between sweet spots for the Rhodes range.
    float g = 0.2f + (triangle_lfo * 0.65f); 

    // Knob 2 (Feedback): Map feedback depth up to 85% to prevent unstable runaway howling
    float fb_amount = feedback * 0.85f;

    // Mix the previous frame's wet output back into the input for deeper, chewing notches
    float filter_input = input + (last_output[channel] * fb_amount);

    // --- 4. FOUR-STAGE ALL-PASS FILTER CASCADE ---
    // Passing audio through 4 stages creates 2 deep, sweeping phase notches.
    float current_sample = filter_input;
    for (int stage = 0; stage < 4; stage++) {
        float next_sample = (g * current_sample) + ap_state[channel][stage] - (g * (g * current_sample + ap_state[channel][stage]));
        
        // Accurate first-order all-pass filter processing structure
        float ap_out = (g * current_sample) + ap_state[channel][stage];
        ap_state[channel][stage] = current_sample - (g * ap_out);
        
        current_sample = ap_out; // Feed output of this stage directly as the input to the next
    }

    // Store final wet output for the next frame's feedback calculation loop
    last_output[channel] = current_sample;

    // --- 5. DRY / WET BLENDING ---
    // Mix the dry signal and phase-shifted signal 50/50 for maximum cancellation depth
    return (input * 0.5f) + (current_sample * 0.5f);
}



float ProcessChorus(float input, int channel, float rate, float depth) {
    if (!effect_active[CHORUS]) return input;

    // Direct pointer to the correct stereo buffer channel
    float* buffer = (channel == 0) ? chorus_buffer_l : chorus_buffer_r;

    // --- 1. LFO SPEED & PHASE CALCULATION ---
    // Executed only once per stereo frame (on channel 0) to keep left/right synced
    if (channel == 0) {
        // Knob 1 (Rate): Map to traditional slow speed (approx 0.1 Hz to 4.0 Hz)
        float lfo_speed = 0.00001f + (rate * 0.0012f);
        chorus_lfo_phase += lfo_speed;
        if (chorus_lfo_phase >= 1.0f) {
            chorus_lfo_phase -= 1.0f;
        }
    }

    // --- 2. SINE LFO GENERATION ---
    // We offset the right channel LFO phase by 90 degrees (0.25f) for a wide stereo image
    float phase_offset = (channel == 1) ? 0.25f : 0.0f;
    float final_phase = chorus_lfo_phase + phase_offset;
    if (final_phase >= 1.0f) final_phase -= 1.0f;

    // Generate smooth sine modulation between -1.0 and 1.0
    float sine_lfo = sinf(final_phase * 2.0f * PI);

    // --- 3. DYNAMIC DELAY CALCULATIONS ---
    // Average delay base depth for Rhodes (~12 milliseconds)
    float base_delay = 576.0f; 

    // Knob 2 (Depth / Width): Modulates delay time up to ~7ms out from the base
    float max_mod_depth = depth * 336.0f; 
    float current_delay = base_delay + (sine_lfo * max_mod_depth);

    // --- 4. LINEAR INTERPOLATION (Preventing Zipper Noise) ---
    // Split the floating-point delay index into integer components
    float read_index = (float)chorus_write_ptr - current_delay;
    if (read_index < 0.0f) read_index += (float)CHORUS_BUFFER_SIZE;

    int idx_a = (int)read_index;
    int idx_b = idx_a + 1;
    if (idx_b >= CHORUS_BUFFER_SIZE) idx_b = 0;

    float fractional_part = read_index - (float)idx_a;

    // Fetch surrounding samples and blend them smoothly
    float sample_a = buffer[idx_a];
    float sample_b = buffer[idx_b];
    float interpolated_sample = sample_a + fractional_part * (sample_b - sample_a);

    // Write current live input to the chorus ring memory
    buffer[chorus_write_ptr] = input;

    // --- 5. BLEND DRY & MODULATED WET SIGNALS ---
    // 50/50 mix delivers the maximum phase cancellation/thickening effect
    float dry_mix = input * 0.5f;
    float wet_mix = interpolated_sample * 0.5f;

    return dry_mix + wet_mix;
}


// Delay extracts live tracking settings directly from unified parameter storage
float ProcessDelay(float input, float *buffer) {
    if (!effect_active[DELAY]) return input;

    // Map Parameter 1 (0.0 to 1.0) to sample length (approx 20ms to 1s)
    int delay_samples = 1000 + (int)(effect_param1[DELAY] * (MAX_DELAY_SAMPLES - 1001));
    
    // Map Parameter 2 (0.0 to 1.0) safely to feedback deep tail (capped at 0.75f to prevent loop clipping)
    float feedback = effect_param2[DELAY] * 0.75f;

    // Calculate and wrap the read pointer
    int read_ptr = write_ptr - delay_samples;
    if (read_ptr < 0) {
        read_ptr += MAX_DELAY_SAMPLES;
    }

    // Read the historical sample from the buffer
    float delayed_sample = buffer[read_ptr];

    // Mix input with feedback loop and save to buffer
    // Soft-clip or attenuate the feedback signal slightly to prevent runaway accumulation clipping
    buffer[write_ptr] = input + (delayed_sample * feedback);

    // FIX DIGITAL CLIPPING: Scale the output mix so the combined dry + wet signals fit cleanly within headroom limits
    float dry_signal = input * 0.6f;
    float wet_signal = delayed_sample * 0.4f;

    return dry_signal + wet_signal;
}


// Spring Reverb Engine Memory (Tuned delay lengths to mimic physical spring acoustics)
#define NUM_SPRING_LINES 3
#define SPRING_LINE_0_SIZE 841
#define SPRING_LINE_1_SIZE 1063
#define SPRING_LINE_2_SIZE 1373

float spring_buf_l[NUM_SPRING_LINES][1400]; // Max size bounded to fit the largest line
float spring_buf_r[NUM_SPRING_LINES][1400];
int spring_write_ptrs[NUM_SPRING_LINES] = {0, 0, 0};

float ProcessSpringReverb(float input, int channel, float size, float mix) {
    if (!effect_active[REVERB]) return input;

    // Direct pointer to the correct stereo memory matrix
    float (*matrix)[1400] = (channel == 0) ? spring_buf_l : spring_buf_r;

    // Define line length mappings using prime numbers to avoid metallic resonant ringing frequencies
    int line_sizes[NUM_SPRING_LINES] = {SPRING_LINE_0_SIZE, SPRING_LINE_1_SIZE, SPRING_LINE_2_SIZE};

    // --- 1. PARAMETER MAPPING (SPRING TANK TUNING) ---
    // Knob 1 (Size/Decay): Maps to spring reflection decay (0.4f to 0.88f)
    // Capped below 0.9f to prevent continuous, muddy feedback wash
    float decay = 0.4f + (size * 0.48f);

    // Knob 2 (Mix): Mix ratio from 100% dry up to 45% wet spring blend
    float wet_gain = mix * 0.45f;
    float dry_gain = 1.0f - (wet_gain * 0.5f); // Maintain perceived uniform volume

    // --- 2. MULTI-TANK FEEDBACK NETWORK CASCADE ---
    float tank_input = input;
    float wet_output = 0.0f;

    for (int i = 0; i < NUM_SPRING_LINES; i++) {
        int sz = line_sizes[i];
        int w_ptr = spring_write_ptrs[i];

        // Calculate cyclic read pointer for this specific spring string line
        int r_ptr = w_ptr - sz;
        if (r_ptr < 0) r_ptr += sz;

        float delayed_sample = matrix[i][r_ptr];

        // Schroeder All-Pass equation: simulates sound scattering bouncing off metal rings
        // Out = -g * In + Delay(In + g * Out)
        float ap_gain = 0.62f; // Classic spring diffusion sweet-spot coefficient
        float feedback_node = tank_input + (delayed_sample * decay);
        float ap_out = (-ap_gain * feedback_node) + matrix[i][r_ptr];

        // Store back to buffer
        matrix[i][w_ptr] = feedback_node - (ap_gain * ap_out);

        // Progress local line pointer independently (channel 0 increments, channel 1 follows)
        if (channel == 1) {
            spring_write_ptrs[i]++;
            if (spring_write_ptrs[i] >= sz) spring_write_ptrs[i] = 0;
        }

        // Accumulate and shift phase for next internal filter stage link
        wet_output += ap_out;
        tank_input = ap_out * 0.5f; // Cross-attenuate input to next spring tank layer
    }

    // --- 3. FINAL MIX BLENDING ---
    return (input * dry_gain) + ((wet_output / (float)NUM_SPRING_LINES) * wet_gain);
}









// Plate Reverb Buffer Sizes (Prime numbers to ensure smooth, non-resonant density)
#define NUM_PLATE_STAGES 4
int plate_sizes[NUM_PLATE_STAGES] = {511, 733, 1109, 1439};

float plate_buf_l[NUM_PLATE_STAGES][1500];
float plate_buf_r[NUM_PLATE_STAGES][1500];
int plate_ptrs[NUM_PLATE_STAGES] = {0, 0, 0, 0};

float ProcessReverb(float input, int channel, float size, float mix) {
    if (!effect_active[REVERB]) return input;

    float (*matrix)[1500] = (channel == 0) ? plate_buf_l : plate_buf_r;

    // Knob 1 (Size/Decay): Controls how smoothly the plate reflections merge (0.3f to 0.85f)
    float decay = 0.3f + (size * 0.55f);

    // Knob 2 (Mix): Dry/Wet Blend
    float wet_gain = mix * 0.50f;
    float dry_gain = 1.0f - (wet_gain * 0.5f);

    float current_sample = input;
    float plate_accumulator = 0.0f;

    // Pass through 4 stages of complementary all-pass loops to diffuse the sound into a smooth "sheet"
    for (int stage = 0; stage < NUM_PLATE_STAGES; stage++) {
        int sz = plate_sizes[stage];
        int w_ptr = plate_ptrs[stage];
        int r_ptr = w_ptr - sz;
        if (r_ptr < 0) r_ptr += sz;

        float delay_out = matrix[stage][r_ptr];

        // Plate Diffuser Equation: Builds instant echo density without a metallic "boing"
        float ap_gain = 0.55f; 
        float node = current_sample + (delay_out * decay);
        float ap_out = (-ap_gain * node) + delay_out;
        matrix[stage][w_ptr] = node - (ap_gain * ap_out);

        if (channel == 1) {
            plate_ptrs[stage]++;
            if (plate_ptrs[stage] >= sz) plate_ptrs[stage] = 0;
        }

        plate_accumulator += ap_out;
        current_sample = ap_out; // Cascade to the next layer
    }

    float wet_output = plate_accumulator / (float)NUM_PLATE_STAGES;
    return (input * dry_gain) + (wet_output * wet_gain);
}

// ==========================================
// MAIN AUDIO ENGINE (SEQUENTIAL CHAIN)
// ==========================================
void AudioCallback(float **in, float **out, size_t size) {
    for (size_t i = 0; i < size; i++) {
        // CORRECT
        float left = in[0][i];
        float right = in[1][i];


        // Process sequentially: Output of one feeds directly into the next
        left = ProcessOverdrive(left, 0, effect_param1[OVERDRIVE], effect_param2[OVERDRIVE]);
        left = ProcessWah(left, 0, effect_param1[WAH], effect_param2[WAH]);
        left = ProcessTremolo(left, 0, effect_param1[TREMOLO], effect_param2[TREMOLO]);
        left = ProcessPhaser(left, 0, effect_param1[PHASER], effect_param2[PHASER]);
        left = ProcessChorus(left, 0, effect_param1[CHORUS], effect_param2[CHORUS]);
        left = ProcessDelay(left, delay_buffer_l);
        left = ProcessReverb(left, 0, effect_param1[REVERB], effect_param2[REVERB]);
// ProcessSpringReverb(float input, int channel, float size, float mix) {


        right = ProcessOverdrive(right, 1, effect_param1[OVERDRIVE], effect_param2[OVERDRIVE]);
        right = ProcessWah(right, 1, effect_param1[WAH], effect_param2[WAH]);
        right = ProcessTremolo(right, 1, effect_param1[TREMOLO], effect_param2[TREMOLO]);
        right = ProcessPhaser(right, 1, effect_param1[PHASER], effect_param2[PHASER]);
        right = ProcessChorus(right, 1, effect_param1[CHORUS], effect_param2[CHORUS]);
        right = ProcessDelay(right, delay_buffer_r);
        right = ProcessReverb(right, 1, effect_param1[REVERB], effect_param2[REVERB]);

        write_ptr++;
        if (write_ptr >= MAX_DELAY_SAMPLES) {
            write_ptr = 0;
        }


        // ADD THIS: Advance chorus memory pointer
        chorus_write_ptr++;
        if (chorus_write_ptr >= CHORUS_BUFFER_SIZE) {
            chorus_write_ptr = 0;
        }

          // CORRECT
          out[0][i] = left * global_vol;
          out[1][i] = right * global_vol;

    }
}

// ==========================================
// HARDWARE INITIALIZATION & CONTROL LOGIC
// ==========================================
void setup() {
    pod = DAISY.init(DAISY_POD, AUDIO_SR_48K);

    for (int i = 0; i < MAX_DELAY_SAMPLES; i++) {
        delay_buffer_l[i] = 0.0f;
        delay_buffer_r[i] = 0.0f;
    }

    for (int i = 0; i < CHORUS_BUFFER_SIZE; i++) {
    chorus_buffer_l[i] = 0.0f;
    chorus_buffer_r[i] = 0.0f;
    }

    for (int i = 0; i < NUM_SPRING_LINES; i++) {
        for (int j = 0; j < 1400; j++) {
            spring_buf_l[i][j] = 0.0f;
            spring_buf_r[i][j] = 0.0f;
        }
    }

    DAISY.begin(AudioCallback);
}

void UpdateControls() {
    pod.DebounceControls();

    // 1. Button 1: Cycle through the selected effect index
    if (pod.buttons[0].RisingEdge()) {
        current_effect++;
        if (current_effect >= NUM_EFFECTS) {
            current_effect = OVERDRIVE;
        }
        // Unhook the knobs immediately when switching menus so they don't jump
        knob1_hooked = false;
        knob2_hooked = false;
    }

    // 2. Button 2: Toggle Bypass (Active / Deactive) for the edited effect
    if (pod.buttons[1].RisingEdge()) {
        effect_active[current_effect] = !effect_active[current_effect];
    }

    // Read current physical hardware positions (0.0 to 1.0)
    float current_knob1_phys = analogRead(PIN_POD_POT_1) / 1023.0f;
    float current_knob2_phys = analogRead(PIN_POD_POT_2) / 1023.0f;
    
    // Get the target saved parameters for the active effect
    float saved_param1 = effect_param1[current_effect];
    float saved_param2 = effect_param2[current_effect];

    // 3. Catch-Up Logic for Knob 1
    if (!knob1_hooked) {
        // Hook if physical knob crosses the saved value from below OR above
        if ((last_knob1_phys <= saved_param1 && current_knob1_phys >= saved_param1) ||
            (last_knob1_phys >= saved_param1 && current_knob1_phys <= saved_param1) ||
            abs(current_knob1_phys - saved_param1) < 0.02f) { // Or if it's already very close
            knob1_hooked = true;
        }
    }
    
    if (knob1_hooked) {
        effect_param1[current_effect] = current_knob1_phys;
    }

    // 4. Catch-Up Logic for Knob 2
    if (!knob2_hooked) {
        if ((last_knob2_phys <= saved_param2 && current_knob2_phys >= saved_param2) ||
            (last_knob2_phys >= saved_param2 && current_knob2_phys <= saved_param2) ||
            abs(current_knob2_phys - saved_param2) < 0.02f) {
            knob2_hooked = true;
        }
    }

    if (knob2_hooked) {
        effect_param2[current_effect] = current_knob2_phys;
    }

    // Save physical positions for the next frame calculation
    last_knob1_phys = current_knob1_phys;
    last_knob2_phys = current_knob2_phys;

    // 5. LED 1: Menu Color Indicators 
    float b = 0.3f; // Brightness scaling factor
    switch (current_effect) {
        case OVERDRIVE: pod.leds[0].Set(b, 0.0f, 0.0f); break; // Red
        case WAH:       pod.leds[0].Set(b, b*0.5f, 0.0f); break; // Orange
        case TREMOLO:   pod.leds[0].Set(b, b, 0.0f); break; // Yellow
        case PHASER:    pod.leds[0].Set(0.0f, b, 0.0f); break; // Green
        case CHORUS:    pod.leds[0].Set(0.0f, b, b); break; // Cyan
        case DELAY:     pod.leds[0].Set(0.0f, 0.0f, b); break; // Blue
        case REVERB:    pod.leds[0].Set(b, 0.0f, b); break; // Magenta
    }

    // 6. LED 2: Active (Green) vs Bypassed (Red) status indicator
    if (effect_active[current_effect]) {
        pod.leds[1].Set(0.0f, b, 0.0f); // Solid Green
    } else {
        pod.leds[1].Set(b, 0.0f, 0.0f); // Solid Red
    }
}

// Ensure this is completely outside of UpdateControls()
void loop() {
    UpdateControls();
    delay(10); 
}


/*
+--------------------------------------------------------------------------+

|  [ BUTTON 1 ] -> Cycles Menu Effects    [ BUTTON 2 ] -> Effect On / Off  |
|  [ LED 1 ]    -> Menu Color Indicator   [ LED 2 ]    -> Green(On)/Red(Off)|
+--------------------------------------------------------------------------+

   EFFECT LAYOUT         LED 1 COLOR     KNOB 1 ROLE        KNOB 2 ROLE
 ──────────────────────────────────────────────────────────────────────────
  1. OVERDRIVE          🔴 Red           Drive Intensity    High-End Tone Filter
  2. WAH                🟠 Orange        Filter Freq        Resonance Peak
  3. TREMOLO            🟡 Yellow        Pulse Speed        Volume Depth
  4. PHASER             🟢 Green         Sweep Rate (LFO)   Resonant Feedback
  5. CHORUS             🔵 Cyan          Modulation Speed   Delay Width
  6. DELAY              🔵 Blue          Echo Time Length   Feedback Repeats
  7. REVERB             🟣 Magenta       Room Space Size    Wet Decay Mix
  */

