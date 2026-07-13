// NifLog.h - spdlog-based diagnostic logger for the NIF parsing/scene-
// building pipeline (core/NifDocument.cpp, core/SceneBuilder.cpp).
//
// NIFDiff.exe is a WIN32-subsystem app with no console, so this logs to a
// fixed file (%TEMP%\NIFDiff\NifDiffDebug.log) plus an MSVC debugger sink in
// Debug builds - both NIFDiff.exe and the console-mode NifValidate.exe/
// ResourceResolveTest.exe share this file, so a "why is this specific NIF
// blank in NIFDiff" report can be answered by opening one log file after
// reproducing in either tool.
//
// Not modeled on FICture2's AppLog.h (whose FIC2_LOG_* macros need
// AppLog::Init/Shutdown wired through the app shell before they do
// anything; a vendored copy of that header lived at third_party/ until the
// dependency cleanup removed it unused). This is a small, self-initializing,
// header-only spdlog logger scoped to core/ instead, so it produces output
// with zero app-shell wiring.
#pragma once

#pragma warning(push)
#pragma warning(disable: 4996) // 'localtime': may be unsafe (spdlog internal)
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#ifdef _DEBUG
#include <spdlog/sinks/msvc_sink.h>
#endif
#pragma warning(pop)

#include <filesystem>
#include <memory>
#include <vector>

namespace nsk
{

// Lazily-initialized, process-wide "nifdoc" logger. Thread-safe (C++11
// static local init) and safe to call from any TU that includes this header.
inline const std::shared_ptr<spdlog::logger>& NifLogger()
{
    static std::shared_ptr<spdlog::logger> s_logger = []
    {
        std::error_code ec;
        std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
        if (ec)
            dir = L".";
        dir /= L"NIFDiff";
        std::filesystem::create_directories(dir, ec);
        const std::filesystem::path logPath = dir / L"NifDiffDebug.log";

        std::vector<spdlog::sink_ptr> sinks;
        sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath.string(), /*truncate=*/false));
#ifdef _DEBUG
        sinks.push_back(std::make_shared<spdlog::sinks::msvc_sink_mt>());
#endif
        auto logger = std::make_shared<spdlog::logger>("nifdoc", sinks.begin(), sinks.end());
        logger->set_level(spdlog::level::trace);
        logger->set_pattern("[%H:%M:%S.%e] [%l] %v");
        logger->flush_on(spdlog::level::trace); // diagnostic tool, not a hot path - flush every line
        return logger;
    }();
    return s_logger;
}

} // namespace nsk

#define NIFLOG_TRACE(...) ::nsk::NifLogger()->trace(__VA_ARGS__)
#define NIFLOG_INFO(...)  ::nsk::NifLogger()->info(__VA_ARGS__)
#define NIFLOG_WARN(...)  ::nsk::NifLogger()->warn(__VA_ARGS__)
#define NIFLOG_ERROR(...) ::nsk::NifLogger()->error(__VA_ARGS__)
