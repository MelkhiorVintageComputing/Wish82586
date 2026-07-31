// SPDX-License-Identifier: MIT
//
// Minimal test framework.
//
//   TEST(name)                     - must pass
//   TEST_PENDING(name, "reason")   - expected to fail until the RTL catches up.
//                                    A pending test that passes is reported as
//                                    XPASS, which is the signal to promote it.
//
// Tests are self registering, so adding one means adding a file in
// tb/cpp/tests/ - nothing else to edit.

#pragma once

#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "env.h"

namespace wtb {

struct TestFailure : public std::runtime_error {
  explicit TestFailure(const std::string& m) : std::runtime_error(m) {}
};

struct TestCase {
  const char* name;
  const char* file;
  void (*fn)(Env&);
  bool pending;
  const char* pending_reason;
};

void register_test(const TestCase& tc);

struct TestRegistrar {
  explicit TestRegistrar(const TestCase& tc) { register_test(tc); }
};

#define TEST(NAME)                                                        \
  static void NAME(::wtb::Env&);                                          \
  static ::wtb::TestRegistrar NAME##_registrar(                           \
      ::wtb::TestCase{#NAME, __FILE__, &NAME, false, nullptr});           \
  static void NAME(::wtb::Env& env)

// A test that passes on MII but not yet on GMII.  The receive unit writes one
// byte per bus transaction, which is about eighty nanoseconds; at gigabit a
// byte arrives every eight.  No FIFO depth fixes a sustained rate deficit -
// the memory side has to move whole words - so on a GMII build these run and
// are expected to fail, exactly like any other pending test.
#if PHY_DATA_W == 8
#define TEST_UNLESS_GMII(NAME, REASON) TEST_PENDING(NAME, REASON)
#else
#define TEST_UNLESS_GMII(NAME, REASON) TEST(NAME)
#endif

#define TEST_PENDING(NAME, REASON)                                        \
  static void NAME(::wtb::Env&);                                          \
  static ::wtb::TestRegistrar NAME##_registrar(                           \
      ::wtb::TestCase{#NAME, __FILE__, &NAME, true, REASON});             \
  static void NAME(::wtb::Env& env)

// ---------------------------------------------------------------------------
// checks
// ---------------------------------------------------------------------------

void fail_at(const char* file, int line, const std::string& msg);
void note(const std::string& msg);   // printed only in verbose mode
void logf(const char* fmt, ...);     // always printed, prefixed with the test name

std::ostream& operator<<(std::ostream& os, const MacAddr& m);

template <typename T>
typename std::enable_if<std::is_integral<T>::value, std::string>::type val_str(T v) {
  std::ostringstream os;
  os << int64_t(v) << " (0x" << std::hex << uint64_t(v) << std::dec << ")";
  return os.str();
}

template <typename T>
typename std::enable_if<!std::is_integral<T>::value, std::string>::type val_str(
    const T& v) {
  std::ostringstream os;
  os << v;
  return os.str();
}

inline std::string val_str(const Bytes& b) { return hex_dump(b); }

template <typename A, typename B>
void check_eq(const char* file, int line, const char* ea, const char* eb,
              const A& a, const B& b) {
  if (!(a == b)) {
    std::ostringstream os;
    os << ea << " == " << eb << "\n    left : " << val_str(a)
       << "\n    right: " << val_str(b);
    fail_at(file, line, os.str());
  }
}

template <typename A, typename B>
void check_ne(const char* file, int line, const char* ea, const char* eb,
              const A& a, const B& b) {
  if (a == b) {
    std::ostringstream os;
    os << ea << " != " << eb << "\n    both : " << val_str(a);
    fail_at(file, line, os.str());
  }
}

#define CHECK(COND)                                                       \
  do {                                                                    \
    if (!(COND)) ::wtb::fail_at(__FILE__, __LINE__, "CHECK(" #COND ")");  \
  } while (0)

#define CHECK_MSG(COND, MSG)                                              \
  do {                                                                    \
    if (!(COND))                                                          \
      ::wtb::fail_at(__FILE__, __LINE__,                                  \
                     std::string("CHECK(" #COND "): ") + (MSG));          \
  } while (0)

#define CHECK_EQ(A, B) ::wtb::check_eq(__FILE__, __LINE__, #A, #B, (A), (B))
#define CHECK_NE(A, B) ::wtb::check_ne(__FILE__, __LINE__, #A, #B, (A), (B))

// Runs a driver call and fails with the driver's own error message.
#define CHECK_DRV(EXPR)                                                   \
  do {                                                                    \
    if (!(EXPR))                                                          \
      ::wtb::fail_at(__FILE__, __LINE__,                                  \
                     std::string(#EXPR " failed: ") + env.drv().last_error()); \
  } while (0)

}  // namespace wtb
