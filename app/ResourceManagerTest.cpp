// ResourceManagerTest - the worker failure contract (a throwing job reaches its
// onFailure on the completion queue, only while its token is current, and the
// worker survives) and GetOrParseNif's cleanup when the parse throws (I/O
// permit returned, in-flight entry removed, joiners released with nullptr).
#include "NifDocument.h"
#include "NifLog.h"
#include "ResourceManager.h"
#include "TestSupport.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{

using Clock = std::chrono::steady_clock;

// Drains completions until `done` holds or the timeout passes.
template <class Pred>
bool DrainUntil(nsk::ResourceManager& mgr, Pred done, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000))
{
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline)
    {
        mgr.DrainCompletions();
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    mgr.DrainCompletions();
    return done();
}

void TestThrowingJobReachesOnFailure()
{
    nsk::ResourceManager mgr;
    mgr.Start(2);
    int requester = 0;
    const nsk::ResourceManager::Token token { &requester, mgr.BumpGeneration(&requester) };

    int failures = 0;
    std::string what;
    mgr.Submit(nsk::ResourceManager::Priority::ActivePane, token,
        [] { throw std::runtime_error("parse exploded"); },
        [&](const std::string& w) { ++failures; what = w; });
    REQUIRE(DrainUntil(mgr, [&] { return failures > 0; }));
    CHECK(failures == 1);
    CHECK(what == "parse exploded");

    // The worker survived: later jobs still run and complete normally.
    std::atomic<int> ran { 0 };
    int completed = 0;
    for (int i = 0; i < 4; ++i)
    {
        mgr.Submit(nsk::ResourceManager::Priority::ActivePane, token,
            [&] { ++ran; mgr.PostCompletion(token, [&] { ++completed; }); });
    }
    REQUIRE(DrainUntil(mgr, [&] { return completed == 4; }));
    CHECK(ran.load() == 4);
    CHECK(failures == 1);
    mgr.Shutdown();
}

void TestNonStandardException()
{
    nsk::ResourceManager mgr;
    mgr.Start(1);
    int requester = 0;
    const nsk::ResourceManager::Token token { &requester, mgr.BumpGeneration(&requester) };
    std::string what;
    mgr.Submit(nsk::ResourceManager::Priority::Thumbnail, token,
        [] { throw 42; },
        [&](const std::string& w) { what = w; });
    REQUIRE(DrainUntil(mgr, [&] { return !what.empty(); }));
    CHECK(what == "non-standard exception");
    mgr.Shutdown();
}

void TestStaleTokenGetsNoFailure()
{
    nsk::ResourceManager mgr;
    mgr.Start(1);
    int requester = 0;
    const nsk::ResourceManager::Token token { &requester, mgr.BumpGeneration(&requester) };

    std::mutex mutex;
    std::condition_variable cv;
    bool release = false;
    std::atomic<bool> started { false };
    int failures = 0;
    mgr.Submit(nsk::ResourceManager::Priority::Thumbnail, token,
        [&]
        {
            started = true;
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [&] { return release; });
            throw std::runtime_error("late failure");
        },
        [&](const std::string&) { ++failures; });

    while (!started)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    mgr.BumpGeneration(&requester); // the request is superseded while running
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    cv.notify_all();

    // Prove the failure was posted and then dropped: a later current completion
    // drains, and the stale onFailure never runs.
    const nsk::ResourceManager::Token current { &requester, mgr.BumpGeneration(&requester) };
    bool marker = false;
    mgr.Submit(nsk::ResourceManager::Priority::Thumbnail, current,
        [&] { mgr.PostCompletion(current, [&] { marker = true; }); });
    REQUIRE(DrainUntil(mgr, [&] { return marker; }));
    CHECK(failures == 0);
    mgr.Shutdown();
}

void TestNoHandlerIsSurvivable()
{
    nsk::ResourceManager mgr;
    mgr.Start(1);
    int requester = 0;
    const nsk::ResourceManager::Token token { &requester, mgr.BumpGeneration(&requester) };
    mgr.Submit(nsk::ResourceManager::Priority::Thumbnail, token,
        [] { throw std::runtime_error("unhandled"); });
    bool after = false;
    mgr.Submit(nsk::ResourceManager::Priority::Thumbnail, token,
        [&] { mgr.PostCompletion(token, [&] { after = true; }); });
    REQUIRE(DrainUntil(mgr, [&] { return after; }));
    mgr.Shutdown();
}

