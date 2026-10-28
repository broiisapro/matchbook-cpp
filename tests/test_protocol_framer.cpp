#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <span>
#include <vector>

#include "net/protocol/codec.hpp"
#include "net/protocol/framer.hpp"
#include "net/protocol/messages.hpp"

namespace {

namespace proto = lob::net;

constexpr std::size_t kHeader = proto::kHeaderSize;

struct MessageStream {
  std::vector<std::byte> bytes;
  std::vector<std::vector<std::byte>> frames;
};

class CollectingSink : public proto::FrameSink {
 public:
  explicit CollectingSink(std::size_t stop_after = std::numeric_limits<std::size_t>::max())
      : stop_after_(stop_after) {}

  bool on_frame(std::span<const std::byte> frame) override {
    frames.emplace_back(frame.begin(), frame.end());
    return frames.size() < stop_after_;
  }

  std::vector<std::vector<std::byte>> frames;

 private:
  std::size_t stop_after_;
};

template <class Message>
std::vector<std::byte> frame_bytes(const Message& message, proto::Seq seq) {
  std::array<std::byte, Message::kWireSize> buffer{};
  const std::size_t written = proto::encode(message, seq, buffer);
  EXPECT_EQ(written, buffer.size());
  return std::vector<std::byte>(buffer.begin(), buffer.end());
}

template <class Message>
void append_message(MessageStream& stream, const Message& message, proto::Seq seq) {
  std::vector<std::byte> frame = frame_bytes(message, seq);
  stream.frames.push_back(frame);
  stream.bytes.insert(stream.bytes.end(), frame.begin(), frame.end());
}

/// sixty mixed messages, five of each type, in a fixed order. Every field is
/// varied so a codec or framing bug that only touches one layout shows up.
MessageStream mixed_stream() {
  MessageStream stream;
  proto::Seq seq = 1;
  for (std::uint64_t i = 0; i < 5; ++i) {
    const auto price = static_cast<lob::Price>(100 + 10 * i);
    const auto qty = static_cast<lob::Qty>(1 + i);
    append_message(
        stream, proto::NewOrder{100 + i, price, qty, lob::Side::Buy, lob::OrderType::Limit}, seq++);
    append_message(stream, proto::Cancel{200 + i}, seq++);
    append_message(stream, proto::Modify{300 + i, price + 1, qty + 1}, seq++);
    append_message(stream, proto::Heartbeat{0x1000 + i}, seq++);
    append_message(stream, proto::Ack{400 + i}, seq++);
    append_message(stream, proto::Reject{500 + i, lob::RejectReason::InvalidPrice}, seq++);
    append_message(stream, proto::Fill{600 + i, 700 + i, price, qty}, seq++);
    append_message(stream, proto::Cancelled{800 + i, qty}, seq++);
    append_message(stream, proto::BookUpdate{price, qty, lob::Side::Sell}, seq++);
    append_message(stream, proto::Trade{900 + i, 1000 + i, price, qty}, seq++);
    append_message(stream, proto::SequenceReset{seq + 1000}, seq);
    ++seq;
    append_message(stream, proto::RecoveryRequest{seq, seq + 50}, seq);
    ++seq;
  }
  return stream;
}

void set_u16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value) {
  bytes[offset] = static_cast<std::byte>(value & 0xffU);
  bytes[offset + 1] = static_cast<std::byte>((value >> 8) & 0xffU);
}

void expect_frames_equal(const std::vector<std::vector<std::byte>>& actual,
                         const std::vector<std::vector<std::byte>>& expected) {
  ASSERT_EQ(actual.size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    ASSERT_EQ(actual[i].size(), expected[i].size()) << "frame " << i;
    EXPECT_TRUE(std::equal(actual[i].begin(), actual[i].end(), expected[i].begin()))
        << "frame " << i << " differs";
  }
}

