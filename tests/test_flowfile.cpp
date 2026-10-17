#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "lob/config.hpp"
#include "lob/types.hpp"
#include "tools/flowfile/flow_record.hpp"
#include "tools/flowfile/flowfile.hpp"
#include "tools/flowfile/replay_config.hpp"

namespace {

using lob::flowfile::FlowError;
using lob::flowfile::FlowErrorInfo;
using lob::flowfile::FlowOp;
using lob::flowfile::FlowReader;
using lob::flowfile::FlowRecord;
using lob::flowfile::FlowWriter;

constexpr std::size_t kHeaderSize = lob::flowfile::kFlowHeaderSize;
constexpr std::size_t kRecordSize = lob::flowfile::kFlowRecordSize;

/// Path under the gtest temp directory, removed when the test ends.
class TempFile {
 public:
  explicit TempFile(std::string_view name)
      : path_(std::filesystem::path(testing::TempDir()) / ("lob_flowfile_" + std::string(name))) {}

  ~TempFile() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  const std::string str() const { return path_.string(); }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

/// Hand-rolled splitmix64, so the "random" records are identical on every
/// library and platform (tests must be reproducible; the tests do not need
/// cryptographic quality).
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed) {}

  std::uint64_t next() {
    state_ += 0x9e3779b97f4a7c15ULL;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  }

  std::uint64_t below(std::uint64_t bound) { return next() % bound; }

 private:
  std::uint64_t state_;
};

std::vector<FlowRecord> random_records(std::size_t count, std::uint64_t seed) {
  Rng rng(seed);
  std::vector<FlowRecord> records;
  records.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const auto op = static_cast<FlowOp>(1 + rng.below(3));
    const auto side = static_cast<lob::Side>(rng.below(2));
    const auto type = static_cast<lob::OrderType>(rng.below(3));
    const auto id = rng.next();
    const auto price = static_cast<lob::Price>(rng.next() % 2001) - 1000;
    const auto qty = static_cast<lob::Qty>(rng.next() % 1000) + 1;
    const auto ts_ns = rng.next();
    records.push_back(FlowRecord{op, side, type, id, price, qty, ts_ns});
  }
  return records;
}

void write_binary(const std::string& path, const std::vector<FlowRecord>& records,
                  FlowErrorInfo& error) {
  FlowWriter writer;
  writer.open(path, error);
  ASSERT_FALSE(error) << error.message;
  for (const FlowRecord& record : records) {
    writer.write(record, error);
    ASSERT_FALSE(error) << error.message;
  }
  writer.close(error);
  ASSERT_FALSE(error) << error.message;
}

std::vector<FlowRecord> read_binary(const std::string& path, FlowErrorInfo& error,
                                    std::uint64_t* header_count = nullptr) {
  std::vector<FlowRecord> records;
  FlowReader reader;
  reader.open(path, error);
  if (error) {
    return records;
  }
  if (header_count != nullptr) {
    *header_count = reader.record_count();
  }
  FlowRecord record{};
  while (reader.read(record, error)) {
    records.push_back(record);
  }
  return records;
}

std::vector<FlowRecord> read_csv_all(const std::string& path, FlowErrorInfo& error) {
  std::vector<FlowRecord> records;
  lob::flowfile::read_csv(
      path,
      [&records](const FlowRecord& record) {
        records.push_back(record);
        return true;
      },
      error);
  return records;
}

void patch_byte(const std::filesystem::path& path, std::size_t offset, std::byte value) {
  std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
  ASSERT_TRUE(file.is_open());
  file.seekp(static_cast<std::streamoff>(offset));
  file.put(static_cast<char>(std::to_integer<unsigned char>(value)));
  file.close();
}

void patch_u64(const std::filesystem::path& path, std::size_t offset, std::uint64_t value) {
  for (std::size_t i = 0; i < 8; ++i) {
    patch_byte(path, offset + i, static_cast<std::byte>((value >> (8 * i)) & 0xffU));
  }
}

// ---------------------------------------------------------------------------
// Binary form
// ---------------------------------------------------------------------------

