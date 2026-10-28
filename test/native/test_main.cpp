// Native test runner: the on-device unit suite (runTests) plus the scenario
// tests, on the host. Exit status is non-zero if anything failed.
#include <cstdio>
#include "hvac_tests.h"

int runScenarios();

int main() {
  int unit = runTests();
  int scen = runScenarios();
  printf("\nnative tests: %d unit failure(s), %d scenario failure(s)\n", unit, scen);
  return (unit || scen) ? 1 : 0;
}
