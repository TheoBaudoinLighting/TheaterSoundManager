#include "tsm_cli.h"
#include "tsm_logger.h"
#include "tsm_runtime.h"
#include "tsm_ui_manager.h"

#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#ifdef _WIN32
    #undef main
    #define SDL_MAIN_HANDLED
#endif

namespace
{

bool InitializeGuiLogger()
{
    try
    {
        TSM::Logger::Options options;
        options.fileLogging = true;
        options.filePath = TSM::GetApplicationDataFilePath("tsm.log");
        TSM::Logger::Init(options);
        return true;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Unable to initialize GUI file logging: %s\n", error.what());
        TSM::Logger::Shutdown();
        try
        {
            TSM::Logger::Options fallback;
            fallback.console = TSM::Logger::ConsoleTarget::Stderr;
            fallback.fileLogging = false;
            TSM::Logger::Init(fallback);
            spdlog::warn("File logging is disabled: {}", error.what());
            return true;
        }
        catch (const std::exception& fallbackError)
        {
            std::fprintf(
                stderr,
                "Unable to initialize fallback logging: %s\n",
                fallbackError.what());
            return false;
        }
    }
}

int RunGraphicalApplication(const std::vector<std::string>& arguments)
{
    const std::string executablePath = TSM::GetExecutablePath(arguments.front());
    if (!InitializeGuiLogger())
        return static_cast<int>(TSM::CliExitCode::InputOutput);

    TSM::RuntimeOptions runtimeOptions;
    runtimeOptions.configPath = TSM::ResolveDefaultConfigPath(executablePath);
    // RFCOMM control is unauthenticated and therefore opt-in in production.
    // A cinema deployment should prefer the local NDJSON CLI supervised by
    // the host application, especially for any safety-related workflow.
    runtimeOptions.startBluetooth =
        std::find(arguments.begin() + 1, arguments.end(), "--bluetooth") !=
        arguments.end();

    TSM::ApplicationRuntime runtime;
    std::string error;
    if (!runtime.Initialize(runtimeOptions, error))
    {
        spdlog::error("Application runtime initialization failed: {}", error);
        TSM::Logger::Shutdown();
        const TSM::CliExitCode exitCode =
            runtime.GetLastErrorKind() == TSM::RuntimeErrorKind::Configuration
                ? TSM::CliExitCode::Configuration
                : runtime.GetLastErrorKind() == TSM::RuntimeErrorKind::InputOutput
                    ? TSM::CliExitCode::InputOutput
                    : TSM::CliExitCode::AudioEngine;
        return static_cast<int>(exitCode);
    }

    auto& ui = TSM::UIManager::GetInstance();
    std::string imguiIniPath;
    try
    {
        imguiIniPath = TSM::GetApplicationDataFilePath("imgui.ini");
    }
    catch (const std::exception& pathError)
    {
        spdlog::warn("ImGui layout persistence is disabled: {}", pathError.what());
    }
    bool uiInitialized = false;
    try
    {
        uiInitialized = ui.Init(
            1920, 1080, runtime.GetResourceRoot(), imguiIniPath);
    }
    catch (const std::exception& uiError)
    {
        spdlog::error("GUI initialization raised an exception: {}", uiError.what());
    }
    if (!uiInitialized)
    {
        spdlog::error("Failed to initialize GUI.");
        ui.Shutdown();
        runtime.Shutdown();
        TSM::Logger::Shutdown();
        return static_cast<int>(TSM::CliExitCode::Internal);
    }

    auto lastTime = std::chrono::high_resolution_clock::now();
    while (ui.IsRunning())
    {
        const auto currentTime = std::chrono::high_resolution_clock::now();
        const float deltaTime = std::chrono::duration<float>(currentTime - lastTime).count();
        lastTime = currentTime;

        runtime.Tick(deltaTime);
        ui.HandleEvents();
        ui.PreRender();
        ui.Render();
        ui.PostRender();
    }

    runtime.Shutdown();
    ui.Shutdown();
    TSM::Logger::Shutdown();
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string> arguments = TSM::GetUtf8CommandLineArguments(argc, argv);
    if (TSM::IsCliInvocation(arguments)) return TSM::RunCli(arguments);
    return RunGraphicalApplication(arguments);
}
