// SPDX-License-Identifier: MIT
//
// Test registry and runner.

#include "test.h"

#include <verilated.h>

#include <unistd.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

namespace wtb {

namespace {

std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}

bool g_verbose = false;
const char* g_current = "";

const char* kRed = "\033[31m";
const char* kGreen = "\033[32m";
const char* kYellow = "\033[33m";
const char* kBlue = "\033[36m";
const char* kReset = "\033[0m";

void no_colour() {
  kRed = kGreen = kYellow = kBlue = kReset = "";
}

}  // namespace

void register_test(const TestCase& tc) { registry().push_back(tc); }

void fail_at(const char* file, int line, const std::string& msg) {
  std::ostringstream os;
  const char* base = std::strrchr(file, '/');
  os << (base ? base + 1 : file) << ":" << line << ": " << msg;
  throw TestFailure(os.str());
}

void note(const std::string& msg) {
  if (g_verbose) std::cout << "      " << msg << "\n";
}

void logf(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  std::cout << "      [" << g_current << "] " << buf << "\n";
}

std::ostream& operator<<(std::ostream& os, const MacAddr& m) { return os << m.str(); }

}  // namespace wtb

namespace {

void usage(const char* argv0) {
  std::cout
      << "usage: " << argv0 << " [options] [test-name-substring ...]\n"
      << "  --list        list the registered tests and exit\n"
      << "  --trace       write build/waves/<test>.vcd for every test that runs\n"
      << "  --verbose,-v  print the notes tests emit\n"
      << "  --run-pending run tests marked pending too (they run by default)\n"
      << "  --skip-pending do not run the tests marked pending\n";
}

}  // namespace

int main(int argc, char** argv) {
  using namespace wtb;

  bool trace = false;
  bool skip_pending = false;
  std::vector<std::string> filters;

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--list") {
      for (const auto& t : registry())
        std::cout << t.name << (t.pending ? "  [pending]" : "") << "\n";
      return 0;
    } else if (a == "--trace") {
      trace = true;
    } else if (a == "--verbose" || a == "-v") {
      g_verbose = true;
    } else if (a == "--skip-pending") {
      skip_pending = true;
    } else if (a == "--run-pending") {
      skip_pending = false;
    } else if (a == "--no-colour" || a == "--no-color") {
      no_colour();
    } else if (a == "--help" || a == "-h") {
      usage(argv[0]);
      return 0;
    } else if (!a.empty() && a[0] == '-') {
      std::cerr << "unknown option: " << a << "\n";
      usage(argv[0]);
      return 2;
    } else {
      filters.push_back(a);
    }
  }

  if (!isatty(1)) no_colour();

  int passed = 0, failed = 0, pending = 0, xpassed = 0, skipped = 0;
  std::vector<std::string> failures;

  for (const auto& t : registry()) {
    bool selected = filters.empty();
    for (const auto& f : filters)
      if (std::string(t.name).find(f) != std::string::npos) selected = true;
    if (!selected) continue;
    if (t.pending && skip_pending) {
      skipped++;
      continue;
    }

    g_current = t.name;
    std::cout << kBlue << "[ RUN      ]" << kReset << " " << t.name << "\n";
    std::cout.flush();

    const auto t0 = std::chrono::steady_clock::now();
    std::string error;
    bool ok = true;
    double sim_us = 0.0;
    try {
      Env env(t.name, trace);
      env.power_on_reset();
      t.fn(env);
      sim_us = env.sim().time_ns() / 1000.0;
    } catch (const TestFailure& e) {
      ok = false;
      error = e.what();
    } catch (const std::exception& e) {
      ok = false;
      error = std::string("unexpected exception: ") + e.what();
    }
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count();

    char timing[64];
    snprintf(timing, sizeof(timing), "(%.0f ms wall, %.1f us sim)", ms, sim_us);

    if (ok && !t.pending) {
      passed++;
      std::cout << kGreen << "[       OK ]" << kReset << " " << t.name << " "
                << timing << "\n";
    } else if (ok && t.pending) {
      xpassed++;
      std::cout << kYellow << "[   XPASS  ]" << kReset << " " << t.name
                << " - passes now, drop the pending marker " << timing << "\n";
    } else if (!ok && t.pending) {
      pending++;
      std::cout << kYellow << "[  PENDING ]" << kReset << " " << t.name << " - "
                << (t.pending_reason ? t.pending_reason : "") << "\n";
      if (g_verbose) std::cout << "      " << error << "\n";
    } else {
      failed++;
      std::cout << kRed << "[  FAILED  ]" << kReset << " " << t.name << " "
                << timing << "\n      " << error << "\n";
      failures.push_back(std::string(t.name) + ": " + error);
    }
    std::cout.flush();
  }

  std::cout << "\n"
            << kGreen << passed << " passed" << kReset << ", " << kRed << failed
            << " failed" << kReset << ", " << kYellow << pending << " pending"
            << kReset;
  if (xpassed) std::cout << ", " << kYellow << xpassed << " unexpectedly passing" << kReset;
  if (skipped) std::cout << ", " << skipped << " skipped";
  std::cout << "\n";

  if (!failures.empty()) {
    std::cout << "\nfailures:\n";
    for (const auto& f : failures) std::cout << "  " << f << "\n";
  }

  return failed == 0 ? 0 : 1;
}
