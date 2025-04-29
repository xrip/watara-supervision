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

// Global state
static SV_CHANNEL channels[2];

// Function to initialize the sound system
inline void sound_init() {
    memset(channels, 0, sizeof(channels));
}

// Function to reset sound state
inline void sound_reset() {
    memset(channels, 0, sizeof(channels));
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

// Function to generate a single sample
// Called at sample rate (44100Hz)
inline int16_t sound_generate_sample() {
    int16_t output = 0;

    // Process both channels
    for (int i = 0; i < 2; i++) {
        SV_CHANNEL *channel = &channels[i];

        if (channel->enabled && channel->size > 0) {
            // Determine if waveform is in high or low state
            const uint16_t threshold = get_duty_threshold(channel);

            // Generate square wave based on position and duty cycle
            // This emulates inverted duty cycles compared to the original code
            if (channel->position < threshold) {
                output += channel->volume;
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

    // Clamp output to 8-bit range (-128 to 127)
    // if (output > 127) output = 127;
    // if (output < -128) output = -128;

    return output << 8;
}

#endif //SOUND_H
