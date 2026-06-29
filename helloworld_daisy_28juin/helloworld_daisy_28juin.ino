#include "DaisyDuino.h"

DaisyHardware pod;

// Two separate audio synthesis objects for independent stereo tuning
static Oscillator osc_left;
static Oscillator osc_right;

// Global control variables
float base_freq = 440.0f;
int32_t detune_hz = 0;
float global_vol = 0.25f; 

// Mute toggle flags
bool left_muted  = false;
bool right_muted = false;

// Variables for managing the blinking speeds
unsigned long last_blink_time_left = 0;
unsigned long last_blink_time_right = 0;
bool blink_state_left = true;
bool blink_state_right = true;

// Audio engine processing callback (48kHz stereo)
void AudioCallback(float **in, float **out, size_t size) {
  for (size_t i = 0; i < size; i++) {
    float sig_l = osc_left.Process();
    float sig_r = osc_right.Process();
    
    // Explicit 2D matrix layout: out[channel_index][sample_index]
    out[0][i] = left_muted  ? 0.0f : (sig_l * global_vol); // Left Headphone Pin
    out[1][i] = right_muted ? 0.0f : (sig_r * global_vol); // Right Headphone Pin
  }
}

void setup() {
  // Initialize the Pod hardware target profile
  pod = DAISY.init(DAISY_POD, AUDIO_SR_48K);
  float sample_rate = DAISY.get_samplerate();

  // Initialize Left Ear Oscillator
  osc_left.Init(sample_rate);
  osc_left.SetWaveform(Oscillator::WAVE_SIN);

  // Initialize Right Ear Oscillator
  osc_right.Init(sample_rate);
  osc_right.SetWaveform(Oscillator::WAVE_SIN);

  // Boot the underlying audio stream
  DAISY.begin(AudioCallback);
}

// Dedicated function to poll inputs and update visuals safely outside the audio thread
void UpdateControls() {
  pod.DebounceControls();

  // 1. Read Pots via isolated hardware ADC track channels
  float knob1_raw = analogRead(PIN_POD_POT_1) / 1023.0f;
  float knob2_raw = analogRead(PIN_POD_POT_2) / 1023.0f; 

  base_freq = 150.0f + (knob1_raw * 700.0f); // Knob 1 -> 150Hz to 850Hz Base Pitch
  global_vol = knob2_raw * 0.4f;             // Knob 2 -> Master volume bounded safely

  // 2. Encoder Turning Interactivity
  int32_t click_movement = pod.encoder.Increment();
  if (click_movement != 0) {
    detune_hz += click_movement; 
    detune_hz = constrain(detune_hz, -50, 50); // Boundary cap
  }
  
  // Encoder Clicking -> Instantly clears pitch difference back to 0
  if (pod.encoder.RisingEdge()) {
    detune_hz = 0;
  }

  // Calculate actual final localized pitches
  float current_freq_left = base_freq - detune_hz;
  float current_freq_right = base_freq + detune_hz;

  osc_left.SetFreq(current_freq_left);
  osc_right.SetFreq(current_freq_right);

  // 3. Button Input Mute Logic
  if (pod.buttons[0].RisingEdge()) { left_muted = !left_muted; }
  if (pod.buttons[1].RisingEdge()) { right_muted = !right_muted; }

  // 4. LED Blinking Timing Loops
  unsigned long interval_left = (unsigned long)(50000.0f / current_freq_left);
  unsigned long interval_right = (unsigned long)(50000.0f / current_freq_right);
  unsigned long current_time = millis();

  if (current_time - last_blink_time_left >= interval_left) {
    last_blink_time_left = current_time;
    blink_state_left = !blink_state_left;
  }
  if (current_time - last_blink_time_right >= interval_right) {
    last_blink_time_right = current_time;
    blink_state_right = !blink_state_right;
  }

  // 5. LED State Assignments (.Set accepts Red, Green, Blue ranges from 0.0 to 1.0)
  float brightness = global_vol / 0.4f; // Scale intensity relative to Knob 2's position

  // Left LED Configuration
  if (left_muted) {
    pod.leds[0].Set(brightness, 0.0f, 0.0f); // Solid Red
  } else {
    if (blink_state_left) {
      pod.leds[0].Set(0.0f, brightness, 0.0f); // Blinking Green
    } else {
      pod.leds[0].Set(0.0f, 0.0f, 0.0f);        // Off phase
    }
  }

  // Right LED Configuration
  if (right_muted) {
    pod.leds[1].Set(brightness, 0.0f, 0.0f); // Solid Red
  } else {
    if (blink_state_right) {
      pod.leds[1].Set(0.0f, brightness, 0.0f); // Blinking Green
    } else {
      pod.leds[1].Set(0.0f, 0.0f, 0.0f);        // Off phase
    }
  }
}

void loop() {
  UpdateControls();
  delay(10); // Prevents ADC pin cross-bleed
}
