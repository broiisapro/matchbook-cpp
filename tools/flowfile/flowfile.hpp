#pragma once

// Binary and CSV flow file I/O on top of the frozen wire format in
// flow_record.hpp. The binary form is a 32-byte header followed by fixed-size
// 40-byte records; the CSV form is one header line plus one line per record
// with the enum fields written as words. Both readers report problems through
// FlowErrorInfo instead of exceptions.

#include <array>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <span>
#include <string>
#include <string_view>

#include "tools/flowfile/flow_record.hpp"

namespace lob::flowfile {

/// I/O buffer size for binary readers and writers; a whole number of records.
inline constexpr std::size_t kFlowIoBufferSize = kFlowRecordSize * 1600;

/// What went wrong. None means success.
enum class FlowError : std::uint8_t {
  None = 0,
  OpenFailed,     ///< the path could not be opened
  WriteFailed,    ///< short write or flush error
  ReadFailed,     ///< short read or I/O error other than end of file
  BadHeader,      ///< magic, version or record size does not match the format
  Truncated,      ///< the file ends before the header's record count is reached
  CountMismatch,  ///< the file holds more data than the header's record count
  InvalidRecord,  ///< an enum byte is out of range on a binary read
  InvalidCsv,     ///< a CSV line does not parse
};

std::string_view to_string(FlowError error);

/// Error code plus a human-readable message. Messages name the record index or
/// the CSV line number when one is known.
struct FlowErrorInfo {
  FlowError code = FlowError::None;
  std::string message;

  explicit operator bool() const { return code != FlowError::None; }
};

/// Writes a binary flow file. The constructor is inert; open() writes the
/// header with record count 0, write() buffers records, and close() flushes,
/// seeks back to the start and rewrites the header with the final count. The
/// destructor closes if the caller did not; explicit close() is the only way
/// to observe a write error.
class FlowWriter {
 public:
  FlowWriter() = default;
  ~FlowWriter();

  FlowWriter(const FlowWriter&) = delete;
  FlowWriter& operator=(const FlowWriter&) = delete;
  FlowWriter(FlowWriter&& other) noexcept;
  FlowWriter& operator=(FlowWriter&& other) noexcept;

  /// Creates or truncates path and writes the count-0 header. On failure file_
  /// stays closed and error is OpenFailed (with strerror text).
  void open(std::string_view path, FlowErrorInfo& error);

  /// Appends one record to the internal buffer, flushing a full buffer to
  /// disk first.
  void write(const FlowRecord& record, FlowErrorInfo& error);

  /// Flushes buffered records, rewrites the header with the final count and
  /// closes the file. Safe to call twice; the second call is a no-op.
  void close(FlowErrorInfo& error);

  std::uint64_t record_count() const { return record_count_; }
  bool is_open() const { return file_ != nullptr; }

 private:
  void flush(FlowErrorInfo& error);

  std::FILE* file_ = nullptr;
  std::uint64_t record_count_ = 0;
  std::array<std::byte, kFlowIoBufferSize> buffer_{};
  std::size_t buffer_used_ = 0;
};

/// Reads a binary flow file. open() validates the header; read() decodes and
/// validates records, returning false at the clean end of the stream or on
/// error. A file that ends before the header count (including a partial
/// trailing record) and a file with extra data after the count are both
/// errors; a short read is never reported as a clean end.
class FlowReader {
 public:
  FlowReader() = default;
  ~FlowReader();

  FlowReader(const FlowReader&) = delete;
  FlowReader& operator=(const FlowReader&) = delete;
  FlowReader(FlowReader&& other) noexcept;
  FlowReader& operator=(FlowReader&& other) noexcept;

  /// Opens path and reads the 32-byte header. error is BadHeader when the
  /// magic, version or record size does not match, or when the file is
  /// smaller than a header.
  void open(std::string_view path, FlowErrorInfo& error);

  /// Reads the next record. Returns false exactly when there is no next
  /// record: at the clean end (error is None) or on failure (error describes
  /// the problem and records_read() counts the records before it).
  bool read(FlowRecord& out, FlowErrorInfo& error);

  /// Record count from the header.
  std::uint64_t record_count() const { return record_count_; }

  /// Records successfully decoded so far.
  std::uint64_t records_read() const { return records_read_; }

  bool is_open() const { return file_ != nullptr; }

 private:
  enum class FillResult : std::uint8_t { Ok, Eof, Error };

  /// Makes at least `need` unread bytes available unless end of file or an I/O
  /// error interrupts. Never reads more than the buffer holds.
  FillResult fill_to(std::size_t need);
  std::size_t available() const { return buffer_len_ - buffer_pos_; }

  std::FILE* file_ = nullptr;
  std::uint64_t record_count_ = 0;
  std::uint64_t records_read_ = 0;
  std::array<std::byte, kFlowIoBufferSize> buffer_{};
  std::size_t buffer_pos_ = 0;
  std::size_t buffer_len_ = 0;
  bool eof_ = false;
};

/// Writes one record per line to path, preceded by the header line
/// `op,side,type,id,price,qty,ts_ns`. Enum fields use the words of
/// to_string(Side) / to_string(OrderType) and submit/cancel/modify.
void write_csv(std::string_view path, std::span<const FlowRecord> records, FlowErrorInfo& error);

/// Streams records from the CSV file at path. The first line must be the exact
/// header; every later line must hold seven fields. The sink is called once
/// per record and may return false to stop reading early (that is not an
/// error). Parse failures set error to InvalidCsv with a 1-based line number
/// in the message, and the sink is not called for the bad line.
using CsvSink = std::function<bool(const FlowRecord&)>;
void read_csv(std::string_view path, const CsvSink& sink, FlowErrorInfo& error);

}  // namespace lob::flowfile
