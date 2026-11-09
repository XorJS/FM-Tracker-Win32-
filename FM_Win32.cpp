// FM Tracker Win32 port of FM.ASM by Jean-Sebastien Royer.
// Native C++17 implementation; no DOS runtime or inline assembly.
// See README.md for intentional bug fixes and compatibility differences.
#include <array>
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <cstdio>
#include "FM_Data.h"
#include "vendor/opl3.h"

namespace fm
{
    constexpr int Channels = 18, Rows = 64, RowBytes = Channels * 3, PatternBytes = RowBytes * Rows;
    constexpr int SampleRate = 48000;
    constexpr uint64_t PitClock = 1193182, PitDivisor = 7786;
    using PatternData = std::array<uint8_t, PatternBytes>;
    using ScreenData = std::array<uint8_t, 8000>;
    const char Hex[] = "0123456789ABCDEF";
    const char * NoteNames[] = { "C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-", "..", "..", "..", ".." };
    constexpr int Operators[] = { 0, 1, 2, 8, 9, 10, 16, 17, 18 };
    struct Song
    {
        uint8_t speed = 0, bpm = 0;
        std::array<uint8_t, 9216> instruments{};
        std::array<uint8_t, 256> order{};
        std::array<PatternData, 256> patterns{};
        Song()
        {
            std::copy_n(instruments_table, 9216, instruments.begin());
            clear();
        }
        void clear()
        {
            order.fill(0);
            for (auto & p : patterns)
                p.fill(255);
        }
        uint16_t ticksPerRow() const { return uint16_t(speed | (uint16_t(bpm) << 8)); }
        std::vector<uint8_t> save() const
        {
            std::vector<uint8_t> out = { 'J', 'S', 'R', '_', 'F', 'M', 'T', '~', speed, bpm };
            out.insert(out.end(), instruments.begin(), instruments.end());
            out.insert(out.end(), order.begin(), order.end());
            std::array<bool, 256> used{};
            for (uint8_t n : order)
                if (n != 255) used[n] = true;
            unsigned count = 0;
            for (bool b : used)
                count += b;
            out.push_back(uint8_t(count));
            for (int n = 0; n < 255; ++n)
                if (used[n])
                {
                    out.push_back(uint8_t(n));
                    out.insert(out.end(), patterns[n].begin(), patterns[n].end());
                }
            return out;
        }
        void load(const std::vector<uint8_t> & in)
        {
            constexpr size_t Header = 8 + 2 + 9216 + 256 + 1; // 9483, not the ASM comment's 9683.
            if (in.size() < Header || std::memcmp(in.data(), "JSR_FMT~", 8)) throw std::runtime_error("Not a valid JSR_FMT~ file.");
            size_t count = in[Header - 1];
            if (in.size() != Header + count * (1 + PatternBytes)) throw std::runtime_error("Truncated FMT file or unexpected trailing data.");
            // Stage changes so failed reads never damage the current song.
            Song next;
            next.speed = in[8];
            next.bpm = in[9];
            std::copy_n(in.begin() + 10, 9216, next.instruments.begin());
            std::copy_n(in.begin() + 9226, 256, next.order.begin());
            std::array<bool, 256> seen{};
            size_t off = Header;
            for (size_t i = 0; i < count; ++i)
            {
                unsigned n = in[off++];
                if (n == 255 || seen[n]) throw std::runtime_error("Invalid or duplicate FMT pattern number.");
                seen[n] = true;
                std::copy_n(in.begin() + off, PatternBytes, next.patterns[n].begin());
                off += PatternBytes;
            }
            *this = std::move(next);
        }
    };