TEST(Flowfile, BinaryRoundTripTenThousandRecords) {
  const std::vector<FlowRecord> records = random_records(10000, 42);
  const TempFile file("roundtrip.bin");

  FlowErrorInfo error;
  write_binary(file.str(), records, error);
  ASSERT_FALSE(error) << error.message;

  std::uint64_t header_count = 0;
  const std::vector<FlowRecord> read = read_binary(file.str(), error, &header_count);
  ASSERT_FALSE(error) << error.message;
  EXPECT_EQ(header_count, 10000U);
  EXPECT_EQ(read, records);
}

TEST(Flowfile, EmptyFileHasZeroRecords) {
  const TempFile file("empty.bin");

  FlowErrorInfo error;
  write_binary(file.str(), {}, error);
  ASSERT_FALSE(error) << error.message;

  std::uint64_t header_count = 0;
  const std::vector<FlowRecord> read = read_binary(file.str(), error, &header_count);
  EXPECT_FALSE(error) << error.message;
  EXPECT_TRUE(read.empty());
  EXPECT_EQ(header_count, 0U);

  EXPECT_EQ(std::filesystem::file_size(file.path()), kHeaderSize);
}

TEST(Flowfile, ZeroByteFileIsBadHeader) {
  const TempFile file("zerobyte.bin");
  {
    std::ofstream out(file.path(), std::ios::binary);
    ASSERT_TRUE(out.is_open());
  }

  FlowReader reader;
  FlowErrorInfo error;
  reader.open(file.str(), error);
  EXPECT_EQ(error.code, FlowError::BadHeader);
  EXPECT_FALSE(error.message.empty());
}

TEST(Flowfile, BadMagicIsBadHeader) {
  const TempFile file("badmagic.bin");
  FlowErrorInfo error;
  write_binary(file.str(), random_records(3, 7), error);
  ASSERT_FALSE(error) << error.message;

  patch_byte(file.path(), 0, std::byte{'X'});

  FlowReader reader;
  reader.open(file.str(), error);
  EXPECT_EQ(error.code, FlowError::BadHeader);
}

TEST(Flowfile, BadVersionAndRecordSizeAreBadHeader) {
  const TempFile file("badversion.bin");
  FlowErrorInfo error;
  write_binary(file.str(), random_records(3, 7), error);
  ASSERT_FALSE(error) << error.message;

  patch_byte(file.path(), 8, std::byte{2});
  {
    FlowReader reader;
    reader.open(file.str(), error);
    EXPECT_EQ(error.code, FlowError::BadHeader);
  }
  patch_byte(file.path(), 8, std::byte{1});

  patch_byte(file.path(), 12, std::byte{41});
  {
    FlowReader reader;
    reader.open(file.str(), error);
    EXPECT_EQ(error.code, FlowError::BadHeader);
  }
}

TEST(Flowfile, TruncatedFileIsAnError) {
  const std::vector<FlowRecord> records = random_records(10, 11);
  const TempFile file("truncated.bin");
  FlowErrorInfo error;
  write_binary(file.str(), records, error);
  ASSERT_FALSE(error) << error.message;

  // Cut into the last record; the header still claims ten records.
  std::filesystem::resize_file(file.path(), kHeaderSize + 9 * kRecordSize + 17);

  FlowReader reader;
  reader.open(file.str(), error);
  ASSERT_FALSE(error) << error.message;
  EXPECT_EQ(reader.record_count(), 10U);

  FlowRecord record{};
  for (int i = 0; i < 9; ++i) {
    ASSERT_TRUE(reader.read(record, error)) << i;
    EXPECT_EQ(record, records[static_cast<std::size_t>(i)]);
  }
  EXPECT_FALSE(reader.read(record, error));
  EXPECT_EQ(error.code, FlowError::Truncated);
  EXPECT_EQ(reader.records_read(), 9U);
}

TEST(Flowfile, HeaderCountLargerThanFileIsTruncated) {
  const TempFile file("count_high.bin");
  FlowErrorInfo error;
  write_binary(file.str(), random_records(5, 12), error);
  ASSERT_FALSE(error) << error.message;

  patch_u64(file.path(), 16, 6);

  FlowReader reader;
  reader.open(file.str(), error);
  ASSERT_FALSE(error) << error.message;
  FlowRecord record{};
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(reader.read(record, error));
  }
  EXPECT_FALSE(reader.read(record, error));
  EXPECT_EQ(error.code, FlowError::Truncated);
}

