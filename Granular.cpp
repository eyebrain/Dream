
// <3 Dream
// Moved Granular from seed to patch folder
#include "../../libDaisy/src/daisy_patch.h" // DaisyPatch, patch hardware
#include "../../DaisySP/Source/daisysp.h"
#include "../../DaisySP/Source/Filters/ladder.h"
#include "../../DaisySP/Source/Filters/svf.h"
#include "../../DaisySP/Source/Effects/decimator.h"
#include "../../DaisySP/Source/Effects/wavefolder.h"
#include "../../DaisySP/Source/Effects/chorus.h"
#include <cstdio>
#include <cstring>
#include "../../field/sampler/samplebuffer.h"
#include "daisy_seed.h" // System, DacHandle
#include "dev/sdram.h"   // DSY_SDRAM_BSS

#include <cstdint> // for uint32_t
#include "../../libDaisy/src/per/qspi.h" // QSPI for settings persistence
#include "../../libDaisy/src/util/oled_fonts.h" // FontDef, Font_7x10, Font_6x8, Font_4x8

using namespace daisy;
#include "../../libDaisy/src/util/oled_fonts.h" // FontDef, Font_7x10, Font_6x8, Font_4x8

using namespace daisy;
using namespace daisysp;

DaisyPatch patch;

// MIDI handling - support both USB and physical MIDI
MidiUsbHandler midi_usb;
MidiUartHandler midi_uart;

// Indicates whether buffers have been initialized and audio callback can access them
volatile bool buffers_ready = false;

// UI state
bool menu_open   = false; // when true, we are inside the Ops menu
int  menu_idx    = 0;     // index for menu selection
float blend_amount = 0.8f; // 0.8 = 80% wet, 0.2 = dry
bool blend_locked = false;

// Audio gain and blend variables
int audio_gain = 100; // percent, 1-100
int audio_gain_pending = 100;
bool audio_gain_editing = false;
bool audio_gain_locked = false;
int audio_blend = 80; // percent, 0-100
int audio_blend_pending = 80;
bool audio_blend_editing = false;
bool audio_blend_locked = false;

// State for back arrow confirmation
bool menu_back_confirm = false;

// Menu placeholders

// Main menu items (no Main Buf length, no Freeze Length)
const char* menu_items[] = {
    "Buffer Options",
    "Audio",
    "DSP options",
    "Gran Midi",
    "CV Ops",
    "Back"
};
const int menu_items_count = sizeof(menu_items) / sizeof(menu_items[0]);

// DSP submenu
const char* dsp_menu_items[] = {
    "Ladder Filter",
    "Soap Filter", 
    "Decimator",
    "Wavefolder",
    "Chorus",
    "DSP Mode",
    "Back"
};
const int dsp_menu_items_count = sizeof(dsp_menu_items) / sizeof(dsp_menu_items[0]);

// DSP effect submenus - now for encoder-based parameter editing
const char* dsp_effect_submenu_items[] = {
    "Param 1: EDIT",
    "Param 2: EDIT",
    "Param 3: EDIT",
    "Wet/Dry",
    "On/Off",
    "Ch In (1-2-3)",
    "MIDI Ch (1-16)",
    "Back"
};
const int dsp_effect_submenu_items_count = sizeof(dsp_effect_submenu_items) / sizeof(dsp_effect_submenu_items[0]);

// CV Ops submenu
const char* cv_ops_menu_items[] = {
    "Scale: Blues",
    "Reese Mode: OFF",
    "CV 1: ON",
    "CV1 MIDI Ch: 2",
    "CV2 MIDI Ch: 3",
    "CV Hold: On",
    "Back"
};
const int cv_ops_menu_items_count = sizeof(cv_ops_menu_items) / sizeof(cv_ops_menu_items[0]);

// Clock settings state
bool clock_submenu_open = false;
int clock_menu_idx = 0;
float audio_clock_threshold_setting = 0.1f; // user adjustable threshold
int clock_source = 0; // 0=auto, 1=audio, 2=gate, 3=MIDI

// Filter knob values (stub)
float filter_hp = 0.0f;
float filter_lp = 1.0f;
float filter_freq = 1000.0f;
float filter_cutoff = 1000.0f;

// Track which DSP effect submenu is open and its index
bool dsp_effect_submenu_open = false;
int dsp_effect_idx = 0; // which effect submenu
int dsp_effect_submenu_idx = 0;
int selected_parameter = -1; // -1 = none selected, 0-3 = param selected for editing

// Audio submenu
const char* audio_menu_items[] = {
    "Gain",
    "Blend", 
    "Sample rate",
    "MIDI Enable",
    "MIDI Type",
    "Clock Settings",
    "Back"
};
const int audio_menu_items_count = sizeof(audio_menu_items) / sizeof(audio_menu_items[0]);

// Clock settings submenu
const char* clock_menu_items[] = {
    "Audio Clock Thresh",
    "Clock Source", 
    "BPM Display",
    "Back"
};
const int clock_menu_items_count = sizeof(clock_menu_items) / sizeof(clock_menu_items[0]);

// DSP mode and effects
bool dsp_mode_active = false; // when true, knobs control DSP effects instead of granular
int current_dsp_effect = 0; // which DSP effect is active (0-4 for the 5 effects)

// CV1 mode selection
bool gran_midi_enabled = false; // if true, MIDI affects granular also, if false, MIDI only affects DSP effects

// DSP effect instances
daisysp::Svf ladder_filter; // Using SVF for ladder filter effect
daisysp::Svf soap_filter; // State Variable Filter for soap filter
daisysp::Decimator decimator;
daisysp::Wavefolder wavefolder;
daisysp::Chorus chorus;

// DSP effect parameters (4 per effect)
struct DSPEffectParams {
    float param1, param2, param3, param4;
    bool locked1, locked2, locked3, locked4;
    int midi_channel; // 1-16, 0 = off
    int input_channel; // 1-3=Audio In 1-3 (In 4 reserved for clock)
    bool enabled;
};

DSPEffectParams dsp_effects[5]; // 0=Ladder, 1=Soap, 2=Decimator, 3=Wavefolder, 4=Chorus

// Update DSP effect parameters based on knob values
void UpdateDSPEffectParameters(int effect_idx) {
    DSPEffectParams& effect = dsp_effects[effect_idx];
    
    // Apply x2 multiplier to parameter values for extended range
    float p1 = effect.param1 * 2.0f;
    float p2 = effect.param2 * 2.0f;
    float p3 = effect.param3 * 2.0f;
    float p4 = effect.param4 * 2.0f;
    
    switch(effect_idx) {
        case 0: // Ladder Filter (using SVF lowpass)
            ladder_filter.SetFreq(p1 * 20000.0f + 20.0f); // 20Hz - 40kHz (doubled range)
            ladder_filter.SetRes(p2 * 0.45f); // 0 - 0.9 resonance (adjusted for x2)
            ladder_filter.SetDrive(p3 * 0.45f + 0.1f); // 0.1 - 1.0 drive (adjusted for x2)
            // param4 not used for ladder filter
            break;
            
        case 1: // Soap Filter (SVF)
            soap_filter.SetFreq(p1 * 20000.0f + 20.0f); // 20Hz - 40kHz (doubled range)
            soap_filter.SetRes(p2 * 0.45f); // 0 - 0.9 resonance (adjusted for x2)
            soap_filter.SetDrive(p3 * 0.45f + 0.1f); // 0.1 - 1.0 drive (adjusted for x2)
            // param4 could control filter type, but keeping simple for now
            break;
            
        case 2: // Decimator
            decimator.SetDownsampleFactor(p1 * 0.45f + 0.1f); // 0.1-1.0 downsample (adjusted for x2)
            decimator.SetBitcrushFactor(p2); // 0-2.0 bitcrush (doubled range)
            // param3 and param4 not used
            break;
            
        case 3: // Wavefolder
            wavefolder.SetGain(p1 * 0.45f + 0.1f); // 0.1-1.0 gain (adjusted for x2)
            wavefolder.SetOffset(p2 * 0.45f + 0.1f); // 0.1-1.0 offset (adjusted for x2)
            // param3 and param4 not used
            break;
            
        case 4: // Chorus
            chorus.SetDelay(p1); // 0-2.0 delay amount (doubled range)
            chorus.SetFeedback(p2); // 0-2.0 feedback (doubled range)
            chorus.SetLfoFreq(p3 * 8.0f + 0.1f); // 0.1-16.1 Hz LFO (doubled range)
            chorus.SetLfoDepth(p4); // 0-2.0 LFO depth (doubled range)
            break;
    }
}

// Buffer Options submenu
const char* buffer_menu_items[] = {
    "Buffer size",
    "Frez Buff",
    "Back"
};
const int buffer_menu_items_count = sizeof(buffer_menu_items) / sizeof(buffer_menu_items[0]);


// Buffer configuration (moved up to ensure macros are defined before use)
#define SAMPLE_RATE 44100
#define BUFFER_A_SECONDS 5  // Default buffer size, max 20 seconds
#define BUFFER_B_SECONDS_NUM 13
#define BUFFER_B_SECONDS_DEN 10
#define BUFFER_A_SIZE (SAMPLE_RATE * BUFFER_A_SECONDS)
#define BUFFER_B_SIZE \
    (SAMPLE_RATE * BUFFER_B_SECONDS_NUM / BUFFER_B_SECONDS_DEN)

// Buffer size state
int buffer_size_seconds = BUFFER_A_SECONDS; // default
int buffer_size_pending = BUFFER_A_SECONDS;
bool buffer_size_editing = false;
bool buffer_size_locked = false;

// Freeze buffer state
float freeze_buffer_size = 0.5f; // default in seconds
float freeze_buffer_pending = 1.5f;
bool freeze_buffer_editing = false;
bool freeze_buffer_locked = false;

// Track which menu is open
enum MenuLevel { MAIN_MENU, BUFFER_MENU, AUDIO_MENU, DSP_MENU, CV_OPS_MENU };
MenuLevel menu_level = MAIN_MENU;
int buffer_menu_idx = 0;
int audio_menu_idx = 0;
int dsp_menu_idx = 0;
int cv_ops_menu_idx = 0;

// CV Hold mode enum (needs to be before struct for struct to use it)
enum CvHoldMode { CV_HOLD_OFF, CV_HOLD_CV1, CV_HOLD_CV2, CV_HOLD_BOTH };

// Settings persistence structure
struct PersistentSettings {
    // DSP Effects settings
    DSPEffectParams dsp_effects[5];
    
    // Audio settings
    int audio_gain;
    int audio_blend;
    bool audio_gain_locked;
    bool audio_blend_locked;
    
    // Buffer settings
    int buffer_size_seconds;
    float freeze_buffer_size;
    bool buffer_size_locked;
    bool freeze_buffer_locked;
    
    // Blend settings
    bool blend_locked;
    
    // Granular MIDI control setting
    bool gran_midi_enabled;
    
    // CV Operations settings
    int cv_scale;           // 0=Blues, 1=Minor, 2=Major, 3=Dorian, 4=Phrygian, 5=Lydian, 6=Mixolydian, 7=Locrian, 8=Chromatic
    bool reese_mode;        // true = Reese mode (CV1 outputs note one scale degree lower than CV2)
    bool cv1_enabled;       // true = CV1 enabled, false = CV1 outputs 0V
    int cv1_midi_channel;   // MIDI channel for CV1 operations (0=any, 1-16=specific channel)
    int cv2_midi_channel;   // MIDI channel for CV2 operations (0=any, 1-16=specific channel)
    CvHoldMode cv_hold_mode; // Combined hold mode: 0=Off, 1=CV1, 2=CV2, 3=Both
    
    // Magic number for validation
    uint32_t magic = 0x4752414E; // "GRAN" in ASCII
    uint32_t version = 2;
};

// QSPI settings storage
// daisy::QSPIHandle qspi;  // Use patch.seed.qspi instead
const uint32_t SETTINGS_ADDRESS = 0; // Start of QSPI flash
const uint32_t SETTINGS_SIZE = sizeof(PersistentSettings);

// Global settings instance
PersistentSettings settings;

// MIDI state
bool midi_enabled = false;
bool midi_use_usb = true; // true = USB MIDI, false = physical MIDI IN
float midi_pitch_bend = 1.0f; // pitch bend multiplier
bool midi_note_active = false;
float midi_note_freq = 440.0f;
uint8_t midi_velocity = 0;
uint8_t midi_note_number = 60; // current MIDI note number (0-127)
uint8_t midi_channel = 1; // current MIDI channel (1-16, 0=any)

// CV-specific MIDI state (filtered by individual channels)
bool cv1_midi_note_active = false;
uint8_t cv1_midi_note_number = 60;
bool cv2_midi_note_active = false;
uint8_t cv2_midi_note_number = 60;

// MIDI CC mappings
float midi_cc_values[128] = {0}; // store CC values 0-127

// CV Operations state
enum ScaleType { SCALE_BLUES, SCALE_MINOR, SCALE_MAJOR, SCALE_PENTATONIC, SCALE_WHOLE, SCALE_LOCRIAN, SCALE_NATURAL, SCALE_COUNT };
ScaleType cv_scale = SCALE_BLUES; // default to Blues scale
bool reese_mode = false; // Reese mode off by default
bool cv1_enabled = true; // CV1 enabled by default
uint8_t cv1_midi_channel = 2; // CV1 MIDI channel (2-5, 0=any)
uint8_t cv2_midi_channel = 3; // CV2 MIDI channel (2-5, 0=any)

// CV Hold mode: 0=Off, 1=CV1 only, 2=CV2 only, 3=Both
CvHoldMode cv_hold_mode = CV_HOLD_BOTH; // default to both on

// CV hold state - last voltages when hold is enabled
float cv1_last_volt = 0.0f;
float cv2_last_volt = 0.0f;

