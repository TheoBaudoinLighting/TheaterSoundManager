// tsm_logger.h

#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/null_sink.h>
#include <memory>
#include <string>

namespace TSM {

class Logger {
public:
    enum class ConsoleTarget {
        Stdout,
        Stderr,
        Disabled
    };

    struct Options {
        ConsoleTarget console = ConsoleTarget::Stdout;
        bool fileLogging = false;
        std::string filePath;
        spdlog::level::level_enum level = spdlog::level::trace;
    };

    static void Init(const Options& options = {}) {
        std::vector<spdlog::sink_ptr> sinks;
        if (options.console == ConsoleTarget::Stdout) {
            auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            sink->set_pattern("[TSM] [%H:%M:%S] [%^%l%$] %v");
            sinks.push_back(std::move(sink));
        } else if (options.console == ConsoleTarget::Stderr) {
            auto sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
            sink->set_pattern("[TSM] [%H:%M:%S] [%^%l%$] %v");
            sinks.push_back(std::move(sink));
        }

        if (options.fileLogging) {
            auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                options.filePath, true);
            fileSink->set_pattern("[TSM] [%H:%M:%S] [%l] %v");
            sinks.push_back(std::move(fileSink));
        }

        if (sinks.empty()) {
            sinks.push_back(std::make_shared<spdlog::sinks::null_sink_mt>());
        }
        auto logger = std::make_shared<spdlog::logger>("TSM", sinks.begin(), sinks.end());
        
        spdlog::set_default_logger(logger);
        spdlog::set_level(options.level);
    }

    static void Shutdown() {
        spdlog::shutdown();
    }
};

} // namespace tsm