TEST(ProtocolFramer, SplitsAtEveryByteBoundary) {
  const MessageStream stream = mixed_stream();
  ASSERT_GT(stream.frames.size(), 50U);

  for (std::size_t split = 0; split <= stream.bytes.size(); ++split) {
    SCOPED_TRACE(testing::Message() << "split " << split);
    proto::StreamFramer framer;
    CollectingSink sink;
    const auto first = framer.push(std::span<const std::byte>(stream.bytes).first(split), sink);
    EXPECT_EQ(first.result, proto::FramerResult::Ok);
    EXPECT_EQ(first.consumed, split);
    const auto second = framer.push(std::span<const std::byte>(stream.bytes).subspan(split), sink);
    EXPECT_EQ(second.result, proto::FramerResult::Ok);
    EXPECT_EQ(second.consumed, stream.bytes.size() - split);

    expect_frames_equal(sink.frames, stream.frames);
    EXPECT_EQ(framer.error(), proto::FramerError::None);
    EXPECT_EQ(framer.buffered(), 0U);
    EXPECT_EQ(framer.frames_delivered(), stream.frames.size());
  }
}

TEST(ProtocolFramer, HandlesRandomChunkSizes) {
  const MessageStream stream = mixed_stream();
  for (const std::uint32_t seed : {1U, 2U, 7U, 12345U, 0xFFFFFFFFU}) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<std::size_t> chunk_size(1, 23);

    proto::StreamFramer framer;
    CollectingSink sink;
    std::size_t offset = 0;
    while (offset < stream.bytes.size()) {
      const std::size_t take = std::min(chunk_size(rng), stream.bytes.size() - offset);
      const auto result =
          framer.push(std::span<const std::byte>(stream.bytes).subspan(offset, take), sink);
      EXPECT_EQ(result.result, proto::FramerResult::Ok) << "seed " << seed;
      EXPECT_EQ(result.consumed, take) << "seed " << seed;
      offset += take;
    }
    ASSERT_EQ(sink.frames.size(), stream.frames.size()) << "seed " << seed;
    EXPECT_TRUE(sink.frames == stream.frames) << "seed " << seed;
    EXPECT_EQ(framer.buffered(), 0U);
  }
}

TEST(ProtocolFramer, HandlesOneByteAtATime) {
  const MessageStream stream = mixed_stream();
  proto::StreamFramer framer;
  CollectingSink sink;
  for (std::size_t i = 0; i < stream.bytes.size(); ++i) {
    const auto result = framer.push(std::span<const std::byte>(stream.bytes).subspan(i, 1), sink);
    ASSERT_EQ(result.result, proto::FramerResult::Ok) << "byte " << i;
    EXPECT_EQ(result.consumed, 1U);
  }
  expect_frames_equal(sink.frames, stream.frames);
  EXPECT_EQ(framer.buffered(), 0U);
}

TEST(ProtocolFramer, DeliversOneFramePerWholeFrameChunk) {
  const MessageStream stream = mixed_stream();
  proto::StreamFramer framer;
  CollectingSink sink;
  for (const auto& frame : stream.frames) {
    const auto result = framer.push(frame, sink);
    EXPECT_EQ(result.result, proto::FramerResult::Ok);
    EXPECT_EQ(result.consumed, frame.size());
  }
  expect_frames_equal(sink.frames, stream.frames);
}

TEST(ProtocolFramer, WaitsForTheRestOfATruncatedStream) {
  const MessageStream stream = mixed_stream();
  const std::size_t last_size = stream.frames.back().size();
  const std::size_t cut = stream.bytes.size() - (last_size / 2);

  proto::StreamFramer framer;
  CollectingSink sink;
  const auto partial = framer.push(std::span<const std::byte>(stream.bytes).first(cut), sink);
  EXPECT_EQ(partial.result, proto::FramerResult::Ok);
  EXPECT_EQ(partial.consumed, cut);
  EXPECT_EQ(sink.frames.size(), stream.frames.size() - 1);
  EXPECT_EQ(framer.error(), proto::FramerError::None);
  EXPECT_EQ(framer.buffered(), last_size / 2);

  const auto rest = framer.push(std::span<const std::byte>(stream.bytes).subspan(cut), sink);
  EXPECT_EQ(rest.result, proto::FramerResult::Ok);
  EXPECT_EQ(rest.consumed, stream.bytes.size() - cut);
  expect_frames_equal(sink.frames, stream.frames);
  EXPECT_EQ(framer.buffered(), 0U);
}

