// tsm_logger.h

#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/fmt/ostr.h>
#include <fmod_common.h>
#include <iostream>

namespace fmt {
    template<>
    struct formatter<FMOD_RESULT> {
        template<typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }

        template<typename FormatContext>
        auto format(const FMOD_RESULT& result, FormatContext& ctx) const {
            return format_to(ctx.out(), "{}", static_cast<int>(result));
        }
    };
}

namespace tsm {

class Logger {
public:
    static void init() {
        try {
            auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            console_sink->set_level(spdlog::level::debug);
            
            auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("logs/tsm.log", true);
            file_sink->set_level(spdlog::level::trace);

            auto logger = std::make_shared<spdlog::logger>("TSM", spdlog::sinks_init_list{console_sink, file_sink});
            logger->set_level(spdlog::level::trace);
            
            logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");

            spdlog::set_default_logger(logger);
            
            spdlog::info("Logger initialized successfully");
        }
        catch (const spdlog::spdlog_ex& ex) {
            std::cerr << "Error initializing logger: " << ex.what() << std::endl;
        }
    }

    static void shutdown() {
        spdlog::shutdown();
    }
};

}
