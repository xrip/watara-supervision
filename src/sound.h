#ifndef SOUND_H
#define SOUND_H

#define UNSCALED_CLOCK 4000000
#define SAMPLE_RATE 44100

// Define duty cycles as percentages of the waveform period
typedef enum {
    DUTY_12_5_PERCENT = 0, // 12.5% duty cycle
    DUTY_25_PERCENT, // 25% duty cycle
    DUTY_50_PERCENT, // 50% duty cycle
    DUTY_75_PERCENT // 75% duty cycle
} DUTY_CYCLE;

// Square wave channel
typedef struct {
    // Registers
    uint8_t reg[4]; // Raw register values
    uint8_t duty; // Duty cycle (0-3)
    uint8_t volume; // Volume level (0-15)
    uint16_t length; // Length counter from register 3

    // Internal state
    bool enabled; // Channel enabled flag
    uint16_t position; // Current position within waveform
    uint16_t size; // Size of one complete waveform in samples
} SV_CHANNEL;

// Noise channel
typedef struct {
    // Registers
    uint8_t reg[3]; // Raw register values (CH4_Freq_Vol, CH4_Length, CH4_Control)
    uint8_t volume; // Volume level (0-15)
    uint8_t frequency; // Frequency setting (0-15)
    uint8_t length; // Length counter

    // Control flags
    bool noise_enable; // Noise enable flag (bit 4 of CH4_Control)
    bool left_output; // Mix with left channel (bit 2 of CH4_Control)
    bool right_output; // Mix with right channel (bit 1 of CH4_Control)
    bool continuous_mode; // Enable continuously vs using length (bit 1 of CH4_Control)
    bool lfsr_mode; // LFSR length: 1=15-bit, 0=7-bit (bit 0 of CH4_Control)

    // Internal state
    uint16_t divisor; // Clock divisor based on frequency setting
    uint16_t position; // Current position within period
    uint16_t lfsr; // Linear feedback shift register for noise generation
} SV_NOISE_CHANNEL;

// Global state
static SV_CHANNEL channels[2];
static SV_NOISE_CHANNEL noise_channel;

// Function to initialize the sound system
inline void sound_init() {
    memset(channels, 0, sizeof(channels));
    memset(&noise_channel, 0, sizeof(noise_channel));

    // Initialize LFSR with non-zero value to avoid getting stuck
    noise_channel.lfsr = 0x7FFF;

    // Default the divisor to something reasonable
    noise_channel.divisor = 8;
}

// Register write handler for square wave channels
inline void sound_wave_write(const int channel_index, const int reg_index, const uint8_t value) {
    if (channel_index < 0 || channel_index > 1 || reg_index < 0 || reg_index > 3) {
        return; // Invalid parameters
    }

    SV_CHANNEL *channel = &channels[channel_index];
    channel->reg[reg_index] = value;

    switch (reg_index) {
        case 0:
        case 1: {
            // Update period from registers 0 and 1
            const uint16_t period_value = channel->reg[0] | (channel->reg[1] & 0x07) << 8;

            // Calculate size (samples per waveform) based on period
            // Size = (SampleRate * Period * 32) / ClockRate
            channel->size = (uint16_t) ((uint32_t) SAMPLE_RATE * ((period_value + 1) << 5) / UNSCALED_CLOCK);

            // Reset position at period change to avoid clicks
            channel->position = 0;
            break;
        }

        case 2: {
            // Update duty cycle and volume
            channel->enabled = (value & 0x40) != 0;
            channel->duty = (value & 0x30) >> 4;
            channel->volume = value & 0x0F;
            break;
        }

        case 3: {
            // Update length counter
            channel->length = value + 1;
            break;
        }
    }
}

// Register write handler for noise channel
inline void sound_noise_write(const int reg_index, const uint8_t value) {
    if (reg_index < 0 || reg_index > 2) {
        return; // Invalid parameters - only 3 registers (0-2)
    }

    noise_channel.reg[reg_index] = value;

    switch (reg_index) {
        case 0: { // CH4_Freq_Vol - Frequency and Volume
            // Update frequency and volume settings
            noise_channel.frequency = (value & 0xF0) >> 4;
            noise_channel.volume = value & 0x0F;

            // Set divisor based on frequency value
            static const uint32_t divisors[16] = {
                8,      // 0 - 500KHz
                32,     // 1 - 125KHz
                64,     // 2 - 62.5KHz
                128,    // 3 - 31.25KHz
                256,    // 4 - 15.625KHz
                512,    // 5 - 7.8125KHz
                1024,   // 6 - 3.90625KHz
                2048,   // 7 - 1.953KHz
                4096,   // 8 - 976.56Hz
                8192,   // 9 - 488.28Hz
                16384,  // A - 244.14Hz
                32768,  // B - 122.07Hz
                65536,  // C - 61.035Hz
                131072, // D - 30.52Hz
                65536,  // E - 61.035Hz (duplicate of C)
                131072  // F - 30.52Hz (duplicate of D)
            };

            noise_channel.divisor = divisors[noise_channel.frequency];
            break;
        }

        case 1: { // CH4_Length - Length counter
            // Update length counter
            noise_channel.length = value;
            break;
        }

        case 2: { // CH4_Control - Control flags
            // Parse control flags
            noise_channel.noise_enable = (value & 0x10) != 0;
            noise_channel.left_output = (value & 0x04) != 0;
            noise_channel.right_output = (value & 0x02) != 0;
            noise_channel.continuous_mode = (value & 0x01) != 0;
            noise_channel.lfsr_mode = (value & 0x01) != 0;

            // Reset LFSR to all 1's when writing to control register
            noise_channel.lfsr = 0x7FFF; // 15 bits all set to 1

            // Reset position counter
            noise_channel.position = 0;
            break;
        }
    }
}

