// 開発用の画面の撮影ツール。WAV を実際の時間の速さで流しながら、プラグインの画面を PNG に書き出す。
//   vcabsh_shot 入力.wav 出力.png [キー=値 ...] [--at 秒]
// --at: 何秒流したところで撮るか（初期値 2 秒）
#include "PluginProcessor.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdio>

static int run (int argc, char** argv)
{
    if (argc < 3)
    {
        std::printf ("usage: vcabsh_shot in.wav out.png [key=value ...] [--at seconds]\n");
        return 1;
    }
    juce::ScopedJuceInitialiser_GUI gui;

    double at = 2.0;
    auto processor = std::make_unique<VcabShiftProcessor>();
    for (int i = 3; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--at" && i + 1 < argc)
        {
            at = std::stod (argv[++i]);
            continue;
        }
        const auto eq = a.find ('=');
        bool ok = false;
        if (eq != std::string::npos)
            for (const auto& p : vcab::paramTable())
                if (a.substr (0, eq) == p.key)
                {
                    auto* param = processor->param (p.id);
                    param->setValueNotifyingHost (param->convertTo0to1 (std::stof (a.substr (eq + 1))));
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
    const double fs = reader->sampleRate;
    const int len = static_cast<int> (reader->lengthInSamples);
    juce::AudioBuffer<float> input (static_cast<int> (reader->numChannels), len);
    reader->read (&input, 0, len, 0, true, true);

    constexpr int kBlock = 256;
    processor->setPlayConfigDetails (2, 2, fs, kBlock);
    processor->prepareToPlay (fs, kBlock);
    std::unique_ptr<juce::AudioProcessorEditor> editor (processor->createEditor());
    editor->setVisible (true);

    juce::AudioBuffer<float> block (2, kBlock);
    juce::MidiBuffer midi;
    int pos = 0;
    const int total = static_cast<int> (at * fs);
    const int perTick = static_cast<int> (fs / 60.0);
    while (pos < total)
    {
        for (int done = 0; done < perTick; done += kBlock)
        {
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < kBlock; ++i)
                {
                    const int s = (pos + i) % len;
                    block.setSample (c, i, input.getSample (std::min (c, input.getNumChannels() - 1), s));
                }
            processor->processBlock (block, midi);
            pos += kBlock;
        }
        juce::MessageManager::getInstance()->runDispatchLoopUntil (16);
    }

    const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
    const juce::File outFile { juce::String (juce::CharPointer_UTF8 (argv[2])) };
    outFile.deleteFile();
    juce::FileOutputStream out (outFile);
    juce::PNGImageFormat png;
    if (! out.openedOk() || ! png.writeImageToStream (image, out))
    {
        std::printf ("cannot write %s\n", argv[2]);
        return 1;
    }
    editor = nullptr;
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
