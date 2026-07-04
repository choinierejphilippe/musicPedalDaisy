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
| 1 | BOOST         | 🟡 Yellow    | Clean Boost Gain   | Treble / Tine Bark Tilt      |
| 2 | OVERDRIVE     | 🔴 Red       | Drive Intensity    | High-End Tone Filter         |
| 3 | WAH           | 🟠 Orange    | Smart Macro Mode* | Resonance Peak Sharpness     |
| 4 | SPRING_REVERB | 🌸 Magenta   | Amp Spring Decay   | Tank Feedback Resonance      |
| 5 | PHASER        | 🟢 Green     | Sweep Rate (LFO)   | Resonant Notch Feedback      |
| 6 | CHORUS        | 🔵 Cyan      | Modulation Speed   | Delay Modulation Width       |
| 7 | DELAY         | 🔵 Blue      | Echo Time Length   | Feedback Repeat Tails        |
| 8 | PLATE_REVERB  | 🟣 Violet    | Plate Room Size    | High Diffusion Space         |
+---+───────────────+──────────────+────────────────────+──────────────────────────────+

* WAH SMART MACRO: 0-45% = LFO Speed | 45-55% = Fixed "Cocked" Wah | 55-100% = Envelope Sensitivity

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

float sample_rate_global = 48000.0f; 

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

// Daisy Oscillators for efficient LFO generation (replaces manual sinf)
Oscillator phaser_lfo_osc;
float current_phaser_lfo_osc = 0;
Oscillator wah_lfo_osc;
float current_wah_lfo_osc = 0;

// Cached parameters (computed only in UpdateControls, not every sample)
// PHASER caches
float phaser_rate_hz = 0.05f;      // Pre-computed from effect_param1[PHASER]
float phaser_feedback_val = 0.0f;  // Pre-computed from effect_param2[PHASER]

// WAH caches (for LFO mode)
float wah_lfo_rate = 0.1f;         // Pre-computed from effect_param1[WAH] when in LFO zone

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

float global_vol = 0.5f;  // Reduced for safe headroom before limiter

// Soft-knee peak limiter to prevent clipping on stacked effects
float ApplySoftLimiter(float sample) {
    // Threshold at 0.85 to leave headroom
    const float threshold = 0.85f;
    
    if (fabsf(sample) <= threshold) {
        return sample;
    }
    
    // Soft-knee limiting: reduce excess by 70%
    float excess = fabsf(sample) - threshold;
    float limited = threshold + (excess * 0.3f);
    
    if (sample < 0.0f) limited = -limited;
    return limited;
}

float ApplyDryWet(float dry, float wet, float mix) {
    return (dry * (1.0f - mix)) + (wet * mix);
}

// À placer dans votre fichier header ou au-dessus de vos fonctions de traitement
struct EnvelopeFollower {
    float env;
    float attack;
    float release;

    // Initialisation
    void Init(float sample_rate, float attack_ms, float release_ms) {
        env = 0.0f;
        // Calcul des coefficients de lissage exponentiel
        attack = expf(-1.0f / (attack_ms * 0.001f * sample_rate));
        release = expf(-1.0f / (release_ms * 0.001f * sample_rate));
    }

    // Traitement : prend les deux canaux pour une réponse mono cohérente
    float Process(float in_l, float in_r) {
        float input = (fabsf(in_l) + fabsf(in_r)) * 0.5f; // Moyenne stéréo
        float coeff = (input > env) ? attack : release;
        env = input + coeff * (env - input);
        return env;
    }
};
// Instance globale (ou à passer par référence dans vos fonctions)
EnvelopeFollower global_env;
float CurrentEnvelopeValue;


//---------------------- FX -------------------

float ProcessBoost(float input, int ch, float g, float t, float live_mix) {
    float gain = 1.0f + (g * 3.0f); 
    float boosted = input * gain;
    static float hp_state[2] = {0.0f, 0.0f};
    hp_state[ch] += 0.15f * (boosted - hp_state[ch]);
    float highs = boosted - hp_state[ch];
    float wet = boosted + (highs * t * 1.5f);
    return ApplyDryWet(input, wet * 0.75f, live_mix);
}

