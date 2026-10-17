#include "tools/flowfile/flowfile.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace lob::flowfile {

namespace {

void set_error(FlowErrorInfo& error, FlowError code, std::string message) {
  error.code = code;
  error.message = std::move(message);
}

std::string io_error_message(const char* what) {
  return std::string(what) + ": " + std::strerror(errno);
}

/// Parses a whole decimal field; rejects empty strings, signs for unsigned
/// fields and any trailing character.
std::optional<std::uint64_t> parse_u64(std::string_view field) {
  if (field.empty()) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  const char* begin = field.data();
  const char* end = begin + field.size();
  const auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc{} || result.ptr != end) {
    return std::nullopt;
  }
  return value;
}

std::optional<std::int64_t> parse_i64(std::string_view field) {
  if (field.empty()) {
    return std::nullopt;
  }
  std::int64_t value = 0;
  const char* begin = field.data();
  const char* end = begin + field.size();
  const auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc{} || result.ptr != end) {
    return std::nullopt;
  }
  return value;
}

const char* op_word(FlowOp op) {
  switch (op) {
    case FlowOp::Submit:
      return "submit";
    case FlowOp::Cancel:
      return "cancel";
    case FlowOp::Modify:
      return "modify";
  }
  return "invalid";
}

std::optional<FlowOp> parse_op(std::string_view word) {
  if (word == "submit") {
    return FlowOp::Submit;
  }
  if (word == "cancel") {
    return FlowOp::Cancel;
  }
  if (word == "modify") {
    return FlowOp::Modify;
  }
  return std::nullopt;
}

std::optional<Side> parse_side(std::string_view word) {
  if (word == "buy") {
    return Side::Buy;
  }
  if (word == "sell") {
    return Side::Sell;
  }
  return std::nullopt;
}

std::optional<OrderType> parse_type(std::string_view word) {
  if (word == "limit") {
    return OrderType::Limit;
  }
  if (word == "market") {
    return OrderType::Market;
  }
  if (word == "ioc") {
    return OrderType::IOC;
  }
  return std::nullopt;
}

/// Validates the enum fields the frozen decode() casts without checking.
/// Returns the problem with the record index appended, or nullopt when the
/// record is well formed.
std::optional<std::string> validate_record(const FlowRecord& record, std::uint64_t index) {
  const auto op = static_cast<std::uint8_t>(record.op);
  if (op < 1 || op > 3) {
    return "record " + std::to_string(index) + ": invalid op byte " + std::to_string(op);
  }
  const auto side = static_cast<std::uint8_t>(record.side);
  if (side > 1) {
    return "record " + std::to_string(index) + ": invalid side byte " + std::to_string(side);
  }
  const auto type = static_cast<std::uint8_t>(record.type);
  if (type > 2) {
    return "record " + std::to_string(index) + ": invalid type byte " + std::to_string(type);
  }
  return std::nullopt;
}

constexpr std::string_view kCsvHeader = "op,side,type,id,price,qty,ts_ns";

constexpr std::size_t kCsvFieldCount = 7;

/// Splits on commas without trimming; a line must yield exactly seven fields.
std::vector<std::string_view> split_csv_line(std::string_view line) {
  std::vector<std::string_view> fields;
  std::size_t start = 0;
  while (true) {
    const std::size_t comma = line.find(',', start);
    if (comma == std::string_view::npos) {
      fields.push_back(line.substr(start));
      return fields;
    }
    fields.push_back(line.substr(start, comma - start));
    start = comma + 1;
  }
}

std::string make_line_error(std::uint64_t line, std::string_view problem) {
  return "line " + std::to_string(line) + ": " + std::string(problem);
}

}  // namespace

std::string_view to_string(FlowError error) {
  switch (error) {
    case FlowError::None:
      return "none";
    case FlowError::OpenFailed:
      return "open failed";
    case FlowError::WriteFailed:
      return "write failed";
    case FlowError::ReadFailed:
      return "read failed";
    case FlowError::BadHeader:
      return "bad header";
    case FlowError::Truncated:
      return "truncated file";
    case FlowError::CountMismatch:
      return "record count mismatch";
    case FlowError::InvalidRecord:
      return "invalid record";
    case FlowError::InvalidCsv:
      return "invalid csv";
  }
  return "unknown";
}

// ---------------------------------------------------------------------------
// FlowWriter
// ---------------------------------------------------------------------------

FlowWriter::~FlowWriter() {
  FlowErrorInfo ignored;
  close(ignored);
}

