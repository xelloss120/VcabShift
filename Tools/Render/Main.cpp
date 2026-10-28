// 開発用の WAV の処理ツール。
//   vcabsh_render 入力.wav 出力.wav [キー=値 ...] [--align] [--track 出力.csv]
// キーは Params.cpp の key（例: pitch=12 formant=6 warp_mode=2）。
// --align: 遅延の分を詰めて、入力と時刻をそろえる。
// --track: 5ms ごとの声の高さと F1〜F4 を CSV に書く（遅延の分を詰めた時刻）。
#include "Engine.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <chrono>
#include <cstdio>
#include <fstream>

static int run (int argc, char** argv)
{
    if (argc < 3)
    {
        std::printf ("usage: vcabsh_render in.wav out.wav [key=value ...] [--align] [--track out.csv]\n");
        return 1;
    }

    auto values = vcab::defaultValues();
    bool align = false;
    std::string trackPath;
    for (int i = 3; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--align")
        {
            align = true;
            continue;
        }
        if (a == "--track" && i + 1 < argc)
        {
            trackPath = argv[++i];
            continue;
        }
        const auto eq = a.find ('=');
        bool ok = false;
        if (eq != std::string::npos)
            for (const auto& p : vcab::paramTable())
                if (a.substr (0, eq) == p.key)
                {
                    values[static_cast<size_t> (p.id)] = std::stof (a.substr (eq + 1));
                    ok = true;
                }
        if (! ok)
        {
            std::printf ("unknown argument: %s\n", a.c_str());
            return 1;
        }
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const juce::File inFile { juce::String (juce::CharPointer_UTF8 (argv[1])) };
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (inFile));
    if (reader == nullptr)
    {
        std::printf ("cannot read %s\n", argv[1]);
        return 1;
    }
    const int numCh = static_cast<int> (reader->numChannels);
    const int len = static_cast<int> (reader->lengthInSamples);
    const double fs = reader->sampleRate;
    juce::AudioBuffer<float> input (numCh, len);
    reader->read (&input, 0, len, 0, true, true);

    constexpr int kBlock = 256;
    vcab::Engine engine;
    engine.prepare (fs, kBlock);
    engine.setSettings (vcab::Settings::fromValues (values));
    const int latency = engine.latencySamples();

    const int total = len + latency;
    std::vector<float> out (static_cast<size_t> (total), 0.0f);
    std::vector<float> zeros (kBlock, 0.0f);
    std::ofstream track;
    if (! trackPath.empty())
    {
        track.open (trackPath);
        track << "time,f0,f1,f2,f3,f4,t1,t2,t3,t4\n";
    }

    const auto start = std::chrono::steady_clock::now();
    int lastTrack = -1;
    for (int pos = 0; pos < total; pos += kBlock)
    {
        const int n = std::min (kBlock, total - pos);
        const float* chans[2] = { zeros.data(), zeros.data() };
        for (int c = 0; c < std::min (numCh, 2); ++c)
            if (pos < len)
                chans[c] = input.getReadPointer (c) + pos;
        // 入力の終わりの先は 0（ブロックの途中で終わるときは、残りを詰めた写しを使う）
        std::vector<float> tail[2];
        if (pos < len && pos + n > len)
            for (int c = 0; c < std::min (numCh, 2); ++c)
            {
                tail[c].assign (static_cast<size_t> (n), 0.0f);
                std::copy (input.getReadPointer (c) + pos, input.getReadPointer (c) + len, tail[c].begin());
                chans[c] = tail[c].data();
            }
        else if (pos >= len)
            for (int c = 0; c < 2; ++c)
                chans[c] = zeros.data();
        engine.process (chans, std::min (numCh, 2), out.data() + pos, n);

        if (track.is_open())
        {
            const int frame = (pos + n) / static_cast<int> (std::lround (0.005 * fs));
            if (frame != lastTrack)
            {
                lastTrack = frame;
                const auto& st = engine.status();
                // 状態は入力の新しい端のもの。遅延の分を詰めた時刻にはしない（目安）
                track << (pos + n) / fs << "," << st.f0.load();
                for (int k = 0; k < 4; ++k)
                    track << "," << st.formant[static_cast<size_t> (k)].load();
                for (int k = 0; k < 4; ++k)
                    track << "," << (st.tracked[static_cast<size_t> (k)].load() ? 1 : 0);
                track << "\n";
            }
        }
    }
    const double secs = std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();

    const juce::File outFile { juce::String (juce::CharPointer_UTF8 (argv[2])) };
    outFile.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = outFile.createOutputStream();
    const auto options = juce::AudioFormatWriterOptions {}.withSampleRate (fs).withNumChannels (1).withBitsPerSample (24);
    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream, options));
    if (writer == nullptr)
    {
        std::printf ("cannot write %s\n", argv[2]);
        return 1;
    }
    const float* p = out.data() + (align ? latency : 0);
    const int outLen = align ? len : total;
    writer->writeFromFloatArrays (&p, 1, outLen);

    std::printf ("latency %d samples (%.2f ms), cpu %.2f%% of realtime\n", latency, latency * 1000.0 / fs, 100.0 * secs / (len / fs));
    return 0;
}

// Windows ではコマンドラインの引数を UTF-8 にして渡す（日本語のファイル名を読めるように）
#ifdef _WIN32
int wmain (int argc, wchar_t** wargv)
{
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i)
        args.push_back (juce::String (wargv[i]).toStdString());
    std::vector<char*> ptrs;
    for (auto& a : args)
        ptrs.push_back (a.data());
    return run (argc, ptrs.data());
}
#else
int main (int argc, char** argv)
{
    return run (argc, argv);
}
#endif