float ProcessOverdrive(float input, int ch, float drive, float tone, float live_mix) {
    float gain = 1.0f + (drive * 14.0f);
    float wet = tanhf(input * gain);
    static float lp_state[2] = {0.0f, 0.0f};
    float cutoff = 0.05f + (tone * 0.95f); 
    lp_state[ch] += cutoff * (wet - lp_state[ch]);
    float makeup_gain = 1.0f / sqrtf(gain);
    return ApplyDryWet(input, lp_state[ch] * makeup_gain, live_mix);
}

//(Touch-Wah + LFO combinés), Q fixe
float ProcessWah_DualEngine(float input, int ch, float freq_knob, float res_knob, float live_mix) {
    // Mémoires d'état stéréo
    static float d1[2] = {0.0f, 0.0f};
    static float d2[2] = {0.0f, 0.0f};
    static float env[2] = {0.0f, 0.0f};
    static float lfo_phase = 0.0f;

    // --- 2. LFO ENGINE ---
    // Mapping du knob 2 pour la vitesse (0.1 Hz à 4.0 Hz)
    float lfo_rate_hz = 0.1f + (res_knob * 3.9f);
    if (ch == 0) { // Mise à jour de la phase une seule fois par sample
        lfo_phase += (2.0f * PI * lfo_rate_hz) / 48000.0f;
        if (lfo_phase >= 2.0f * PI) lfo_phase -= 2.0f * PI;
    }
    // LFO normalisé entre 0.0 et 1.0
    float lfo_val = (sinf(lfo_phase) * 0.5f) + 0.5f;

    // --- 3. MODULATION MIX ---
    // On combine l'enveloppe (multipliée par le knob 1 de sensibilité) et le LFO (profondeur fixe)
    float env_mod = CurrentEnvelopeValue * (freq_knob * 12.0f); // Amplification de l'enveloppe
    float mod = env_mod + (lfo_val * 0.3f); 
    
    // Sécurité pour garder le filtre dans sa plage de fonctionnement
    if (mod > 1.0f) mod = 1.0f;
    if (mod < 0.0f) mod = 0.0f;

    // --- 4. STATE VARIABLE FILTER (SVF) ---
    // Plage de fréquences : 300 Hz à 2200 Hz
    float target_freq = 300.0f + (mod * 1900.0f);
    float f = 2.0f * sinf(PI * target_freq / 48000.0f);
    if (f > 1.0f) f = 1.0f;

    // Résonance (Q) fixe et musicale. Damping ~0.25 équivaut à un Q de ~4.0.
    float damping = 0.25f; 

    // Calcul du SVF Chamberlin
    float hp = input - d2[ch] - (damping * d1[ch]);
    float bp = (f * hp) + d1[ch];
    float lp = (f * bp) + d2[ch];
    d1[ch] = bp; 
    d2[ch] = lp;

    // Le son Wah classique est un mélange (souvent 80% Passe-Bande, 20% Passe-Bas pour le corps)
    float wet = (bp * 0.8f) + (lp * 0.2f);
    
    // --- 5. MIX DRY/WET OPTIMISÉ ---
    // On boost légèrement le wet (* 1.5f) pour compenser la perte de volume inhérente aux filtres résonants
    return (input * (1.0f - live_mix)) + (wet * live_mix * 1.5f);
}

