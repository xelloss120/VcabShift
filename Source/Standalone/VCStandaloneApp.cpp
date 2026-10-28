// EXE 版のアプリ。JUCE 9.0.4 の juce_audio_plugin_client_Standalone.cpp の StandaloneFilterApp をもとに、
// 見た目（LookAndFeel）と、JUCE の画面の文字の日本語訳を加えたもの。
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_plugin_client/detail/juce_CreatePluginFilter.h>

#include "../Plugin/LookAndFeel.h"
#include "../Plugin/PluginProcessor.h"
#include "VCStandaloneWindow.h"

namespace
{

// JUCE の画面（音声の設定、状態の保存）の文字の訳
// 訳の中に )" が出てくるので、区切りの印を付けた生の文字列にする
const char* const kTranslations = R"TR(language: Japanese

"Audio/MIDI Settings" = "オーディオ設定"
"Audio/MIDI Settings..." = "オーディオ設定…"
"Save current state" = "今の設定を保存"
"Save current state..." = "今の設定を保存…"
"Load a saved state" = "保存した設定を読み込む"
"Load a saved state..." = "保存した設定を読み込む…"
"Reset to default state" = "初期値に戻す"
"Error whilst saving" = "保存できませんでした"
"Error whilst loading" = "読み込めませんでした"
"Couldn't write to the specified file!" = "指定したファイルに書き込めませんでした。"
"Couldn't read from the specified file!" = "指定したファイルを読み込めませんでした。"
"Audio device type:" = "デバイスの種類:"
"Device:" = "デバイス:"
"Input:" = "入力:"
"Output:" = "出力:"
"Test" = "テスト"
"Plays a test tone" = "テスト音を鳴らします"
"Active input channels:" = "使う入力チャンネル:"
"Active output channels:" = "使う出力チャンネル:"
"Sample rate:" = "サンプルレート:"
"Audio buffer size:" = "バッファの大きさ:"
"Control Panel" = "コントロールパネル"
"Opens the device's own control panel" = "デバイスのコントロールパネルを開きます"
"Reset Device" = "デバイスを開き直す"
"Resets the audio interface - sometimes needed after changing a device's properties in its custom control panel" = "オーディオインターフェースを開き直します（デバイスのコントロールパネルで設定を変えた後に必要なことがあります）"
"Show advanced settings..." = "詳しい設定を表示…"
"(no audio input channels found)" = "（入力チャンネルがありません）"
"(no audio output channels found)" = "（出力チャンネルがありません）"
"Left" = "左"
"Right" = "右"
"none" = "なし"
"OK" = "OK"
"Error when trying to open audio device!" = "音声デバイスを開けませんでした。"
"Couldn't open the input device!" = "入力デバイスを開けませんでした。"
"Couldn't open the output device!" = "出力デバイスを開けませんでした。"
"Couldn't start the input device!" = "入力デバイスを始められませんでした。"
"Couldn't start the output device!" = "出力デバイスを始められませんでした。"
"Couldn't open the output device (buffer size mismatch)" = "出力デバイスを開けませんでした（バッファの大きさが合いません）。"
"The input and output devices don't share a common sample rate!" = "入力と出力のデバイスに、共通のサンプルレートがありません。"
)TR";

} // namespace

namespace juce
{

class VCStandaloneApp final : public JUCEApplication
{
public:
    VCStandaloneApp()
    {
        PropertiesFile::Options options;
        options.applicationName = CharPointer_UTF8 (JucePlugin_Name);
        options.filenameSuffix = ".settings";
        options.osxLibrarySubFolder = "Application Support";
        options.folderName = "";
        appProperties.setStorageParameters (options);
    }

    const String getApplicationName() override { return CharPointer_UTF8 (JucePlugin_Name); }
    const String getApplicationVersion() override { return JucePlugin_VersionString; }
    bool moreThanOneInstanceAllowed() override { return true; }
    void anotherInstanceStarted (const String&) override {}

    void initialise (const String&) override
    {
        LocalisedStrings::setCurrentMappings (new LocalisedStrings (String (CharPointer_UTF8 (kTranslations)), false));
        LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

        if (Desktop::getInstance().getDisplays().displays.isEmpty())
        {
            pluginHolder = createPluginHolder();
            return;
        }
        // 画面の「オーディオ設定」ボタンから設定画面を開けるようにする（画面は窓を作るときに作られるので、その前に入れる）
        auto holder = createPluginHolder();
        if (auto* p = dynamic_cast<VcabShiftProcessor*> (holder->processor.get()))
            p->openAudioSettings = [h = holder.get()] { h->showAudioSettingsDialog(); };
        mainWindow = std::make_unique<VCStandaloneFilterWindow> (getApplicationName(),
                                                                 lookAndFeel.findColour (ResizableWindow::backgroundColourId),
                                                                 std::move (holder));
        mainWindow->setVisible (true);
    }

    void shutdown() override
    {
        pluginHolder = nullptr;
        mainWindow = nullptr;
        appProperties.saveIfNeeded();
        LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

    void systemRequestedQuit() override
    {
        if (pluginHolder != nullptr)
            pluginHolder->savePluginState();
        if (mainWindow != nullptr)
            mainWindow->pluginHolder->savePluginState();

        if (ModalComponentManager::getInstance()->cancelAllModalComponents())
        {
            Timer::callAfterDelay (100, []()
            {
                if (auto app = JUCEApplicationBase::getInstance())
                    app->systemRequestedQuit();
            });
        }
        else
            quit();
    }

private:
    VCLookAndFeel lookAndFeel;
    ApplicationProperties appProperties;
    std::unique_ptr<VCStandaloneFilterWindow> mainWindow;
    std::unique_ptr<VCStandalonePluginHolder> pluginHolder;

    std::unique_ptr<VCStandalonePluginHolder> createPluginHolder()
    {
        return std::make_unique<VCStandalonePluginHolder> (appProperties.getUserSettings(), false, String {}, nullptr,
                                                           Array<VCStandalonePluginHolder::PluginInOuts> {}, false);
    }
};

} // namespace juce

juce::JUCEApplicationBase* juce_CreateApplication();
juce::JUCEApplicationBase* juce_CreateApplication() { return new juce::VCStandaloneApp(); }