TEST(ProtocolFramer, BuffersAPartialHeader) {
  const MessageStream stream = mixed_stream();

  proto::StreamFramer framer;
  CollectingSink sink;
  const auto first = framer.push(std::span<const std::byte>(stream.bytes).first(8), sink);
  EXPECT_EQ(first.result, proto::FramerResult::Ok);
  EXPECT_EQ(sink.frames.size(), 0U);
  EXPECT_EQ(framer.buffered(), 8U);
  EXPECT_EQ(framer.frames_delivered(), 0U);

  const auto second = framer.push(std::span<const std::byte>(stream.bytes).subspan(8, 8), sink);
  EXPECT_EQ(second.result, proto::FramerResult::Ok);
  EXPECT_EQ(sink.frames.size(), 0U);
  EXPECT_EQ(framer.buffered(), kHeader);

  // The rest of the first frame completes it.
  const std::size_t first_frame_size = stream.frames[0].size();
  const auto third = framer.push(
      std::span<const std::byte>(stream.bytes).subspan(kHeader, first_frame_size - kHeader), sink);
  EXPECT_EQ(third.result, proto::FramerResult::Ok);
  ASSERT_EQ(sink.frames.size(), 1U);
  EXPECT_TRUE(sink.frames[0] == stream.frames[0]);
  EXPECT_EQ(framer.buffered(), 0U);
}

TEST(ProtocolFramer, RejectsBadHeadersAsSoonAsTheyArrive) {
  const auto good = frame_bytes(proto::Ack{1}, 1);
  const auto good_result = [&good](proto::StreamFramer& framer, CollectingSink& sink) {
    return framer.push(good, sink);
  };

  struct Case {
    const char* name;
    proto::FramerError expected;
    std::size_t offset;
    std::uint16_t value;
  };
  // Each case mutates a valid Ack header in exactly one way. Length 15 differs
  // from the 16-byte body; 41 is larger than any body in the table.
  const std::array<Case, 3> cases{{
      {"unknown_type", proto::FramerError::UnknownType, proto::kHeaderTypeOffset, 99},
      {"bad_length", proto::FramerError::BadLength, proto::kHeaderLengthOffset, 15},
      {"oversize", proto::FramerError::Oversize, proto::kHeaderLengthOffset, 41},
  }};

  for (const Case& test : cases) {
    proto::StreamFramer framer;
    CollectingSink sink;
    EXPECT_EQ(good_result(framer, sink).result, proto::FramerResult::Ok) << test.name;

    std::vector<std::byte> bad = frame_bytes(proto::Ack{2}, 2);
    set_u16(bad, test.offset, test.value);
    // Only the header is offered: a bad header is reported without waiting
    // for a body that may never arrive.
    const auto result = framer.push(std::span<const std::byte>(bad).first(kHeader), sink);
    EXPECT_EQ(result.result, proto::FramerResult::Error) << test.name;
    EXPECT_EQ(framer.error(), test.expected) << test.name;
    EXPECT_EQ(framer.buffered(), 0U) << test.name;
    EXPECT_EQ(sink.frames.size(), 1U) << test.name;
  }

  {
    proto::StreamFramer framer;
    CollectingSink sink;
    EXPECT_EQ(good_result(framer, sink).result, proto::FramerResult::Ok);
    std::vector<std::byte> bad = frame_bytes(proto::Ack{2}, 2);
    bad[proto::kHeaderReservedOffset] = std::byte{1};
    const auto result = framer.push(std::span<const std::byte>(bad).first(kHeader), sink);
    EXPECT_EQ(result.result, proto::FramerResult::Error);
    EXPECT_EQ(framer.error(), proto::FramerError::BadReserved);
    EXPECT_EQ(sink.frames.size(), 1U);
  }
}

