/** Live Recording Test for Daisy Patch
 ** Records 10 seconds of audio input to SD card, then loads it back into buffer
 ** Gate 1: Trigger recording start
 */

#include "libDaisy/src/daisy_patch.h"
#include "libDaisy/src/util/WavWriter.h"
#include "libDaisy/src/util/WavPlayer.h"
//#include <string.h>

using namespace daisy;

// Constants
static constexpr float  kSampleRate      = 48000.f;
static constexpr size_t kTransferSize    = 16384;
static constexpr size_t kRecordSeconds   = 10;
static constexpr size_t kRecordSamples   = kRecordSeconds * kSampleRate;
static constexpr size_t kBufferSizeSamps = kRecordSamples * 2; // stereo

// Hardware
DaisyPatch               patch;
FatFSInterface           fsi;
// Recording buffer in SDRAM (too big for SRAM)
float DSY_SDRAM_BSS recording_buffer[kBufferSizeSamps];
size_t record_position = 0;

// State
enum class State
{
    IDLE,
    RECORDING,
    SAVING,
    PLAYING
};

State   current_state  = State::IDLE;
size_t  record_counter = 0;
bool    gate_prev      = false;
char    current_file[64];
uint8_t file_counter   = 0;
bool    sd_mounted     = false;

void AudioCallback(AudioHandle::InputBuffer  in,
                   AudioHandle::OutputBuffer out,
                   size_t                    size)
{
    // Read gate input
    bool gate_now = patch.gate_input[DaisyPatch::GATE_IN_1].State();
    bool gate_trig = gate_now && !gate_prev;
    gate_prev = gate_now;

    // State machine
    switch(current_state)
    {
        case State::IDLE:
            if(gate_trig)
            {
                // Start recording to RAM buffer
                record_counter = 0;
                record_position = 0;
                current_state  = State::RECORDING;
                patch.seed.SetLed(true); // LED on during recording
            }
            // Pass through audio
            for(size_t i = 0; i < size; i++)
            {
                out[0][i] = in[0][i];
                out[1][i] = in[1][i];
            }
            break;

        case State::RECORDING:
            for(size_t i = 0; i < size; i++)
            {
                // Record stereo to buffer
                if(record_position < kBufferSizeSamps - 1)
                {
                    recording_buffer[record_position++] = in[0][i];
                    recording_buffer[record_position++] = in[1][i];
                }
                record_counter++;

                // Pass through audio
                out[0][i] = in[0][i];
                out[1][i] = in[1][i];

                // Check if done recording
                if(record_counter >= kRecordSamples)
                {
                    // Move to saving state (will save in main loop, not in audio callback)
                    current_state = State::SAVING;
                    patch.seed.SetLed(false);
                    break;
                }
            }
            break;

        case State::SAVING:
        case State::PLAYING:
            // Play back from RAM buffer (looping)
            for(size_t i = 0; i < size; i++)
            {
                if(record_position < kBufferSizeSamps - 1)
                {
                    out[0][i] = recording_buffer[record_position++];
                    out[1][i] = recording_buffer[record_position++];
                }
                else
                {
                    // Loop back to start
                    record_position = 0;
                    out[0][i] = recording_buffer[record_position++];
                    out[1][i] = recording_buffer[record_position++];
                }
            }
            break;
    }
}