FlowWriter::FlowWriter(FlowWriter&& other) noexcept
    : file_(other.file_),
      record_count_(other.record_count_),
      buffer_(other.buffer_),
      buffer_used_(other.buffer_used_) {
  other.file_ = nullptr;
  other.record_count_ = 0;
  other.buffer_used_ = 0;
}

FlowWriter& FlowWriter::operator=(FlowWriter&& other) noexcept {
  if (this != &other) {
    FlowErrorInfo ignored;
    close(ignored);
    file_ = other.file_;
    record_count_ = other.record_count_;
    buffer_ = other.buffer_;
    buffer_used_ = other.buffer_used_;
    other.file_ = nullptr;
    other.record_count_ = 0;
    other.buffer_used_ = 0;
  }
  return *this;
}

void FlowWriter::open(std::string_view path, FlowErrorInfo& error) {
  error = {};
  if (is_open()) {
    set_error(error, FlowError::OpenFailed, "writer is already open");
    return;
  }

  const std::string path_string(path);
  file_ = std::fopen(path_string.c_str(), "wb");
  if (file_ == nullptr) {
    set_error(error, FlowError::OpenFailed, io_error_message("cannot open for writing"));
    return;
  }

  std::array<std::byte, kFlowHeaderSize> header{};
  encode_header(FlowHeader{0}, header);
  if (std::fwrite(header.data(), 1, header.size(), file_) != header.size()) {
    set_error(error, FlowError::WriteFailed, io_error_message("cannot write header"));
    std::fclose(file_);
    file_ = nullptr;
    return;
  }
  record_count_ = 0;
  buffer_used_ = 0;
}

void FlowWriter::write(const FlowRecord& record, FlowErrorInfo& error) {
  error = {};
  if (!is_open()) {
    set_error(error, FlowError::WriteFailed, "writer is not open");
    return;
  }

  if (buffer_used_ + kFlowRecordSize > buffer_.size()) {
    flush(error);
    if (error) {
      return;
    }
  }

  std::array<std::byte, kFlowRecordSize> encoded{};
  encode(record, encoded);
  std::memcpy(buffer_.data() + buffer_used_, encoded.data(), encoded.size());
  buffer_used_ += kFlowRecordSize;
  ++record_count_;
}

void FlowWriter::flush(FlowErrorInfo& error) {
  if (buffer_used_ == 0) {
    return;
  }
  if (std::fwrite(buffer_.data(), 1, buffer_used_, file_) != buffer_used_) {
    set_error(error, FlowError::WriteFailed, io_error_message("cannot write records"));
    return;
  }
  buffer_used_ = 0;
}

void FlowWriter::close(FlowErrorInfo& error) {
  error = {};
  if (!is_open()) {
    return;
  }

  flush(error);
  if (error) {
    std::fclose(file_);
    file_ = nullptr;
    return;
  }

  if (std::fseek(file_, 0, SEEK_SET) != 0) {
    set_error(error, FlowError::WriteFailed, io_error_message("cannot seek to rewrite header"));
    std::fclose(file_);
    file_ = nullptr;
    return;
  }

  std::array<std::byte, kFlowHeaderSize> header{};
  encode_header(FlowHeader{record_count_}, header);
  if (std::fwrite(header.data(), 1, header.size(), file_) != header.size()) {
    set_error(error, FlowError::WriteFailed, io_error_message("cannot rewrite header"));
    std::fclose(file_);
    file_ = nullptr;
    return;
  }

  if (std::fclose(file_) != 0) {
    set_error(error, FlowError::WriteFailed, io_error_message("cannot close"));
    file_ = nullptr;
    return;
  }
  file_ = nullptr;
}

// ---------------------------------------------------------------------------
// FlowReader
// ---------------------------------------------------------------------------

FlowReader::~FlowReader() {
  if (file_ != nullptr) {
    std::fclose(file_);
  }
}

FlowReader::FlowReader(FlowReader&& other) noexcept
    : file_(other.file_),
      record_count_(other.record_count_),
      records_read_(other.records_read_),
      buffer_(other.buffer_),
      buffer_pos_(other.buffer_pos_),
      buffer_len_(other.buffer_len_),
      eof_(other.eof_) {
  other.file_ = nullptr;
  other.record_count_ = 0;
  other.records_read_ = 0;
  other.buffer_pos_ = 0;
  other.buffer_len_ = 0;
  other.eof_ = false;
}

FlowReader& FlowReader::operator=(FlowReader&& other) noexcept {
  if (this != &other) {
    if (file_ != nullptr) {
      std::fclose(file_);
    }
    file_ = other.file_;
    record_count_ = other.record_count_;
    records_read_ = other.records_read_;
    buffer_ = other.buffer_;
    buffer_pos_ = other.buffer_pos_;
    buffer_len_ = other.buffer_len_;
    eof_ = other.eof_;
    other.file_ = nullptr;
    other.record_count_ = 0;
    other.records_read_ = 0;
    other.buffer_pos_ = 0;
    other.buffer_len_ = 0;
    other.eof_ = false;
  }
  return *this;
}

