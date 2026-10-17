#include "tools/flowfile/tardis.hpp"

#include <zlib.h>

#include <array>
#include <charconv>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace lob::flowfile {
namespace {

constexpr std::string_view kTardisHeader =
    "exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount";

constexpr std::size_t kTardisFieldCount = 8;

constexpr std::array<std::int64_t, 19> kPow10 = {
    1,
    10,
    100,
    1000,
    10000,
    100000,
    1000000,
    10000000,
    100000000,
    1000000000,
    10000000000,
    100000000000,
    1000000000000,
    10000000000000,
    100000000000000,
    1000000000000000,
    10000000000000000,
    100000000000000000,
    1000000000000000000,
};

std::string make_line_error(std::uint64_t line, std::string_view problem) {
  return "line " + std::to_string(line) + ": " + std::string(problem);
}

/// Strict non-negative decimal: digits with at most one '.' and both parts
/// present; at most 18 digits total so the result fits an int64.
bool parse_decimal_digits(std::string_view text, std::int64_t& units, std::uint32_t& scale) {
  if (text.empty()) {
    return false;
  }
  const std::size_t dot = text.find('.');
  if (dot != std::string_view::npos && text.find('.', dot + 1) != std::string_view::npos) {
    return false;
  }
  const std::string_view integer_part = dot == std::string_view::npos ? text : text.substr(0, dot);
  const std::string_view fraction_part =
      dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
  if (integer_part.empty() || (dot != std::string_view::npos && fraction_part.empty())) {
    return false;
  }
  if (integer_part.size() + fraction_part.size() > 18) {
    return false;
  }

  std::int64_t value = 0;
  for (const char c : integer_part) {
    if (c < '0' || c > '9') {
      return false;
    }
    value = value * 10 + (c - '0');
  }
  for (const char c : fraction_part) {
    if (c < '0' || c > '9') {
      return false;
    }
    value = value * 10 + (c - '0');
  }
  units = value;
  scale = static_cast<std::uint32_t>(fraction_part.size());
  return true;
}

std::vector<std::string_view> split_fields(std::string_view line) {
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

bool ends_with(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

bool read_plain_file(const std::string& path, std::string& out, TardisErrorInfo& error) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    error.code = TardisErrorCode::OpenFailed;
    error.message = "cannot open " + path + " for reading";
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  if (input.bad()) {
    error.code = TardisErrorCode::ReadFailed;
    error.message = "cannot read " + path;
    return false;
  }
  return true;
}

}  // namespace

std::string_view to_string(TardisErrorCode code) {
  switch (code) {
    case TardisErrorCode::None:
      return "none";
    case TardisErrorCode::OpenFailed:
      return "open failed";
    case TardisErrorCode::ReadFailed:
      return "read failed";
    case TardisErrorCode::BadHeader:
      return "bad header";
    case TardisErrorCode::BadRow:
      return "bad row";
    case TardisErrorCode::BadTimestamp:
      return "bad timestamp";
    case TardisErrorCode::BadSnapshotFlag:
      return "bad is_snapshot flag";
    case TardisErrorCode::UnknownSide:
      return "unknown side";
    case TardisErrorCode::OffGridPrice:
      return "price not on the tick grid";
    case TardisErrorCode::OffGridAmount:
      return "amount not on the lot grid";
    case TardisErrorCode::NoSnapshot:
      return "no snapshot row";
  }
  return "unknown";
}

std::optional<DecimalSpec> parse_decimal_spec(std::string_view text) {
  std::int64_t units = 0;
  std::uint32_t scale = 0;
  if (!parse_decimal_digits(text, units, scale) || units == 0) {
    return std::nullopt;
  }
  return DecimalSpec{units, scale};
}

std::optional<std::int64_t> decimal_to_grid(std::string_view text, const DecimalSpec& spec) {
  std::int64_t units = 0;
  std::uint32_t scale = 0;
  if (!parse_decimal_digits(text, units, scale)) {
    return std::nullopt;
  }

  if (scale > spec.scale) {
    const std::int64_t divisor = kPow10[scale - spec.scale];
    if (units % divisor != 0) {
      return std::nullopt;
    }
    units /= divisor;
  } else if (scale < spec.scale) {
    const std::int64_t multiplier = kPow10[spec.scale - scale];
    if (units > std::numeric_limits<std::int64_t>::max() / multiplier) {
      return std::nullopt;
    }
    units *= multiplier;
  }

  if (units % spec.units != 0) {
    return std::nullopt;
  }
  return units / spec.units;
}

TardisConverter::TardisConverter(const TardisOptions& options, CsvSink sink)
    : options_(options), sink_(std::move(sink)) {}

TardisConverter::LevelMap& TardisConverter::book(Side side) {
  return side == Side::Buy ? bids_ : asks_;
}

TardisConverter::Level& TardisConverter::level_at(Side side, Price price) {
  return book(side)[price];
}

void TardisConverter::erase_if_empty(Side side, Price price) {
  LevelMap& levels = book(side);
  const auto found = levels.find(price);
  if (found != levels.end() && found->second.orders.empty() && found->second.qty == 0) {
    levels.erase(found);
  }
}

bool TardisConverter::emit(const FlowRecord& record) {
  if (stopped_) {
    return false;
  }
  if (!sink_(record)) {
    stopped_ = true;
    return false;
  }
  ++records_emitted_;
  return true;
}

void TardisConverter::cancel_all(Side side, Level& level, std::uint64_t ts_ns) {
  while (!level.orders.empty()) {
    const SyntheticOrder order = level.orders.back();
    level.orders.pop_back();
    emit(FlowRecord{FlowOp::Cancel, side, OrderType::Limit, order.id, 0, 0, ts_ns});
  }
  level.qty = 0;
}

void TardisConverter::clear_book(std::uint64_t ts_ns) {
  for (auto& entry : bids_) {
    cancel_all(Side::Buy, entry.second, ts_ns);
  }
  for (auto& entry : asks_) {
    cancel_all(Side::Sell, entry.second, ts_ns);
  }
  bids_.clear();
  asks_.clear();
}

void TardisConverter::cancel_crossed(Side incoming_side, Price price, std::uint64_t ts_ns) {
  if (incoming_side == Side::Buy) {
    const auto end = asks_.upper_bound(price);
    for (auto it = asks_.begin(); it != end; ++it) {
      cancel_all(Side::Sell, it->second, ts_ns);
    }
    asks_.erase(asks_.begin(), end);
  } else {
    const auto begin = bids_.lower_bound(price);
    for (auto it = begin; it != bids_.end(); ++it) {
      cancel_all(Side::Buy, it->second, ts_ns);
    }
    bids_.erase(begin, bids_.end());
  }
}

void TardisConverter::add_synthetic_order(Side side, Price price, Qty qty, std::uint64_t ts_ns) {
  Level& level = level_at(side, price);
  const OrderId id = next_id_++;
  emit(FlowRecord{FlowOp::Submit, side, OrderType::Limit, id, price, qty, ts_ns});
  level.orders.push_back(SyntheticOrder{id, qty});
  level.qty += qty;
}

void TardisConverter::apply_snapshot_level(Side side, Price price, Qty new_size,
                                           std::uint64_t ts_ns) {
  Level& level = level_at(side, price);
  if (!level.orders.empty()) {
    cancel_all(side, level, ts_ns);
  }
  if (new_size > 0) {
    cancel_crossed(side, price, ts_ns);
    add_synthetic_order(side, price, new_size, ts_ns);
  }
  erase_if_empty(side, price);
}

void TardisConverter::apply_delta_level(Side side, Price price, Qty new_size, std::uint64_t ts_ns) {
  Level& level = level_at(side, price);
  const Qty current = level.qty;
  if (new_size == current) {
    erase_if_empty(side, price);
    return;
  }
  if (new_size > current) {
    cancel_crossed(side, price, ts_ns);
    add_synthetic_order(side, price, new_size - current, ts_ns);
    return;
  }

  Qty to_remove = current - new_size;
  while (to_remove > 0 && !level.orders.empty()) {
    SyntheticOrder& back = level.orders.back();
    if (back.qty <= to_remove) {
      emit(FlowRecord{FlowOp::Cancel, side, OrderType::Limit, back.id, 0, 0, ts_ns});
      to_remove -= back.qty;
      level.orders.pop_back();
    } else {
      back.qty -= to_remove;
      emit(FlowRecord{FlowOp::Modify, side, OrderType::Limit, back.id, price, back.qty, ts_ns});
      to_remove = 0;
    }
  }
  level.qty = new_size;
  erase_if_empty(side, price);
}

bool TardisConverter::process_line(std::string_view line, std::uint64_t line_number,
                                   TardisErrorInfo& error) {
  error = {};
  if (!line.empty() && line.back() == '\r') {
    line.remove_suffix(1);
  }
  if (line_number == 1) {
    if (line != kTardisHeader) {
      error.code = TardisErrorCode::BadHeader;
      error.message = make_line_error(1, "expected header '" + std::string(kTardisHeader) + "'");
      return false;
    }
    return true;
  }
  if (stopped_ || reached_max_rows()) {
    return true;
  }
  return process_data_row(line, line_number, error);
}

bool TardisConverter::process_data_row(std::string_view line, std::uint64_t line_number,
                                       TardisErrorInfo& error) {
  const std::vector<std::string_view> fields = split_fields(line);
  if (fields.size() != kTardisFieldCount) {
    error.code = TardisErrorCode::BadRow;
    error.message =
        make_line_error(line_number, "expected " + std::to_string(kTardisFieldCount) +
                                         " fields, found " + std::to_string(fields.size()));
    return false;
  }

  std::uint64_t timestamp = 0;
  const std::string_view timestamp_text = fields[2];
  if (timestamp_text.empty()) {
    error.code = TardisErrorCode::BadTimestamp;
    error.message = make_line_error(line_number, "empty timestamp");
    return false;
  }
  const auto timestamp_result = std::from_chars(
      timestamp_text.data(), timestamp_text.data() + timestamp_text.size(), timestamp);
  if (timestamp_result.ec != std::errc{} ||
      timestamp_result.ptr != timestamp_text.data() + timestamp_text.size() ||
      timestamp > std::numeric_limits<std::uint64_t>::max() / 1000) {
    error.code = TardisErrorCode::BadTimestamp;
    error.message =
        make_line_error(line_number, "bad timestamp '" + std::string(timestamp_text) + "'");
    return false;
  }

  bool is_snapshot = false;
  if (fields[4] == "true") {
    is_snapshot = true;
  } else if (fields[4] == "false") {
    is_snapshot = false;
  } else {
    error.code = TardisErrorCode::BadSnapshotFlag;
    error.message =
        make_line_error(line_number, "bad is_snapshot '" + std::string(fields[4]) + "'");
    return false;
  }

  Side side = Side::Buy;
  if (fields[5] == "bid" || fields[5] == "buy") {
    side = Side::Buy;
  } else if (fields[5] == "ask" || fields[5] == "sell") {
    side = Side::Sell;
  } else {
    error.code = TardisErrorCode::UnknownSide;
    error.message = make_line_error(line_number, "unknown side '" + std::string(fields[5]) + "'");
    return false;
  }

  const auto price = decimal_to_grid(fields[6], options_.tick);
  if (!price.has_value()) {
    error.code = TardisErrorCode::OffGridPrice;
    error.message = make_line_error(
        line_number, "price '" + std::string(fields[6]) + "' is not on the tick grid");
    return false;
  }
  const auto amount = decimal_to_grid(fields[7], options_.lot);
  if (!amount.has_value()) {
    error.code = TardisErrorCode::OffGridAmount;
    error.message = make_line_error(
        line_number, "amount '" + std::string(fields[7]) + "' is not on the lot grid");
    return false;
  }

  const std::uint64_t ts_ns = timestamp * 1000;
  if (is_snapshot) {
    if (saw_delta_) {
      clear_book(ts_ns);
      saw_delta_ = false;
    }
    loaded_snapshot_ = true;
    apply_snapshot_level(side, *price, *amount, ts_ns);
  } else {
    if (!loaded_snapshot_) {
      return true;
    }
    saw_delta_ = true;
    apply_delta_level(side, *price, *amount, ts_ns);
  }
  ++rows_processed_;
  return true;
}

bool convert_tardis_text(std::string_view text, const TardisOptions& options, const CsvSink& sink,
                         TardisErrorInfo& error, std::uint64_t* rows_out,
                         std::uint64_t* records_out) {
  error = {};
  TardisConverter converter(options, sink);

  std::uint64_t line_number = 0;
  std::size_t start = 0;
  while (true) {
    // A trailing newline ends the input; do not treat it as an empty line.
    if (start == text.size() && line_number > 0) {
      break;
    }
    const std::size_t end = text.find('\n', start);
    const std::string_view line =
        end == std::string_view::npos ? text.substr(start) : text.substr(start, end - start);
    ++line_number;
    if (!converter.process_line(line, line_number, error)) {
      return false;
    }
    if (converter.reached_max_rows() || end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }

  if (!converter.has_snapshot()) {
    error.code = TardisErrorCode::NoSnapshot;
    error.message = "input has no snapshot row";
    return false;
  }
  if (rows_out != nullptr) {
    *rows_out = converter.rows_processed();
  }
  if (records_out != nullptr) {
    *records_out = converter.records_emitted();
  }
  return true;
}

bool read_gzip_file(std::string_view path, std::string& out, TardisErrorInfo& error) {
  error = {};
  const std::string path_string(path);
  gzFile file = gzopen(path_string.c_str(), "rb");
  if (file == nullptr) {
    error.code = TardisErrorCode::OpenFailed;
    error.message = "cannot open " + path_string + " for reading";
    return false;
  }

  out.clear();
  std::array<char, 1 << 16> buffer{};
  int got = 0;
  while ((got = gzread(file, buffer.data(), static_cast<unsigned int>(buffer.size()))) > 0) {
    out.append(buffer.data(), static_cast<std::size_t>(got));
  }
  if (got < 0) {
    int code = 0;
    const char* message = gzerror(file, &code);
    error.code = TardisErrorCode::ReadFailed;
    error.message = "cannot read " + path_string + ": " + (message == nullptr ? "" : message);
    gzclose(file);
    return false;
  }
  gzclose(file);
  return true;
}

bool convert_tardis_file(std::string_view path, const TardisOptions& options, const CsvSink& sink,
                         TardisErrorInfo& error, std::uint64_t* rows_out,
                         std::uint64_t* records_out) {
  error = {};
  std::string text;
  if (ends_with(path, ".gz")) {
    if (!read_gzip_file(path, text, error)) {
      return false;
    }
  } else if (!read_plain_file(std::string(path), text, error)) {
    return false;
  }
  return convert_tardis_text(text, options, sink, error, rows_out, records_out);
}

}  // namespace lob::flowfile