void TestParseExceptionCleansUp()
{
    nsk::ResourceManager mgr;
    const int freeBefore = mgr.IoPermitsFree();

    std::mutex mutex;
    std::condition_variable cv;
    bool release = false;
    std::atomic<int> loaderCalls { 0 };
    std::atomic<bool> inLoader { false };
    mgr.SetNifLoaderForTesting(
        [&](nsk::NifDocument&, const std::wstring&, std::string*) -> bool
        {
            ++loaderCalls;
            inLoader = true;
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [&] { return release; });
            throw std::runtime_error("corrupt counts");
        });

    const std::wstring path = L"Z:\\does-not-exist\\throwing.nif";
    std::atomic<bool> ownerThrew { false };
    std::thread owner([&]
    {
        try
        {
            mgr.GetOrParseNif(path, nullptr, nsk::ResourceManager::Priority::Thumbnail, true);
        }
        catch (const std::runtime_error&)
        {
            ownerThrew = true;
        }
    });
    while (!inLoader)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(mgr.IoPermitsInUse() == 1); // the owner holds a permit while parsing

    // A second request for the same file joins the in-flight parse; the join
    // hook tells us it holds the in-flight future before the owner is released.
    std::mutex joinMutex;
    std::condition_variable joinCv;
    bool joined = false;
    mgr.SetNifJoinHookForTesting([&]
    {
        std::lock_guard<std::mutex> lock(joinMutex);
        joined = true;
        joinCv.notify_all();
    });
    std::atomic<bool> joinerDone { false };
    std::shared_ptr<const nsk::NifDocument> joinerDoc =
        std::make_shared<nsk::NifDocument>(); // must become nullptr
    std::atomic<bool> joinerThrew { false };
    std::thread joiner([&]
    {
        try
        {
            joinerDoc = mgr.GetOrParseNif(path, nullptr, nsk::ResourceManager::Priority::Thumbnail, true);
        }
        catch (...)
        {
            joinerThrew = true;
        }
        joinerDone = true;
    });
    bool joinedInTime = false;
    {
        std::unique_lock<std::mutex> lock(joinMutex);
        joinedInTime = joinCv.wait_for(lock, std::chrono::seconds(10), [&] { return joined; });
    }
    CHECK(joinedInTime); // bounded wait, for diagnostics only
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    cv.notify_all();
    owner.join();
    joiner.join();
    mgr.SetNifJoinHookForTesting({});

    CHECK(ownerThrew.load());
    CHECK(loaderCalls.load() == 1);   // the joiner joined instead of re-parsing
    CHECK(!joinerThrew.load());
    CHECK(joinerDoc == nullptr);      // released with nullptr, exactly once
    CHECK(mgr.IoPermitsInUse() == 0); // permit returned on the exception path
    CHECK(mgr.IoPermitsFree() == freeBefore);

    // The in-flight entry is gone: a new request parses again (and a plain
    // load failure still yields nullptr without throwing).
    mgr.SetNifLoaderForTesting(
        [&](nsk::NifDocument&, const std::wstring&, std::string*) { ++loaderCalls; return false; });
    std::string error;
    CHECK(mgr.GetOrParseNif(path, &error, nsk::ResourceManager::Priority::Thumbnail, true) == nullptr);
    CHECK(loaderCalls.load() == 2);
    CHECK(mgr.IoPermitsFree() == freeBefore);
}

// A header-only Skyrim SE NIF with zero blocks: the smallest input for which
// NifDocument::isValid() holds, so a test can reach the cache-publication step.
std::vector<std::uint8_t> MinimalNif()
{
    std::vector<std::uint8_t> b;
    auto u8 = [&](std::uint8_t v) { b.push_back(v); };
    auto u16 = [&](std::uint16_t v) { u8(static_cast<std::uint8_t>(v)); u8(static_cast<std::uint8_t>(v >> 8)); };
    auto u32 = [&](std::uint32_t v) { u16(static_cast<std::uint16_t>(v)); u16(static_cast<std::uint16_t>(v >> 16)); };
    for (char c : std::string("Gamebryo File Format, Version 20.2.0.7\n"))
        u8(static_cast<std::uint8_t>(c));
    u32(0x14020007); // version 20.2.0.7
    u8(1);           // endian: little
    u32(12);         // user version
    u32(0);          // num blocks
    u32(100);        // BS version (Skyrim SE)
    u8(0);           // author
    u8(0);           // process script
    u8(0);           // export script
    u16(0);          // num block types
    u32(0);          // num strings
    u32(0);          // max string length
    u32(0);          // num groups
    u8(0);           // padding: the header parser requires bytes after the groups
    return b;
}