    struct Tracker
    {
        Song song;
        ScreenData screen{};
        opl3_chip chip{};
        uint8_t orderPos = 0, instrument = 0, octave = 0;
        int row = 0, channel = 0, field = 0;
        bool orderMode = false, instrumentMode = false, naming = false;
        int namePos = 0;
        std::array<uint8_t, 24> nameEdit{};
        bool highNibble = true;
        uint8_t previousNibble = 0;
        enum PlayMode
        {
            Stopped,
            Pattern,
            Music
        } playMode = Stopped;
        uint32_t musicPos = 0, patternPos = 0, counter = 1;
        uint64_t pitPhase = 0;
        uint64_t rowEvents = 0;
        std::vector<std::pair<uint16_t, uint8_t>> * trace = nullptr;
        Tracker() { reset(); }
        uint8_t * cell() { return song.patterns[song.order[orderPos]].data() + row * RowBytes + channel * 3; }
        void output(unsigned bank, unsigned reg, uint8_t value)
        {
            uint16_t address = uint16_t((bank << 8) | reg);
            if (trace) trace->emplace_back(address, value);
            OPL3_WriteRegBuffered(&chip, address, value);
        }
        void reset()
        {
            OPL3_Reset(&chip, SampleRate);
            for (unsigned i = 0; i < 256; ++i)
            {
                output(0, (256 - i) & 255, 0);
                output(1, (256 - i) & 255, 0);
            }
            output(0, 2, 255);
            output(0, 4, 128);
            output(0, 4, 1);
            output(0, 0xbd, 0);
            output(1, 5, 1);
        }
        void stop()
        {
            playMode = Stopped;
            for (unsigned c = 0; c < 9; ++c)
                for (unsigned bank = 0; bank < 2; ++bank)
                {
                    output(bank, 0xa0 + c, 0);
                    output(bank, 0xb0 + c, 0);
                }
        }
        void start(PlayMode mode)
        {
            stop();
            if (!song.ticksPerRow()) return;
            playMode = mode;
            musicPos = patternPos = 0;
            counter = 1;
            pitPhase = 0;
        }
        void playRow(const uint8_t * cells, bool patternPlayback)
        {
            for (unsigned bank = 0; bank < 2; ++bank)
                for (unsigned c = 0; c < 9; ++c)
                {
                    const uint8_t * n = cells + (bank * 9 + c) * 3;
                    if (n[0] == 255) continue; // Empty cell does not key-off a sustained note.
                    // Invalid low nibbles used to index past N_Val into NotesId.
                    if ((n[0] & 15) >= 12) continue;
                    output(bank, 0xa0 + c, 0);
                    output(bank, 0xb0 + c, 0);
                    if (bank && patternPlayback) output(bank, 0xb0 + c, 0); // duplicate write in ASM
                    const uint8_t * ins = song.instruments.data() + unsigned(n[1]) * 36;
                    for (unsigned op = 0; op < 2; ++op)
                    {
                        unsigned slot = Operators[c] + op * 3;
                        output(bank, 0x20 + slot, ins[op * 6]);
                        output(bank, 0x40 + slot, uint8_t(n[2] ^ 255)); // original ignores instrument TL/KSL byte
                        output(bank, 0x60 + slot, ins[op * 6 + 2]);
                        output(bank, 0x80 + slot, ins[op * 6 + 3]);
                        output(bank, 0xe0 + slot, ins[op * 6 + 4]);
                        output(bank, 0xc0 + c, uint8_t(ins[op * 6 + 5] | (bank ? 0x20 : 0x10)));
                    }
                    unsigned note = n[0] & 15;
                    unsigned freq = n_val[note * 2] | (unsigned(n_val[note * 2 + 1]) << 8);
                    output(bank, 0xa0 + c, uint8_t(freq));
                    output(bank, 0xb0 + c, uint8_t((freq >> 8) | ((n[0] >> 4) << 2) | 32));
                }
            ++rowEvents;
        }
        void tick()
        {
            if (playMode == Stopped || --counter) return;
            counter = song.ticksPerRow(); // A zero value during playback wraps like the 32-bit DOS counter.
            if (playMode == Pattern)
            {
                playRow(song.patterns[song.order[orderPos]].data() + (patternPos & 63) * RowBytes, true);
                ++patternPos;
            }
            else
            {
                if (song.order[0] == 255)
                {
                    musicPos = 0;
                    return;
                }
                // Bound the order list and restart at order[0], not arbitrary pattern 0.
                if ((musicPos >> 6) >= 256 || song.order[musicPos >> 6] == 255) musicPos = 0;
                orderPos = uint8_t(musicPos >> 6);
                row = int(musicPos & 63);
                playRow(song.patterns[song.order[orderPos]].data() + row * RowBytes, false);
                ++musicPos;
            }
        }
        void renderAudio(int16_t * pcm, size_t frames)
        {
            for (size_t i = 0; i < frames; ++i)
            {
                pitPhase += PitClock;
                if (pitPhase >= uint64_t(SampleRate) * PitDivisor)
                {
                    pitPhase -= uint64_t(SampleRate) * PitDivisor;
                    tick();
                }
                OPL3_GenerateResampled(&chip, pcm + i * 2);
            }
        }
        void resetNibble()
        {
            highNibble = true;
            previousNibble = 0;
        }
        void hexInput(unsigned digit)
        {
            uint8_t v = uint8_t(highNibble ? digit << 4 : (previousNibble << 4) | digit);
            if (orderMode)
            {
                song.order[orderPos] = v;
            }
            else if (field == 2)
            {
                cell()[2] = v;
                if (cell()[0] == 255) cell()[1] = 0;
            }
            else
                return; // Instrument column is read-only in the original.
            previousNibble = uint8_t(digit);
            highNibble = !highNibble;
        }
        void noteInput(uint8_t packed)
        {
            if (orderMode || field != 0) return;
            cell()[0] = uint8_t(((((packed >> 4) + octave) & 7) << 4) | (packed & 15));
            cell()[1] = instrument; // Preserve volume; FF means no attenuation after XOR.
            row = std::min(row + 1, 63);
        }
        void insert()
        {
            auto & p = song.patterns[song.order[orderPos]];
            for (int y = 63; y > row; --y)
                std::copy_n(p.data() + (y - 1) * 54 + channel * 3, 3, p.data() + y * 54 + channel * 3);
            std::fill_n(cell(), 3, 255);
        }
        void erase()
        {
            std::fill_n(cell(), 3, 255);
            row = std::min(row + 1, 63);
        }
        void backspace()
        {
            row = std::max(0, row - 1);
            auto & p = song.patterns[song.order[orderPos]];
            for (int y = row; y < 63; ++y)
                std::copy_n(p.data() + (y + 1) * 54 + channel * 3, 3, p.data() + y * 54 + channel * 3);
            std::fill_n(p.data() + 63 * 54 + channel * 3, 3, 255);
        }
        void instrumentKey(int f, bool modulator, bool shift)
        {
            uint8_t * p = song.instruments.data() + unsigned(instrument) * 36 + (modulator ? 0 : 6);
            if (shift)
            {
                if (f >= 1 && f <= 4) p[0] ^= uint8_t(1u << (f + 3));
                return;
            }
            switch (f)
            {
                case 1:
                    p[2] = uint8_t((p[2] & 15) | ((p[2] + 16) & 240));
                    break;
                case 2:
                    p[2] = uint8_t((p[2] & 240) | ((p[2] + 1) & 15));
                    break;
                case 3:
                    p[3] = uint8_t((p[3] & 15) | ((p[3] + 16) & 240));
                    break;
                case 4:
                    p[3] = uint8_t((p[3] & 240) | ((p[3] + 1) & 15));
                    break;
                case 5:
                    p[4] = uint8_t((p[4] + 1) & 7);
                    break;
                case 6:
                    p[1] = uint8_t((p[1] & 192) | ((p[1] + 1) & 63));
                    break;
                case 7:
                    p[1] = uint8_t(p[1] + 64);
                    break;
                case 8:
                    p[5] = uint8_t((p[5] & 1) | ((p[5] + 2) & 14));
                    break;
                case 9:
                    p[5] ^= 1;
                    break;
                case 10:
                    p[0] = uint8_t((p[0] & 240) | ((p[0] + 1) & 15));
                    break;
            }
        }
        void put(int x, int y, uint8_t ch)
        {
            if (x >= 0 && x < 80 && y >= 0 && y < 50) screen[(y * 80 + x) * 2] = ch;
        }
        void text(int x, int y, const char * s)
        {
            while (*s)
                put(x++, y, uint8_t(*s++));
        }
        void hex(int x, int y, uint8_t n)
        {
            put(x, y, Hex[n >> 4]);
            put(x + 1, y, Hex[n & 15]);
        }
        void attr(int x, int y, int n, uint8_t value)
        {
            for (int i = 0; i < n; ++i)
                if (x + i >= 0 && x + i < 80 && y >= 0 && y < 50) screen[(y * 80 + x + i) * 2 + 1] = value;
        }
        void names(int top)
        {
            for (int j = 0; j < 10; ++j)
            {
                uint8_t i = uint8_t(instrument + j);
                hex(1, top + j, i);
                for (int k = 0; k < 24; ++k)
                    put(4 + k, top + j, song.instruments[unsigned(i) * 36 + 12 + k]);
            }
            attr(1, top, 26, 15);
            for (int j = 0; j < 8; ++j)
                put(28, top + 1 + j, 176);
            put(28, top + 1 + (instrument >> 5), 219);
        }
        void draw()
        {
            std::copy_n(instrumentMode ? ins_m : fm_main, 8000, screen.begin());
            if (instrumentMode)
            {
                names(3);
                for (int k = 0; k < 24; ++k)
                    put(51 + k, 3, naming ? nameEdit[k] : song.instruments[unsigned(instrument) * 36 + 12 + k]);
                if (naming)
                {
                    attr(51, 3, 24, 11);
                    attr(51 + namePos, 3, 1, 0x10);
                }
                for (int op = 0; op < 2; ++op)
                {
                    const uint8_t * p = song.instruments.data() + unsigned(instrument) * 36 + op * 6;
                    int dx = op == 0 ? 40 : 0; // original shows carrier on left, modulator on right
                    put(19 + dx, 18, Hex[p[2] >> 4]);
                    put(19 + dx, 20, Hex[p[2] & 15]);
                    put(19 + dx, 22, Hex[p[3] >> 4]);
                    put(19 + dx, 24, Hex[p[3] & 15]);
                    put(19 + dx, 28, Hex[p[4] & 7]);
                    hex(21 + dx, 30, p[1] & 63);
                    put(21 + dx, 32, Hex[p[1] >> 6]);
                    put(21 + dx, 34, Hex[(p[5] >> 1) & 7]);
                    text(28 + dx, 36, (p[5] & 1) ? "On  " : "Off ");
                    put(25 + dx, 40, Hex[p[0] & 15]);
                    for (int k = 0; k < 4; ++k)
                        text(28 + dx, 44 + k, (p[0] & (16 << k)) ? "On  " : "Off ");
                }
                return;
            }
            names(38);
            hex(78, 21, song.speed);
            hex(78, 24, song.bpm);
            hex(78, 27, octave);
            hex(78, 30, instrument);
            hex(78, 33, song.order[orderPos]);
            hex(75, 36, uint8_t(row));
            int top = std::max(0, row - 15);
            const auto & p = song.patterns[song.order[orderPos]];
            for (int bank = 0; bank < 2; ++bank)
                for (int y = 0; y < 16; ++y)
                {
                    int sy = 3 + bank * 17 + y, r = top + y;
                    hex(1, sy, uint8_t(r));
                    for (int c = 0; c < 9; ++c)
                    {
                        int x = 3 + c * 8;
                        const uint8_t * n = p.data() + r * 54 + (bank * 9 + c) * 3;
                        text(x, sy, NoteNames[n[0] & 15]);
                        if ((n[0] & 15) < 12) put(x + 2, sy, uint8_t('0' + (n[0] >> 4)));
                        if (n[0] != 255) hex(x + 3, sy, n[1]);
                        if (n[0] != 255 || n[2] != 255) hex(x + 5, sy, n[2]);
                    }
                }
            if (!orderMode)
            {
                int x = 3 + (channel % 9) * 8 + (field == 0 ? 0 : field == 1 ? 3
                                                                             : 5);
                attr(x, 3 + (channel / 9) * 17 + row - top, field == 0 ? 3 : 2, 15);
            }
            for (int j = 0; j < 16; ++j)
            {
                uint8_t pos = uint8_t(orderPos + j);
                hex(76, 3 + j, song.order[pos]);
                if (pos == 0) put(78, 3 + j, '*');
            }
            put(79, 4 + orderPos / 19, 219);
            if (orderMode) attr(76, 3, 2, 15);
            if (playMode == Music)
            {
                for (int bank = 0; bank < 2; ++bank)
                    attr(1, 3 + bank * 17 + row - top, 73, 15);
            }
        }
    };
} // namespace fm

