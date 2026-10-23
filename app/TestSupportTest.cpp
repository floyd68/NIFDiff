// TestSupportTest - verifies TestSupport.h itself. Run with no arguments it
// checks single evaluation, failure counting and REQUIRE early exit (in every
// configuration, Release included). With --deliberate-failure it fails one
// CHECK on purpose; CTest runs that mode as a WILL_FAIL test to prove a failed
// check reaches the process exit code.
#include "TestSupport.h"

#include <cstring>
#include <iostream>

namespace
{

// Meta-checks on the framework can't use the framework they are verifying.
int g_metaFailures = 0;

void Expect(bool condition, const char* message)
{
    if (!condition)
    {
        ++g_metaFailures;
        std::cout << "META FAIL: " << message << '\n';
    }
}

void VerifySingleEvaluation()
{
    int calls = 0;
    auto passing = [&] { ++calls; return true; };
    auto failing = [&] { ++calls; return false; };

    CHECK(passing());
    Expect(calls == 1, "passing CHECK evaluates its expression once");
    Expect(nsk::test::FailureCount() == 0, "passing CHECK records no failure");

    CHECK(failing());
    Expect(calls == 2, "failing CHECK evaluates its expression once");
    Expect(nsk::test::FailureCount() == 1, "failing CHECK records one failure");

    REQUIRE(passing());
    Expect(calls == 3, "passing REQUIRE evaluates its expression once");
    nsk::test::FailureCount() = 0;
}

void VerifyRequireStopsBody()
{
    int calls = 0;
    bool reachedAfterRequire = false;
    const int exitCode = nsk::test::Run([&]
    {
        REQUIRE([&] { ++calls; return false; }());
        reachedAfterRequire = true;
    });
    Expect(calls == 1, "failing REQUIRE evaluates its expression once");
    Expect(!reachedAfterRequire, "failing REQUIRE stops the test body");
    Expect(exitCode != 0, "failing REQUIRE yields a non-zero exit code");
    Expect(nsk::test::FailureCount() == 1, "failing REQUIRE records one failure");
    nsk::test::FailureCount() = 0;
}

void VerifyExceptionIsFailure()
{
    const int exitCode = nsk::test::Run([] { throw std::exception(); });
    Expect(exitCode != 0, "unexpected exception yields a non-zero exit code");
    nsk::test::FailureCount() = 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--deliberate-failure") == 0)
        return nsk::test::Run([] { CHECK(1 + 1 == 3); });

    VerifySingleEvaluation();
    VerifyRequireStopsBody();
    VerifyExceptionIsFailure();
    Expect(nsk::test::Run([] { CHECK(true); }) == 0, "all-passing body yields exit code 0");

    if (g_metaFailures != 0)
    {
        std::cout << g_metaFailures << " meta failure(s)\n";
        return 1;
    }
    std::cout << "TestSupportTest: all checks passed\n";
    return 0;
}