// (Sélecteur de mode + Q ajustable) freq (Knob 1) : Sélecteur Macro. De 0 à 45% = LFO (Vitesse). De 45 à 55% = Fixe. De 55 à 100% = Enveloppe (Sensibilité).
float ProcessWah_SmartMacro(float input, int ch, float freq_knob, float res_knob, float live_mix) {
    static float d1[2] = {0.0f, 0.0f};
    static float d2[2] = {0.0f, 0.0f};
    static float env[2] = {0.0f, 0.0f};

    // --- 2. MACRO LOGIC (Bouton 1 divisé en 3 zones) ---
    float mod = 0.0f;
    
    if (freq_knob < 0.45f) {
        // ZONE LFO (0% à 45%) : Uses pre-computed wah_lfo_rate from UpdateControls
        // Daisy oscillator handles the sinf() efficiently
        float lfo_val = (current_wah_lfo_osc * 0.5f) + 0.5f;
        mod = lfo_val;
    } 
    else if (freq_knob > 0.55f) {
        // ZONE ENVELOPE (55% à 100%) : Contrôle de la sensibilité
        float sens = (freq_knob - 0.55f) / 0.45f; // Normalisé de 0.0 à 1.0
        mod = CurrentEnvelopeValue * (sens * 15.0f); 
        if (mod > 1.0f) mod = 1.0f;
    } 
    else {
        // ZONE MORTE (45% à 55%) : Cocked Wah (Filtre bloqué dans les bas-médiums)
        mod = 0.25f; 
    }

    // --- 3. STATE VARIABLE FILTER (SVF) ---
    float target_freq = 300.0f + (mod * 1900.0f);
    float f = 2.0f * sinf(PI * target_freq / 48000.0f);
    if (f > 1.0f) f = 1.0f;

    // --- 4. RÉSONANCE VARIABLE (Mappée sur le knob 2) ---
    // Q varie de ~0.5 (très plat) à ~10.0 (très pointu). Le damping va de 2.0 à 0.1.
    float damping = 2.0f - (res_knob * 1.9f); 
    // Limite de sécurité stricte pour éviter l'explosion mathématique du filtre
    if (damping < 0.05f) damping = 0.05f; 

    // Calcul du SVF
    float hp = input - d2[ch] - (damping * d1[ch]);
    float bp = (f * hp) + d1[ch];
    float lp = (f * bp) + d2[ch];
    d1[ch] = bp; 
    d2[ch] = lp;

    float wet = (bp * 0.8f) + (lp * 0.2f);
    
    // On ajoute un rattrapage de gain dynamique basé sur la résonance
    float make_up_gain = 1.0f + (res_knob * 1.5f);
    
    return (input * (1.0f - live_mix)) + (wet * live_mix * make_up_gain);
}

float ProcessWah(float input, int ch, float freq, float res, float live_mix) {
    //return ProcessWah_DualEngine(input, ch, freq, res, live_mix)    ;
    return ProcessWah_SmartMacro(input, ch, freq, res, live_mix) ;
}

float ProcessSpringReverb(float input, int ch, float size, float live_mix) {
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
    return ApplyDryWet(input, wet_accum / 3.0f, live_mix);
}

// Mémoires d'état stéréo [canal][étage]
    static float ap_state[2][4] = {{0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}};
    static float last_output[2] = {0.0f, 0.0f};