#ifndef FM_TEST
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <commdlg.h>
#include <shellapi.h>
#include <avrt.h>
#include <process.h>
#include <mutex>
#include <memory>

#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#endif

namespace
{
    std::unique_ptr<fm::Tracker> app;
    HWND window = nullptr;
    HFONT font = nullptr;
    HWAVEOUT wave = nullptr;
    // 6 x 10 ms: tolerate scheduling jitter without tying playback to painting.
    constexpr unsigned AudioFrames = 480, AudioBuffers = 6;
    constexpr UINT AudioErrorMessage = WM_APP + 1;
    HANDLE audioEvent = nullptr, stopEvent = nullptr, audioThread = nullptr;
    std::recursive_mutex trackerMutex;
    struct AudioBlock
    {
        WAVEHDR hdr{};
        std::array<int16_t, AudioFrames * 2> data{};
        bool prepared = false;
    };
    std::array<AudioBlock, AudioBuffers> audio;
    bool closing = false, dirty = false;
    std::wstring currentPath;
    std::array<bool, 128> keys{};
    constexpr COLORREF Colors[] = { RGB(0, 0, 0), RGB(0, 0, 170), RGB(0, 170, 0), RGB(0, 170, 170), RGB(170, 0, 0), RGB(170, 0, 170), RGB(170, 85, 0), RGB(170, 170, 170), RGB(85, 85, 85), RGB(85, 85, 255), RGB(85, 255, 85), RGB(85, 255, 255), RGB(255, 85, 85), RGB(255, 85, 255), RGB(255, 255, 85), RGB(255, 255, 255) };
    std::array<wchar_t, 256> glyph{};
    void title()
    {
        std::wstring s = L"FM Tracker 1.0 — " + (currentPath.empty() ? std::wstring(L"Untitled") : currentPath) + (dirty ? L" *" : L"");
        SetWindowTextW(window, s.c_str());
    }
    void changed()
    {
        dirty = true;
        title();
    }
    void error(const char * message)
    {
        MessageBoxA(window, message, "FM Tracker", MB_OK | MB_ICONERROR);
    }
    void closeAudio()
    {
        // Never call while holding trackerMutex: the worker may be finishing a block.
        if (stopEvent) SetEvent(stopEvent);
        if (audioThread)
        {
            WaitForSingleObject(audioThread, INFINITE);
            CloseHandle(audioThread);
            audioThread = nullptr;
        }
        if (wave)
        {
            waveOutReset(wave);
            for (auto & b : audio)
                if (b.prepared)
                {
                    waveOutUnprepareHeader(wave, &b.hdr, sizeof(b.hdr));
                    b.prepared = false;
                }
            waveOutClose(wave);
            wave = nullptr;
        }
        if (audioEvent)
        {
            CloseHandle(audioEvent);
            audioEvent = nullptr;
        }
        if (stopEvent)
        {
            CloseHandle(stopEvent);
            stopEvent = nullptr;
        }
    }
    bool submit(AudioBlock & b)
    {
        {
            std::lock_guard<std::recursive_mutex> lock(trackerMutex);
            app->renderAudio(b.data.data(), AudioFrames);
        }
        return waveOutWrite(wave, &b.hdr, sizeof(b.hdr)) == MMSYSERR_NOERROR;
    }
    unsigned __stdcall audioMain(void *)
    {
        DWORD taskIndex = 0;
        HANDLE task = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);
        if (!task) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        bool ok = true;
        // Device is paused until every initial buffer has been queued.
        for (auto & b : audio)
        {
            if (!submit(b))
            {
                ok = false;
                break;
            }
        }
        if (ok) ok = waveOutRestart(wave) == MMSYSERR_NOERROR;
        const HANDLE events[] = { stopEvent, audioEvent };
        while (ok && WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0)
        {
            // Reset BEFORE scanning; completions during the scan remain signaled.
            ResetEvent(audioEvent);
            for (auto & b : audio)
            {
                if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) break;
                if ((b.hdr.dwFlags & WHDR_DONE) && !submit(b))
                {
                    ok = false;
                    break;
                }
            }
            if (!ok) break;
            DWORD result = WaitForMultipleObjects(2, events, FALSE, INFINITE);
            if (result == WAIT_OBJECT_0) break;
            if (result != WAIT_OBJECT_0 + 1)
            {
                ok = false;
                break;
            }
        }
        if (task) AvRevertMmThreadCharacteristics(task);
        if (!ok) PostMessageW(window, AudioErrorMessage, 0, 0);
        return 0;
    }
    bool openAudio()
    {
        audioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!audioEvent || !stopEvent)
        {
            closeAudio();
            return false;
        }
        WAVEFORMATEX fmt{};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = 2;
        fmt.nSamplesPerSec = fm::SampleRate;
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = 4;
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * 4;
        if (waveOutOpen(&wave, WAVE_MAPPER, &fmt, reinterpret_cast<DWORD_PTR>(audioEvent), 0, CALLBACK_EVENT) != MMSYSERR_NOERROR)
        {
            wave = nullptr;
            closeAudio();
            return false;
        }
        if (waveOutPause(wave) != MMSYSERR_NOERROR)
        {
            closeAudio();
            return false;
        }
        for (auto & b : audio)
        {
            b.hdr = {};
            b.hdr.lpData = reinterpret_cast<LPSTR>(b.data.data());
            b.hdr.dwBufferLength = sizeof(b.data);
            if (waveOutPrepareHeader(wave, &b.hdr, sizeof(b.hdr)) != MMSYSERR_NOERROR)
            {
                closeAudio();
                return false;
            }
            b.prepared = true;
        }
        ResetEvent(audioEvent);
        audioThread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, audioMain, nullptr, 0, nullptr));
        if (!audioThread)
        {
            closeAudio();
            return false;
        }
        return true;
    }
    std::wstring dialog(bool save)
    {
        wchar_t path[32768]{};
        if (save && !currentPath.empty()) wcsncpy_s(path, currentPath.c_str(), _TRUNCATE);
        OPENFILENAMEW of{};
        of.lStructSize = sizeof(of);
        of.hwndOwner = window;
        of.lpstrFilter = L"FM Tracker (*.fmt)\0*.fmt\0All files\0*.*\0\0";
        of.lpstrFile = path;
        of.nMaxFile = 32768;
        of.lpstrDefExt = L"fmt";
        of.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
        bool ok = save ? GetSaveFileNameW(&of) != 0 : GetOpenFileNameW(&of) != 0;
        keys.fill(false);
        return ok ? path : L"";
    }
    bool saveFile()
    {
        auto path = dialog(true);
        if (path.empty()) return false;
        std::vector<uint8_t> bytes;
        {
            std::lock_guard<std::recursive_mutex> lock(trackerMutex);
            bytes = app->song.save();
        }
        // Same-directory temporary file + replace avoids truncating the previous save on failure.
        wchar_t temp[MAX_PATH]{};
        auto slash = path.find_last_of(L"/\\");
        std::wstring dir = slash == std::wstring::npos ? L"." : path.substr(0, slash + 1);
        if (!GetTempFileNameW(dir.c_str(), L"fmt", 0, temp))
        {
            error("Cannot create temporary save file.");
            return false;
        }
        HANDLE f = CreateFileW(temp, GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD wrote = 0;
        bool ok = f != INVALID_HANDLE_VALUE;
        if (ok)
        {
            ok = WriteFile(f, bytes.data(), DWORD(bytes.size()), &wrote, nullptr) && wrote == bytes.size();
            if (ok) ok = FlushFileBuffers(f) != 0;
            CloseHandle(f);
        }
        if (ok) ok = MoveFileExW(temp, path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
        if (!ok)
        {
            DeleteFileW(temp);
            error("Could not save the FMT file.");
            return false;
        }
        currentPath = path;
        dirty = false;
        title();
        return true;
    }
    bool discard()
    {
        if (!dirty) return true;
        int r = MessageBoxW(window, L"Save changes to the current song?", L"FM Tracker", MB_YESNOCANCEL | MB_ICONQUESTION);
        keys.fill(false);
        return r == IDNO || (r == IDYES && saveFile());
    }
    void loadFile(const std::wstring & path)
    {
        if (path.empty()) return;
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE)
        {
            error("Could not open the FMT file.");
            return;
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(f, &size) || size.QuadPart < 0 || size.QuadPart > 1000000)
        {
            CloseHandle(f);
            error("Invalid FMT file size.");
            return;
        }
        std::vector<uint8_t> bytes(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        bool ok = ReadFile(f, bytes.data(), DWORD(bytes.size()), &got, nullptr) && got == bytes.size();
        CloseHandle(f);
        if (!ok)
        {
            error("Could not read the complete FMT file.");
            return;
        }
        auto loaded = std::make_unique<fm::Song>();
        try
        {
            loaded->load(bytes);
        }
        catch (const std::exception & e)
        {
            error(e.what());
            return;
        }
        {
            std::lock_guard<std::recursive_mutex> lock(trackerMutex);
            app->stop();
            app->song = std::move(*loaded);
            app->reset();
            app->orderPos = 0;
            app->row = 0;
            app->resetNibble();
        }
        currentPath = path;
        dirty = false;
        title();
    }
    void help()
    {
        MessageBoxW(window,
                    L"Tracker\nF1/F2: instrument -/+     F3/F4: octave -/+\nF5: play song     F6: play pattern     F8: stop\nF7: instrument editor     F9/F10: Speed -/+\nF11/F12: BPM byte -/+\nCtrl: track mode     Alt: order mode\nArrows/Tab: move     Home/End/Page Up/Page Down: rows\nZ-row / Q-row: enter notes     0-9,A-F: volume/order hex\nInsert/Delete/Backspace: insert/clear/remove cell\nShift+F1: new     Shift+F2: open     Shift+F3: save\nShift+F4: quit     Escape: quit/back\n\nInstrument editor\nF1/F2: instrument -/+     F3: edit name     F4/Escape: back\nHold Alt (modulator) or Ctrl (carrier):\nF1 attack, F2 decay, F3 sustain, F4 release, F5 waveform,\nF6 attenuation, F7 KSL, F8 feedback, F9 connection, F10 multiplier\nWith Shift: F1 KSR, F2 sustained envelope, F3 vibrato, F4 tremolo\n\nTiming matches DOS: ticks = Speed + 256*BPM.\nSet Speed to 06 and BPM to 00 for a useful starting tempo.",
                    L"FM Tracker — controls", MB_OK);
        keys.fill(false);
    }
    void key(unsigned scan, bool repeat)
    {
        std::unique_lock<std::recursive_mutex> lock(trackerMutex);
        auto & t = *app;
        bool shift = keys[42] || keys[54], ctrl = keys[29], alt = keys[56];
        if (t.naming)
        {
            if (scan == 1)
            {
                t.naming = false;
                return;
            }
            if (scan == 28)
            {
                std::copy(t.nameEdit.begin(), t.nameEdit.end(), t.song.instruments.begin() + unsigned(t.instrument) * 36 + 12);
                t.naming = false;
                changed();
                return;
            }
            if (scan == 75) t.namePos = std::max(0, t.namePos - 1);
            if (scan == 77) t.namePos = std::min(23, t.namePos + 1);
            if (scan == 14)
            {
                t.namePos = std::max(0, t.namePos - 1);
                t.nameEdit[t.namePos] = ' ';
            }
            for (size_t i = 0; i < sizeof(text_i); i += 2)
                if (text_i[i] == scan)
                {
                    t.nameEdit[t.namePos] = text_i[i + 1];
                    t.namePos = std::min(23, t.namePos + 1);
                    break;
                }
            return;
        }
        if (t.instrumentMode)
        {
            if (scan == 1 || (scan == 62 && !ctrl && !alt))
            {
                t.instrumentMode = false;
                return;
            }
            if (scan >= 59 && scan <= 68 && (ctrl || alt))
            {
                if (!repeat)
                {
                    t.instrumentKey(int(scan) - 58, alt, shift);
                    changed();
                }
                return;
            }
            if (!repeat)
            {
                if (scan == 59) --t.instrument;
                if (scan == 60) ++t.instrument;
                if (scan == 61)
                {
                    t.naming = true;
                    t.namePos = 0;
                    std::copy_n(t.song.instruments.begin() + unsigned(t.instrument) * 36 + 12, 24, t.nameEdit.begin());
                }
            }
            return;
        }
        if (shift && scan >= 59 && scan <= 62)
        {
            if (repeat) return;
            lock.unlock(); // No modal dialogs, file I/O or shutdown under the audio lock.
            if (scan == 59 && discard())
            {
                {
                    std::lock_guard<std::recursive_mutex> edit(trackerMutex);
                    t.stop();
                    t.song.clear();
                    t.orderPos = 0;
                    t.row = 0;
                    t.resetNibble();
                }
                currentPath.clear();
                changed();
            }
            if (scan == 60 && discard()) loadFile(dialog(false));
            if (scan == 61) saveFile();
            if (scan == 62) SendMessageW(window, WM_CLOSE, 0, 0);
            return;
        }
        if (scan == 1)
        {
            lock.unlock();
            if (!repeat) SendMessageW(window, WM_CLOSE, 0, 0);
            return;
        }
        if (scan == 29)
        {
            t.orderMode = false;
            t.resetNibble();
            return;
        }
        if (scan == 56)
        {
            t.orderMode = true;
            t.resetNibble();
            return;
        }
        if (!repeat)
        {
            switch (scan)
            {
                case 59:
                    --t.instrument;
                    return;
                case 60:
                    ++t.instrument;
                    return;
                case 61:
                    t.octave = (t.octave - 1) & 7;
                    return;
                case 62:
                    t.octave = (t.octave + 1) & 7;
                    return;
                case 63:
                case 64:
                    if (!wave)
                    {
                        lock.unlock();
                        error("No audio device is open.");
                        return;
                    }
                    t.start(scan == 63 ? fm::Tracker::Music : fm::Tracker::Pattern);
                    return;
                case 65:
                    t.instrumentMode = true;
                    return;
                case 66:
                    t.stop();
                    return;
                case 67:
                    --t.song.speed;
                    changed();
                    return;
                case 68:
                    ++t.song.speed;
                    changed();
                    return;
                case 87:
                    --t.song.bpm;
                    changed();
                    return;
                case 88:
                    ++t.song.bpm;
                    changed();
                    return;
            }
        }
        if (t.playMode == fm::Tracker::Music) return; // Avoid editing the row being advanced by playback.
        switch (scan)
        {
            case 72:
                if (t.orderMode)
                    --t.orderPos;
                else
                    t.row = std::max(0, t.row - 1);
                t.resetNibble();
                return;
            case 80:
                if (t.orderMode)
                    ++t.orderPos;
                else
                    t.row = std::min(63, t.row + 1);
                t.resetNibble();
                return;
            case 71:
                if (t.orderMode)
                    t.orderPos = 0;
                else
                    t.row = 0;
                t.resetNibble();
                return;
            case 79:
                if (t.orderMode)
                    t.orderPos = 255;
                else
                    t.row = 63;
                t.resetNibble();
                return;
            case 73:
                if (!t.orderMode) t.row = std::max(0, t.row - 8);
                t.resetNibble();
                return;
            case 81:
                if (!t.orderMode) t.row = std::min(63, t.row + 8);
                t.resetNibble();
                return;
            case 15:
                if (!t.orderMode) t.channel = (t.channel + 1) % 18;
                t.resetNibble();
                return;
            case 77:
                if (!t.orderMode)
                {
                    if (++t.field == 3)
                    {
                        t.field = 0;
                        t.channel = (t.channel + 1) % 18;
                    }
                }
                t.resetNibble();
                return;
            case 75:
                if (!t.orderMode)
                {
                    if (--t.field < 0)
                    {
                        t.field = 2;
                        t.channel = (t.channel + 17) % 18;
                    }
                }
                t.resetNibble();
                return;
            case 82:
                if (!t.orderMode)
                {
                    t.insert();
                    changed();
                }
                return;
            case 83:
                if (!t.orderMode)
                {
                    t.erase();
                    changed();
                }
                return;
            case 14:
                if (!t.orderMode)
                {
                    t.backspace();
                    changed();
                }
                return;
        }
        if (t.orderMode || t.field == 2)
        {
            for (unsigned i = 0; i < 16; ++i)
                if (key_hexa_press[i] == scan)
                {
                    t.hexInput(i);
                    changed();
                    return;
                }
        }
        else if (t.field == 0)
        {
            for (size_t i = 0; i < sizeof(notesid); i += 2)
                if (notesid[i] == scan)
                {
                    t.noteInput(notesid[i + 1]);
                    changed();
                    return;
                }
        }
    }
    void paint()
    {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(window, &ps);
        RECT client{};
        GetClientRect(window, &client);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, 800, 800);
        auto oldBitmap = SelectObject(mem, bitmap);
        auto oldFont = SelectObject(mem, font);
        fm::ScreenData screen;
        {
            std::lock_guard<std::recursive_mutex> lock(trackerMutex);
            app->draw();
            screen = app->screen;
        }
        // GDI rendering must never hold the audio lock.
        SetBkMode(mem, OPAQUE);
        for (int y = 0; y < 50; ++y)
            for (int x = 0; x < 80; ++x)
            {
                unsigned offset = (y * 80 + x) * 2;
                uint8_t a = screen[offset + 1];
                wchar_t ch = glyph[screen[offset]];
                RECT r = { x * 10, y * 16, (x + 1) * 10, (y + 1) * 16 };
                SetTextColor(mem, Colors[a & 15]);
                SetBkColor(mem, Colors[(a >> 4) & 15]);
                ExtTextOutW(mem, r.left, r.top, ETO_OPAQUE | ETO_CLIPPED, &r, &ch, 1, nullptr);
            }
        SetStretchBltMode(dc, HALFTONE/*COLORONCOLOR*/);
        StretchBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, 800, 800, SRCCOPY);
        SelectObject(mem, oldFont);
        SelectObject(mem, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(mem);
        EndPaint(window, &ps);
    }
    LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
            case WM_PAINT:
                paint();
                return 0;
            case WM_ERASEBKGND:
                return 1;
            case WM_SIZE:
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            case WM_TIMER:
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            case AudioErrorMessage:
                closeAudio();
                {
                    std::lock_guard<std::recursive_mutex> lock(trackerMutex);
                    app->stop();
                }
                if (!closing) error("Audio output failed.");
                return 0;
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN: {
                unsigned s = (unsigned(lp) >> 16) & 127;
                bool repeat = (uint64_t(lp) & (1ull << 30)) != 0;
                keys[s] = true;
                key(s, repeat);
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            case WM_KEYUP:
            case WM_SYSKEYUP:
                keys[(unsigned(lp) >> 16) & 127] = false;
                return 0;
            case WM_KILLFOCUS:
                keys.fill(false);
                return 0;
            case WM_SYSCOMMAND:
                if ((wp & 0xfff0) == SC_KEYMENU) return 0;
                break;
            case WM_COMMAND:
                switch (LOWORD(wp))
                {
                    case 100:
                        if (discard())
                        {
                            {
                                std::lock_guard<std::recursive_mutex> lock(trackerMutex);
                                app->stop();
                                app->song.clear();
                                app->orderPos = 0;
                                app->row = 0;
                                app->resetNibble();
                            }
                            currentPath.clear();
                            changed();
                        }
                        break;
                    case 101:
                        if (discard()) loadFile(dialog(false));
                        break;
                    case 102:
                        saveFile();
                        break;
                    case 103:
                        SendMessageW(h, WM_CLOSE, 0, 0);
                        break;
                    case 104:
                        help();
                        break;
                }
                return 0;
            case WM_CLOSE:
                if (discard())
                {
                    closing = true;
                    KillTimer(h, 1);
                    closeAudio();
                    DestroyWindow(h);
                }
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show)
{
    app = std::make_unique<fm::Tracker>();
    for (int i = 0; i < 256; ++i)
    {
        char c = char(i);
        MultiByteToWideChar(437, 0, &c, 1, &glyph[i], 1);
    }
    constexpr wchar_t controls[] = { L' ', L'\u263a', L'\u263b', L'\u2665', L'\u2666', L'\u2663', L'\u2660', L'\u2022', L'\u25d8', L'\u25cb', L'\u25d9', L'\u2642', L'\u2640', L'\u266a', L'\u266b', L'\u263c', L'\u25ba', L'\u25c4', L'\u2195', L'\u203c', L'\u00b6', L'\u00a7', L'\u25ac', L'\u21a8', L'\u2191', L'\u2193', L'\u2192', L'\u2190', L'\u221f', L'\u2194', L'\u25b2', L'\u25bc' };
    std::copy_n(controls, 32, glyph.begin());
    glyph[127] = L'\u2302';
    font = CreateFontW(-16, 10, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, OEM_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, FIXED_PITCH, L"Consolas");
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"FMTrackerWin32";
    if (!RegisterClassW(&wc))
    {
        DeleteObject(font);
        return 1;
    }
    HMENU menu = CreateMenu(), file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, 100, L"New\tShift+F1");
    AppendMenuW(file, MF_STRING, 101, L"Open...\tShift+F2");
    AppendMenuW(file, MF_STRING, 102, L"Save As...\tShift+F3");
    AppendMenuW(file, MF_STRING, 103, L"Exit");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"File");
    AppendMenuW(menu, MF_STRING, 104, L"Controls");
    RECT r = { 0, 0, 800, 800 };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, TRUE);
    window = CreateWindowExW(0, wc.lpszClassName, L"FM Tracker", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, menu, instance, nullptr);
    if (!window)
    {
        DeleteObject(font);
        return 1;
    }
    title();
    ShowWindow(window, show);
    UpdateWindow(window);
    if (!openAudio()) error("Could not open 48 kHz stereo audio. Editing and file operations remain available.");
    SetTimer(window, 1, 16, nullptr);
    int argc = 0;
    LPWSTR * argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv)
    {
        if (argc > 1) loadFile(argv[1]);
        LocalFree(argv);
    }
    MSG msg{};
    int result;
    while ((result = GetMessageW(&msg, nullptr, 0, 0)) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    closing = true;
    closeAudio();
    DeleteObject(font);
    return result < 0 ? 1 : int(msg.wParam);
}
#endif
