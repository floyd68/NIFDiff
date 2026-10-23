// ScanWiringTest - archive-scan hook capture, cancellation, and the
// LateBoundSignal that delivers the scan's completion to a receiver created
// later (the startup wiring in NIFDiffApp).
//
// The scan fixtures are garbage *.bsa files: every one is a scan candidate,
// fails to open, and still goes through the I/O gate - enough to count hook
// invocations without real archives.
#include "LateBoundSignal.h"
#include "ResourceResolver.h"
#include "TestSupport.h"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace
{

fs::path MakeDataDir(const wchar_t* name, int archives)
{
    const fs::path dir = fs::temp_directory_path() / L"nifdiff_scan_wiring_test" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    for (int i = 0; i < archives; ++i)
    {
        std::ofstream out(dir / (L"fake" + std::to_wstring(i) + L".bsa"), std::ios::binary);
        out << "not an archive";
    }
    return dir;
}

void TestHooksCapturedAtLaunch()
{
    constexpr int kArchives = 6;
    const fs::path dir = MakeDataDir(L"capture", kArchives);

    std::atomic<int> acquired { 0 };
    std::atomic<int> released { 0 };
    std::atomic<int> completed { 0 };

    nsk::ResourceResolver resolver;
    resolver.SetAutoLoadArchives(true);
    resolver.SetIoGate([&] { ++acquired; }, [&] { ++released; });
    resolver.SetOnScanCompleted([&] { ++completed; });
    resolver.SetGameData(dir.wstring()); // launches the scan
    // Replacing the hooks after launch must not affect the running scan
    // (and is no longer a data race: the worker reads its own copies).
    resolver.SetIoGate({}, {});
    resolver.SetOnScanCompleted({});
    resolver.WaitForArchiveScan();

    CHECK(acquired.load() == kArchives);
    CHECK(released.load() == kArchives);
    CHECK(completed.load() == 1);
    CHECK(resolver.ArchiveCount() == 0); // none of the fakes opens

    // The next scan runs with the hooks current at its launch: none.
    acquired = 0;
    released = 0;
    completed = 0;
    resolver.ReloadArchives();
    resolver.WaitForArchiveScan();
    CHECK(acquired.load() == 0);
    CHECK(released.load() == 0);
    CHECK(completed.load() == 0);
}

void TestGateNeedsBothHalves()
{
    constexpr int kArchives = 3;
    const fs::path dir = MakeDataDir(L"half", kArchives);

    std::atomic<int> acquired { 0 };
    nsk::ResourceResolver resolver;
    resolver.SetAutoLoadArchives(true);
    resolver.SetIoGate([&] { ++acquired; }, {}); // release half missing
    resolver.SetGameData(dir.wstring());
    resolver.WaitForArchiveScan();
    CHECK(acquired.load() == 0); // an unpaired acquire would leak permits
}

void TestCancelStopsScan()
{
    // Far more candidates than scan workers (hardware_concurrency / 2), and
    // every worker is held at its first gate acquire until the scan has been
    // cancelled - so the number of archives touched is bounded by the worker
    // count, deterministically.
    constexpr int kArchives = 256;
    const fs::path dir = MakeDataDir(L"cancel", kArchives);

    std::mutex mutex;
    std::condition_variable cv;
    bool gateOpen = false;
    std::atomic<int> acquired { 0 };
    std::atomic<int> released { 0 };
    std::atomic<int> completed { 0 };

    nsk::ResourceResolver resolver;
    resolver.SetAutoLoadArchives(true);
    resolver.SetIoGate(
        [&]
        {
            ++acquired;
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [&] { return gateOpen; });
        },
        [&] { ++released; });
    resolver.SetOnScanCompleted([&] { ++completed; });
    resolver.SetGameData(dir.wstring());

    resolver.CancelArchiveScan();
    {
        std::lock_guard<std::mutex> lock(mutex);
        gateOpen = true;
    }
    cv.notify_all();
    resolver.WaitForArchiveScan();

    const unsigned hw = std::thread::hardware_concurrency();
    const int maxWorkers = static_cast<int>(std::max(1u, (hw > 0 ? hw : 2) / 2));
    CHECK(acquired.load() <= maxWorkers);
    CHECK(acquired.load() < kArchives);
    CHECK(released.load() == acquired.load()); // every acquired permit is returned
    CHECK(completed.load() == 0);              // a cancelled scan does not report completion
    CHECK(resolver.IsArchiveScanReady());

    // A later reload scans normally again.
    {
        std::lock_guard<std::mutex> lock(mutex);
        gateOpen = true;
    }
    acquired = 0;
    released = 0;
    resolver.ReloadArchives();
    resolver.WaitForArchiveScan();
    CHECK(acquired.load() == kArchives);
    CHECK(released.load() == kArchives);
    CHECK(completed.load() == 1);
}

void TestLateBoundSignal()
{
    {
        nsk::LateBoundSignal signal;
        int calls = 0;
        signal.Fire(); // before Bind: remembered
        signal.Fire(); // repeated early fires collapse into one pending fire
        CHECK(calls == 0);
        signal.Bind([&] { ++calls; });
        CHECK(calls == 1);
        signal.Fire();
        CHECK(calls == 2);
    }
    {
        nsk::LateBoundSignal signal;
        int calls = 0;
        signal.Bind([&] { ++calls; }); // nothing pending
        CHECK(calls == 0);
        signal.Fire();
        CHECK(calls == 1);
    }
    {
        nsk::LateBoundSignal signal;
        int calls = 0;
        signal.Fire();
        signal.Reset(); // drops the pending fire
        signal.Bind([&] { ++calls; });
        CHECK(calls == 0);
        signal.Reset(); // drops the target
        signal.Fire();
        CHECK(calls == 0);
    }
    {
        // Fire racing Bind from another thread: the fire is delivered exactly
        // once whichever side wins.
        for (int round = 0; round < 200; ++round)
        {
            nsk::LateBoundSignal signal;
            std::atomic<int> calls { 0 };
            std::thread firer([&] { signal.Fire(); });
            signal.Bind([&] { ++calls; });
            firer.join();
            CHECK(calls.load() == 1);
        }
    }
}

} // namespace

int main()
{
    const int exitCode = nsk::test::Run([]
    {
        TestHooksCapturedAtLaunch();
        TestGateNeedsBothHalves();
        TestCancelStopsScan();
        TestLateBoundSignal();
    });
    std::error_code ec;
    fs::remove_all(fs::temp_directory_path() / L"nifdiff_scan_wiring_test", ec);
    return exitCode;
}
