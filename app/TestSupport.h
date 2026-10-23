// TestSupport.h - checks for the test executables that stay active in every
// build configuration. <cassert> compiles out under NDEBUG together with any
// call written inside it, so a Release test run would neither check nor even
// perform those operations.
//
//   CHECK(expr)    record a failure and keep going
//   REQUIRE(expr)  record a failure and stop the current test body (use it
//                  for preconditions such as device/window creation that the
//                  following code dereferences)
//
// Both evaluate `expr` exactly once. Run the test body through
// nsk::test::Run, which stops at a failed REQUIRE and returns the process
// exit code (0 only when nothing failed):
//
//   int main() { return nsk::test::Run([] { REQUIRE(Setup()); CHECK(x == 1); }); }
#pragma once

#include <exception>
#include <iostream>

namespace nsk::test
{

// Thrown by REQUIRE; caught by Run.
struct RequireFailure
{
};

inline int& FailureCount()
{
    static int failures = 0;
    return failures;
}

inline void ReportFailure(const char* kind, const char* expression, const char* file, int line)
{
    ++FailureCount();
    std::cout << "FAIL: " << kind << '(' << expression << ") at " << file << ':' << line << '\n';
}

inline int ExitCode()
{
    if (FailureCount() == 0)
        return 0;
    std::cout << FailureCount() << " failure(s)\n";
    return 1;
}

template <class Body>
int Run(Body&& body)
{
    try
    {
        body();
    }
    catch (const RequireFailure&)
    {
        // Already reported; the remaining checks depend on the failed one.
    }
    catch (const std::exception& e)
    {
        ++FailureCount();
        std::cout << "FAIL: unexpected exception: " << e.what() << '\n';
    }
    catch (...)
    {
        ++FailureCount();
        std::cout << "FAIL: unexpected non-standard exception\n";
    }
    return ExitCode();
}

} // namespace nsk::test

#define CHECK(expr)                                                              \
    do                                                                           \
    {                                                                            \
        if (!(expr))                                                             \
            ::nsk::test::ReportFailure("CHECK", #expr, __FILE__, __LINE__);      \
    } while (0)

#define REQUIRE(expr)                                                            \
    do                                                                           \
    {                                                                            \
        if (!(expr))                                                             \
        {                                                                        \
            ::nsk::test::ReportFailure("REQUIRE", #expr, __FILE__, __LINE__);    \
            throw ::nsk::test::RequireFailure {};                                \
        }                                                                        \
    } while (0)