// Scale definitions (semitones from root)
const int BLUES_SCALE[] = {0, 3, 5, 6, 7, 10}; // Blues scale
const int MINOR_SCALE[] = {0, 2, 3, 5, 7, 8, 10}; // Natural minor
const int MAJOR_SCALE[] = {0, 2, 4, 5, 7, 9, 11}; // Major
const int PENTATONIC_SCALE[] = {0, 2, 4, 7, 9}; // Major pentatonic
const int WHOLE_SCALE[] = {0, 2, 4, 6, 8, 10}; // Whole tone
const int LOCRIAN_SCALE[] = {0, 1, 3, 5, 6, 8, 10}; // Locrian
const int NATURAL_SCALE[] = {0, 2, 4, 5, 7, 9, 11}; // Natural (same as major for now)

const int* SCALE_DEFINITIONS[] = {BLUES_SCALE, MINOR_SCALE, MAJOR_SCALE, PENTATONIC_SCALE, WHOLE_SCALE, LOCRIAN_SCALE, NATURAL_SCALE};
const int SCALE_SIZES[] = {6, 7, 7, 5, 6, 7, 7};
const char* SCALE_NAMES[] = {"Blues", "Minor", "Major", "Pentatonic", "Whole", "Locrian", "Natural"};

// Quantize voltage to nearest scale note
float QuantizeToScale(float voltage, ScaleType scale) {
    if (scale >= SCALE_COUNT) return voltage;
    
    // Convert voltage (-5 to +5) to semitones (0-120 for 10 octaves)
    float semitones = (voltage + 5.0f) * 12.0f;
    
    // Find octave and note within octave
    int octave = (int)(semitones / 12.0f);
    float note_in_octave = fmodf(semitones, 12.0f);
    
    // Find nearest scale note
    const int* scale_def = SCALE_DEFINITIONS[scale];
    int scale_size = SCALE_SIZES[scale];
    
    int nearest_note = 0;
    float min_distance = 12.0f;
    
    for (int i = 0; i < scale_size; i++) {
        float distance = fabsf(note_in_octave - scale_def[i]);
        if (distance < min_distance) {
            min_distance = distance;
            nearest_note = scale_def[i];
        }
    }
    
    // Convert back to voltage
    float quantized_semitones = octave * 12.0f + nearest_note;
    return (quantized_semitones / 12.0f) - 5.0f;
}

// Clock detection and sync
bool external_clock_detected = false;
bool external_audio_clock_active = false;
bool external_gate_clock_active = false;
float audio_clock_threshold = 0.1f; // threshold for detecting audio clock pulses
uint32_t last_audio_clock_pulse = 0;
uint32_t last_gate_clock_pulse = 0;
uint32_t gate_pulse_end_time = 0; // when to turn off gate output
float detected_bpm = 120.0f; // detected BPM from external clock
uint32_t clock_pulse_interval = 0; // samples between clock pulses
uint32_t clock_pulse_counter = 0;

// Randomization state
float randomization_amt = 0.0f; // 0 = off, >0 = on
uint32_t last_randomize = 0;
float playhead_within_window = 0.0f; // 0..1, updated per grain

// Settings persistence functions
bool LoadSettings() {
    // Read settings from QSPI
    uint8_t* qspi_data = (uint8_t*)patch.seed.qspi.GetData(SETTINGS_ADDRESS);
    memcpy(&settings, qspi_data, SETTINGS_SIZE);
    
    // Validate settings
    if(settings.magic != 0x4752414E || settings.version != 2) {
        return false; // Invalid settings, use defaults
    }
    
    // Apply loaded settings
    memcpy(dsp_effects, settings.dsp_effects, sizeof(dsp_effects));
    audio_gain = settings.audio_gain;
    audio_blend = settings.audio_blend;
    audio_gain_locked = settings.audio_gain_locked;
    audio_blend_locked = settings.audio_blend_locked;
    buffer_size_seconds = settings.buffer_size_seconds;
    freeze_buffer_size = settings.freeze_buffer_size;
    buffer_size_locked = settings.buffer_size_locked;
    freeze_buffer_locked = settings.freeze_buffer_locked;
    blend_locked = settings.blend_locked;
    gran_midi_enabled = settings.gran_midi_enabled;
    cv_scale = (ScaleType)settings.cv_scale;
    reese_mode = settings.reese_mode;
    cv1_enabled = settings.cv1_enabled;
    cv1_midi_channel = settings.cv1_midi_channel;
    cv2_midi_channel = settings.cv2_midi_channel;
    cv_hold_mode = settings.cv_hold_mode;
    
    return true;
}

void SaveSettings() {
    // Update settings structure with current values
    memcpy(settings.dsp_effects, dsp_effects, sizeof(dsp_effects));
    settings.audio_gain = audio_gain;
    settings.audio_blend = audio_blend;
    settings.audio_gain_locked = audio_gain_locked;
    settings.audio_blend_locked = audio_blend_locked;
    settings.buffer_size_seconds = buffer_size_seconds;
    settings.freeze_buffer_size = freeze_buffer_size;
    settings.buffer_size_locked = buffer_size_locked;
    settings.freeze_buffer_locked = freeze_buffer_locked;
    settings.blend_locked = blend_locked;
    settings.gran_midi_enabled = gran_midi_enabled;
    settings.cv_scale = cv_scale;
    settings.reese_mode = reese_mode;
    settings.cv1_enabled = cv1_enabled;
    settings.cv1_midi_channel = cv1_midi_channel;
    settings.cv2_midi_channel = cv2_midi_channel;
    settings.cv_hold_mode = cv_hold_mode;
    
    // Erase sector and write settings to QSPI
    patch.seed.qspi.EraseSector(SETTINGS_ADDRESS);
    patch.seed.qspi.Write(SETTINGS_ADDRESS, SETTINGS_SIZE, (uint8_t*)&settings);
}

// MIDI message handler
void HandleMidiMessage(MidiEvent m)
{
    switch(m.type)
    {
        case NoteOn:
        {
            // Use MIDI note to set grain pitch
            if(m.data[1] > 0) // velocity > 0
            {
                midi_note_active = true;
                midi_velocity = m.data[1];
                midi_note_number = m.data[0];
                midi_channel = m.channel + 1; // MIDI channels are 0-15, we store 1-16
                // Convert MIDI note to frequency (A4 = 69 = 440Hz)
                float note = (float)m.data[0] - 69.0f;
                midi_note_freq = 440.0f * powf(2.0f, note / 12.0f);
                
                // CV operations: check both CV1 and CV2 channels
                uint8_t incoming_channel = m.channel + 1; // MIDI channels are 0-15, we store 1-16
                if(cv1_midi_channel == 0 || incoming_channel == cv1_midi_channel) {
                    cv1_midi_note_active = true;
                    cv1_midi_note_number = m.data[0];
                }
                if(cv2_midi_channel == 0 || incoming_channel == cv2_midi_channel) {
                    cv2_midi_note_active = true;
                    cv2_midi_note_number = m.data[0];
                }
            }
            else
            {
                midi_note_active = false;
                cv1_midi_note_active = false;
                cv2_midi_note_active = false;
            }
            break;
        }
        case NoteOff:
        {
            midi_note_active = false;
            cv1_midi_note_active = false;
            cv2_midi_note_active = false;
            break;
        }
        case ControlChange:
        {
            uint8_t cc = m.data[0];
            uint8_t value = m.data[1];
            uint8_t channel = m.channel; // MIDI channel (0-15)
            midi_cc_values[cc] = (float)value / 127.0f;
            
            // Check DSP effects first (they have priority if channel matches)
            bool dsp_handled = false;
            for(int i = 0; i < 5; i++) {
                if(dsp_effects[i].midi_channel == channel + 1) { // MIDI channels are 1-16, array is 0-15
                    // Map CCs to DSP effect parameters
                    switch(cc) {
                        case 16: dsp_effects[i].param1 = midi_cc_values[cc]; break; // General Purpose 1
                        case 17: dsp_effects[i].param2 = midi_cc_values[cc]; break; // General Purpose 2
                        case 18: dsp_effects[i].param3 = midi_cc_values[cc]; break; // General Purpose 3
                        case 19: dsp_effects[i].param4 = midi_cc_values[cc]; break; // General Purpose 4
                        case 80: dsp_effects[i].enabled = (value > 63); break; // General Purpose 5 - enable/disable
                    }
                    SaveSettings(); // Save when DSP settings change
                    dsp_handled = true;
                    break; // Only one effect per channel
                }
            }
            
            if(!dsp_handled) {
                // Map common CCs to parameters
                switch(cc)
                {
                    case 1: // Mod wheel - randomization
                        randomization_amt = midi_cc_values[cc];
                        break;
                    case 7: // Volume - gain
                        audio_gain = (int)(midi_cc_values[cc] * 100.0f);
                        SaveSettings();
                        break;
                    case 10: // Pan - blend
                        if(!audio_blend_locked) {
                            blend_amount = midi_cc_values[cc];
                            audio_blend = (int)(blend_amount * 100.0f);
                            SaveSettings();
                        }
                        break;
                    case 74: // Filter cutoff - window size
                        // Map to CTRL_1 (grain size)
                        break;
                    case 71: // Filter resonance - density (not implemented)
                        // Map to CTRL_2 (density) - placeholder for future implementation
                        break;
                }
            }
            break;
        }
        case PitchBend:
        {
            // Pitch bend range of +/- 2 semitones
            int16_t bend = ((uint16_t)m.data[1] << 7) | m.data[0];
            float bend_norm = (float)bend / 8192.0f - 1.0f; // -1 to 1
            midi_pitch_bend = powf(2.0f, bend_norm * 2.0f / 12.0f);
            break;
        }
        default:
            break;
    }
}

int  buf_len_idx = 2;     // default index into buffer length options (2 -> 18s)
const int      buf_len_options[]  = {6, 9, 12, 15, 18};
uint32_t       last_encoder_press = 0;
const uint32_t DOUBLE_PRESS_MS    = 400;



// Use SDRAM for large buffers
DSY_SDRAM_BSS static float bufferA_storage[BUFFER_A_SIZE];
// bufferB_storage not needed because SampleBuffer has internal storage

// Simple circular buffer for Buffer A (supports circular overwrite while gate high)
struct CircularBuffer
{
    float* buf;
    size_t size;         // physical storage size
    size_t logical_size; // active logical buffer size (<= size)
    size_t write_ptr;
    bool   recording;
    bool   full;
    // fractional read position to support variable-rate playback
    float read_pos;
    float play_rate; // samples per output sample (1.0 = normal)
    // window playback (size and center within the filled buffer)
    size_t window_size;   // in samples (logical samples inside filled)
    size_t window_center; // logical index within 0..filled-1
    bool   playing;

    void Init(float* storage, size_t s)
    {
        buf           = storage;
        size          = s;
        logical_size  = s; // default uses whole physical buffer
        write_ptr     = 0;
        read_pos      = 0.f;
        play_rate     = 1.0f;
        window_size   = s; // default to full buffer
        window_center = 0;
        recording     = false;
        playing       = false;
        full          = false;
        Clear();
    }
    void Clear()
    {
        memset(buf, 0, sizeof(float) * size);
        write_ptr = 0;
        read_pos  = 0.f;
        full      = false;
    }
    void StartRecord()
    {
        // Begin a fresh recording at the logical start of the buffer.
        // Reset write pointer and playback state so stopping/starting doesn't
        // leave the window in an unexpected state.
        recording = true;
        write_ptr = 0;
        full = false;
        playing = false;
        read_pos = 0.f;
    }
    void StopRecord()
    {
        // Stop recording and start playback from the beginning of the filled region
        // This prevents the window shrinking to near-zero when recording stops
        recording = false;
        if(GetFillSamples() > 0)
        {
            playing  = true;
            read_pos = 0.f;
        }
    }

    // Expose whether buffer is currently recording so external code can
    // adjust behavior (e.g., determining window size during/after record).
    bool IsRecording() const { return recording; }
    void Write(float s)
    {
        if(recording)
        {
            buf[write_ptr++] = s;
            if(write_ptr >= logical_size)
            {
                write_ptr = 0;
                full      = true;
                // stop recording when logical buffer full
                recording = false;
                // prepare for playback
                playing  = true;
                read_pos = 0.f;
            }
        }
    }
    size_t GetFillSamples() const { return full ? logical_size : write_ptr; }
    void   SetLogicalSize(size_t s)
    {
        if(s == 0)
            s = 1;
        if(s > size)
            s = size;
        logical_size = s;
        if(write_ptr > logical_size)
            write_ptr = logical_size;
        if(write_ptr == logical_size)
            full = true;
    }
    size_t GetLogicalSize() const { return logical_size; }
    // Linear interpolation helper
    static inline float LinearInterp(float x0, float x1, float t)
    {
        return x0 + (x1 - x0) * t;
    }
    
    // Cubic interpolation for higher quality
    static inline float CubicInterp(float x0, float x1, float x2, float x3, float t)
    {
        // Catmull-Rom spline interpolation
        float a = -0.5f * x0 + 1.5f * x1 - 1.5f * x2 + 0.5f * x3;
        float b = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
        float c = -0.5f * x0 + 0.5f * x2;
        float d = x1;
        return a * t * t * t + b * t * t + c * t + d;
    }