TEST(Flowfile, HeaderCountSmallerThanFileIsCountMismatch) {
  const TempFile file("count_low.bin");
  FlowErrorInfo error;
  write_binary(file.str(), random_records(5, 13), error);
  ASSERT_FALSE(error) << error.message;

  patch_u64(file.path(), 16, 4);

  FlowReader reader;
  reader.open(file.str(), error);
  ASSERT_FALSE(error) << error.message;
  FlowRecord record{};
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(reader.read(record, error));
  }
  EXPECT_FALSE(reader.read(record, error));
  EXPECT_EQ(error.code, FlowError::CountMismatch);
}

TEST(Flowfile, PartialTrailingRecordAfterLastCountedRecordIsCountMismatch) {
  const TempFile file("trailing.bin");
  FlowErrorInfo error;
  write_binary(file.str(), random_records(5, 14), error);
  ASSERT_FALSE(error) << error.message;

  // Append 20 stray bytes after the last record; the header count is correct.
  {
    std::ofstream out(file.path(), std::ios::binary | std::ios::app);
    ASSERT_TRUE(out.is_open());
    out << std::string(20, 'x');
  }

  FlowReader reader;
  reader.open(file.str(), error);
  ASSERT_FALSE(error) << error.message;
  FlowRecord record{};
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(reader.read(record, error));
  }
  EXPECT_FALSE(reader.read(record, error));
  EXPECT_EQ(error.code, FlowError::CountMismatch);
  EXPECT_NE(error.message.find("partial trailing record"), std::string::npos) << error.message;
}

TEST(Flowfile, InvalidEnumByteAtRecordK) {
  const std::vector<FlowRecord> records = random_records(10, 15);
  const TempFile file("badenum.bin");
  FlowErrorInfo error;
  write_binary(file.str(), records, error);
  ASSERT_FALSE(error) << error.message;

  patch_byte(file.path(), kHeaderSize + 7 * kRecordSize + 0, std::byte{9});

  FlowReader reader;
  reader.open(file.str(), error);
  ASSERT_FALSE(error) << error.message;
  FlowRecord record{};
  for (int i = 0; i < 7; ++i) {
    ASSERT_TRUE(reader.read(record, error));
  }
  EXPECT_FALSE(reader.read(record, error));
  EXPECT_EQ(error.code, FlowError::InvalidRecord);
  EXPECT_EQ(reader.records_read(), 7U);
  EXPECT_NE(error.message.find("record 7"), std::string::npos) << error.message;
  EXPECT_NE(error.message.find("op"), std::string::npos) << error.message;
}

TEST(Flowfile, InvalidSideAndTypeBytesAreRejected) {
  const std::vector<FlowRecord> records = random_records(3, 16);
  const TempFile file("badenum2.bin");
  FlowErrorInfo error;
  write_binary(file.str(), records, error);
  ASSERT_FALSE(error) << error.message;

  patch_byte(file.path(), kHeaderSize + 1 * kRecordSize + 1, std::byte{7});
  {
    FlowReader reader;
    reader.open(file.str(), error);
    ASSERT_FALSE(error) << error.message;
    FlowRecord record{};
    ASSERT_TRUE(reader.read(record, error));
    EXPECT_FALSE(reader.read(record, error));
    EXPECT_EQ(error.code, FlowError::InvalidRecord);
    EXPECT_NE(error.message.find("side"), std::string::npos) << error.message;
  }

  patch_byte(file.path(), kHeaderSize + 1 * kRecordSize + 1, std::byte{0});
  patch_byte(file.path(), kHeaderSize + 2 * kRecordSize + 2, std::byte{5});
  {
    FlowReader reader;
    reader.open(file.str(), error);
    ASSERT_FALSE(error) << error.message;
    FlowRecord record{};
    ASSERT_TRUE(reader.read(record, error));
    ASSERT_TRUE(reader.read(record, error));
    EXPECT_FALSE(reader.read(record, error));
    EXPECT_EQ(error.code, FlowError::InvalidRecord);
    EXPECT_NE(error.message.find("type"), std::string::npos) << error.message;
  }
}

