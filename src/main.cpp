#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>

#include <windows.h>

#include "MiniFB.h"
#include "m6502/m6502.h"

static M6502 cpu;

static uint8_t VRAM[8192];
static uint8_t RAM[8192];
static uint8_t ROM[128 << 10];

#define WATARA_SCREEN_WIDTH 160
#define WATARA_SCREEN_HEIGHT 160
static uint16_t SCREEN[WATARA_SCREEN_HEIGHT][WATARA_SCREEN_WIDTH];

#define RGB565(r, g, b) ((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3))
static const uint16_t watara_palette[] = {
        RGB565(0x7b, 0xc7, 0x7b),
        RGB565(0x52, 0xa6, 0x8c),
        RGB565(0x2e, 0x62, 0x60),
        RGB565(0x0d, 0x32, 0x2e),
};

static uint8_t *key_status = (uint8_t *) mfb_keystatus();

size_t rom_size;

static uint8_t irq_timer_counter = 0;
static uint8_t irq_enabled = true;
static uint8_t nmi_enabled = true;
static uint16_t timer_prescaler = 256;
static uint16_t bank = 0;

uint8_t lcd_registers[4] = {
        160, // LCD_X_Size
        160, // LCD_Y_Size
        0,   // X_Scroll
        0,   // Y_Scroll
};

static inline void readfile(const char *pathname, uint8_t *dst) {


    FILE *file = fopen(pathname, "rb");
    fseek(file, 0, SEEK_END);
    rom_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    fread(dst, sizeof(uint8_t), rom_size, file);
    fclose(file);
}


extern "C" uint8_t Rd6502(uint16_t address) {
    if (address <= 0x1FFF) {
        return RAM[address];
    }

    if (address == 0x2023) {
        return irq_timer_counter;
    }

    if (address == 0x2024) {
        printf("IRQ timer STATUS reset\n");
        irq_enabled = false;
        return 1;
    }

/* Reset Sound DMA IRQ flag:
        7       0
        ---------
        ???? ????

        When this register is read, it resets the audio DMA IRQ flag (clears status reg bit too)
 */
    if (address == 0x2025) {
        printf("Sound DMA STATUS reset\n");
        return 0;
    }

/* IRQ Status:
    7       0
    ---------
    ???? ??DT

    D: DMA Audio system (1 = DMA audio finished)
    T: IRQ Timer expired (1 = expired)
*/
    if (address == 0x2027) {
        irq_enabled = false;
        printf("IRQ STATUS read\n");
        return 0b11;
    }

    if (address >= 0x2000 && address <= 0x2007) {
        return lcd_registers[address & 3];
    }

/* 2020 - Controller

    Controller:
    7       0
    ---------
    SLAB UDLR

    S: Start button
    L: Select button
    A: A button
    B: B button
    U: Up on D-pad
    D: Down on D-pad
    L: Left on D-pad
    R: Right on D-pad

    Pressing a button results in that bit going LOW.  Bits are high for buttons that are not pressed. (i.e. the register returns FFh when no buttons are pressed).
*/
    if (address == 0x2020) {
        uint8_t buttons = 0b11111111;

        if (key_status[0x27]) buttons ^= 0b1;
        if (key_status[0x25]) buttons ^= 0b10;

        if (key_status[0x28]) buttons ^= 0b100;
        if (key_status[0x26]) buttons ^= 0b1000;

        if (key_status['X']) buttons ^= 0b10000;
        if (key_status['Z']) buttons ^= 0b100000;

        if (key_status[0x0d]) buttons ^= 0b10000000;
        if (key_status[0x20]) buttons ^= 0b10000000;

        return buttons;
    }


    // HI ROM. Last 16384 bytes of ROM
    if (address >= 0xC000) {
        return ROM[rom_size - 16384 + (address - 0xC000)]; // TODO: precalculate once
    }

    // LO ROM bank
    if (address >= 0x8000 && address <= 0xBFFF) {
        return ROM[bank + (address - 0x8000)];
    }

    if (address >= 0x4000) {
        return VRAM[address - 0x4000];
    }

    printf("READ >>>>>>>>> 0x%04x PC:%04x\r\n", address, cpu.PC.W);
    return 0xFF;
}

extern "C" void Wr6502(uint16_t address, uint8_t value) {
    if (address <= 0x1FFF) {
        RAM[address] = value;
        return;
    }

    if (address >= 0x2000 && address <= 0x2007) {
        lcd_registers[address & 3] = value;
        return;
    }

    if (address >= 0x2008 && address <= 0x200D) {
        printf("DMA register write\n");
        return;
    }

    if (address >= 0x2021 && address <= 0x2022) {
        printf("Link port\n");
        return;
    }

    if (address >= 0x2010 && address <= 0x201C) {
//        printf("SOUND register write\n");
        return;
    }

    if (address >= 0x2028 && address <= 0x202F) {
//        printf("SOUND register 2  write\n");
        // address & 4
        return;
    }
/* IRQ Timer:
    7       0
    ---------
    TTTT TTTT

    T: IRQ Timer.  Readable and writable.

    When a value is written to this register, the timer will start decrementing until it is 00h, then it will stay at 00h.  When the timer expires, it sets a flag which triggers an IRQ.  This timer is clocked by a prescaler, which is reset when the timer is written to.  This prescaler can divide the system clock by 256 or 16384.

    Writing 00h to the IRQ Timer register results in an instant IRQ. It does not wrap to FFh and continue counting;  it just stays at 00h and fires off an IRQ.
*/
    if (address == 0x2023) {
        irq_timer_counter = value;

        if (value == 0 && irq_enabled)
            Int6502(&cpu, INT_IRQ);

        printf("irq_timer_counter %d\n", value);
        timer_prescaler = 256;
        return;
    }

/*
 * System Control:
    7       0
    ---------
    BBBS D?IN

   B: Bank select bits for 8000-BFFF.
   N: Enable the NMI (1 = enable)
   I: Enable the IRQ (1 = enable)
   S: IRQ Timer prescaler.  1 = divide by 16384, 0 = divide by 256
   D: Display enable. 1 = enable display, 0 = disable display

   Writing to this register resets the LCD rendering system and makes it start rendering from the upper left corner, regardless of the bit pattern
 */
    if (address == 0x2026) {
        bank = (value >> 5) * 16384;
        nmi_enabled = 1 == (value & 1);
        irq_enabled = 2 == (value & 2);
        timer_prescaler = 1 == (value & 5) ? 16384 : 256;
        printf("timer_prescaler irq_enabled nmi_enabled  %d %d %d\n", timer_prescaler, irq_enabled, nmi_enabled);
        return;
    }

    if (address >= 0x4000) {
        VRAM[address - 0x4000] = value;
        return;
    }

    printf("WRITE >>>>>>>>> 0x%04x : 0x%02x PC:%04x\r\n", address, value, cpu.PC.W);
}