    float Read()
    {
        if(!playing)
            return 0.f;

        size_t filled = GetFillSamples();
        if(filled == 0)
            return 0.f;

        // determine effective window size and bounds
        size_t win_size = window_size;
        if(win_size == 0)
            win_size = filled;
        if(win_size > filled)
            win_size = filled;

        // wrap read_pos within window range
        while(read_pos >= (float)win_size)
            read_pos -= (float)win_size;
        while(read_pos < 0.f)
            read_pos += (float)win_size;

        size_t i0   = (size_t)floorf(read_pos);
        size_t i1   = (i0 + 1) % win_size;
        size_t i2   = (i0 + 2) % win_size;
        size_t i3   = (i0 + 3) % win_size;
        float  frac = read_pos - (float)i0;

        // map logical window indices to physical buffer indices
        size_t oldest = full ? write_ptr : 0;
        int64_t half          = (int64_t)win_size / 2;
        int64_t center        = (int64_t)window_center;
        int64_t start_logical = center - half;

        auto logical_to_idx = [&](int64_t logical_index) -> size_t {
            while(logical_index < 0)
                logical_index += (int64_t)filled;
            if((size_t)logical_index >= filled)
                logical_index = logical_index % (int64_t)filled;
            return (oldest + (size_t)logical_index) % size;
        };

        int64_t idx_logical0  = start_logical + (int64_t)i0;
        int64_t idx_logical1  = start_logical + (int64_t)i1;
        int64_t idx_logical2  = start_logical + (int64_t)i2;
        int64_t idx_logical3  = start_logical + (int64_t)i3;

        size_t idx_0  = logical_to_idx(idx_logical0);
        size_t idx_1  = logical_to_idx(idx_logical1);
        size_t idx_2  = logical_to_idx(idx_logical2);
        size_t idx_3  = logical_to_idx(idx_logical3);

        float x0  = buf[idx_0];
        float x1  = buf[idx_1];
        float x2  = buf[idx_2];
        float x3  = buf[idx_3];

        float out = CubicInterp(x0, x1, x2, x3, frac);

        // advance read position by play_rate (in samples of window)
        read_pos += play_rate;
        if(read_pos >= (float)win_size)
            read_pos -= (float)win_size;
        if(read_pos < 0.f)
            read_pos += (float)win_size;

        return out;
    }
    void Play(bool start = true)
    {
        playing = start;
        if(start)
            read_pos = 0.f;
    }
    void SetRate(float r) { play_rate = r; }
    void SetWindowFromCenterFrac(float center_frac, size_t filled)
    {
        if(filled == 0)
        {
            window_center = 0;
            return;
        }
        size_t c = (size_t)(center_frac * (float)filled);
        if(c >= filled)
            c = filled - 1;
        window_center = c;
    }
    void SetWindowSizeFrac(float size_frac, size_t filled)
    {
        if(filled == 0)
        {
            window_size = 0;
            return;
        }
        size_t s = (size_t)(size_frac * (float)filled);
        if(s < 1)
            s = 0; // Allow minimum of 0 samples for very small windows
        if(s > filled)
            s = filled;
        window_size = s;
    }
    float GetPlayheadPos() const
    {
        if(size == 0 || window_size == 0)
            return 0.f;
        return (float)read_pos / (float)window_size;
    }
};

static CircularBuffer bufferA;

// Buffer B implemented with SampleBuffer (we will capture live audio into it)
static SampleBuffer<BUFFER_B_SIZE> bufferB;

// rolling prebuffer (still available but not used for freeze capture now)
static float prebuf[BUFFER_B_SIZE];
static size_t              prebuf_write = 0;
static size_t              bufferB_read_count
    = 0; // tracks sample reads for bufferB playback progress

// Freeze recording state
static bool freeze_recording
    = false; // true while Gate2 is held and we are recording into bufferB

// UI state derived from buffers
float    bufferA_fill      = 0.f; // 0..1
float    bufferB_fill      = 0.f; // 0..1
bool     bufferA_full      = false;
bool     bufferB_full      = false;
bool     bufferA_recording = false;
uint32_t bufferA_start_ms  = 0;

// Playback placeholders
bool  bufferA_playing  = false;
bool  bufferB_playing  = false;
float bufferA_play_pos = 0.f; // 0..1
float bufferB_play_pos = 0.f;