TEST(Flowfile, WriterDestructorClosesAndWritesCount) {
  const TempFile file("raii.bin");
  const std::vector<FlowRecord> records = random_records(4, 17);
  {
    FlowErrorInfo error;
    FlowWriter writer;
    writer.open(file.str(), error);
    ASSERT_FALSE(error) << error.message;
    for (const FlowRecord& record : records) {
      writer.write(record, error);
      ASSERT_FALSE(error) << error.message;
    }
    // No close(); the destructor must flush and write the final count.
  }

  FlowErrorInfo error;
  std::uint64_t header_count = 0;
  const std::vector<FlowRecord> read = read_binary(file.str(), error, &header_count);
  ASSERT_FALSE(error) << error.message;
  EXPECT_EQ(header_count, 4U);
  EXPECT_EQ(read, records);
}

TEST(Flowfile, WritingMoreRecordsThanOneBufferRoundTrips) {
  // More than kFlowIoBufferSize / kFlowRecordSize records exercises the
  // flush-and-refill path on both sides.
  const std::vector<FlowRecord> records = random_records(4000, 18);
  const TempFile file("bigbuffer.bin");
  FlowErrorInfo error;
  write_binary(file.str(), records, error);
  ASSERT_FALSE(error) << error.message;
  ASSERT_GT(records.size() * kRecordSize, lob::flowfile::kFlowIoBufferSize);

  const std::vector<FlowRecord> read = read_binary(file.str(), error);
  ASSERT_FALSE(error) << error.message;
  EXPECT_EQ(read, records);
}

// ---------------------------------------------------------------------------
// CSV form
// ---------------------------------------------------------------------------

TEST(Flowfile, CsvRoundTrip) {
  const std::vector<FlowRecord> records = random_records(1000, 21);
  const TempFile file("roundtrip.csv");
  FlowErrorInfo error;

  lob::flowfile::write_csv(file.str(), records, error);
  ASSERT_FALSE(error) << error.message;

  const std::vector<FlowRecord> read = read_csv_all(file.str(), error);
  ASSERT_FALSE(error) << error.message;
  EXPECT_EQ(read, records);
}

TEST(Flowfile, CsvWritesWordsAndHeader) {
  const TempFile file("words.csv");
  const std::vector<FlowRecord> records{
      FlowRecord{FlowOp::Submit, lob::Side::Buy, lob::OrderType::Limit, 1, 100, 5, 999},
      FlowRecord{FlowOp::Cancel, lob::Side::Sell, lob::OrderType::Market, 2, 0, 0, 1000},
      FlowRecord{FlowOp::Modify, lob::Side::Buy, lob::OrderType::IOC, 3, -7, 12, 1001},
  };
  FlowErrorInfo error;
  lob::flowfile::write_csv(file.str(), records, error);
  ASSERT_FALSE(error) << error.message;

  std::ifstream input(file.path());
  ASSERT_TRUE(input.is_open());
  std::string line;
  ASSERT_TRUE(std::getline(input, line));
  EXPECT_EQ(line, "op,side,type,id,price,qty,ts_ns");
  ASSERT_TRUE(std::getline(input, line));
  EXPECT_EQ(line, "submit,buy,limit,1,100,5,999");
  ASSERT_TRUE(std::getline(input, line));
  EXPECT_EQ(line, "cancel,sell,market,2,0,0,1000");
  ASSERT_TRUE(std::getline(input, line));
  EXPECT_EQ(line, "modify,buy,ioc,3,-7,12,1001");
}

TEST(Flowfile, BinaryAndCsvAreEquivalent) {
  const std::vector<FlowRecord> records = random_records(500, 22);
  const TempFile binary("equivalent.bin");
  const TempFile csv("equivalent.csv");
  FlowErrorInfo error;

  write_binary(binary.str(), records, error);
  ASSERT_FALSE(error) << error.message;
  lob::flowfile::write_csv(csv.str(), records, error);
  ASSERT_FALSE(error) << error.message;

  const std::vector<FlowRecord> from_binary = read_binary(binary.str(), error);
  ASSERT_FALSE(error) << error.message;
  const std::vector<FlowRecord> from_csv = read_csv_all(csv.str(), error);
  ASSERT_FALSE(error) << error.message;
  EXPECT_EQ(from_binary, from_csv);
}