// Helper function: get threshold position for current duty cycle
static inline uint16_t get_duty_threshold(const SV_CHANNEL *channel) {
    switch (channel->duty) {
        case DUTY_12_5_PERCENT:
            return channel->size / 8; // 12.5%
        case DUTY_25_PERCENT:
            return channel->size / 4; // 25%
        case DUTY_50_PERCENT:
            return channel->size / 2; // 50%
        case DUTY_75_PERCENT:
            return (channel->size * 3) / 4; // 75%
        default:
            return channel->size / 2; // Default to 50%
    }
}

// Helper function: update LFSR for noise generation
static inline void update_noise_lfsr() {
    // Calculate feedback bit using bits 14 and 15 (actually indices 13 and 14 in zero-based)
    uint16_t bit0 = noise_channel.lfsr & 1;
    uint16_t bit1 = (noise_channel.lfsr & 2) >> 1;
    uint16_t feedback = bit0 ^ bit1;

    // Shift the register right by 1
    noise_channel.lfsr >>= 1;

    // Apply feedback to highest bit (bit 14 for 15-bit mode, bit 6 for 7-bit mode)
    if (feedback) {
        if (noise_channel.lfsr_mode) {
            // 15-bit mode: feedback to bit 14
            noise_channel.lfsr |= 0x4000;
        } else {
            // 7-bit mode: truncate to 7 bits and feedback to bit 6
            noise_channel.lfsr &= 0x7F;  // Mask to keep only 7 bits
            if (feedback) {
                noise_channel.lfsr |= 0x40;  // Set bit 6
            }
        }
    }

    // Ensure LFSR never becomes 0 (would get stuck)
    if (noise_channel.lfsr == 0) {
        noise_channel.lfsr = noise_channel.lfsr_mode ? 0x7FFF : 0x7F;
    }
}

// Function to generate a single sample
// Called at sample rate (44100Hz)
inline int16_t sound_generate_sample() {
    int16_t left_output = 0;
    int16_t right_output = 0;
    int16_t final_output = 0;

    // Process both square wave channels
    for (int i = 0; i < 2; i++) {
        SV_CHANNEL *channel = &channels[i];

        if (channel->enabled && channel->size > 0) {
            // Determine if waveform is in high or low state
            const uint16_t threshold = get_duty_threshold(channel);

            // Generate square wave based on position and duty cycle
            if (channel->position < threshold) {
                // Mix into both channels for now (could be updated if Supervision has panning)
                left_output += channel->volume;
                right_output += channel->volume;
            }

            // Advance position
            channel->position++;
            if (channel->position >= channel->size) {
                channel->position = 0;

                // Decrement length counter if active
                if (channel->length > 0) {
                    channel->length--;
                    if (channel->length == 0) {
                        channel->enabled = false; // Disable when length expires
                    }
                }
            }
        }
    }

    // Process noise channel
    if (noise_channel.noise_enable) {
        // Check if we should update the LFSR (based on frequency/divisor)
        // We need to scale the divisor to match our sample rate
        uint16_t noise_period = (uint16_t)((uint32_t)SAMPLE_RATE * noise_channel.divisor / UNSCALED_CLOCK);
        if (noise_period == 0) noise_period = 1; // Avoid division by zero

        noise_channel.position++;
        if (noise_channel.position >= noise_period) {
            noise_channel.position = 0;

            // Update LFSR
            update_noise_lfsr();

            // Handle length counter if not in continuous mode
            if (!noise_channel.continuous_mode && noise_channel.length > 0) {
                noise_channel.length--;
                if (noise_channel.length == 0) {
                    noise_channel.noise_enable = false; // Disable when length expires
                }
            }
        }

        // Generate noise output based on LFSR state (use lowest bit)
        if (noise_channel.noise_enable && (noise_channel.lfsr & 1)) {
            // Mix to appropriate output channels based on control flags
            if (noise_channel.left_output) {
                left_output += noise_channel.volume;
            }
            if (noise_channel.right_output) {
                right_output += noise_channel.volume;
            }
        }
    }



    // Clamp output to volume range (0-15) and scale to 16-bit range
    // if (final_output > 15) final_output = 15;
    // if (final_output < 0) final_output = 0;

    return (int16_t)(final_output << 8); // Scale to use more of 16-bit range
}

#endif //SOUND_H
