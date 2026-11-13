// 開発用の WAV の処理ツール。
//   vcabsh_render 入力.wav 出力.wav [キー=値 ...] [--align] [--track 出力.csv]
// キーは Params.cpp の key（例: pitch=12 formant=6 warp_mode=2）。
// --align: 遅延の分を詰めて、入力と時刻をそろえる。
// --track: 5ms ごとの声の高さと F1〜F4 を CSV に書く（遅延の分を詰めた時刻）。
// --block: 1 回に処理する標本の数（初期値 256。リアルタイムの環境の違いを試すため）。
// --marks: 置いた有声の印ごとの { 時刻（標本、入力の時刻）, 周期, 有声, 相互相関の値 } を CSV に書く。
// --frames: 決まったフレームごとの { 窓の中心の時刻（秒、入力の時刻）, 有声, 高さ } を CSV に書く。
// 終わりに、ブロックごとの処理時間の最大と 99.9% 点も出す（途切れの目安。ブロックの長さに対する割合）。
// --lookahead n / --octave-jump p: ピッチ検出の先読みのフレームの数と、オクターブ跳びの確率（開発用。初期値 2 / 0.001）。
#include "Engine.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>

static int run (int argc, char** argv)
{
    if (argc < 3)
    {
        std::printf ("usage: vcabsh_render in.wav out.wav [key=value ...] [--align] [--track out.csv] [--block n]\n");
        return 1;
    }

    auto values = vcab::defaultValues();
    bool align = false;
    int block = 256;
    std::string trackPath, framesPath, marksPath, grainsPath, streamsPath, backfillPath;
    vcab::Engine::Tuning tuning;
    for (int i = 3; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--align")
        {
            align = true;
            continue;
        }
        if (a == "--block" && i + 1 < argc)
        {
            block = std::max (1, std::stoi (argv[++i]));
            continue;
        }
        if (a == "--no-cues")
        {
            tuning.voicingCues = false;
            continue;
        }
        if (a == "--grains" && i + 1 < argc)
        {
            grainsPath = argv[++i];
            continue;
        }
        if (a == "--backfill" && i + 1 < argc)
        {
            backfillPath = argv[++i];
            continue;
        }
        if (a == "--streams" && i + 1 < argc)
        {
            streamsPath = argv[++i];
            continue;
        }
        if (a == "--marks" && i + 1 < argc)
        {
            marksPath = argv[++i];
            continue;
        }
        if (a == "--frames" && i + 1 < argc)
        {
            framesPath = argv[++i];
            continue;
        }
        if (a == "--lookahead" && i + 1 < argc)
        {
            tuning.pitchLookahead = std::stoi (argv[++i]);
            continue;
        }
        if (a == "--octave-jump" && i + 1 < argc)
        {
            tuning.octaveJump = std::stof (argv[++i]);
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

    const int kBlock = block;
    vcab::Engine engine;
    std::vector<std::array<float, 3>> frameLog;
    engine.setTuning (tuning);
    if (! framesPath.empty())
        frameLog.reserve (static_cast<size_t> (len / (0.005 * fs)) + 16);
    engine.setFrameLog (framesPath.empty() ? nullptr : &frameLog);
    std::vector<std::array<double, 4>> markLog;
    if (! marksPath.empty())
        markLog.reserve (static_cast<size_t> (len / 40) + 16);
    engine.setMarkLog (marksPath.empty() ? nullptr : &markLog);
    std::vector<std::array<double, 4>> grainLog;
    if (! grainsPath.empty())
        grainLog.reserve (static_cast<size_t> (len / 20) + 16);
    engine.setGrainLog (grainsPath.empty() ? nullptr : &grainLog);
    std::vector<std::array<float, 3>> streamLog;
    if (! streamsPath.empty())
        streamLog.reserve (static_cast<size_t> (len) + 65536);
    engine.setStreamLog (streamsPath.empty() ? nullptr : &streamLog);
    std::vector<std::array<double, 3>> backfillLog;
    if (! backfillPath.empty())
        backfillLog.reserve (65536);
    engine.setBackfillLog (backfillPath.empty() ? nullptr : &backfillLog);
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

    std::vector<double> blockSecs;
    blockSecs.reserve (static_cast<size_t> (total / kBlock) + 2);
    // プラグイン（PluginProcessor::processBlock）と同じく、非正規化数を 0 として扱う（無音でフィルターの値が
    // 非正規化数になると計算が遅くなり、負荷の測り方がプラグインと変わるため）
    juce::ScopedNoDenormals noDenormals;
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
        const auto t0 = std::chrono::steady_clock::now();
        engine.process (chans, std::min (numCh, 2), out.data() + pos, n);
        blockSecs.push_back (std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count());

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

    if (! backfillPath.empty())
    {
        std::ofstream bl (backfillPath);
        bl << "onset,out,placed" << std::endl;
        for (const auto& r : backfillLog)
            bl << static_cast<int64_t> (r[0]) << "," << r[1] << "," << r[2] << std::endl;
    }

    if (! streamsPath.empty())
    {
        // 3 チャンネル、32 ビット浮動小数（有声の流れ、無声の流れ、つなぎの角度）。時刻は出力（遅延を詰めない）
        const juce::File sf { juce::String (juce::CharPointer_UTF8 (streamsPath.c_str())) };
        sf.deleteFile();
        std::unique_ptr<juce::OutputStream> ss = sf.createOutputStream();
        const auto so = juce::AudioFormatWriterOptions {}.withSampleRate (fs).withNumChannels (3).withBitsPerSample (32)
                            .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
        std::unique_ptr<juce::AudioFormatWriter> sw (wav.createWriterFor (ss, so));
        std::vector<float> ch[3];
        for (auto& c : ch)
            c.reserve (streamLog.size());
        for (const auto& r : streamLog)
            for (int k = 0; k < 3; ++k)
                ch[k].push_back (r[static_cast<size_t> (k)]);
        const float* cp[3] = { ch[0].data(), ch[1].data(), ch[2].data() };
        if (sw != nullptr)
            sw->writeFromFloatArrays (cp, 3, static_cast<int> (streamLog.size()));
    }

    if (! framesPath.empty())
    {
        std::ofstream fl (framesPath);
        fl << "t,voiced,f0" << std::endl;
        for (const auto& r : frameLog)
            fl << r[0] << "," << r[1] << "," << r[2] << '\n';
    }

    if (! marksPath.empty())
    {
        std::ofstream ml (marksPath);
        ml << "t,period,voiced,score" << std::endl;
        for (const auto& r : markLog)
            ml << static_cast<int64_t> (r[0]) << "," << r[1] << "," << r[2] << "," << r[3] << std::endl;
    }

    if (! grainsPath.empty())
    {
        std::ofstream gl (grainsPath);
        gl << "out,src,period" << std::endl;
        for (const auto& r : grainLog)
            gl << static_cast<int64_t> (r[0]) << "," << static_cast<int64_t> (r[1]) << "," << r[2] << std::endl;
    }

    std::printf ("latency %d samples (%.2f ms), cpu %.2f%% of realtime\n", latency, latency * 1000.0 / fs, 100.0 * secs / (len / fs));
    if (! blockSecs.empty())
    {
        std::vector<double> sorted (blockSecs);
        std::sort (sorted.begin(), sorted.end());
        const double blockLen = kBlock / fs;
        const double worst = sorted.back();
        const double p999 = sorted[std::min (sorted.size() - 1, static_cast<size_t> (0.999 * static_cast<double> (sorted.size())))];
        // 最初の 1 秒（準備の直後でキャッシュなどが温まっていない間）を除いた最大と、その位置
        const size_t skip = std::min (blockSecs.size() - 1, static_cast<size_t> (fs / kBlock));
        const auto it = std::max_element (blockSecs.begin() + static_cast<std::ptrdiff_t> (skip), blockSecs.end());
        const double later = *it;
        const double at = static_cast<double> (std::distance (blockSecs.begin(), it)) * kBlock / fs;
        std::printf ("block max %.1f us (%.1f%% of block), p99.9 %.1f us (%.1f%%), after 1s max %.1f us (%.1f%%) at %.2fs, %zu blocks\n",
                     worst * 1.0e6, 100.0 * worst / blockLen, p999 * 1.0e6, 100.0 * p999 / blockLen,
                     later * 1.0e6, 100.0 * later / blockLen, at, sorted.size());
    }
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
