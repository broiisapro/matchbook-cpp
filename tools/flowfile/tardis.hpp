#pragma once

// Tardis `incremental_book_L2` CSV to flow file conversion. The converter
// keeps a synthetic book whose per-level queues mirror the absolute level
// sizes in the input: an increase submits one limit order for the delta,
// a decrease reduces or cancels the newest synthetic orders first, and the
// crossed opposite levels are cancelled before a level grows, so a replay of
// the output never trades.

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "lob/types.hpp"
#include "tools/flowfile/flowfile.hpp"

namespace lob::flowfile {

/// A decimal scale: value * 10^-scale. `0.00001` is `{1, 5}`, `1` is `{1, 0}`.
struct DecimalSpec {
  std::int64_t units;
  std::uint32_t scale;

  friend bool operator==(const DecimalSpec&, const DecimalSpec&) = default;
};

/// Parses a plain non-negative decimal into units and scale. Rejects signs,
/// empty strings, more than one '.', a missing integer or fraction part and
/// more than 18 digits.
std::optional<DecimalSpec> parse_decimal_spec(std::string_view text);

/// Converts a non-negative decimal string into an exact multiple of `spec`
/// (e.g. a price into ticks). Returns nullopt when the value is malformed,
/// has more significant digits than `spec.scale` or is not divisible by
/// `spec.units` — that is, when it is off the grid.
std::optional<std::int64_t> decimal_to_grid(std::string_view text, const DecimalSpec& spec);

enum class TardisErrorCode : std::uint8_t {
  None = 0,
  OpenFailed,       ///< the input path could not be opened
  ReadFailed,       ///< gzip or file read failure
  BadHeader,        ///< the CSV header is not the expected one
  BadRow,           ///< wrong field count
  BadTimestamp,     ///< timestamp is not a plain unsigned integer
  BadSnapshotFlag,  ///< is_snapshot is neither true nor false
  UnknownSide,      ///< side is not bid/ask or buy/sell
  OffGridPrice,     ///< price is not an exact multiple of the tick
  OffGridAmount,    ///< amount is not an exact multiple of the lot
  NoSnapshot,       ///< input ends before a snapshot row
};

std::string_view to_string(TardisErrorCode code);

/// Error code plus a human-readable message; messages name the CSV line.
struct TardisErrorInfo {
  TardisErrorCode code = TardisErrorCode::None;
  std::string message;

  explicit operator bool() const { return code != TardisErrorCode::None; }
};

struct TardisOptions {
  DecimalSpec tick{1, 5};      ///< --tick 0.00001
  DecimalSpec lot{1, 4};       ///< --lot 0.0001
  std::uint64_t max_rows = 0;  ///< 0 = unlimited
};

/// Stateful row-by-row converter. Feed each input line in order; line 1 must
/// be the header. Rows before the first snapshot are skipped; a snapshot that
/// follows delta rows clears the whole synthetic book first. Every emitted
/// record uses the row's timestamp times 1000 as ts_ns.
class TardisConverter {
 public:
  TardisConverter(const TardisOptions& options, CsvSink sink);

  bool process_line(std::string_view line, std::uint64_t line_number, TardisErrorInfo& error);

  std::uint64_t rows_processed() const { return rows_processed_; }
  std::uint64_t records_emitted() const { return records_emitted_; }
  bool has_snapshot() const { return loaded_snapshot_; }
  bool reached_max_rows() const {
    return options_.max_rows != 0 && rows_processed_ >= options_.max_rows;
  }
  bool stopped() const { return stopped_; }

 private:
  struct SyntheticOrder {
    OrderId id;
    Qty qty;
  };
  struct Level {
    Qty qty = 0;
    std::deque<SyntheticOrder> orders;
  };
  using LevelMap = std::map<Price, Level>;

  bool process_data_row(std::string_view line, std::uint64_t line_number, TardisErrorInfo& error);
  LevelMap& book(Side side);
  Level& level_at(Side side, Price price);
  void erase_if_empty(Side side, Price price);
  bool emit(const FlowRecord& record);
  void cancel_all(Side side, Level& level, std::uint64_t ts_ns);
  void clear_book(std::uint64_t ts_ns);
  void cancel_crossed(Side incoming_side, Price price, std::uint64_t ts_ns);
  void add_synthetic_order(Side side, Price price, Qty qty, std::uint64_t ts_ns);
  void apply_snapshot_level(Side side, Price price, Qty new_size, std::uint64_t ts_ns);
  void apply_delta_level(Side side, Price price, Qty new_size, std::uint64_t ts_ns);

  TardisOptions options_;
  CsvSink sink_;
  LevelMap bids_;
  LevelMap asks_;
  OrderId next_id_ = 1;
  std::uint64_t rows_processed_ = 0;
  std::uint64_t records_emitted_ = 0;
  bool loaded_snapshot_ = false;
  bool saw_delta_ = false;
  bool stopped_ = false;
};

/// Converts already-decompressed CSV text. Sets error to NoSnapshot when the
/// input ends without a snapshot row. rows_out/records_out receive the
/// processed row and emitted record counts when non-null.
bool convert_tardis_text(std::string_view text, const TardisOptions& options, const CsvSink& sink,
                         TardisErrorInfo& error, std::uint64_t* rows_out = nullptr,
                         std::uint64_t* records_out = nullptr);

/// Reads a whole gzip file into memory; samples are a few MB.
bool read_gzip_file(std::string_view path, std::string& out, TardisErrorInfo& error);

/// Converts a plain CSV or a `.gz`-suffixed gzip CSV file at path.
bool convert_tardis_file(std::string_view path, const TardisOptions& options, const CsvSink& sink,
                         TardisErrorInfo& error, std::uint64_t* rows_out = nullptr,
                         std::uint64_t* records_out = nullptr);

}  // namespace lob::flowfile