int main(void)
{
    // Step 1: Basic hardware init
    patch.seed.Configure();
    patch.seed.Init();
    
    // Removed hardware init blink sequence to reduce test clutter
    
    // Step 2: Full patch init
    patch.Init();
    
    // Blink 5 times - patch init OK
    for(int i = 0; i < 5; i++)
    {
        patch.seed.SetLed(true);
        System::Delay(100);
        patch.seed.SetLed(false);
        System::Delay(100);
    }
    System::Delay(500);
    
    // Initialize SD Card
    SdmmcHandler sd;
    SdmmcHandler::Config sd_cfg;
    sd_cfg.Defaults();
    sd_cfg.speed = SdmmcHandler::Speed::FAST;
    sd_cfg.width = SdmmcHandler::BusWidth::BITS_4;
    sd.Init(sd_cfg);

    // Try to mount SD card
    FatFSInterface::Config fsi_cfg;
    fsi_cfg.media = FatFSInterface::Config::MEDIA_SD;
    fsi.Init(fsi_cfg);
    
    f_mount(&fsi.GetSDFileSystem(), "/", 1);
    FRESULT fres = f_mount(&fsi.GetSDFileSystem(), "/", 1);
    
    if(fres == FR_OK)
    {
        sd_mounted = true;
        // Blink 7 times - SD mounted OK
        for(int i = 0; i < 7; i++)
        {
            patch.seed.SetLed(true);
            System::Delay(100);
            patch.seed.SetLed(false);
            System::Delay(100);
        }
    }
    else
    {
        sd_mounted = false;
        // Blink 3 long - SD mount failed
        for(int i = 0; i < 3; i++)
        {
            patch.seed.SetLed(true);
            System::Delay(300);
            patch.seed.SetLed(false);
            System::Delay(300);
        }
    }
    System::Delay(500);
    
    // Start audio
    patch.StartAdc();
    patch.StartAudio(AudioCallback);

    // Main loop - update display
    while(1)
    {
        // Handle saving state (do this in main loop, not audio callback)
        if(current_state == State::SAVING && sd_mounted)
        {
            // Create filename
            sprintf(current_file, "rec%03d.wav", file_counter++);
            
            // Create WAV file manually
            FIL file;
            if(f_open(&file, current_file, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK)
            {
                // WAV file header structure
                struct WavHeader
                {
                    char     riff[4];        // "RIFF"
                 //   uint32_t file_size;      // File size - 8
                    char     wave[4];        // "WAVE"
                    char     fmt[4];         // "fmt "
                //    uint32_t fmt_size;       // 16 for PCM
                    uint16_t audio_format;   // 1 for PCM
                    uint16_t num_channels;   // 2 for stereo
                 //  uint32_t sample_rate;    // 48000
                //    uint32_t byte_rate;      // sample_rate * num_channels * bytes_per_sample
                    uint16_t block_align;    // num_channels * bytes_per_sample
                    uint16_t bits_per_sample;// 16
                    char     data[4];        // "data"
                  //  uint32_t data_size;      // num_samples * num_channels * bytes_per_sample
                } header;
                
                // Fill header
                memcpy(header.riff, "RIFF", 4);
                memcpy(header.wave, "WAVE", 4);
                memcpy(header.fmt, "fmt ", 4);
                memcpy(header.data, "data", 4);
              //  header.fmt_size = 16;
                header.audio_format = 1; // PCM
                header.num_channels = 2;
           //     header.sample_rate = (uint32_t)kSampleRate;
                header.bits_per_sample = 16;
              //  header.byte_rate = header.sample_rate * header.num_channels * 2;
                header.block_align = header.num_channels * 2;
              //  header.data_size = kBufferSizeSamps * 2; // 2 bytes per sample
              //  header.file_size = 36 + header.data_size;
                
                // Write header
                UINT bytes_written;
                f_write(&file, &header, sizeof(header), &bytes_written);
                
                // Write audio data in chunks
                int16_t transfer_buffer[kTransferSize];
                size_t samples_written = 0;
                
                while(samples_written < kBufferSizeSamps)
                {
                    size_t chunk_size = kTransferSize;
                    if(samples_written + chunk_size > kBufferSizeSamps)
                        chunk_size = kBufferSizeSamps - samples_written;
                    
                    // Convert float to int16
                    for(size_t i = 0; i < chunk_size; i++)
                    {
                        float sample = recording_buffer[samples_written + i];
                        sample = sample > 1.0f ? 1.0f : (sample < -1.0f ? -1.0f : sample);
                        transfer_buffer[i] = (int16_t)(sample * 32767.0f);
                    }
                    
                    f_write(&file, transfer_buffer, chunk_size * sizeof(int16_t), &bytes_written);
                    samples_written += chunk_size;
                }
                
                f_close(&file);
            }
            
            // Now start playback
            record_position = 0;
            current_state = State::PLAYING;
        }
        else if(current_state == State::SAVING && !sd_mounted)
        {
            // No SD card, just go straight to playback
            record_position = 0;
            current_state = State::PLAYING;
        }
        
        // Update LED
        patch.seed.SetLed((System::GetNow() & 1023) > 511);
        
        // Update display
        patch.display.Fill(false);
        patch.display.SetCursor(0, 0);
        patch.display.WriteString("RecordTest", Font_6x8, true);
        
        if(!sd_mounted)
        {
            patch.display.SetCursor(70, 0);
            patch.display.WriteString("NoSD", Font_6x8, true);
        }
        
        patch.display.SetCursor(0, 15);
        switch(current_state)
        {
            case State::IDLE:
                patch.display.WriteString("IDLE - Gate 1", Font_6x8, true);
                patch.display.SetCursor(0, 30);
                patch.display.WriteString("to record", Font_6x8, true);
                break;
            case State::RECORDING:
                patch.display.WriteString("RECORDING...", Font_6x8, true);
                char buf[32];
                sprintf(buf, "%d/%d sec", (int)(record_counter / kSampleRate), kRecordSeconds);
                patch.display.SetCursor(0, 30);
                patch.display.WriteString(buf, Font_6x8, true);
                break;
            case State::SAVING:
                patch.display.WriteString("SAVING TO SD", Font_6x8, true);
                break;
            case State::PLAYING:
                patch.display.WriteString("PLAYING LOOP", Font_6x8, true);
                patch.display.SetCursor(0, 30);
                if(sd_mounted)
                {
                    patch.display.WriteString(current_file, Font_6x8, true);
                }
                break;
        }
        
        patch.display.Update();
        System::Delay(50);
    }
}