void TestCachePublicationFailure()
{
    nsk::ResourceManager mgr;
    const std::vector<std::uint8_t> bytes = MinimalNif();
    std::atomic<int> loaderCalls { 0 };
    mgr.SetNifLoaderForTesting(
        [&](nsk::NifDocument& doc, const std::wstring&, std::string* error)
        {
            ++loaderCalls;
            return doc.loadFromMemory(bytes, error);
        });
    std::atomic<bool> failPublish { true };
    mgr.SetNifCachePublishHookForTesting([&]
    {
        if (failPublish)
            throw std::bad_alloc();
    });

    const std::wstring path = L"Z:\\does-not-exist\\publish.nif";
    // Before the fix this deadlocked: the cleanup re-locked the cache mutex the
    // thread already held. Run it on a thread so a regression fails instead of
    // hanging the suite.
    std::shared_ptr<const nsk::NifDocument> doc;
    std::atomic<bool> finished { false };
    std::atomic<bool> threw { false };
    std::thread t([&]
    {
        try
        {
            doc = mgr.GetOrParseNif(path, nullptr, nsk::ResourceManager::Priority::Thumbnail, true);
        }
        catch (...)
        {
            threw = true;
        }
        finished = true;
    });
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (!finished && Clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    REQUIRE(finished.load()); // (a hang would leave the thread blocked; the process then aborts)
    t.join();

    CHECK(!threw.load());
    CHECK(doc != nullptr);              // the parsed document is still handed out
    CHECK(mgr.NifCacheSize() == 0);     // ...but not cached
    CHECK(mgr.IoPermitsInUse() == 0);

    // Cache and LRU stayed consistent: the next request parses again and,
    // with publication working, is cached and then served from the cache.
    failPublish = false;
    CHECK(mgr.GetOrParseNif(path, nullptr, nsk::ResourceManager::Priority::Thumbnail, true) != nullptr);
    CHECK(loaderCalls.load() == 2);
    CHECK(mgr.NifCacheSize() == 1);
    CHECK(mgr.GetOrParseNif(path, nullptr, nsk::ResourceManager::Priority::Thumbnail, true) != nullptr);
    CHECK(loaderCalls.load() == 2);
}

// Run as its own process (see main): the logger cannot open its file, which
// makes every log call throw for the rest of the process.
int RunLoggerUnavailable()
{
    return nsk::test::Run([]
    {
        bool loggerThrows = false;
        try
        {
            nsk::NifLogger();
        }
        catch (...)
        {
            loggerThrows = true;
        }
        REQUIRE(loggerThrows); // the setup really broke the logger

        nsk::ResourceManager mgr;
        mgr.Start(1);
        int requester = 0;
        const nsk::ResourceManager::Token token { &requester, mgr.BumpGeneration(&requester) };
        std::string what;
        mgr.Submit(nsk::ResourceManager::Priority::ActivePane, token,
            [] { throw std::runtime_error("parse exploded"); },
            [&](const std::string& w) { what = w; });
        REQUIRE(DrainUntil(mgr, [&] { return !what.empty(); }));
        CHECK(what == "parse exploded");
        mgr.Shutdown();
    });
}

void TestParseExceptionInJobReachesOnFailure()
{
    nsk::ResourceManager mgr;
    mgr.Start(2);
    mgr.SetNifLoaderForTesting(
        [](nsk::NifDocument&, const std::wstring&, std::string*) -> bool { throw std::length_error("vector too long"); });
    int requester = 0;
    const nsk::ResourceManager::Token token { &requester, mgr.BumpGeneration(&requester) };
    std::string what;
    mgr.Submit(nsk::ResourceManager::Priority::ActivePane, token,
        [&] { mgr.GetOrParseNif(L"Z:\\x.nif", nullptr, nsk::ResourceManager::Priority::ActivePane, true); },
        [&](const std::string& w) { what = w; });
    REQUIRE(DrainUntil(mgr, [&] { return !what.empty(); }));
    CHECK(what == "vector too long");
    CHECK(mgr.IoPermitsInUse() == 0);
    mgr.Shutdown();
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc > 1 && std::wstring(argv[1]) == L"--logger-unavailable")
    {
        // Point TEMP/TMP at a fresh folder whose "NIFDiff" entry is a FILE, so
        // the logger cannot create its directory or open its log file.
        const std::filesystem::path root =
            std::filesystem::temp_directory_path() / L"nifdiff_rm_logger_test";
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root);
        std::ofstream(root / L"NIFDiff") << "blocks the log directory";
        _wputenv_s(L"TEMP", root.c_str());
        _wputenv_s(L"TMP", root.c_str());
        const int exitCode = RunLoggerUnavailable();
        std::filesystem::remove_all(root, ec);
        return exitCode;
    }

    return nsk::test::Run([]
    {
        TestThrowingJobReachesOnFailure();
        TestNonStandardException();
        TestStaleTokenGetsNoFailure();
        TestNoHandlerIsSurvivable();
        TestParseExceptionCleansUp();
        TestCachePublicationFailure();
        TestParseExceptionInJobReachesOnFailure();
    });
}