void FlowReader::open(std::string_view path, FlowErrorInfo& error) {
  error = {};
  if (is_open()) {
    set_error(error, FlowError::OpenFailed, "reader is already open");
    return;
  }

  const std::string path_string(path);
  file_ = std::fopen(path_string.c_str(), "rb");
  if (file_ == nullptr) {
    set_error(error, FlowError::OpenFailed, io_error_message("cannot open for reading"));
    return;
  }

  std::array<std::byte, kFlowHeaderSize> header{};
  const std::size_t got = std::fread(header.data(), 1, header.size(), file_);
  if (got != header.size()) {
    if (std::ferror(file_)) {
      set_error(error, FlowError::ReadFailed, io_error_message("cannot read header"));
    } else {
      set_error(error, FlowError::BadHeader,
                "file is shorter than the " + std::to_string(kFlowHeaderSize) + "-byte header");
    }
    std::fclose(file_);
    file_ = nullptr;
    return;
  }

  const auto decoded = decode_header(header);
  if (!decoded.has_value()) {
    const std::byte* data = header.data();
    const bool magic_ok =
        std::equal(kFlowMagic.begin(), kFlowMagic.end(), data, [](char expected, std::byte actual) {
          return actual == static_cast<std::byte>(static_cast<unsigned char>(expected));
        });
    if (!magic_ok) {
      set_error(error, FlowError::BadHeader, "not a flow file: bad magic");
    } else if (detail::load_u32(data + 8) != kFlowVersion) {
      set_error(error, FlowError::BadHeader,
                "unsupported flow file version " + std::to_string(detail::load_u32(data + 8)));
    } else {
      set_error(error, FlowError::BadHeader,
                "unsupported flow record size " + std::to_string(detail::load_u32(data + 12)));
    }
    std::fclose(file_);
    file_ = nullptr;
    return;
  }

  record_count_ = decoded->record_count;
  records_read_ = 0;
  buffer_pos_ = 0;
  buffer_len_ = 0;
  eof_ = false;
}

FlowReader::FillResult FlowReader::fill_to(std::size_t need) {
  if (available() >= need) {
    return FillResult::Ok;
  }
  if (buffer_pos_ > 0) {
    const std::size_t unread = available();
    std::memmove(buffer_.data(), buffer_.data() + buffer_pos_, unread);
    buffer_len_ = unread;
    buffer_pos_ = 0;
  }
  while (buffer_len_ < need && !eof_) {
    const std::size_t got =
        std::fread(buffer_.data() + buffer_len_, 1, buffer_.size() - buffer_len_, file_);
    if (got == 0) {
      if (std::ferror(file_)) {
        return FillResult::Error;
      }
      eof_ = true;
      break;
    }
    buffer_len_ += got;
  }
  return buffer_len_ >= need ? FillResult::Ok : FillResult::Eof;
}

bool FlowReader::read(FlowRecord& out, FlowErrorInfo& error) {
  error = {};
  if (!is_open()) {
    set_error(error, FlowError::ReadFailed, "reader is not open");
    return false;
  }

  if (records_read_ == record_count_) {
    const FillResult trailing = fill_to(1);
    if (trailing == FillResult::Error) {
      set_error(error, FlowError::ReadFailed, io_error_message("cannot read"));
      return false;
    }
    if (trailing == FillResult::Ok) {
      const FillResult full_record = fill_to(kFlowRecordSize);
      const std::string base =
          "file holds more data than the header's " + std::to_string(record_count_) + " records";
      set_error(error, FlowError::CountMismatch,
                full_record == FillResult::Ok ? base + " (extra records)"
                                              : base + " (partial trailing record)");
      return false;
    }
    return false;
  }

  const FillResult filled = fill_to(kFlowRecordSize);
  if (filled == FillResult::Error) {
    set_error(error, FlowError::ReadFailed, io_error_message("cannot read"));
    return false;
  }
  if (filled == FillResult::Eof) {
    set_error(error, FlowError::Truncated,
              "file ends after " + std::to_string(records_read_) + " of " +
                  std::to_string(record_count_) + " records");
    return false;
  }

  std::array<std::byte, kFlowRecordSize> raw{};
  std::memcpy(raw.data(), buffer_.data() + buffer_pos_, raw.size());
  const FlowRecord record = decode(raw);
  if (const auto problem = validate_record(record, records_read_)) {
    set_error(error, FlowError::InvalidRecord, *problem);
    return false;
  }
  buffer_pos_ += kFlowRecordSize;
  ++records_read_;
  out = record;
  return true;
}

