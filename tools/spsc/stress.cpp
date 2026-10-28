#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

#include "lob/spsc_ring.hpp"

namespace {

/// Ring capacity for the stress stream. Large enough that both threads spend
/// most of their time moving elements rather than waiting on each other.
constexpr std::size_t kCapacity = 4096;

/// Payload of a message, a bijective mix of its sequence number, so a single
/// corrupted or duplicated word fails the content check.
std::uint64_t payload_for(std::uint64_t sequence) {
  std::uint64_t value = sequence + 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31U);
}

struct Message {
  std::uint64_t sequence;
  std::uint64_t payload;
};

/// One pause instruction, used while the ring is full/empty so the core does
/// not burn its whole pipeline polling. Unlike a yield it stays in user space.
inline void cpu_relax() {
#if defined(__aarch64__)
  __asm__ __volatile__("yield");
#else
  __builtin_ia32_pause();
#endif
}

void usage(const char* program) {
  std::fprintf(stderr,
               "usage: %s [--messages N] [--help]\n"
               "  --messages N  number of messages to push and validate (default 100000000)\n",
               program);
}

bool parse_messages(const char* text, std::uint64_t& out) {
  if (text == nullptr || *text == '\0') {
    return false;
  }
  std::uint64_t value = 0;
  for (const char* digit = text; *digit != '\0'; ++digit) {
    if (*digit < '0' || *digit > '9') {
      return false;
    }
    const std::uint64_t next = value * 10 + static_cast<std::uint64_t>(*digit - '0');
    if (next < value) {
      return false;  // overflow
    }
    value = next;
  }
  if (value == 0) {
    return false;
  }
  out = value;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::uint64_t messages = 100'000'000;
  for (int arg = 1; arg < argc; ++arg) {
    if (std::strcmp(argv[arg], "--messages") == 0) {
      if (arg + 1 >= argc || !parse_messages(argv[++arg], messages)) {
        std::fprintf(stderr, "spsc_stress: --messages expects a positive decimal count\n");
        return 2;
      }
    } else if (std::strcmp(argv[arg], "--help") == 0 || std::strcmp(argv[arg], "-h") == 0) {
      usage(argv[0]);
      return 0;
    } else {
      std::fprintf(stderr, "spsc_stress: unknown argument '%s'\n", argv[arg]);
      usage(argv[0]);
      return 2;
    }
  }

  lob::SpscRing<Message, kCapacity> ring;
  std::atomic<bool> violation{false};
  std::uint64_t first_bad_expected_sequence = 0;
  std::uint64_t first_bad_sequence = 0;
  std::uint64_t first_bad_expected_payload = 0;
  std::uint64_t first_bad_payload = 0;
  bool bad_content = false;

  const auto start = std::chrono::steady_clock::now();

  std::thread producer([&] {
    for (std::uint64_t sequence = 0; sequence < messages; ++sequence) {
      const Message message{sequence, payload_for(sequence)};
      while (!ring.try_push(message)) {
        cpu_relax();
      }
    }
  });

  std::thread consumer([&] {
    std::uint64_t expected_sequence = 0;
    Message message{};
    while (expected_sequence < messages) {
      if (!ring.try_pop(message)) {
        cpu_relax();
        continue;
      }
      if (violation.load(std::memory_order_relaxed)) {
        // A violation already ended validation; keep draining so the producer
        // can never block on a full ring.
        ++expected_sequence;
        continue;
      }
      if (message.sequence != expected_sequence) {
        first_bad_expected_sequence = expected_sequence;
        first_bad_sequence = message.sequence;
        violation.store(true, std::memory_order_relaxed);
        ++expected_sequence;
        continue;
      }
      if (message.payload != payload_for(expected_sequence)) {
        first_bad_expected_sequence = expected_sequence;
        first_bad_sequence = message.sequence;
        first_bad_expected_payload = payload_for(expected_sequence);
        first_bad_payload = message.payload;
        bad_content = true;
        violation.store(true, std::memory_order_relaxed);
        ++expected_sequence;
        continue;
      }
      ++expected_sequence;
    }
  });

  producer.join();
  consumer.join();
  const auto end = std::chrono::steady_clock::now();

  if (violation.load()) {
    if (bad_content) {
      std::fprintf(stderr,
                   "spsc_stress: content violation at sequence %llu: expected payload %llu, got "
                   "%llu\n",
                   static_cast<unsigned long long>(first_bad_expected_sequence),
                   static_cast<unsigned long long>(first_bad_expected_payload),
                   static_cast<unsigned long long>(first_bad_payload));
    } else {
      std::fprintf(stderr, "spsc_stress: order violation: expected sequence %llu, got %llu\n",
                   static_cast<unsigned long long>(first_bad_expected_sequence),
                   static_cast<unsigned long long>(first_bad_sequence));
    }
    return 1;
  }

  const double seconds = std::chrono::duration<double>(end - start).count();
  const double per_second = seconds > 0.0 ? static_cast<double>(messages) / seconds : 0.0;
  const double ns_per_message = per_second > 0.0 ? 1e9 / per_second : 0.0;

  std::printf("messages: %llu\n", static_cast<unsigned long long>(messages));
  std::printf("ring capacity: %zu\n", kCapacity);
  std::printf("elapsed: %.3f s\n", seconds);
  std::printf("order ok, content ok\n");
  std::printf("messages/s: %.0f (%.1f M/s, %.2f ns per message)\n", per_second, per_second / 1e6,
              ns_per_message);
  return 0;
}
