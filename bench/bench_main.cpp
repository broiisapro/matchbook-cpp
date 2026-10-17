#include <benchmark/benchmark.h>

namespace {

void BM_placeholder(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(state.iterations());
  }
}
BENCHMARK(BM_placeholder);

}  // namespace

BENCHMARK_MAIN();