// ---------------------------------------------------------------------------
// CSV
// ---------------------------------------------------------------------------

void write_csv(std::string_view path, std::span<const FlowRecord> records, FlowErrorInfo& error) {
  error = {};
  const std::string path_string(path);
  std::FILE* file = std::fopen(path_string.c_str(), "wb");
  if (file == nullptr) {
    set_error(error, FlowError::OpenFailed, io_error_message("cannot open for writing"));
    return;
  }

  if (std::fputs("op,side,type,id,price,qty,ts_ns\n", file) == EOF) {
    set_error(error, FlowError::WriteFailed, io_error_message("cannot write csv header"));
    std::fclose(file);
    return;
  }

  for (const FlowRecord& record : records) {
    const std::string line = std::string(op_word(record.op)) + "," +
                             std::string(to_string(record.side)) + "," +
                             std::string(to_string(record.type)) + "," + std::to_string(record.id) +
                             "," + std::to_string(record.price) + "," + std::to_string(record.qty) +
                             "," + std::to_string(record.ts_ns) + "\n";
    if (std::fwrite(line.data(), 1, line.size(), file) != line.size()) {
      set_error(error, FlowError::WriteFailed, io_error_message("cannot write csv record"));
      std::fclose(file);
      return;
    }
  }

  if (std::fclose(file) != 0) {
    set_error(error, FlowError::WriteFailed, io_error_message("cannot close"));
  }
}

void read_csv(std::string_view path, const CsvSink& sink, FlowErrorInfo& error) {
  error = {};
  const std::string path_string(path);
  std::ifstream input(path_string, std::ios::binary);
  if (!input.is_open()) {
    set_error(error, FlowError::OpenFailed, "cannot open " + path_string + " for reading");
    return;
  }

  std::string line;
  if (!std::getline(input, line)) {
    set_error(error, FlowError::InvalidCsv, make_line_error(1, "missing header"));
    return;
  }
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  if (line != kCsvHeader) {
    set_error(error, FlowError::InvalidCsv,
              make_line_error(1, "expected header '" + std::string(kCsvHeader) + "'"));
    return;
  }

  std::uint64_t line_number = 1;
  while (std::getline(input, line)) {
    ++line_number;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const std::vector<std::string_view> fields = split_csv_line(line);
    if (fields.size() != kCsvFieldCount) {
      set_error(
          error, FlowError::InvalidCsv,
          make_line_error(line_number, "expected " + std::to_string(kCsvFieldCount) +
                                           " fields, found " + std::to_string(fields.size())));
      return;
    }

    const auto op = parse_op(fields[0]);
    if (!op.has_value()) {
      set_error(error, FlowError::InvalidCsv,
                make_line_error(line_number, "invalid op '" + std::string(fields[0]) + "'"));
      return;
    }
    const auto side = parse_side(fields[1]);
    if (!side.has_value()) {
      set_error(error, FlowError::InvalidCsv,
                make_line_error(line_number, "invalid side '" + std::string(fields[1]) + "'"));
      return;
    }
    const auto type = parse_type(fields[2]);
    if (!type.has_value()) {
      set_error(error, FlowError::InvalidCsv,
                make_line_error(line_number, "invalid type '" + std::string(fields[2]) + "'"));
      return;
    }
    const auto id = parse_u64(fields[3]);
    if (!id.has_value()) {
      set_error(error, FlowError::InvalidCsv,
                make_line_error(line_number, "invalid id '" + std::string(fields[3]) + "'"));
      return;
    }
    const auto price = parse_i64(fields[4]);
    if (!price.has_value()) {
      set_error(error, FlowError::InvalidCsv,
                make_line_error(line_number, "invalid price '" + std::string(fields[4]) + "'"));
      return;
    }
    const auto qty = parse_i64(fields[5]);
    if (!qty.has_value()) {
      set_error(error, FlowError::InvalidCsv,
                make_line_error(line_number, "invalid qty '" + std::string(fields[5]) + "'"));
      return;
    }
    const auto ts_ns = parse_u64(fields[6]);
    if (!ts_ns.has_value()) {
      set_error(error, FlowError::InvalidCsv,
                make_line_error(line_number, "invalid ts_ns '" + std::string(fields[6]) + "'"));
      return;
    }

    const FlowRecord record{*op, *side, *type, *id, *price, *qty, *ts_ns};
    if (!sink(record)) {
      return;
    }
  }
}

}  // namespace lob::flowfile