void UpdateControls()
{
    patch.ProcessAllControls();

    // Encoder navigation
    int enc = patch.encoder.Increment();
    if(menu_open)
    {
        if(menu_level == MAIN_MENU)
        {
            menu_back_confirm = (strcmp(menu_items[menu_idx], "Back") == 0);
            if(enc != 0)
            {
                int new_idx = menu_idx + enc;
                if(new_idx < 0) new_idx = 0;
                if(new_idx >= menu_items_count) new_idx = menu_items_count - 1;
                menu_idx = new_idx;
            }
            // Enter Buffer Options submenu
            if(strcmp(menu_items[menu_idx], "Buffer Options") == 0 && patch.encoder.RisingEdge())
            {
                menu_level = BUFFER_MENU;
                buffer_menu_idx = 0;
                menu_back_confirm = false;
                last_encoder_press = System::GetNow();
                return;
            }
            // Enter Audio submenu
            if(strcmp(menu_items[menu_idx], "Audio") == 0 && patch.encoder.RisingEdge())
            {
                menu_level = AUDIO_MENU;
                audio_menu_idx = 0;
                menu_back_confirm = false;
                last_encoder_press = System::GetNow();
                return;
            }
            // Enter DSP submenu
            if(strcmp(menu_items[menu_idx], "DSP options") == 0 && patch.encoder.RisingEdge())
            {
                menu_level = DSP_MENU;
                dsp_menu_idx = 0;
                menu_back_confirm = false;
                last_encoder_press = System::GetNow();
                return;
            }
            // Gran Midi toggle
            if(strcmp(menu_items[menu_idx], "Gran Midi") == 0 && patch.encoder.RisingEdge())
            {
                gran_midi_enabled = !gran_midi_enabled; // Toggle on/off
                SaveSettings();
                last_encoder_press = System::GetNow();
                return;
            }
            // CV Ops submenu
            if(strcmp(menu_items[menu_idx], "CV Ops") == 0 && patch.encoder.RisingEdge())
            {
                menu_level = CV_OPS_MENU;
                cv_ops_menu_idx = 0;
                last_encoder_press = System::GetNow();
                return;
            }
            // Only allow encoder press to exit menu when Back is highlighted
            if(menu_back_confirm && patch.encoder.RisingEdge())
            {
                menu_open = false;
                menu_back_confirm = false;
                last_encoder_press = System::GetNow();
                return;
            }
            if(patch.encoder.RisingEdge())
            {
                uint32_t now = System::GetNow();
                if(now - last_encoder_press <= DOUBLE_PRESS_MS)
                {
                    bufferA_play_pos = 0.f;
                    bufferB_play_pos = 0.f;
                }
                last_encoder_press = now;
            }
            return;
        }
        else if(menu_level == BUFFER_MENU)
        {
            menu_back_confirm = (strcmp(buffer_menu_items[buffer_menu_idx], "Back") == 0);
            // Main Buf size editing mode
            if(buffer_menu_idx == 0 && !menu_back_confirm) {
                // Main Buf size
                if(!buffer_size_editing && patch.encoder.RisingEdge()) {
                    buffer_size_editing = true;
                    buffer_size_pending = buffer_size_seconds;
                    buffer_size_locked = false;
                    last_encoder_press = System::GetNow();
                    return;
                }
                if(buffer_size_editing) {
                    if(enc != 0) {
                        int step = 5;
                        int min_val = 1;  // Allow smaller buffers
                        int max_val = 20;  // Allow up to 20 seconds
                        int idx = (buffer_size_pending - min_val) / step;
                        idx += enc;
                        if(idx < 0) idx = 0;
                        if(idx > (max_val - min_val) / step) idx = (max_val - min_val) / step;
                        buffer_size_pending = min_val + idx * step;
                    }
                    if(patch.encoder.RisingEdge()) {
                        buffer_size_seconds = buffer_size_pending;
                        // Don't start recording here - just update the size for future recordings
                        // The buffer will be reinitialized with the new size when gate is triggered
                        buffer_size_editing = false;
                        buffer_size_locked = true;
                        SaveSettings(); // Save when buffer settings change
                        last_encoder_press = System::GetNow();
                        return;
                    }
                    return;
                }
            } else if(buffer_menu_idx == 1 && !menu_back_confirm) {
                // Freeze Buffer
                if(!freeze_buffer_editing && patch.encoder.RisingEdge()) {
                    freeze_buffer_editing = true;
                    freeze_buffer_pending = freeze_buffer_size;
                    freeze_buffer_locked = false;
                    last_encoder_press = System::GetNow();
                    return;
                }
                if(freeze_buffer_editing) {
                    if(enc != 0) {
                        float step = 1.0f; // 1 second steps
                        float min_val = 1.0f; // 1 second minimum
                        float max_val = 4.0f; // 4 seconds maximum
                        int idx = (int)((freeze_buffer_pending - min_val) / step + 0.5f);
                        idx += enc;
                        if(idx < 0) idx = 0;
                        if(idx > (int)((max_val - min_val) / step)) idx = (int)((max_val - min_val) / step);
                        freeze_buffer_pending = min_val + idx * step;
                    }
                    if(patch.encoder.RisingEdge()) {
                        freeze_buffer_size = freeze_buffer_pending;
                        // TODO: Apply freeze buffer size to actual buffer logic if needed
                        freeze_buffer_editing = false;
                        freeze_buffer_locked = true;
                        SaveSettings(); // Save when buffer settings change
                        last_encoder_press = System::GetNow();
                        return;
                    }
                    return;
                }
            } else {
                buffer_size_locked = false;
                freeze_buffer_locked = false;
            }
            // Normal menu navigation if not editing
            if(!buffer_size_editing && enc != 0)
            {
                int new_idx = buffer_menu_idx + enc;
                if(new_idx < 0) new_idx = 0;
                if(new_idx >= buffer_menu_items_count) new_idx = buffer_menu_items_count - 1;
                buffer_menu_idx = new_idx;
            }
            // Only allow encoder press to exit buffer menu when Back is highlighted
            if(!buffer_size_editing && menu_back_confirm && patch.encoder.RisingEdge())
            {
                menu_level = MAIN_MENU;
                menu_back_confirm = false;
                last_encoder_press = System::GetNow();
                return;
            }
            if(!buffer_size_editing && patch.encoder.RisingEdge())
            {
                uint32_t now = System::GetNow();
                if(now - last_encoder_press <= DOUBLE_PRESS_MS)
                {
                    bufferA_play_pos = 0.f;
                    bufferB_play_pos = 0.f;
                }
                last_encoder_press = now;
            }
            return;
        }
        else if(menu_level == AUDIO_MENU)
        {
            menu_back_confirm = (strcmp(audio_menu_items[audio_menu_idx], "Back") == 0);
            // Gain (1-100%)
            if(audio_menu_idx == 0 && !menu_back_confirm) {
                if(!audio_gain_editing && patch.encoder.RisingEdge()) {
                    audio_gain_editing = true;
                    audio_gain_pending = audio_gain;
                    audio_gain_locked = false;
                    last_encoder_press = System::GetNow();
                    return;
                }
                if(audio_gain_editing) {
                    if(enc != 0) {
                        audio_gain_pending += enc;
                        if(audio_gain_pending < 1) audio_gain_pending = 1;
                        if(audio_gain_pending > 100) audio_gain_pending = 100;
                    }
                    if(patch.encoder.RisingEdge()) {
                        audio_gain = audio_gain_pending;
                        audio_gain_editing = false;
                        audio_gain_locked = true;
                        SaveSettings(); // Save when audio settings change
                        last_encoder_press = System::GetNow();
                        return;
                    }
                    return;
                }
            } else if(audio_menu_idx == 1 && !menu_back_confirm) {
                // Blend (0-100%)
                if(!audio_blend_editing && patch.encoder.RisingEdge()) {
                    audio_blend_editing = true;
                    audio_blend_pending = audio_blend;
                    audio_blend_locked = false;
                    last_encoder_press = System::GetNow();
                    return;
                }
                if(audio_blend_editing) {
                    if(enc != 0) {
                        audio_blend_pending += enc;
                        if(audio_blend_pending < 0) audio_blend_pending = 0;
                        if(audio_blend_pending > 100) audio_blend_pending = 100;
                    }
                    if(patch.encoder.RisingEdge()) {
                        audio_blend = audio_blend_pending;
                        blend_amount = audio_blend / 100.0f;
                        audio_blend_editing = false;
                        audio_blend_locked = true;
                        SaveSettings(); // Save when audio settings change
                        last_encoder_press = System::GetNow();
                        return;
                    }
                    return;
                }
            } else if(audio_menu_idx == 3 && !menu_back_confirm) {
                // MIDI Enable toggle
                if(patch.encoder.RisingEdge()) {
                    midi_enabled = !midi_enabled;
                    last_encoder_press = System::GetNow();
                    return;
                }
            } else if(audio_menu_idx == 4 && !menu_back_confirm) {
                // MIDI Type toggle (USB vs Physical)
                if(patch.encoder.RisingEdge()) {
                    midi_use_usb = !midi_use_usb;
                    last_encoder_press = System::GetNow();
                    return;
                }
            } else if(audio_menu_idx == 5 && !menu_back_confirm) {
                // Clock Settings submenu
                if(patch.encoder.RisingEdge()) {
                    clock_submenu_open = true;
                    clock_menu_idx = 0;
                    last_encoder_press = System::GetNow();
                    return;
                }
            } else {
                audio_gain_locked = false;
                audio_blend_locked = false;
            }
            // Normal menu navigation if not editing
            if(!audio_gain_editing && !audio_blend_editing && enc != 0)
            {
                int new_idx = audio_menu_idx + enc;
                if(new_idx < 0) new_idx = 0;
                if(new_idx >= audio_menu_items_count) new_idx = audio_menu_items_count - 1;
                audio_menu_idx = new_idx;
            }
            // Only allow encoder press to exit audio menu when Back is highlighted
            if(!audio_gain_editing && !audio_blend_editing && menu_back_confirm && patch.encoder.RisingEdge())
            {
                menu_level = MAIN_MENU;
                menu_back_confirm = false;
                last_encoder_press = System::GetNow();
                return;
            }
            if(!audio_gain_editing && !audio_blend_editing && patch.encoder.RisingEdge())
            {
                uint32_t now = System::GetNow();
                if(now - last_encoder_press <= DOUBLE_PRESS_MS)
                {
                    bufferA_play_pos = 0.f;
                    bufferB_play_pos = 0.f;
                }
                last_encoder_press = now;
            }
            return;
        }
        else if(menu_level == AUDIO_MENU && clock_submenu_open)
        {
            // Clock settings submenu
            menu_back_confirm = (strcmp(clock_menu_items[clock_menu_idx], "Back") == 0);
            
            if(clock_menu_idx == 0 && !menu_back_confirm) {
                // Audio Clock Threshold
                if(patch.encoder.RisingEdge()) {
                    // Could add editing mode here if needed
                    last_encoder_press = System::GetNow();
                    return;
                }
            } else if(clock_menu_idx == 1 && !menu_back_confirm) {
                // Clock Source selection
                if(enc != 0) {
                    clock_source = (clock_source + enc) % 4;
                    if(clock_source < 0) clock_source = 3;
                }
                if(patch.encoder.RisingEdge()) {
                    last_encoder_press = System::GetNow();
                    return;
                }
            } else if(clock_menu_idx == 2 && !menu_back_confirm) {
                // BPM Display (read-only)
                if(patch.encoder.RisingEdge()) {
                    last_encoder_press = System::GetNow();
                    return;
                }
            }
            
            // Normal submenu navigation
            if(enc != 0)
            {
                int new_idx = clock_menu_idx + enc;
                if(new_idx < 0) new_idx = 0;
                if(new_idx >= clock_menu_items_count) new_idx = clock_menu_items_count - 1;
                clock_menu_idx = new_idx;
            }
            
            // Exit submenu when Back is selected
            if(menu_back_confirm && patch.encoder.RisingEdge())
            {
                clock_submenu_open = false;
                clock_menu_idx = 0;
                last_encoder_press = System::GetNow();
                return;
            }
            
            if(patch.encoder.RisingEdge())
            {
                uint32_t now = System::GetNow();
                if(now - last_encoder_press <= DOUBLE_PRESS_MS)
                {
                    bufferA_play_pos = 0.f;
                    bufferB_play_pos = 0.f;
                }
                last_encoder_press = now;
            }
            return;
        }
        else if(menu_level == DSP_MENU)
        {
            if(!dsp_effect_submenu_open) {
                menu_back_confirm = (strcmp(dsp_menu_items[dsp_menu_idx], "Back") == 0);
                if(enc != 0)
                {
                    int new_idx = dsp_menu_idx + enc;
                    if(new_idx < 0) new_idx = 0;
                    if(new_idx >= dsp_menu_items_count) new_idx = dsp_menu_items_count - 1;
                    dsp_menu_idx = new_idx;
                }
                // Enter effect submenu on encoder press (not Back or DSP Mode)
                bool is_dsp_mode_toggle = (strcmp(dsp_menu_items[dsp_menu_idx], "DSP Mode") == 0);
                if(!menu_back_confirm && !is_dsp_mode_toggle && patch.encoder.RisingEdge()) {
                    dsp_effect_submenu_open = true;
                    dsp_effect_idx = dsp_menu_idx;
                    current_dsp_effect = dsp_menu_idx; // Set current effect for knob control
                    dsp_effect_submenu_idx = 0;
                    last_encoder_press = System::GetNow();
                    return;
                }
                // Handle DSP Mode Toggle
                if(is_dsp_mode_toggle && patch.encoder.RisingEdge()) {
                    dsp_mode_active = !dsp_mode_active;
                    if(!dsp_mode_active) {
                        current_dsp_effect = -1; // Reset when turning DSP mode off
                    }
                    last_encoder_press = System::GetNow();
                    return;
                }
                // Only allow encoder press to exit DSP menu when Back is highlighted
                if(menu_back_confirm && patch.encoder.RisingEdge())
                {
                    menu_level = MAIN_MENU;
                    menu_back_confirm = false;
                    last_encoder_press = System::GetNow();
                    return;
                }
                if(patch.encoder.RisingEdge())
                {
                    uint32_t now = System::GetNow();
                    if(now - last_encoder_press <= DOUBLE_PRESS_MS)
                    {
                        bufferA_play_pos = 0.f;
                        bufferB_play_pos = 0.f;
                    }
                    last_encoder_press = now;
                }
                return;
            } else {
                // Effect submenu navigation
                menu_back_confirm = (strcmp(dsp_effect_submenu_items[dsp_effect_submenu_idx], "Back") == 0);
                
                // Encoder turn: navigate menu or adjust selected parameter
                if(enc != 0) {
                    if(selected_parameter >= 0) {
                        // Adjust selected parameter
                        DSPEffectParams& effect = dsp_effects[dsp_effect_idx];
                        float* param_ptr = nullptr;
                        switch(selected_parameter) {
                            case 0: param_ptr = &effect.param1; break;
                            case 1: param_ptr = &effect.param2; break;
                            case 2: param_ptr = &effect.param3; break;
                            case 3: param_ptr = &effect.param4; break;
                        }
                        if(param_ptr) {
                            *param_ptr += enc * 0.025f; // Adjust by 0.025 per encoder step (normal speed)
                            if(*param_ptr < 0.0f) *param_ptr = 0.0f;
                            if(*param_ptr > 1.0f) *param_ptr = 1.0f;
                            UpdateDSPEffectParameters(dsp_effect_idx);
                            SaveSettings();
                        }
                    } else {
                        // Navigate menu items
                        int new_idx = dsp_effect_submenu_idx + enc;
                        if(new_idx < 0) new_idx = 0;
                        if(new_idx >= dsp_effect_submenu_items_count) new_idx = dsp_effect_submenu_items_count - 1;
                        dsp_effect_submenu_idx = new_idx;
                    }
                }

                // Encoder press: select/deselect parameters or handle other actions
                if(!menu_back_confirm && patch.encoder.RisingEdge()) {
                    if(dsp_effect_submenu_idx >= 0 && dsp_effect_submenu_idx <= 3) {
                        // Select/deselect parameter for editing
                        if(selected_parameter == dsp_effect_submenu_idx) {
                            // Deselect and lock/unlock
                            DSPEffectParams& effect = dsp_effects[dsp_effect_idx];
                            switch(selected_parameter) {
                                case 0: effect.locked1 = !effect.locked1; break;
                                case 1: effect.locked2 = !effect.locked2; break;
                                case 2: effect.locked3 = !effect.locked3; break;
                                case 3: effect.locked4 = !effect.locked4; break;
                            }
                            SaveSettings();
                            selected_parameter = -1; // Deselect
                        } else {
                            // Select this parameter
                            selected_parameter = dsp_effect_submenu_idx;
                        }
                    } else if(dsp_effect_submenu_idx == 4) { // On/Off
                        DSPEffectParams& effect = dsp_effects[dsp_effect_idx];
                        effect.enabled = !effect.enabled;
                        SaveSettings();
                    } else if(dsp_effect_submenu_idx == 5) { // Ch In
                        DSPEffectParams& effect = dsp_effects[dsp_effect_idx];
                        effect.input_channel = (effect.input_channel % 3) + 1; // cycle 1->2->3->1 (Audio In 1,2,3)
                        SaveSettings();
                    } else if(dsp_effect_submenu_idx == 6) { // MIDI Ch
                        DSPEffectParams& effect = dsp_effects[dsp_effect_idx];
                        effect.midi_channel = (effect.midi_channel + 1) % 17; // cycle 0-16 (0=off, 1-16=channels)
                        SaveSettings();
                    }

                    last_encoder_press = System::GetNow();
                    return;
                }
                // Only allow encoder press to exit effect submenu when Back is highlighted
                if(menu_back_confirm && patch.encoder.RisingEdge())
                {
                    dsp_effect_submenu_open = false;
                    dsp_effect_submenu_idx = 0;
                    // Don't reset current_dsp_effect - keep controlling the same effect when DSP mode is active
                    last_encoder_press = System::GetNow();
                    return;
                }
                if(patch.encoder.RisingEdge())
                {
                    uint32_t now = System::GetNow();
                    if(now - last_encoder_press <= DOUBLE_PRESS_MS)
                    {
                        bufferA_play_pos = 0.f;
                        bufferB_play_pos = 0.f;
                    }
                    last_encoder_press = now;
                }
                return;
            }
        }
        else if(menu_level == CV_OPS_MENU)
        {
            // Ensure menu index is valid
            if(cv_ops_menu_idx >= cv_ops_menu_items_count) cv_ops_menu_idx = cv_ops_menu_items_count - 1;
            if(cv_ops_menu_idx < 0) cv_ops_menu_idx = 0;
            
            menu_back_confirm = (strcmp(cv_ops_menu_items[cv_ops_menu_idx], "Back") == 0);
            if(enc != 0)
            {
                int new_idx = cv_ops_menu_idx + enc;
                if(new_idx < 0) new_idx = 0;
                if(new_idx >= cv_ops_menu_items_count) new_idx = cv_ops_menu_items_count - 1;
                cv_ops_menu_idx = new_idx;
            }
            
            // Handle menu item selection
            if(!menu_back_confirm && patch.encoder.RisingEdge()) {
                if(cv_ops_menu_idx == 0) { // Scale selection
                    cv_scale = (ScaleType)((cv_scale + 1) % SCALE_COUNT);
                    SaveSettings();
                } else if(cv_ops_menu_idx == 1) { // Reese Mode
                    reese_mode = !reese_mode;
                    SaveSettings();
                } else if(cv_ops_menu_idx == 2) { // CV 1 On/Off
                    cv1_enabled = !cv1_enabled;
                    SaveSettings();
                } else if(cv_ops_menu_idx == 3) { // CV1 MIDI Channel
                    cv1_midi_channel = (cv1_midi_channel % 4) + 2; // Cycle 2-5
                    SaveSettings();
                } else if(cv_ops_menu_idx == 4) { // CV2 MIDI Channel
                    cv2_midi_channel = (cv2_midi_channel % 4) + 2; // Cycle 2-5
                    SaveSettings();
                } else if(cv_ops_menu_idx == 5) { // CV Hold Mode
                    cv_hold_mode = (CvHoldMode)((cv_hold_mode + 1) % 4); // Cycle Off, 1, 2, On
                    SaveSettings();
                }
                last_encoder_press = System::GetNow();
                return;
            }
            
            // Only allow encoder press to exit CV Ops menu when Back is highlighted
            if(menu_back_confirm && patch.encoder.RisingEdge())
            {
                menu_level = MAIN_MENU;
                menu_back_confirm = false;
                last_encoder_press = System::GetNow();
                return;
            }
            
            if(patch.encoder.RisingEdge())
            {
                uint32_t now = System::GetNow();
                if(now - last_encoder_press <= DOUBLE_PRESS_MS)
                {
                    bufferA_play_pos = 0.f;
                    bufferB_play_pos = 0.f;
                }
                last_encoder_press = now;
            }
            return;
        }
    }
    // Not in menu: encoder press opens menu
    if(patch.encoder.RisingEdge())
    {
        uint32_t now = System::GetNow();
        if(now - last_encoder_press <= DOUBLE_PRESS_MS)
        {
            // double-press: reset nominal pitch
            bufferA_play_pos = 0.f;
            bufferB_play_pos = 0.f;
        }
        else
        {
            // Open Ops menu from anywhere on main screen
            menu_open = true;
            menu_idx = 0;
            menu_back_confirm = false;
        }
        last_encoder_press = now;
    }

    // Gate1: record-and-hold control for Buffer A
    if(patch.gate_input[DaisyPatch::GATE_IN_1].Trig())
    {
        // rising edge -> start a fresh recording into the buffer (circular while high)
        // Use the configured buffer size (menu now limits to available storage)
        size_t buffer_samples = buffer_size_seconds * SAMPLE_RATE;
        bufferA.Init(bufferA_storage, buffer_samples);
        bufferA.StartRecord();
        // Do NOT start playback immediately; playback will only begin once the
        // logical buffer becomes full (see CircularBuffer::Write which sets full)
        bufferA_recording = true;
        bufferA_start_ms  = System::GetNow();
        bufferA_playing   = false;
    }
    // Stop recording on gate low but KEEP the buffer for playback
    if(!patch.gate_input[DaisyPatch::GATE_IN_1].State() && bufferA_recording)
    {
        bufferA.StopRecord();
        bufferA_recording = false;
        // Don't clear the buffer - keep it for playback!
        // The buffer will be ready for granular playback once it fills up
    }

    // Update UI fill from bufferA - use actual buffer size, not fixed size
    size_t current_buffer_size = bufferA.GetLogicalSize();
    if(current_buffer_size > 0) {
        bufferA_fill = (float)bufferA.GetFillSamples() / (float)current_buffer_size;
    } else {
        bufferA_fill = 0.f;
    }

    // Read knobs - only for granular parameters (DSP parameters are MIDI-controlled only)
    // Granular mode: knobs control granular parameters
    // Window size (Grain window) -> Ctrl 1; window center -> Ctrl 3 (Pos)
    {
        float win_knob = patch.GetKnobValue(DaisyPatch::CTRL_1); // 0..1
        float win_frac = 0.0001f + 0.9999f * win_knob;             // 0.01%..100% (very small min)

        // Randomization (CTRL_2): left = off, right = max, smooth curve
        float rand_knob = patch.GetKnobValue(DaisyPatch::CTRL_2); // 0..1
        if(rand_knob < 0.05f) {
            randomization_amt = 0.0f;
        } else {
            // Exponential curve for more control in lower range
            randomization_amt = powf((rand_knob - 0.05f) / 0.95f, 1.5f);
        }

        // Determine whether the main buffer is considered "full" (ready for playback and knobs)
        bufferA_full = (bufferA.GetFillSamples() >= bufferA.GetLogicalSize());

        if(bufferA_full)
        {
            // Only apply knob-controlled window settings when buffer is full
            bufferA.SetWindowSizeFrac(win_frac, bufferA.GetLogicalSize());
            float center_knob = patch.GetKnobValue(DaisyPatch::CTRL_3);
            bufferA.SetWindowFromCenterFrac(center_knob, bufferA.GetLogicalSize());
        }
        else
        {
            // While not full, lock the window to the current filled region so it doesn't
            // collapse or respond to knobs; center in the middle of the filled region.
            size_t filled = bufferA.GetFillSamples();
            bufferA.SetWindowSizeFrac(1.0f, filled > 0 ? filled : 1);
            bufferA.SetWindowFromCenterFrac(0.5f, filled > 0 ? filled : 1);
        }
    }

    // Gate2: start/stop a live freeze that records into its own buffer and plays circularly
    if(patch.gate_input[DaisyPatch::GATE_IN_2].Trig())
    {
        // start fresh: init/clear, start recording AND playback immediately
        bufferB.Init();
        bufferB.Record(true);
        bufferB.Play(true); // enable live circular playback
        freeze_recording   = true;
        bufferB_read_count = 0;
        bufferB_full       = false;
        bufferB_fill       = 0.f;
        bufferB_playing    = true;
    }

    // When Gate2 goes low, stop recording/playback and reset the freeze buffer
    if(!patch.gate_input[DaisyPatch::GATE_IN_2].State() && (freeze_recording || bufferB.IsPlaying()))
    {
        // force-stop recording and playback and clear buffer
        bufferB.Record(false);
        bufferB.Play(false);
        bufferB.Init(); // clear contents
        freeze_recording    = false;
        bufferB_full        = false;
        bufferB_fill        = 0.f;
        bufferB_playing     = false;
        bufferB_play_pos    = 0.f;
        bufferB_read_count  = 0;
    }
}