float ProcessPhaser(float input, int ch, float rate_hz, float feedback_val, float live_mix) {
              
    // Constantes d'imperfection vintage (±4%)
    //const float imperfections[4] = {0.97f, 1.04f, 0.99f, 1.02f};
const float imperfections[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    // LFO output from Daisy oscillator (efficient vs manual sinf)
    float lfo_sine = (current_phaser_lfo_osc * 0.5f) + 0.5f;

    // --- 2. SWEEP RANGE (Logarithmique) ---
    // Plage: 150 Hz à 3000 Hz (3000 / 150 = 20.0)
   // float f_target = 150.0f * powf(20.0f, lfo_sine);
   // float f_target = 150.0f + (lfo_sine * 2200.0f); // Linéaire au lieu de powf (plus rapide)
    float f_target = 150.0f + (rate_hz * 2200.0f); // Linéaire au lieu de powf (plus rapide)

    // Clamp feedback to prevent runaway accumulation
    //float fb_safe = constrain(feedback_val, -0.85f, 0.85f);

    static float feedback_filter[2] = {0.0f, 0.0f};
    float fb_signal = last_output[ch] * feedback_val;

    // Passe-bas simple sur le feedback (évite l'instabilité dans les hautes)
    feedback_filter[ch] = (fb_signal * 0.2f) + (feedback_filter[ch] * 0.8f);

    float current = input +  feedback_filter[ch];
    //current = SoftClip(current);

    // --- 4. LES 4 ÉTAGES ALL-PASS ---
    for (int stage = 0; stage < 4; stage++) {
        // Ajout de l'imperfection analogique, clamped
        float f_stage = constrain(f_target * imperfections[stage], 100.0f, 3000.0f);
        
        // Calcul du coefficient de filtre bilinéaire
        //float omega = PI * f_stage / 48000.0f;
        float w = tanf( PI * f_stage / sample_rate_global);
        //float g = (1.0f - tanf(omega)) / (1.0f + tanf(omega));
        //float g = (1.0f - omega) / (1.0f + omega);
        // FIX 3: Coefficient all-pass stable (1er ordre)
        // g = (1 - w/2) / (1 + w/2) est une approximation standard
        float g = (1.0f - w) / (1.0f + w);

        // Application du filtre passe-tout with normalization
        //float ap_out = (g * current) + ap_state[ch][stage];
        //ap_state[ch][stage] = current - (g * ap_out);
        //current = ap_out;

        // All-pass stable (Formule de Schroeder)
        float ap_in = current;
        float ap_out = (g * ap_in) + ap_state[ch][stage];
        ap_state[ch][stage] = ap_in - (g * ap_out);
        current = ap_out;
        //current = SoftClip(ap_out); // On avance l'étage suivant avec la sortie filtrée
    }

    // Mémorisation pour la boucle de feedback (with safety clipping)
    last_output[ch] = current;

    // --- 5. MIXAGE PHASER & WET/DRY ---
    // Le son "Phaser" authentique est créé en additionnant le Dry et le signal déphasé (50/50).
    //float phaser_wet_signal = (input * 0.5f) + (current * 0.5f);
    float phaser_wet_signal = (input - current) * 0.5f;

    // L'encodeur gère l'intensité globale via ApplyDryWet
    return ApplyDryWet(input, phaser_wet_signal, live_mix);
}

float ProcessChorus(float input, int ch, float rate, float depth, float live_mix) {
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
    return ApplyDryWet(input, wet, live_mix);
}

float ProcessDelay(float input, float *buffer, float live_mix) {
    int d_samples = 1000 + (int)(effect_param1[DELAY] * (MAX_DELAY_SAMPLES - 1001));
    float feedback = effect_param2[DELAY] * 0.75f;
    int r_ptr = write_ptr - d_samples;
    if (r_ptr < 0) r_ptr += MAX_DELAY_SAMPLES;
    float delayed = buffer[r_ptr];
    buffer[write_ptr] = input + (delayed * feedback);
    return ApplyDryWet(input, delayed, live_mix);
}

float ProcessPlateReverb(float input, int ch, float size, float live_mix) {
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
    return ApplyDryWet(input, accumulator / 4.0f, live_mix);
}


void AudioCallback(float **in, float **out, size_t size) {
    //pod.ProcessAllControls();


    for (size_t i = 0; i < size; i++) {
        // FIX: Extract individual channel samples explicitly [channel][sample_index]
        float left = in[0][i]; 
        float right = in[1][i];
        
        // Envelope follower callback
        CurrentEnvelopeValue = global_env.Process(left, right);
        current_phaser_lfo_osc = phaser_lfo_osc.Process();
        current_wah_lfo_osc = wah_lfo_osc.Process();

        // --- LEFT RIG INS-CHANNEL FLOW (only process active effects) ---
        if (effect_active[BOOST]) {
            left = ProcessBoost(left, 0, effect_param1[BOOST], effect_param2[BOOST], effect_mix[BOOST]);
        }
        if (effect_active[OVERDRIVE]) {
            left = ProcessOverdrive(left, 0, effect_param1[OVERDRIVE], effect_param2[OVERDRIVE], effect_mix[OVERDRIVE]);
        }
        if (effect_active[WAH]) {
            left = ProcessWah(left, 0, effect_param1[WAH], effect_param2[WAH], effect_mix[WAH]);
        }
        if (effect_active[SPRING_REVERB]) {
            left = ProcessSpringReverb(left, 0, effect_param1[SPRING_REVERB], effect_mix[SPRING_REVERB]);
        }
        if (effect_active[PHASER]) {
            left = ProcessPhaser(left, 0, effect_param1[PHASER],phaser_feedback_val, effect_mix[PHASER]);
        }
        if (effect_active[CHORUS]) {
            left = ProcessChorus(left, 0, effect_param1[CHORUS], effect_param2[CHORUS], effect_mix[CHORUS]);
        }
        if (effect_active[DELAY]) {
            left = ProcessDelay(left, delay_buffer_l, effect_mix[DELAY]);
        }
        if (effect_active[PLATE_REVERB]) {
            left = ProcessPlateReverb(left, 0, effect_param1[PLATE_REVERB], effect_mix[PLATE_REVERB]);
        }

        // --- RIGHT RIG INS-CHANNEL FLOW (only process active effects) ---
        if (effect_active[BOOST]) {
            right = ProcessBoost(right, 1, effect_param1[BOOST], effect_param2[BOOST], effect_mix[BOOST]);
        }
        if (effect_active[OVERDRIVE]) {
            right = ProcessOverdrive(right, 1, effect_param1[OVERDRIVE], effect_param2[OVERDRIVE], effect_mix[OVERDRIVE]);
        }
        if (effect_active[WAH]) {
            right = ProcessWah(right, 1, effect_param1[WAH], effect_param2[WAH], effect_mix[WAH]);
        }
        if (effect_active[SPRING_REVERB]) {
            right = ProcessSpringReverb(right, 1, effect_param1[SPRING_REVERB], effect_mix[SPRING_REVERB]);
        }
        if (effect_active[PHASER]) {
            //right = ProcessPhaser(right, 1, phaser_rate_hz, phaser_feedback_val, effect_mix[PHASER]);
            right = ProcessPhaser(right, 1,effect_param1[PHASER], phaser_feedback_val, effect_mix[PHASER]);
        }
        if (effect_active[CHORUS]) {
            right = ProcessChorus(right, 1, effect_param1[CHORUS], effect_param2[CHORUS], effect_mix[CHORUS]);
        }
        if (effect_active[DELAY]) {
            right = ProcessDelay(right, delay_buffer_r, effect_mix[DELAY]);
        }
        if (effect_active[PLATE_REVERB]) {
            right = ProcessPlateReverb(right, 1, effect_param1[PLATE_REVERB], effect_mix[PLATE_REVERB]);
        }

        write_ptr++; 
        if (write_ptr >= MAX_DELAY_SAMPLES) write_ptr = 0;
        chorus_write_ptr++; 
        if (chorus_write_ptr >= CHORUS_BUFFER_SIZE) chorus_write_ptr = 0;

        // Apply soft-knee limiter to prevent clipping from stacked effects
        left = ApplySoftLimiter(left* global_vol);
        right = ApplySoftLimiter(right* global_vol);
        
        // FIX: Route scalar sample variables cleanly back out to 2D channel outputs
        out[0][i] = left ; 
        out[1][i] = right ;
    }
}


void setup() {
    pod = DAISY.init(DAISY_POD, AUDIO_SR_48K);

    // --- INITIALISATION DU SUIVEUR D'ENVELOPPE ---
    // On utilise la fréquence d'échantillonnage définie par DAISY.init
    // 10ms pour l'attaque, 100ms pour le relâchement
    //Mon conseil : Commencez avec 10/100. Si vous trouvez que votre pédale manque de "punch" sur les attaques,
    //réduisez l'attaque vers 5 ms. Si vous trouvez que l'effet est trop instable ou "nerveux", augmentez le relâchement vers 150 ms ou 200 ms.
    global_env.Init(AUDIO_SR_48K, 10.0f, 100.0f);
    
    // Initialize Daisy oscillators for LFOs
    phaser_lfo_osc.Init(AUDIO_SR_48K);
    phaser_lfo_osc.SetWaveform(Oscillator::WAVE_SIN);
    phaser_lfo_osc.SetFreq(0.5f);
    phaser_lfo_osc.SetAmp(1.0f);
    // Exemple : Décalage de phase pour effet stéréo
    //phaser_lfo_osc_left.SetPhase(0.0f);
    //phaser_lfo_osc_right.SetPhase(0.25f); // Décalage de 90 degrés
    
    wah_lfo_osc.Init(AUDIO_SR_48K);
    wah_lfo_osc.SetWaveform(Oscillator::WAVE_SIN);
    wah_lfo_osc.SetFreq(wah_lfo_rate);

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
    //pod.ProcessAnalogControls(); 
    pod.ProcessAllControls();
    //pod.DebounceControls(); // FIX: This single method automatically updates the encoder under the hood.

    // 3-Second Dual Hold Master Reset
    if (pod.buttons[0].Pressed() && pod.buttons[1].Pressed()) {
        if (!dual_hold_active) {
            dual_hold_active = true; 
            buttons_held_start_time = millis();
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

    // Button 1: Cycle effect (RisingEdge detects press) - BEFORE reset flash
    if (!dual_hold_active) {
        if (pod.buttons[0].RisingEdge()) {
            current_effect++; 
            if (current_effect >= NUM_EFFECTS) current_effect = BOOST;
            knob1_hooked = false; 
            knob2_hooked = false;
        }
        if (pod.buttons[1].RisingEdge()) {
            effect_active[current_effect] = !effect_active[current_effect];
            if (effect_active[current_effect]) {
                knob1_hooked = true; 
                knob2_hooked = true;
            }
        }
    }

    // Reset flash display (return won't block buttons - they already ran)
    if (is_flashing_reset) {
        if (millis() - reset_flash_timer < 300) {
            pod.leds[0].Set(0.5f, 0.0f, 0.0f); 
            pod.leds[1].Set(0.5f, 0.0f, 0.0f); 
            return;
        } else { 
            is_flashing_reset = false; 
        }
    }

    // FIX 2: Increased step size to 0.05f (5% per click). 
    // Since an encoder notch returns a raw integer 1 or -1, this requires exactly 20 notch turns 
    // to sweep your audio mix cleanly across the entire gamut without feeling sluggish or dropping ticks.
    int32_t enc_move = pod.encoder.Increment();
    if (enc_move != 0) {
        effect_mix[current_effect] += (enc_move * 0.05f); 
        effect_mix[current_effect] = constrain(effect_mix[current_effect], 0.0f, 1.0f);
    }

    //float cur_k1 = analogRead(PIN_POD_POT_1) / 1023.0f;
    // my_freq.Init(pod.knob1, 55.0f, 880.0f, Parameter::LOGARITHMIC);
    float cur_k1 = pod.GetKnobValue(0);
    float cur_k2 = pod.GetKnobValue(1);
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

    // === OPTIMIZATION: Cache expensive parameter mappings (only computed on knob change) ===
    // PHASER parameter caching (replaces per-sample powf calculations)
    if (current_effect == PHASER && knob1_hooked) {
        // Rate knob: 0.05 Hz to 10.0 Hz exponential mapping 
        //float phaser_rate_hz = f_min * exp2f(val * log2f(f_max / f_min));
        //phaser_rate_hz = 0.05f * powf(200.0f, effect_param1[PHASER]);  // $0.05 \times 200^1 = 10.0 \text{ Hz}$
        //7.64 =  log2f(10.0f / 0.05)
        //phaser_rate_hz = 0.05f * exp2f(effect_param1[PHASER] * 7.64f);
        phaser_rate_hz = 0.05f + effect_param1[PHASER] * 10.0f;
        phaser_lfo_osc.SetFreq(phaser_rate_hz);
    }
    if (current_effect == PHASER && knob2_hooked) {
        // Feedback knob: cubic bipolar mapping
        phaser_feedback_val = 7.9f * powf(effect_param2[PHASER] - 0.5f, 3.0f);
    }

    // WAH parameter caching (LFO mode only)
    if (current_effect == WAH && knob1_hooked && effect_param1[WAH] < 0.45f) {
        // LFO rate: 0.1 Hz to 4.0 Hz when in LFO zone (0-45%)
        wah_lfo_rate = 0.1f + ((effect_param1[WAH] / 0.45f) * 3.9f);
        wah_lfo_osc.SetFreq(wah_lfo_rate);
    }

   // LED 1 Menu Color Indicators - Luminosité fixe à 0.7
    // Skip normal LED updates during reset flash
    if (!is_flashing_reset) {
        float b = 0.7f; 
        
        switch (current_effect) {
            // Couleurs franches pour un contraste maximal sous les projecteurs
            case BOOST:         pod.leds[0].Set(b, b * 0.8f, 0.0f); break; // Jaune orangé
            case OVERDRIVE:     pod.leds[0].Set(b, 0.0f, 0.0f);     break; // Rouge pur
            case WAH:           pod.leds[0].Set(b, b * 0.5f, 0.0f); break; // Orange vif
            case SPRING_REVERB: pod.leds[0].Set(b, 0.0f, b * 0.5f); break; // Magenta
            case PHASER:        pod.leds[0].Set(0.0f, b, 0.0f);     break; // Vert pur
            case CHORUS:        pod.leds[0].Set(0.0f, b, b);        break; // Cyan
            case DELAY:         pod.leds[0].Set(0.0f, 0.0f, b);     break; // Bleu pur
            case PLATE_REVERB:  pod.leds[0].Set(b * 0.8f, 0.0f, b); break; // Violet
        }

           // LED 2 Discrete 10-Step Visual Feedback
        if (!effect_active[current_effect]) {
            pod.leds[1].Set(b, 0.0f, 0.0f); // Solid Red ALWAYS means bypassed
        } else {
            int step = (int)(effect_mix[current_effect] * 10.0f + 0.5f);
            
        switch(step) {
            case 0:  pod.leds[1].Set(0.0f, 0.0f, b);     break; //   0% Mix: Pure Blue
            case 1:  pod.leds[1].Set(b, b * 0.4f, 0.0f); break; //  10% Mix: Vibrant Orange (Jumps far from Blue)
            case 2:  pod.leds[1].Set(0.0f, b, b);        break; //  20% Mix: Electric Cyan
            case 3:  pod.leds[1].Set(b, 0.0f, b * 0.5f); break; //  30% Mix: Bright Magenta/Pink
            case 4:  pod.leds[1].Set(0.0f, b, 0.0f);     break; //  40% Mix: Pure Green
            case 5:  pod.leds[1].Set(b, b, 0.0f);        break; //  50% Mix: Bright Yellow
            case 6:  pod.leds[1].Set(0.0f, b * 0.3f, b); break; //  60% Mix: Deep Sapphire Blue
            case 7:  pod.leds[1].Set(b, 0.0f, b);        break; //  70% Mix: Intense Purple
            case 8:  pod.leds[1].Set(b * 0.3f, b, 0.0f); break; //  80% Mix: Lime Green
            case 9:  pod.leds[1].Set(b, b * 0.2f, b);    break; //  90% Mix: Pastel Rose
            case 10: pod.leds[1].Set(b, b, b);           break; // 100% Mix: Pure White (All channels maxed)
            default: pod.leds[1].Set(0.0f, 0.0f, b);     break; // Default fallback
            }
        }
    }

}

void loop() {
    UpdateControls();
    delay(5); 
}