TEST(Flowfile, CsvBadFieldReportsLineNumber) {
  const TempFile file("badfield.csv");
  {
    std::ofstream out(file.path(), std::ios::binary);
    ASSERT_TRUE(out.is_open());
    out << "op,side,type,id,price,qty,ts_ns\n"
        << "submit,buy,limit,1,100,5,1\n"
        << "submt,buy,limit,2,100,5,2\n";
  }

  FlowErrorInfo error;
  const std::vector<FlowRecord> records = read_csv_all(file.str(), error);
  EXPECT_EQ(error.code, FlowError::InvalidCsv);
  EXPECT_EQ(records.size(), 1U);
  EXPECT_NE(error.message.find("line 3"), std::string::npos) << error.message;
}

TEST(Flowfile, CsvBadNumericFieldReportsLineNumber) {
  const TempFile file("badnumber.csv");
  {
    std::ofstream out(file.path(), std::ios::binary);
    ASSERT_TRUE(out.is_open());
    out << "op,side,type,id,price,qty,ts_ns\n"
        << "submit,buy,limit,12x,100,5,1\n";
  }

  FlowErrorInfo error;
  read_csv_all(file.str(), error);
  EXPECT_EQ(error.code, FlowError::InvalidCsv);
  EXPECT_NE(error.message.find("line 2"), std::string::npos) << error.message;
  EXPECT_NE(error.message.find("id"), std::string::npos) << error.message;
}

TEST(Flowfile, CsvWrongFieldCountIsAnError) {
  const TempFile file("fields.csv");
  {
    std::ofstream out(file.path(), std::ios::binary);
    ASSERT_TRUE(out.is_open());
    out << "op,side,type,id,price,qty,ts_ns\n"
        << "submit,buy,limit,1,100,5\n";
  }

  FlowErrorInfo error;
  read_csv_all(file.str(), error);
  EXPECT_EQ(error.code, FlowError::InvalidCsv);
  EXPECT_NE(error.message.find("line 2"), std::string::npos) << error.message;
}

TEST(Flowfile, CsvWrongHeaderIsAnError) {
  const TempFile file("header.csv");
  {
    std::ofstream out(file.path(), std::ios::binary);
    ASSERT_TRUE(out.is_open());
    out << "op,side,type,id,price,qty\n"
        << "submit,buy,limit,1,100,5,1\n";
  }

  FlowErrorInfo error;
  read_csv_all(file.str(), error);
  EXPECT_EQ(error.code, FlowError::InvalidCsv);
  EXPECT_NE(error.message.find("line 1"), std::string::npos) << error.message;
}

TEST(Flowfile, CsvSinkMayStopEarly) {
  const TempFile file("stop.csv");
  const std::vector<FlowRecord> records = random_records(10, 23);
  FlowErrorInfo error;
  lob::flowfile::write_csv(file.str(), records, error);
  ASSERT_FALSE(error) << error.message;

  std::size_t seen = 0;
  lob::flowfile::read_csv(
      file.str(),
      [&seen](const FlowRecord&) {
        ++seen;
        return seen < 3;
      },
      error);
  EXPECT_FALSE(error) << error.message;
  EXPECT_EQ(seen, 3U);
}

TEST(Flowfile, MissingFileReportsOpenFailed) {
  const TempFile file("does_not_exist.bin");
  FlowReader reader;
  FlowErrorInfo error;
  reader.open(file.str(), error);
  EXPECT_EQ(error.code, FlowError::OpenFailed);
}

TEST(ReplayConfig, DefaultsAreValidAndShared) {
  const lob::Config config = lob::flowfile::default_replay_config();
  EXPECT_EQ(config.min_price, 1);
  EXPECT_EQ(config.max_price, 200000);
  EXPECT_EQ(config.max_orders, 1048576U);
  EXPECT_EQ(config.max_levels, 200000U);
  EXPECT_TRUE(lob::valid(config));
}

}  // namespace