TEST(ProtocolFramer, ErrorIsLatchedUntilReset) {
  proto::StreamFramer framer;
  CollectingSink sink;
  const auto good = frame_bytes(proto::Cancel{1}, 1);
  ASSERT_EQ(framer.push(good, sink).result, proto::FramerResult::Ok);

  std::vector<std::byte> bad = frame_bytes(proto::Cancel{2}, 2);
  set_u16(bad, proto::kHeaderTypeOffset, 0xFFFF);
  ASSERT_EQ(framer.push(bad, sink).result, proto::FramerResult::Error);
  ASSERT_EQ(framer.error(), proto::FramerError::UnknownType);

  const auto latched = framer.push(good, sink);
  EXPECT_EQ(latched.result, proto::FramerResult::Error);
  EXPECT_EQ(latched.consumed, 0U);
  EXPECT_EQ(sink.frames.size(), 1U);

  framer.reset();
  EXPECT_EQ(framer.error(), proto::FramerError::None);
  EXPECT_EQ(framer.buffered(), 0U);
  EXPECT_EQ(framer.frames_delivered(), 0U);

  const auto after = framer.push(good, sink);
  EXPECT_EQ(after.result, proto::FramerResult::Ok);
  EXPECT_EQ(sink.frames.size(), 2U);
  EXPECT_TRUE(sink.frames[1] == good);
}

TEST(ProtocolFramer, StopsOnSinkRequestAndResumesWithoutLoss) {
  const MessageStream stream = mixed_stream();
  constexpr std::size_t kStopAfter = 5;

  proto::StreamFramer framer;
  CollectingSink stopping_sink(kStopAfter);
  const auto stopped = framer.push(stream.bytes, stopping_sink);
  EXPECT_EQ(stopped.result, proto::FramerResult::Stopped);
  ASSERT_EQ(stopping_sink.frames.size(), kStopAfter);

  std::size_t delivered_bytes = 0;
  for (std::size_t i = 0; i < kStopAfter; ++i) {
    delivered_bytes += stream.frames[i].size();
  }
  EXPECT_EQ(stopped.consumed, delivered_bytes);
  EXPECT_EQ(framer.buffered(), 0U);
  EXPECT_EQ(framer.error(), proto::FramerError::None);

  CollectingSink rest;
  const auto resumed =
      framer.push(std::span<const std::byte>(stream.bytes).subspan(stopped.consumed), rest);
  EXPECT_EQ(resumed.result, proto::FramerResult::Ok);
  ASSERT_EQ(rest.frames.size(), stream.frames.size() - kStopAfter);
  for (std::size_t i = 0; i < rest.frames.size(); ++i) {
    EXPECT_TRUE(rest.frames[i] == stream.frames[kStopAfter + i]) << "frame " << i;
  }
}

TEST(ProtocolFramer, EmptyChunkIsAccepted) {
  proto::StreamFramer framer;
  CollectingSink sink;
  const auto result = framer.push(std::span<const std::byte>{}, sink);
  EXPECT_EQ(result.result, proto::FramerResult::Ok);
  EXPECT_EQ(result.consumed, 0U);
  EXPECT_EQ(sink.frames.size(), 0U);
  EXPECT_EQ(framer.buffered(), 0U);
}

TEST(ProtocolFramer, HandsTheSinkWholeDecodableFrames) {
  class DecodingSink : public proto::FrameSink {
   public:
    bool on_frame(std::span<const std::byte> frame) override {
      proto::Header header{};
      EXPECT_EQ(proto::decode_header(frame, header), proto::DecodeStatus::Ok);
      EXPECT_EQ(frame.size(), kHeader + header.length);
      ++count;
      return true;
    }

    std::size_t count = 0;
  };

  const MessageStream stream = mixed_stream();
  proto::StreamFramer framer;
  DecodingSink sink;
  std::size_t offset = 0;
  while (offset < stream.bytes.size()) {
    const std::size_t take = std::min<std::size_t>(37, stream.bytes.size() - offset);
    ASSERT_EQ(
        framer.push(std::span<const std::byte>(stream.bytes).subspan(offset, take), sink).result,
        proto::FramerResult::Ok);
    offset += take;
  }
  EXPECT_EQ(sink.count, stream.frames.size());
  EXPECT_EQ(framer.frames_delivered(), stream.frames.size());
}

}  // namespace
