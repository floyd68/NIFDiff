// ResourceManagerTest - the worker failure contract (a throwing job reaches its
// onFailure on the completion queue, only while its token is current, and the
// worker survives) and GetOrParseNif's cleanup when the parse throws (I/O
// permit returned, in-flight entry removed, joiners released with nullptr).
#include "NifDocument.h"
#include "ResourceManager.h"
#include "TestSupport.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

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

    // A second request for the same file joins the in-flight parse.
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
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // let it reach the join
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    cv.notify_all();
    owner.join();
    joiner.join();

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

int main()
{
    return nsk::test::Run([]
    {
        TestThrowingJobReachesOnFailure();
        TestNonStandardException();
        TestStaleTokenGetsNoFailure();
        TestNoHandlerIsSurvivable();
        TestParseExceptionCleansUp();
        TestParseExceptionInJobReachesOnFailure();
    });
}