void UpdateDisplay()
{
    // sync derived states
    bufferA_playing = bufferA.playing;
    bufferB_playing = bufferB.IsPlaying();
    bufferA_fill    = (float)bufferA.GetFillSamples() / (float)BUFFER_A_SIZE;
    // Determine Buffer B fill display as before
    if(bufferB.IsPlaying())
        bufferB_fill = bufferB_play_pos;
    else
        bufferB_fill = bufferB_full ? 1.0f : 0.0f;

    // Main buffer 'full' state used elsewhere. Keep up-to-date here for UI & logic
    bufferA_full = (bufferA.GetFillSamples() >= bufferA.GetLogicalSize());

    patch.display.Fill(false);

    // Status indicators on bottom row
    patch.display.SetCursor(0, 50);
    patch.display.WriteString(bufferA_recording ? "R" : " ", Font_6x8, true);
    patch.display.SetCursor(10, 50);
    patch.display.WriteString(midi_enabled ? "M" : " ", Font_6x8, true);
    patch.display.SetCursor(20, 50);
    if(external_audio_clock_active)
        patch.display.WriteString("C", Font_6x8, true);
    else if(external_gate_clock_active)
        patch.display.WriteString("G", Font_6x8, true);
    else
        patch.display.WriteString(" ", Font_6x8, true);

    // Show blend percent or DSP mode on bottom right
    char btitle[16];
    if(dsp_mode_active && current_dsp_effect >= 0 && current_dsp_effect < 5) {
        const char* effect_names[] = {"LAD", "SOAP", "DEC", "WAVE", "CHOR"};
        sprintf(btitle, "DSP:%s", effect_names[current_dsp_effect]);
    } else {
        sprintf(btitle, "B:%d%%", (int)(blend_amount * 100.f));
    }
    patch.display.SetCursor(86, 50);
    patch.display.WriteString(btitle, Font_6x8, true);
    // right icon for freeze (transient '*' while Gate2 is held)
        patch.display.Fill(false);
        if(menu_open) {
            int visible_items = 5;
            if(menu_level == MAIN_MENU) {
                int start_idx = menu_idx - (menu_idx >= visible_items ? visible_items - 1 : 0);
                if(start_idx < 0) start_idx = 0;
                int end_idx = start_idx + visible_items;
                if(end_idx > menu_items_count) end_idx = menu_items_count;
                for(int idx = start_idx, j = 0; idx < end_idx; idx++, j++)
                {
                    patch.display.SetCursor(10, 4 + j * 11);
                    if(idx == menu_idx)
                        patch.display.WriteString("> ", Font_7x10, true);
                    else
                        patch.display.WriteString("  ", Font_7x10, true);
                    
                    if(idx == 3) { // Gran Midi
                        char buf[20];
                        sprintf(buf, "Gran Midi: %s", gran_midi_enabled ? "ON" : "OFF");
                        patch.display.WriteString(buf, Font_7x10, true);
                    } else {
                        patch.display.WriteString(menu_items[idx], Font_7x10, true);
                    }
                }
                if(menu_back_confirm)
                {
                    // Draw a single large, bold left arrow for the back button (centered)
                    patch.display.DrawRect(10, 10, 118, 54, true, true);
                    // Use a huge arrow made with multiple characters
                    patch.display.SetCursor(35, 20);
                    patch.display.WriteString("<<<", Font_7x10, false);
                    patch.display.SetCursor(35, 30);
                    patch.display.WriteString(" < ", Font_7x10, false);
                    patch.display.SetCursor(35, 40);
                    patch.display.WriteString("<<<", Font_7x10, false);
                }
            } else if(menu_level == BUFFER_MENU) {
                int visible_items = 5; // Show 5 items at a time
                int start_idx = buffer_menu_idx - (buffer_menu_idx >= visible_items ? visible_items - 1 : 0);
                if(start_idx < 0) start_idx = 0;
                int end_idx = start_idx + visible_items;
                if(end_idx > buffer_menu_items_count) end_idx = buffer_menu_items_count;
                for(int idx = start_idx, j = 0; idx < end_idx; idx++, j++)
                {
                    patch.display.SetCursor(6, 4 + j * 11); // move 4px left (was 10)
                    if(idx == buffer_menu_idx)
                        patch.display.WriteString("> ", Font_6x8, true);
                    else
                        patch.display.WriteString("  ", Font_6x8, true);
                    // Show buffer size editing UI
                    if(idx == 0) {
                        char buf[32];
                        if(buffer_size_editing) {
                            sprintf(buf, "Buffer size: %ds", buffer_size_pending);
                        } else {
                            if(buffer_size_locked) {
                                sprintf(buf, "Buffer size: %ds *", buffer_size_seconds);
                            } else {
                                sprintf(buf, "Buffer size: %ds", buffer_size_seconds);
                            }
                        }
                        patch.display.WriteString(buf, Font_6x8, true);
                    } else if(idx == 1) {
                        char buf[32];
                        if(freeze_buffer_editing) {
                            sprintf(buf, "Frez Buff: %ds", (int)freeze_buffer_pending);
                        } else {
                            if(freeze_buffer_locked) {
                                sprintf(buf, "Frez Buff: %ds *", (int)freeze_buffer_size);
                            } else {
                                sprintf(buf, "Frez Buff: %ds", (int)freeze_buffer_size);
                            }
                        }
                        patch.display.WriteString(buf, Font_6x8, true);
                    } else {
                        patch.display.WriteString(buffer_menu_items[idx], Font_6x8, true);
                    }
                }
                if(menu_back_confirm)
                {
                    patch.display.DrawRect(10, 10, 118, 54, true, true);
                    patch.display.SetCursor(35, 20);
                    patch.display.WriteString("<<<", Font_7x10, false);
                    patch.display.SetCursor(35, 30);
                    patch.display.WriteString(" < ", Font_7x10, false);
                    patch.display.SetCursor(35, 40);
                    patch.display.WriteString("<<<", Font_7x10, false);
                }
            } else if(menu_level == AUDIO_MENU) {
                // Show all audio menu items (7 total, should fit on screen with small font)
                int visible_items = 7; // Show all items
                int start_idx = 0; // Always start from beginning
                int end_idx = audio_menu_items_count;
                for(int idx = start_idx, j = 0; idx < end_idx && j < visible_items; idx++, j++)
                {
                    patch.display.SetCursor(6, 4 + j * 10); // tighter spacing
                    if(idx == audio_menu_idx)
                        patch.display.WriteString("> ", Font_6x8, true);
                    else
                        patch.display.WriteString("  ", Font_6x8, true);
                    if(idx == 0) {
                        char buf[32];
                        if(audio_gain_editing) {
                            sprintf(buf, "Gain: %d%%", audio_gain_pending);
                        } else {
                            if(audio_gain_locked) {
                                sprintf(buf, "Gain: %d%% *", audio_gain);
                            } else {
                                sprintf(buf, "Gain: %d%%", audio_gain);
                            }
                        }
                        patch.display.WriteString(buf, Font_6x8, true);
                    } else if(idx == 1) {
                        char buf[32];
                        if(audio_blend_editing) {
                            sprintf(buf, "Blend: %d%%", audio_blend_pending);
                        } else {
                            if(audio_blend_locked) {
                                sprintf(buf, "Blend: %d%% *", audio_blend);
                            } else {
                                sprintf(buf, "Blend: %d%%", audio_blend);
                            }
                        }
                        patch.display.WriteString(buf, Font_6x8, true);
                    } else if(idx == 2) {
                        patch.display.WriteString("Sample rate", Font_6x8, true);
                    } else if(idx == 3) {
                        char buf[32];
                        sprintf(buf, "MIDI: %s", midi_enabled ? "ON" : "OFF");
                        patch.display.WriteString(buf, Font_6x8, true);
                    } else if(idx == 4) {
                        char buf[32];
                        sprintf(buf, "Type: %s", midi_use_usb ? "USB" : "DIN");
                        patch.display.WriteString(buf, Font_6x8, true);
                    } else if(idx == 5) {
                        patch.display.WriteString("Clock Settings", Font_6x8, true);
                    } else if(idx == 6) {
                        patch.display.WriteString("Back", Font_6x8, true);
                    }
                }
            } else if(menu_level == DSP_MENU) {
                if(!dsp_effect_submenu_open) {
                    // Show DSP menu items with scrolling (7 total, show 5 at a time)
                    int visible_items = 5;
                    int start_idx = dsp_menu_idx - (dsp_menu_idx >= visible_items ? visible_items - 1 : 0);
                    if(start_idx < 0) start_idx = 0;
                    int end_idx = start_idx + visible_items;
                    if(end_idx > dsp_menu_items_count) end_idx = dsp_menu_items_count;
                    for(int idx = start_idx, j = 0; idx < end_idx; idx++, j++)
                    {
                        patch.display.SetCursor(10, 4 + j * 11);
                        if(idx == dsp_menu_idx)
                            patch.display.WriteString("> ", Font_7x10, true);
                        else
                            patch.display.WriteString("  ", Font_7x10, true);
                        patch.display.WriteString(dsp_menu_items[idx], Font_7x10, true);
                    }
                    if(menu_back_confirm)
                    {
                        patch.display.DrawRect(10, 10, 118, 54, true, true);
                        patch.display.SetCursor(35, 20);
                        patch.display.WriteString("<<<", Font_7x10, false);
                        patch.display.SetCursor(35, 30);
                        patch.display.WriteString(" < ", Font_7x10, false);
                        patch.display.SetCursor(35, 40);
                        patch.display.WriteString("<<<", Font_7x10, false);
                    }
                } else {
                    // Effect submenu display with scrolling
                    int visible_items = 5; // Show 5 items at a time
                    int start_idx = dsp_effect_submenu_idx - (dsp_effect_submenu_idx >= visible_items ? visible_items - 1 : 0);
                    if(start_idx < 0) start_idx = 0;
                    int end_idx = start_idx + visible_items;
                    if(end_idx > dsp_effect_submenu_items_count) end_idx = dsp_effect_submenu_items_count;
                    
                    for(int idx = start_idx, j = 0; idx < end_idx; idx++, j++) {
                        patch.display.SetCursor(10, 4 + j * 11);
                        if(idx == dsp_effect_submenu_idx)
                            patch.display.WriteString("> ", Font_6x8, true);
                        else
                            patch.display.WriteString("  ", Font_6x8, true);
                        
                        DSPEffectParams& effect = dsp_effects[dsp_effect_idx];
                        char buf[32]; // Increased buffer size for display strings
                        bool is_selected = (selected_parameter >= 0 && idx == selected_parameter);
                        
                        if(idx == 0) {
                            // P1: Show meaningful values based on effect type (x2 range)
                            switch(dsp_effect_idx) {
                                case 0: // Ladder Filter - Frequency
                                case 1: // Soap Filter - Frequency
                                    sprintf(buf, "F:%d%s%s", (int)(effect.param1 * 40000.0f + 20.0f), effect.locked1 ? "*" : "", is_selected ? " [SEL]" : "");
                                    break;
                                case 2: // Decimator - Downsample (0.1 to 1.9)
                                    {
                                        float val = effect.param1 * 1.8f + 0.1f;
                                        int whole = (int)val;
                                        int decimal = (int)((val - whole) * 10);
                                        sprintf(buf, "D:%d.%d%s%s", whole, decimal, effect.locked1 ? "*" : "", is_selected ? " [SEL]" : "");
                                    }
                                    break;
                                case 3: // Wavefolder - Gain (0.1 to 1.9)
                                    {
                                        float val = effect.param1 * 1.8f + 0.1f;
                                        int whole = (int)val;
                                        int decimal = (int)((val - whole) * 10);
                                        sprintf(buf, "G:%d.%d%s%s", whole, decimal, effect.locked1 ? "*" : "", is_selected ? " [SEL]" : "");
                                    }
                                    break;
                                case 4: // Chorus - Delay (0.0 to 2.0)
                                    {
                                        int whole = (int)(effect.param1 * 2.0f);
                                        int decimal = (int)((effect.param1 * 2.0f - whole) * 10);
                                        sprintf(buf, "Del:%d.%d%s%s", whole, decimal, effect.locked1 ? "*" : "", is_selected ? " [SEL]" : "");
                                    }
                                    break;
                            }
                        } else if(idx == 1) {
                            // P2: Show meaningful values based on effect type (x2 range)
                            switch(dsp_effect_idx) {
                                case 0: // Ladder Filter - Resonance (0.0 to 1.8)
                                case 1: // Soap Filter - Resonance
                                    {
                                        float val = effect.param2 * 1.8f;
                                        int decimal = (int)(val * 10);
                                        sprintf(buf, "R:0.%d%s%s", decimal, effect.locked2 ? "*" : "", is_selected ? " [SEL]" : "");
                                    }
                                    break;
                                case 2: // Decimator - Bitcrush (0.0 to 2.0)
                                    {
                                        int whole = (int)(effect.param2 * 2.0f);
                                        int decimal = (int)((effect.param2 * 2.0f - whole) * 10);
                                        sprintf(buf, "B:%d.%d%s%s", whole, decimal, effect.locked2 ? "*" : "", is_selected ? " [SEL]" : "");
                                    }
                                    break;
                                case 3: // Wavefolder - Offset (0.1 to 1.9)
                                    {
                                        float val = effect.param2 * 1.8f + 0.1f;
                                        int whole = (int)val;
                                        int decimal = (int)((val - whole) * 10);
                                        sprintf(buf, "O:%d.%d%s%s", whole, decimal, effect.locked2 ? "*" : "", is_selected ? " [SEL]" : "");
                                    }
                                    break;
                                case 4: // Chorus - Feedback (0.0 to 2.0)
                                    {
                                        int whole = (int)(effect.param2 * 2.0f);
                                        int decimal = (int)((effect.param2 * 2.0f - whole) * 10);
                                        sprintf(buf, "Fb:%d.%d%s%s", whole, decimal, effect.locked2 ? "*" : "", is_selected ? " [SEL]" : "");
                                    }
                                    break;
                            }
                        } else if(idx == 2) {
                            // P3: Show meaningful values based on effect type (x2 range)
                            switch(dsp_effect_idx) {
                                case 0: // Ladder Filter - Drive (0.1 to 1.9)
                                case 1: // Soap Filter - Drive
                                    {
                                        float val = effect.param3 * 1.8f + 0.1f;
                                        int whole = (int)val;
                                        int decimal = (int)((val - whole) * 10);
                                        sprintf(buf, "Dr:%d.%d%s%s", whole, decimal, effect.locked3 ? "*" : "", is_selected ? " [SEL]" : "");
                                    }
                                    break;
                                case 4: // Chorus - LFO Frequency (0.1 to 16.1)
                                    {
                                        float val = effect.param3 * 16.0f + 0.1f;
                                        int whole = (int)val;
                                        int decimal = (int)((val - whole) * 10);
                                        sprintf(buf, "LFO:%d.%d%s%s", whole, decimal, effect.locked3 ? "*" : "", is_selected ? " [SEL]" : "");
                                    }
                                    break;
                                default:
                                    {
                                        int whole = (int)(effect.param3 * 2.0f);
                                        int decimal = (int)((effect.param3 * 2.0f - whole) * 10);
                                        sprintf(buf, "P3:%d.%d%s%s", whole, decimal, effect.locked3 ? "*" : "", is_selected ? " [SEL]" : "");
                                    }
                                    break;
                            }
                        } else if(idx == 3) {
                            // Wet/Dry mix (1-100%)
                            int wet_dry_percent = (int)(effect.param4 * 200.0f) + 1; // 1-201 range (doubled)
                            if(wet_dry_percent > 100) wet_dry_percent = 100; // Cap at 100% for display
                            sprintf(buf, "Wet/Dry: %d%%%s%s", wet_dry_percent, effect.locked4 ? "*" : "", is_selected ? " [SEL]" : "");
                        } else if(idx == 4) { // On/Off
                            sprintf(buf, "On/Off: %s%s", effect.enabled ? "ON" : "OFF", is_selected ? " [SEL]" : "");
                        } else if(idx == 5) { // Ch In
                            sprintf(buf, "Ch In: %d%s", effect.input_channel, is_selected ? " [SEL]" : "");
                        } else if(idx == 6) { // MIDI Ch
                            if(effect.midi_channel == 0) {
                                sprintf(buf, "MIDI: OFF%s", is_selected ? " [SEL]" : "");
                            } else {
                                sprintf(buf, "MIDI: %d%s", effect.midi_channel, is_selected ? " [SEL]" : "");
                            }
                        } else { // Back
                            sprintf(buf, "Back%s", is_selected ? " [SEL]" : "");
                        }
                        patch.display.WriteString(buf, Font_6x8, true);
                    }
                    // Back arrow overlay if Back is selected
                    if(menu_back_confirm) {
                        patch.display.DrawRect(10, 10, 118, 54, true, true);
                        patch.display.SetCursor(35, 20);
                        patch.display.WriteString("<<<", Font_7x10, false);
                        patch.display.SetCursor(35, 30);
                        patch.display.WriteString(" < ", Font_7x10, false);
                        patch.display.SetCursor(35, 40);
                        patch.display.WriteString("<<<", Font_7x10, false);
                    }
                }
            } else if(menu_level == CV_OPS_MENU) {
                // Show CV Ops menu items
                int visible_items = 8; // Show all items
                int start_idx = 0; // Always start from beginning
                int end_idx = cv_ops_menu_items_count;
                for(int idx = start_idx, j = 0; idx < end_idx && j < visible_items; idx++, j++)
                {
                    patch.display.SetCursor(6, 4 + j * 10); // tighter spacing
                    if(idx == cv_ops_menu_idx)
                        patch.display.WriteString("> ", Font_6x8, true);
                    else
                        patch.display.WriteString("  ", Font_6x8, true);
                    
                    char buf[32];
                    if(idx == 0) { // Scale
                        int scale_idx = (int)cv_scale;
                        if(scale_idx >= 0 && scale_idx < 7) {
                            sprintf(buf, "Scale: %s", SCALE_NAMES[scale_idx]);
                        } else {
                            sprintf(buf, "Scale: Invalid");
                        }
                    } else if(idx == 1) { // Reese Mode
                        sprintf(buf, "Reese Mode: %s", reese_mode ? "ON" : "OFF");
                    } else if(idx == 2) { // CV 1 On/Off
                        sprintf(buf, "CV 1: %s", cv1_enabled ? "ON" : "OFF");
                    } else if(idx == 3) { // CV1 MIDI Channel
                        sprintf(buf, "CV1 MIDI Ch: %d", cv1_midi_channel);
                    } else if(idx == 4) { // CV2 MIDI Channel
                        sprintf(buf, "CV2 MIDI Ch: %d", cv2_midi_channel);
                    } else if(idx == 5) { // CV Hold Mode
                        const char* hold_names[] = {"Off", "1", "2", "On"};
                        sprintf(buf, "CV Hold: %s", hold_names[(int)cv_hold_mode]);
                    } else { // Back
                        sprintf(buf, "Back");
                    }
                    patch.display.WriteString(buf, Font_6x8, true);
                }
                
                // Back arrow overlay if Back is selected
                if(menu_back_confirm) {
                    patch.display.DrawRect(10, 10, 118, 54, true, true);
                    patch.display.SetCursor(35, 20);
                    patch.display.WriteString("<<<", Font_7x10, false);
                    patch.display.SetCursor(35, 30);
                    patch.display.WriteString(" < ", Font_7x10, false);
                    patch.display.SetCursor(35, 40);
                    patch.display.WriteString("<<<", Font_7x10, false);
                }
            }
            patch.display.Update();
            return;
        }

    // Knob readouts (small above bars) - show DSP params when DSP mode active
    char  buf[32];
    if(dsp_mode_active && current_dsp_effect >= 0 && current_dsp_effect < 5) {
        // DSP mode: show current effect parameters with meaningful values
        DSPEffectParams& effect = dsp_effects[current_dsp_effect];
        const char* effect_names[] = {"LAD", "SOAP", "DEC", "WAVE", "CHOR"};
        
        // P1 with meaningful display
        switch(current_dsp_effect) {
            case 0: case 1: sprintf(buf, "%s F:%d", effect_names[current_dsp_effect], (int)(effect.param1 * 19980.0f + 20.0f)); break;
            case 2: sprintf(buf, "%s D:%d", effect_names[current_dsp_effect], (int)(effect.param1 * 90.0f + 10.0f)); break;
            case 3: sprintf(buf, "%s G:%d", effect_names[current_dsp_effect], (int)(effect.param1 * 90.0f + 10.0f)); break;
            case 4: sprintf(buf, "%s Del:%d", effect_names[current_dsp_effect], (int)(effect.param1 * 100.0f)); break;
        }
        patch.display.SetCursor(2, 28);
        patch.display.WriteString(buf, Font_4x8, true);
        
        // P2 with meaningful display
        switch(current_dsp_effect) {
            case 0: case 1: sprintf(buf, "R:%d", (int)(effect.param2 * 90.0f)); break;
            case 2: sprintf(buf, "B:%d", (int)(effect.param2 * 100.0f)); break;
            case 3: sprintf(buf, "O:%d", (int)(effect.param2 * 90.0f + 10.0f)); break;
            case 4: sprintf(buf, "Fb:%d", (int)(effect.param2 * 100.0f)); break;
        }
        patch.display.SetCursor(34, 28);
        patch.display.WriteString(buf, Font_4x8, true);
        
        // P3 with meaningful display
        switch(current_dsp_effect) {
            case 0: case 1: sprintf(buf, "Dr:%d", (int)(effect.param3 * 90.0f + 10.0f)); break;
            case 4: sprintf(buf, "LFO:%d", (int)(effect.param3 * 4.0f + 0.1f)); break;
            default: sprintf(buf, "P3:%d", (int)(effect.param3 * 100.0f)); break;
        }
        patch.display.SetCursor(66, 28);
        patch.display.WriteString(buf, Font_4x8, true);
        
        // P4 with meaningful display
        switch(current_dsp_effect) {
            case 4: sprintf(buf, "Dep:%d", (int)(effect.param4 * 100.0f)); break;
            default: sprintf(buf, "P4:%d", (int)(effect.param4 * 100.0f)); break;
        }
        patch.display.SetCursor(98, 28);
        patch.display.WriteString(buf, Font_4x8, true);
    } else {
        // Granular mode: show MIDI note info above granular parameters
        if(midi_note_active) {
            // Convert MIDI note number to note name (blues scale)
            const char* note_names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
            int octave = (midi_note_number / 12) - 1; // MIDI note 60 = C4
            int note_idx = midi_note_number % 12;
            sprintf(buf, "MIDI: %s%d Ch%d", note_names[note_idx], octave, midi_channel);
        } else {
            sprintf(buf, "MIDI: --");
        }
        patch.display.SetCursor(2, 18);
        patch.display.WriteString(buf, Font_4x8, true);

        // Granular mode: show granular parameters
        // Gs = grain size fraction (0..1 mapped to small ms range); clamp display
        float gsz = patch.GetKnobValue(DaisyPatch::CTRL_1) * 0.5f;

        // Density: not implemented yet - placeholder for future grain density control
        // float den_raw = patch.GetKnobValue(DaisyPatch::CTRL_2); // 0..1
        // float den_percent = 0.f;                                // 0..140
        // if(den_raw > 0.4f)
        // {
        //     float v = (den_raw - 0.4f) / 0.6f; // 0..1 over upper 60%
        //     den_percent = powf(v, 0.8f) * 140.f; // shaped mapping to 0..140
        // }

        float pos   = patch.GetKnobValue(DaisyPatch::CTRL_3) * 100.f;
        // Pitch: reduce sensitivity by ~15% (scale down the prior multiplier)
        float pitch = (patch.GetKnobValue(DaisyPatch::CTRL_4) - 0.5f) * 8.5f; // was *10

        // Compact knob readouts: smaller font (4x8), two-letter prefixes, no units
        int   gsz_i = (int)(gsz * 1000.f + 0.5f);
        if(gsz_i > 999)
            gsz_i = 999; // clamp so display doesn't overflow
        int   pos_i = (int)(pos + 0.5f);
        float pit_f = pitch * 12.f; // semitoneish for display

        sprintf(buf, "Gs%u", (unsigned)gsz_i);
        patch.display.SetCursor(2, 28);
        patch.display.WriteString(buf, Font_4x8, true);

        sprintf(buf, "Po%u", (unsigned)pos_i);
        patch.display.SetCursor(34, 28);
        patch.display.WriteString(buf, Font_4x8, true);

        // Show pitch as integer semitones for clarity
        int pit_i = (int)round(pit_f);
        sprintf(buf, "Pi%+d", pit_i);
        patch.display.SetCursor(66, 28);
        patch.display.WriteString(buf, Font_4x8, true);
    }

    // Single expanded Buffer A bar (full width)
    const int bar_y = 44;
    const int bar_h = 12;
    const int bar_x = 2;
    const int bar_w = 124;
    Rectangle barA(bar_x, bar_y, bar_w, bar_h);
    patch.display.DrawRect(barA.GetX(),
                           barA.GetY(),
                           barA.GetX() + barA.GetWidth(),
                           barA.GetY() + barA.GetHeight(),
                           true,
                           false);
    // filled portion
    int fillA_w = (int)((barA.GetWidth() - 2) * bufferA_fill);
    if(fillA_w > 0)
    {
        Rectangle fillA(
            barA.GetX() + 1, barA.GetY() + 1, fillA_w, barA.GetHeight() - 2);
        patch.display.DrawRect(fillA.GetX(),
                               fillA.GetY(),
                               fillA.GetX() + fillA.GetWidth(),
                               fillA.GetY() + fillA.GetHeight(),
                               true,
                               true);
    }
    // Draw window rectangle overlay (shows current window within buffer)
    if(bufferA_full)
    {
        float win_start = ((float)bufferA.window_center - (float)bufferA.window_size / 2.0f) / (float)bufferA.GetLogicalSize();
        float win_width = (float)bufferA.window_size / (float)bufferA.GetLogicalSize();
        if(win_start < 0.f) win_start = 0.f;
        if(win_start + win_width > 1.f) win_width = 1.f - win_start;
        int win_x = barA.GetX() + 1 + (int)((barA.GetWidth() - 2) * win_start);
        int win_w = (int)((barA.GetWidth() - 2) * win_width);
        if(win_w < 2) win_w = 2;
        // Draw filled window rectangle with contrasting color (invert)
        patch.display.DrawRect(win_x, barA.GetY() + 1, win_x + win_w, barA.GetY() + barA.GetHeight() - 2, false, true);
        // Draw window outline for extra visibility
        patch.display.DrawRect(win_x, barA.GetY() + 1, win_x + win_w, barA.GetY() + barA.GetHeight() - 2, true, false);
    }
    // playhead if playing (visible vertical line + small triangle marker inside window)
    if(bufferA_playing && bufferA_full)
    {
        // Calculate window position and width in pixels
        float win_start = ((float)bufferA.window_center - (float)bufferA.window_size / 2.0f) / (float)bufferA.GetLogicalSize();
        float win_width = (float)bufferA.window_size / (float)bufferA.GetLogicalSize();
        if(win_start < 0.f) win_start = 0.f;
        if(win_start + win_width > 1.f) win_width = 1.f - win_start;
        int win_x = barA.GetX() + 1 + (int)((barA.GetWidth() - 2) * win_start);
        int win_w = (int)((barA.GetWidth() - 2) * win_width);
        if(win_w < 2) win_w = 2;
        // Playhead position within window
        int phx = win_x + (int)(win_w * bufferA.GetPlayheadPos());
        patch.display.DrawLine(phx,
                               barA.GetY() + 1,
                               phx,
                               barA.GetY() + barA.GetHeight() - 2,
                               true);
        // small triangle marker just above the window
        patch.display.DrawRect(
            phx - 1, barA.GetY() - 4, phx + 1, barA.GetY() - 2, true, true);
    }
    // highlight if full
    if(bufferA_full)
    {
        Rectangle invA = barA.Reduced(1);
        patch.display.DrawRect(invA.GetX(),
                               invA.GetY(),
                               invA.GetX() + invA.GetWidth(),
                               invA.GetY() + invA.GetHeight(),
                               true,
                               false);
    }

    // Always draw the title last so it's visible
    patch.display.SetCursor(40, 0);
    patch.display.WriteString("<3Dream", Font_7x10, true);

    patch.display.Update();
}

