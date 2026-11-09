#define FM_TEST
#include "FM_Win32.cpp"
#include <cassert>
#include <iostream>
#include <memory>
int main()
{
    auto t = std::make_unique<fm::Tracker>();
    static_assert(sizeof(fm_main) == 8000 && sizeof(ins_m) == 8000);
    assert(t->song.instruments[0] == 0 && t->song.instruments[2] == 0x82);
    // All 255 serializable patterns, including the DOS saver omission: FE.
    for (unsigned i = 0; i < 256; ++i)
        t->song.order[i] = uint8_t(i);
    for (unsigned i = 0; i < 255; ++i)
        t->song.patterns[i][0] = uint8_t(i);
    t->song.speed = 6;
    t->song.bpm = 1;
    auto bytes = t->song.save();
    assert(bytes.size() == 9483 + 255 * 3457);
    auto restored = std::make_unique<fm::Song>();
    restored->load(bytes);
    assert(restored->save() == bytes);
    assert(restored->patterns[254][0] == 254);
    for (auto bad : { std::vector<uint8_t>{}, std::vector<uint8_t>(bytes.begin(), bytes.end() - 1) })
    {
        try
        {
            restored->load(bad);
            assert(false);
        }
        catch (const std::runtime_error &)
        {
        }
        assert(restored->save() == bytes);
    }
    auto bad = bytes;
    bad[0] = 'X';
    try
    {
        restored->load(bad);
        assert(false);
    }
    catch (const std::runtime_error &)
    {
    }
    bad = bytes;
    bad[9483 + 3457] = bad[9483];
    try
    {
        restored->load(bad);
        assert(false);
    }
    catch (const std::runtime_error &)
    {
    }
    t->song.clear();
    t->row = 63;
    t->channel = 17;
    t->noteInput(0x0b);
    assert(t->row == 63 && t->cell()[0] == 0x0b);
    t->insert();
    assert(t->cell()[0] == 255);
    t->row = 0;
    t->backspace();
    assert(t->row == 0);
    t->field = 2;
    t->resetNibble();
    t->hexInput(10);
    t->hexInput(5);
    assert(t->cell()[2] == 0xa5);
    t->orderMode = true;
    t->resetNibble();
    t->hexInput(15);
    t->hexInput(14);
    assert(t->song.order[0] == 254);
    t->song.order.fill(255);
    t->song.order[0] = 3;
    t->song.speed = 6;
    t->song.bpm = 0;
    auto & p = t->song.patterns[3];
    p.fill(255);
    p[0] = 0x40;
    p[1] = 0;
    p[2] = 0xff;
    p[27] = 0x40;
    p[28] = 0;
    p[29] = 0x80;
    std::vector<std::pair<uint16_t, uint8_t>> trace;
    t->trace = &trace;
    t->playRow(p.data(), true);
    assert(trace.size() == 33); // left 16 writes, right 17 (original duplicate B0)
    assert((trace[0] == std::make_pair(uint16_t(0xa0), uint8_t(0))));
    assert((trace[3] == std::make_pair(uint16_t(0x40), uint8_t(0))));
    assert((trace[7] == std::make_pair(uint16_t(0xc0), uint8_t(0x11))));
    assert((trace[14] == std::make_pair(uint16_t(0xa0), uint8_t(343))));
    assert((trace[15] == std::make_pair(uint16_t(0xb0), uint8_t(0x31))));
    assert((trace[20] == std::make_pair(uint16_t(0x140), uint8_t(0x7f))));
    t->trace = nullptr;
    t->start(fm::Tracker::Music);
    t->musicPos = 64;
    t->counter = 1;
    t->tick();
    assert(t->orderPos == 0 && t->musicPos == 1); // restart first order, which selects pattern 3
    t->start(fm::Tracker::Pattern);
    auto before = t->rowEvents;
    std::vector<int16_t> pcm(fm::SampleRate * 2);
    t->renderAudio(pcm.data(), fm::SampleRate);
    assert(t->rowEvents - before == 26); // 153 PIT interrupts, rows on 1,7,...151
    assert(std::any_of(pcm.begin(), pcm.end(), [](int16_t s) { return s != 0; }));
    // Scrolling and all order/channel edges stay within the screen and pattern.
    for (int row = 0; row < 64; ++row)
        for (int ch = 0; ch < 18; ++ch)
        {
            t->row = row;
            t->channel = ch;
            t->draw();
        }
    t->instrumentMode = true;
    for (int i = 0; i < 256; ++i)
    {
        t->instrument = uint8_t(i);
        t->draw();
    }
    t->song.order.fill(255);
    bytes = t->song.save();
    restored->load(bytes);
    assert(bytes.size() == 9483);
    // The device may request different block sizes; PCM and tempo must not depend on them.
    auto whole = std::make_unique<fm::Tracker>();
    auto blocks = std::make_unique<fm::Tracker>();
    whole->song.speed = 1;
    for (int r = 0; r < 64; ++r)
        for (int ch = 0; ch < 18; ++ch)
        {
            auto * c = whole->song.patterns[0].data() + r * 54 + ch * 3;
            c[0] = uint8_t(0x30 + (r + ch) % 12);
            c[1] = uint8_t(ch % 10);
            c[2] = 255;
        }
    blocks->song = whole->song;
    whole->start(fm::Tracker::Pattern);
    blocks->start(fm::Tracker::Pattern);
    constexpr size_t Frames = fm::SampleRate * 3;
    std::vector<int16_t> continuous(Frames * 2), chunked(Frames * 2);
    whole->renderAudio(continuous.data(), Frames);
    for (size_t offset = 0; offset < Frames;)
    {
        size_t count = std::min(size_t(480), Frames - offset);
        blocks->renderAudio(chunked.data() + offset * 2, count);
        offset += count;
    }
    assert(continuous == chunked);
    assert(whole->rowEvents == blocks->rowEvents);
    assert(whole->rowEvents == uint64_t(Frames) * fm::PitClock / (fm::SampleRate * fm::PitDivisor));
    std::cout << "PASS: FMT round-trip/validation, FE save, edit bounds, OPL writes, song loop, PIT timing, PCM output, screen bounds, audio block invariance\n";
}