#define AUDIO_FREQ 44100
#define AUDIO_BUFFER_LENGTH ((AUDIO_FREQ /60 +1) * 2)

int16_t audiobuffer[AUDIO_BUFFER_LENGTH] = { 0 };

DWORD WINAPI SoundThread(LPVOID lpParam) {
    WAVEHDR waveHeaders[4];

    WAVEFORMATEX format = { 0 };
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = AUDIO_FREQ;
    format.wBitsPerSample = 16;
    format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    HANDLE waveEvent = CreateEvent(NULL, 1, 0, NULL);

    HWAVEOUT hWaveOut;
    waveOutOpen(&hWaveOut, WAVE_MAPPER, &format, (DWORD_PTR) waveEvent, 0, CALLBACK_EVENT);

    for (size_t i = 0; i < 4; i++) {
        int16_t audio_buffers[4][AUDIO_BUFFER_LENGTH * 2];
        waveHeaders[i] = {
                .lpData = (char *) audio_buffers[i],
                .dwBufferLength = AUDIO_BUFFER_LENGTH * 2,
        };
        waveOutPrepareHeader(hWaveOut, &waveHeaders[i], sizeof(WAVEHDR));
        waveHeaders[i].dwFlags |= WHDR_DONE;
    }
    WAVEHDR *currentHeader = waveHeaders;


    while (true) {
        if (WaitForSingleObject(waveEvent, INFINITE)) {
            fprintf(stderr, "Failed to wait for event.\n");
            return 1;
        }

        if (!ResetEvent(waveEvent)) {
            fprintf(stderr, "Failed to reset event.\n");
            return 1;
        }

// Wait until audio finishes playing
        while (currentHeader->dwFlags & WHDR_DONE) {
            //PSG_calc_stereo(&psg, audiobuffer, AUDIO_BUFFER_LENGTH);
            memcpy(currentHeader->lpData, audiobuffer, AUDIO_BUFFER_LENGTH * 2);
            waveOutWrite(hWaveOut, currentHeader, sizeof(WAVEHDR));

            currentHeader++;
            if (currentHeader == waveHeaders + 4) { currentHeader = waveHeaders; }
        }
    }
    return 0;
}

extern "C" byte Loop6502(M6502 *R) {
    static int timer = 0;

    if (irq_enabled && irq_timer_counter == 0) {
        printf("Counter expired, IRQ\n");
        irq_enabled = 0;
        return INT_IRQ;
    }

    if (timer_prescaler == 256) {
        irq_timer_counter--;
    } else {
        timer += 256;
        if (timer == timer_prescaler) {
            irq_timer_counter--;
            timer = 0;
        }
    }
    return INT_QUIT;
}

int main(int argc, char **argv) {
    int scale = 4;
    int ghosting_level = 0;

    if (!argv[1]) {
        printf("Usage: watara.exe <rom.bin> [scale_factor] [ghosting_level]\n");
        return -1;
    }

    if (argc > 2) {
        scale = atoi(argv[2]);
    }


    if (argc > 3) {
        ghosting_level = atoi(argv[3]);
    }

    if (!mfb_open("Watara Supervision", WATARA_SCREEN_WIDTH, WATARA_SCREEN_HEIGHT, scale))
        return 0;

    CreateThread(NULL, 0, SoundThread, NULL, 0, NULL);

    readfile(argv[1], ROM);
    memset(VRAM, 0x00, sizeof(VRAM));
    memset(RAM, 0x00, sizeof(RAM));
    Reset6502(&cpu);

    cpu.IPeriod = 256;

    for (;;) {
        for (int i = 0; i < 256; i++) {
            Run6502(&cpu);

            auto *screen = (uint16_t *) &SCREEN;
            int offset = lcd_registers[2] / 4 + lcd_registers[3] * 0x30;

            for (int y = 0; y < WATARA_SCREEN_HEIGHT; y++) {
                auto *vram_line = VRAM + offset;
                uint8_t pixel = *vram_line++;

                for (int x = 0; x < 160;) {
                    screen[x++] = watara_palette[pixel & 3];
                    pixel >>= 2;
                    screen[x++] = watara_palette[pixel & 3];
                    pixel >>= 2;
                    screen[x++] = watara_palette[pixel & 3];
                    pixel >>= 2;
                    screen[x++] = watara_palette[pixel & 3];

                    pixel = *vram_line++;
                }
                screen += 160;
                offset += 0x30;
            }
        }

        if (nmi_enabled)
            Int6502(&cpu, INT_NMI);


        if (mfb_update(SCREEN, 60) == -1)
            return 1;
    }
}