// Audio processing optimizations for highest quality
#define AUDIO_BLOCK_SIZE 32  // Process audio in blocks for better cache performance
#define MAX_GRAINS 32        // Limit simultaneous grains to prevent CPU overload
#define INTERPOLATION_ORDER 4 // Higher interpolation for smoother playback

// Audio quality monitoring and optimization
static float cpu_usage = 0.0f;
static uint32_t cpu_check_counter = 0;
static uint32_t last_cpu_time = 0;

// CPU usage monitoring for audio quality optimization
void UpdateCPUUsage() {
    cpu_check_counter++;
    if(cpu_check_counter >= SAMPLE_RATE / 10) { // Update 10 times per second
        uint32_t now = System::GetNow();
        uint32_t elapsed = now - last_cpu_time;
        cpu_usage = (float)elapsed / (float)(SAMPLE_RATE / 10) * 100.0f;
        last_cpu_time = now;
        cpu_check_counter = 0;

        // If CPU usage is too high, reduce quality settings
        if(cpu_usage > 80.0f) {
            // Could implement dynamic quality reduction here
        }
    }
}

void AudioCallback(AudioHandle::InputBuffer  in,
                   AudioHandle::OutputBuffer out,
                   size_t                    size)
{
    // CPU usage monitoring for quality optimization
    UpdateCPUUsage();

    // Audio processing and recording with block processing for better performance
    for(size_t block_start = 0; block_start < size; block_start += AUDIO_BLOCK_SIZE) {
        size_t block_end = (block_start + AUDIO_BLOCK_SIZE > size) ? size : block_start + AUDIO_BLOCK_SIZE;

        for(size_t i = block_start; i < block_end; i++) {
        // If buffers aren't ready, output silence and skip processing
        if(!buffers_ready)
        {
            out[0][i] = 0.f;
            out[1][i] = 0.f;
            out[2][i] = 0.f;
            out[3][i] = 0.f;
            continue;
        }

        float sample = in[0][i]; // mono input 1



        // Audio clock detection on Audio In 4
        static float audio_clock_prev = 0.0f;
        static uint32_t audio_clock_samples_since_pulse = 0;
        
        float audio_in_4 = in[3][i];
        bool audio_clock_trigger = false;
        
        if(audio_in_4 > audio_clock_threshold && audio_clock_prev <= audio_clock_threshold)
        {
            audio_clock_trigger = true;
            external_audio_clock_active = true;
            last_audio_clock_pulse = System::GetNow();
            audio_clock_samples_since_pulse = 0;
        }
        else if(audio_in_4 > audio_clock_threshold)
        {
            audio_clock_samples_since_pulse++;
        }
        
        audio_clock_prev = audio_in_4;
        
        // Detect if external clock is present (activity on audio in 4)
        external_clock_detected = external_audio_clock_active;

        // update rolling prebuffer
        prebuf[prebuf_write++] = sample;
        if(prebuf_write >= BUFFER_B_SIZE)
            prebuf_write = 0;

        // If recording buffer A, write sample into circular buffer
        bufferA.Write(sample);

        // Audio input clock detection (Audio In 4 for external clock)
        if(audio_clock_trigger)
        {
            external_audio_clock_active = true;
            last_audio_clock_pulse = System::GetNow();
            // Use audio input as primary clock when active
            
            // Output clock pulse on gate output (10ms pulse)
            patch.gate_output.Write(true);
            gate_pulse_end_time = System::GetNow() + 10; // 10ms pulse
            
            // DSP effect clock modulation for Audio In 4 (hardwired)
            // Chorus effect always responds to Audio In 4 clock
            if(dsp_effects[4].enabled) { // Chorus effect
                static bool audio4_toggle = false;
                audio4_toggle = !audio4_toggle;
                dsp_effects[4].param4 = audio4_toggle ? 0.8f : 0.2f;
                UpdateDSPEffectParameters(4);
            }
        }
        
        // Determine which clock source to use
        bool use_external_clock = external_audio_clock_active; // Only audio clock now
        
        // Timeout external clocks if no recent activity
        uint32_t now = System::GetNow();
        if(external_gate_clock_active && (now - last_gate_clock_pulse) > 2000) // 2 seconds timeout
        {
            external_gate_clock_active = false;
        }
        
        if(!external_gate_clock_active)
        {
            // No external clock detected, use MIDI clock if enabled
            use_external_clock = false;
            external_gate_clock_active = false;
        }

        // If main buffer is not full yet, do not produce audio or respond to knobs
        bool main_full_now = (bufferA.GetFillSamples() >= bufferA.GetLogicalSize());
        if(!main_full_now)
        {
            out[0][i] = 0.f;
            out[1][i] = 0.f;
            out[2][i] = 0.f;
            out[3][i] = 0.f;
            continue;
        }

        // Update Buffer A rate from Pitch knob (map semitones -> playback rate)
        // Only apply pitch when buffer is full and not actively recording so recording isn't affected by rate.
        if(bufferA.playing && !bufferA.IsRecording())
        {
            float pitch_knob = (patch.GetKnobValue(DaisyPatch::CTRL_4) - 0.5f) * 8.5f; // reduced sensitivity
            float semitones = pitch_knob * 12.f; // semitones
            
            // Add MIDI pitch control (only if Gran Midi is enabled)
            if(midi_note_active && gran_midi_enabled)
            {
                // Use MIDI note as base frequency, apply pitch bend and knob offset
                float midi_base_rate = midi_note_freq / 440.0f; // 440Hz = normal playback
                semitones += log2f(midi_base_rate) * 12.0f;
                semitones *= midi_pitch_bend; // apply pitch bend only when note is active
            }
            
            float rate = powf(2.f, semitones / 12.f);
            bufferA.SetRate(rate);
        }


        // --- Randomization logic: Only move playhead within window, not window itself ---
        static uint32_t random_counter = 0;
        static uint32_t random_interval = 1;
        static bool external_clock_triggered_randomization = false;
        
        if(randomization_amt > 0.0f)
        {
            if(use_external_clock)
            {
                // Sync randomization to external clock pulses
                if(audio_clock_trigger)
                {
                    // Trigger randomization on each clock pulse
                    playhead_within_window = (float)rand() / (float)RAND_MAX;
                    if(playhead_within_window < 0.f) playhead_within_window = 0.f;
                    if(playhead_within_window > 1.f) playhead_within_window = 1.f;
                    external_clock_triggered_randomization = true;
                }
                else
                {
                    external_clock_triggered_randomization = false;
                }
            }
            else
            {
                // Original MIDI/internal clock randomization
                // Map knob to interval: left = slow (max interval), right = fast (min interval)
                float min_ms = 800.0f, max_ms = 120.0f; // slower max speed
                float ms = min_ms * powf(max_ms / min_ms, randomization_amt); // exponential mapping
                random_interval = (uint32_t)((ms / 1000.0f) * SAMPLE_RATE);
                if(random_interval < 1) random_interval = 1;
                if(random_counter == 0)
                {
                    // Pick a new random playhead within window (0..1)
                    playhead_within_window = (float)rand() / (float)RAND_MAX;
                    // Clamp to [0,1] for safety
                    if(playhead_within_window < 0.f) playhead_within_window = 0.f;
                    if(playhead_within_window > 1.f) playhead_within_window = 1.f;
                }
                random_counter = (random_counter + 1) % random_interval;
            }
        }
        else
        {
            playhead_within_window = 0.0f; // always start at window start
            random_counter = 0;
            external_clock_triggered_randomization = false;
        }


        // --- Playback: window is set by knobs, playhead is set by randomizer ---
        float outA = 0.f;
        static bool was_randomizing = false;
        static float last_random_playhead = 0.f;
        if(bufferA.playing && bufferA_full)
        {
            size_t win_size = bufferA.window_size;
            if(randomization_amt > 0.0f)
            {
                if(use_external_clock)
                {
                    // External clock triggered randomization
                    if(external_clock_triggered_randomization)
                    {
                        bufferA.read_pos = playhead_within_window * (float)win_size;
                        last_random_playhead = playhead_within_window;
                    }
                    // Otherwise, let read_pos advance naturally
                }
                else
                {
                    // On each randomization event, jump to a new random position within the window
                    if(!was_randomizing || playhead_within_window != last_random_playhead)
                    {
                        bufferA.read_pos = playhead_within_window * (float)win_size;
                        last_random_playhead = playhead_within_window;
                    }
                    // Otherwise, let read_pos advance naturally
                }
            }
            was_randomizing = (randomization_amt > 0.0f);
            // When randomization is off, let playhead advance naturally (do not reset)
            outA = bufferA.Read();
        }

        // Buffer B playback (looping recorded portion) — use ReadLooping() so it plays circularly
        float outB = bufferB.ReadLooping();

        // Blend: dry = input, wet = granular (outA/outB)
        float dry = sample;
        float wet = (outB == 0.f) ? outA : outB;
        float out_sample = (1.0f - blend_amount) * dry + blend_amount * wet;
        // Output gain (reduced to prevent distortion + user controllable gain)
        out_sample *= 0.8f * (audio_gain / 100.0f);

        // advance progress counter based on current recorded size (safe for short captures)
        size_t curB = bufferB.GetCurrentRecordedSize();
        if(curB > 0)
        {
            bufferB_read_count++;
            if(bufferB_read_count >= curB)
                bufferB_read_count = 0;
        }

        // If freeze recording is active, write audio input channel 1 into bufferB (so freeze captures input only)
        if(freeze_recording && bufferB.IsRecording())
        {
            // write input sample into bufferB
            bufferB.Write(sample);
            // if sample buffer stopped recording because full, stop freeze recording and start playback
            if(!bufferB.IsRecording())
            {
                freeze_recording = false;
                bufferB_full     = true;
                bufferB.Play(true);
                bufferB_read_count = 0;
            }
        }

        // Apply DSP effects (always active when effects are enabled)
        // Initialize DSP outputs to zero (don't include main output here)
        float dsp_mix_left = 0.0f;
        float dsp_mix_right = 0.0f;

        for(int effect_idx = 0; effect_idx < 5; effect_idx++) {
            if(dsp_effects[effect_idx].enabled) {
                // Get input from specified channel
                float dsp_input = 0.0f; // default to silence
                if(dsp_effects[effect_idx].input_channel == 1) dsp_input = in[0][i]; // audio in 1
                else if(dsp_effects[effect_idx].input_channel == 2) dsp_input = in[1][i]; // audio in 2
                else if(dsp_effects[effect_idx].input_channel == 3) dsp_input = in[2][i]; // audio in 3

                float dsp_output = dsp_input;

                // Apply the effect
                float wet_signal = dsp_input;
                switch(effect_idx) {
                    case 0: // Ladder Filter (SVF lowpass)
                        ladder_filter.Process(wet_signal);
                        wet_signal = ladder_filter.Low();
                        break;
                    case 1: // Soap Filter (SVF bandpass)
                        soap_filter.Process(wet_signal);
                        wet_signal = soap_filter.Band();
                        break;
                    case 2: // Decimator
                        wet_signal = decimator.Process(wet_signal);
                        break;
                    case 3: // Wavefolder
                        wet_signal = wavefolder.Process(wet_signal);
                        break;
                    case 4: // Chorus
                        wet_signal = chorus.Process(wet_signal);
                        break;
                }

                // Apply wet/dry mix using param4
                float wet_dry = dsp_effects[effect_idx].param4; // 0.0 = dry, 1.0 = wet
                dsp_output = dsp_input * (1.0f - wet_dry) + wet_signal * wet_dry;

                // Mix DSP effects into stereo outputs 1-2
                dsp_mix_left += dsp_output * 0.5f; // left channel with level control
                dsp_mix_right += dsp_output * 0.5f; // right channel with level control
            }
        }

        // Set final output: main output plus DSP effects
        out[1][i] = out_sample + dsp_mix_left;
        out[2][i] = out_sample + dsp_mix_right;

            out[0][i] = out_sample;
            // out[1][i] and out[2][i] already contain DSP effect outputs
        // Compute CV voltages with scale quantization and Reese mode
        float cvA_volt, cvB_volt;
        
        // CV2 (MIDI V/Oct) - always quantized to scale
        if(cv2_midi_note_active) {
            // MIDI note as V/oct: MIDI note 60 (C4) = 0V, each octave = 1V
            cvB_volt = ((float)cv2_midi_note_number - 60.0f) / 12.0f; // V/oct standard
            // Apply scale quantization
            cvB_volt = QuantizeToScale(cvB_volt, cv_scale);
            // Clamp to -5V to +5V range
            cvB_volt = cvB_volt < -5.0f ? -5.0f : (cvB_volt > 5.0f ? 5.0f : cvB_volt);
            // Store the voltage for hold mode
            cv2_last_volt = cvB_volt;
        } else {
            // Use hold mode or output 0V when no note active
            if(cv_hold_mode == CV_HOLD_CV2 || cv_hold_mode == CV_HOLD_BOTH) {
                cvB_volt = cv2_last_volt;
            } else {
                cvB_volt = 0.0f; // 0V when hold disabled and no note active
            }
        }
        
        // CV1 - buffer playhead or Reese mode
        if(reese_mode && cv2_midi_note_active) {
            // Reese mode: output note one scale degree lower than CV2
            float reese_volt = ((float)cv2_midi_note_number - 60.0f) / 12.0f; // Same as CV2
            reese_volt -= 1.0f / 12.0f; // One semitone lower
            cvA_volt = QuantizeToScale(reese_volt, cv_scale);
            cvA_volt = cvA_volt < -5.0f ? -5.0f : (cvA_volt > 5.0f ? 5.0f : cvA_volt);
            // Store the voltage for hold mode
            cv1_last_volt = cvA_volt;
        } else {
            // Normal mode: use hold mode or buffer A playhead
            if((cv_hold_mode == CV_HOLD_CV1 || cv_hold_mode == CV_HOLD_BOTH) && cv1_midi_note_active) {
                // We have an active note, calculate and store it
                float note_volt = ((float)cv1_midi_note_number - 60.0f) / 12.0f;
                cvA_volt = QuantizeToScale(note_volt, cv_scale);
                cvA_volt = cvA_volt < -5.0f ? -5.0f : (cvA_volt > 5.0f ? 5.0f : cvA_volt);
                cv1_last_volt = cvA_volt;
            } else if(cv_hold_mode == CV_HOLD_CV1 || cv_hold_mode == CV_HOLD_BOTH) {
                // Hold the last voltage
                cvA_volt = cv1_last_volt;
            } else {
                // Output 0V when hold disabled and no note active
                cvA_volt = 0.0f;
            }
        }

        // Apply CV enable/disable
        if(!cv1_enabled) {
            cvA_volt = 0.0f; // Center voltage when disabled
        }
        // CV2 is always enabled (no separate control for it)

        // Write to CV outputs using DAC (map -5..+5 V -> 12-bit DAC 0..4095)
        auto clampf = [](float v, float lo, float hi) {
            return v < lo ? lo : (v > hi ? hi : v);
        };
        float    cvA_clamped = clampf(cvA_volt, -5.f, 5.f);
        float    cvB_clamped = clampf(cvB_volt, -5.f, 5.f);
        uint16_t dacA
            = static_cast<uint16_t>(((cvA_clamped + 5.f) / 10.f) * 4095.0f);
        uint16_t dacB
            = static_cast<uint16_t>(((cvB_clamped + 5.f) / 10.f) * 4095.0f);
        patch.seed.dac.WriteValue(DacHandle::Channel::ONE, dacA);
        patch.seed.dac.WriteValue(DacHandle::Channel::TWO, dacB);

        // Calculate CV sum before assigning to output
        float cv_sum = (out[0][i] + out[1][i] + out[2][i]) / 3.0f; // Average to prevent clipping
        
        // Mirror CV onto audio outputs 3 and 4 (scale to -1..1 for audio pipeline)
        out[2][i] = cv_sum; // CV sum of all outputs (Audio Out 3)
        // Audio Out 4: Available for other signals (MIDI V/Oct now on CV Out 2)
        out[3][i] = 0.0f; // Clear Audio Out 4
        } // End block processing inner loop
    } // End block processing outer loop

    // Update playhead positions for UI
    bufferA_play_pos = bufferA.GetPlayheadPos();
    // For bufferB, derive play_pos from the sample read counter and current recorded size
    {
        size_t curB = bufferB.GetCurrentRecordedSize();
        if(curB > 0)
            bufferB_play_pos = (float)bufferB_read_count / (float)curB;
        else
            bufferB_play_pos = 0.f;
    }

    // Menu feedback: show if Options menu is open
    // (display logic will draw the menu when menu_open==true)
    
    // Turn off gate output after pulse duration
    if(gate_pulse_end_time > 0 && System::GetNow() >= gate_pulse_end_time)
    {
        patch.gate_output.Write(false);
        gate_pulse_end_time = 0;
    }
}

// Uncomment to build a minimal diagnostic image that only tests the display + LED
// (useful to determine whether the flashing/display issue is in the environment or in the Granular logic)
#define DIAG_DISPLAY_TEST 1

// Boot-trace: toggles LED at several init checkpoints so we can see where startup fails.
// Disabled by default for normal boot flow.
//#define BOOT_TRACE 1

// Diagnostic audio monitor: when enabled, route ADC input 1 directly to outputs 1/2
// so we can verify the audio input-to-output path independent of buffer logic.
// (disabled in normal operation)

// Guards audio callback until buffers are initialized at boot to avoid touching
// uninitialized objects during early startup.

int main(void)
{
#if defined(BOOT_TRACE)
    // Step 1: initialize seed only (we already know this is OK from EarlyBoot)
    patch.seed.Configure();
    patch.seed.Init();
    // Indicate seed init success with two quick blinks
    for(int i=0;i<2;i++){ patch.seed.SetLed(true); System::Delay(120); patch.seed.SetLed(false); System::Delay(120); }

    // Step 2: try initializing the display directly (same as DaisyPatch::InitDisplay())
    {
        OledDisplay<SSD130x4WireSpi128x64Driver>::Config display_config;
        display_config.driver_config.transport_config.pin_config.dc = seed::D9;
        display_config.driver_config.transport_config.pin_config.reset = seed::D30;
        patch.display.Init(display_config);
        patch.display.Fill(false);
        patch.display.SetCursor(0, 0);
        patch.display.WriteString("display init", Font_6x8, true);
        patch.display.Update();
        for(int i=0;i<3;i++){ patch.seed.SetLed(true); System::Delay(80); patch.seed.SetLed(false); System::Delay(80); }
    }

    // Step 3: initialize SDRAM-backed buffers
    bufferA.Init(bufferA_storage, BUFFER_A_SIZE);
    bufferB.Init();
    for(int i=0;i<4;i++){ patch.seed.SetLed(true); System::Delay(60); patch.seed.SetLed(false); System::Delay(60); }

    // Step 2: initialize display and draw a message
    patch.display.Fill(false);
    patch.display.SetCursor(0, 0);
    patch.display.WriteString("display init", Font_6x8, true);
    patch.display.Update();
    for(int i=0;i<3;i++){ patch.seed.SetLed(true); System::Delay(100); patch.seed.SetLed(false); System::Delay(100); }

    // Step 3: try SDRAM-backed buffer initialization (this previously caused issues)
    bufferA.Init(bufferA_storage, BUFFER_A_SIZE);
    bufferB.Init();
    for(int i=0;i<4;i++){ patch.seed.SetLed(true); System::Delay(80); patch.seed.SetLed(false); System::Delay(80); }

    // Step 4: start ADC + audio (safe to start after buffers)
    patch.StartAdc();
    patch.StartAudio(AudioCallback);
    for(int i=0;i<5;i++){ patch.seed.SetLed(true); System::Delay(60); patch.seed.SetLed(false); System::Delay(60); }

    // If we reached here, indicate success with a steady blink pattern (slow)
    while(1)
    {
        patch.seed.SetLed(true);
        System::Delay(300);
        patch.seed.SetLed(false);
        System::Delay(300);
    }
#else
    // Safe startup modeled after PolyOsc: initialize hardware and start audio early,
    // but defer heavy buffer initialization until after audio is running. This helps
    // isolate early init crashes and allows us to incrementally enable features.
    patch.Init();

    // Initialize DAC for CV outputs
    patch.seed.dac.Init(DacHandle::Config());

    // Initialize USB first
    UsbHandle usb;
    usb.Init(UsbHandle::FS_INTERNAL);

    // Initialize MIDI handlers (both USB and UART)
    MidiUsbHandler::Config midi_usb_config;
    midi_usb_config.transport_config.periph = MidiUsbTransport::Config::INTERNAL;
    midi_usb.Init(midi_usb_config);
    midi_usb.StartReceive(); // Enable USB MIDI on startup

    MidiUartHandler::Config midi_uart_config;
    midi_uart_config.transport_config.periph = UartHandler::Config::Peripheral::USART_1;
    midi_uart_config.transport_config.rx = Pin(PORTB, 7); // MIDI RX pin
    midi_uart_config.transport_config.tx = Pin(PORTB, 6); // MIDI TX pin
    midi_uart.Init(midi_uart_config);
    midi_uart.StartReceive();

    // Quick boot indicator so we can tell if the firmware actually started
    patch.display.Fill(false);
    patch.display.SetCursor(25, 10);
    patch.display.WriteString("where Machines", Font_7x10, true);
    patch.display.SetCursor(30, 25);
    patch.display.WriteString("Come To Dream <3", Font_7x10, true);
#if DIAG_AUDIO_MONITOR
#endif
    patch.display.Update();
    patch.seed.SetLed(true);
    System::Delay(3000);

    // Start ADC and audio immediately (audio callback is guarded until buffers_ready)
    patch.StartAdc();
    patch.StartAudio(AudioCallback);

    // Defer SDRAM and buffer initialization until the system is running/stable
    System::Delay(500); // let things settle briefly
    bufferA.Init(bufferA_storage, BUFFER_A_SIZE);
    bufferB.Init();
    prebuf_write = 0;
    bufferA_fill = 0.f;
    bufferB_fill = 0.f;

    // Try to load saved settings
    bool settings_loaded = LoadSettings();
    if(!settings_loaded) {
        // Settings invalid or not found, use defaults (already set)
    }

    // Initialize DSP effects
    ladder_filter.Init(SAMPLE_RATE);
    soap_filter.Init(SAMPLE_RATE);
    decimator.Init();
    wavefolder.Init();
    chorus.Init(SAMPLE_RATE);

    // Update all DSP effect parameters with current settings
    for(int i = 0; i < 5; i++) {
        UpdateDSPEffectParameters(i);
    }

    // Initialize DSP effect parameters (only if settings not loaded)
    if(!settings_loaded) {
        for(int i = 0; i < 5; i++) {
            // Set meaningful default values instead of 0.5f for all
            switch(i) {
                case 0: // Ladder Filter
                    dsp_effects[i].param1 = 0.3f; // ~6kHz frequency
                    dsp_effects[i].param2 = 0.2f; // Low resonance
                    dsp_effects[i].param3 = 0.1f; // Low drive
                    dsp_effects[i].param4 = 0.5f; // 50% wet/dry
                    break;
                case 1: // Soap Filter
                    dsp_effects[i].param1 = 0.3f; // ~6kHz frequency
                    dsp_effects[i].param2 = 0.2f; // Low resonance
                    dsp_effects[i].param3 = 0.1f; // Low drive
                    dsp_effects[i].param4 = 0.5f; // 50% wet/dry
                    break;
                case 2: // Decimator
                    dsp_effects[i].param1 = 0.1f; // Low downsampling
                    dsp_effects[i].param2 = 0.1f; // Low bitcrush
                    dsp_effects[i].param3 = 0.0f; // Unused
                    dsp_effects[i].param4 = 0.5f; // 50% wet/dry
                    break;
                case 3: // Wavefolder
                    dsp_effects[i].param1 = 0.3f; // Moderate gain
                    dsp_effects[i].param2 = 0.5f; // Center offset
                    dsp_effects[i].param3 = 0.0f; // Unused
                    dsp_effects[i].param4 = 0.5f; // 50% wet/dry
                    break;
                case 4: // Chorus
                    dsp_effects[i].param1 = 0.3f; // Moderate delay
                    dsp_effects[i].param2 = 0.2f; // Low feedback
                    dsp_effects[i].param3 = 0.0f; // Unused
                    dsp_effects[i].param4 = 0.5f; // 50% wet/dry
                    break;
            }
            dsp_effects[i].locked1 = false;
            dsp_effects[i].locked2 = false;
            dsp_effects[i].locked3 = false;
            dsp_effects[i].locked4 = false;
            dsp_effects[i].midi_channel = i + 2; // Default: Ladder=2, Soap=3, Decimator=4, Wavefolder=5, Chorus=6
            dsp_effects[i].input_channel = 1; // default to audio in 1
            dsp_effects[i].enabled = (i == 0); // Enable Ladder Filter by default for testing
        }
    }

    // Mark buffers as ready so audio callback will begin normal processing
    buffers_ready = true;

    while(1)
    {
        // Process MIDI messages (both USB and UART)
        if(midi_use_usb) {
            midi_usb.Listen();
            while(midi_usb.HasEvents())
            {
                HandleMidiMessage(midi_usb.PopEvent());
            }
        } else {
            midi_uart.Listen();
            while(midi_uart.HasEvents())
            {
                HandleMidiMessage(midi_uart.PopEvent());
            }
        }
        
        UpdateControls();
        UpdateDisplay();
        System::Delay(10);
    }
#endif
}
