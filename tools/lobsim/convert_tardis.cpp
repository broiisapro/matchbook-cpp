#include <charconv>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "lob/types.hpp"
#include "tools/flowfile/flowfile.hpp"
#include "tools/flowfile/tardis.hpp"
#include "tools/lobsim/commands.hpp"

namespace lobsim {
namespace {

using lob::flowfile::DecimalSpec;
using lob::flowfile::FlowErrorInfo;
using lob::flowfile::FlowRecord;
using lob::flowfile::TardisErrorInfo;
using lob::flowfile::TardisOptions;

constexpr std::string_view kUsage =
    "usage: lobsim convert-tardis IN --out F [options]\n"
    "\n"
    "Converts a Tardis incremental_book_L2 CSV file (decompressed on the fly\n"
    "when IN ends in .gz) into a synthetic flow file whose replay never trades.\n"
    "Exit codes: 0 success, 2 usage error, 1 input or output failure.\n"
    "\n"
    "required:\n"
    "  IN                  input CSV or CSV.gz file\n"
    "  --out F             output flow file (binary, or CSV when F ends in .csv)\n"
    "\n"
    "options:\n"
    "  --tick D            price tick as an exact decimal (default 0.00001)\n"
    "  --lot D             lot size as an exact decimal (default 0.0001)\n"
    "  --max-rows N        stop after N data rows (default: all)\n";

struct ConvertOptions {
  std::string input;
  std::string out;
  DecimalSpec tick{1, 5};
  DecimalSpec lot{1, 4};
  std::uint64_t max_rows = 0;
};

bool parse_decimal(std::string_view flag, std::string_view value, DecimalSpec& spec) {
  const auto parsed = lob::flowfile::parse_decimal_spec(value);
  if (!parsed.has_value()) {
    std::fprintf(stderr, "convert-tardis: %s needs a positive decimal, got '%s'\n",
                 std::string(flag).c_str(), std::string(value).c_str());
    return false;
  }
  spec = *parsed;
  return true;
}

bool parse_args(int argc, char** argv, ConvertOptions& options) {
  bool have_out = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view flag = argv[i];
    if (flag.empty() || flag.front() != '-') {
      if (!options.input.empty()) {
        std::fprintf(stderr, "convert-tardis: unexpected extra argument '%s'\n", argv[i]);
        return false;
      }
      options.input = std::string(flag);
      continue;
    }
    if (i + 1 >= argc) {
      std::fprintf(stderr, "convert-tardis: '%s' needs a value\n", argv[i]);
      return false;
    }
    const std::string_view value = argv[++i];

    if (flag == "--out") {
      options.out = std::string(value);
      have_out = true;
    } else if (flag == "--tick") {
      if (!parse_decimal(flag, value, options.tick)) {
        return false;
      }
    } else if (flag == "--lot") {
      if (!parse_decimal(flag, value, options.lot)) {
        return false;
      }
    } else if (flag == "--max-rows") {
      const std::string_view text = value;
      if (text.empty()) {
        std::fputs("convert-tardis: --max-rows needs an integer\n", stderr);
        return false;
      }
      const auto result = std::from_chars(text.data(), text.data() + text.size(), options.max_rows);
      if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        std::fputs("convert-tardis: --max-rows needs a non-negative integer\n", stderr);
        return false;
      }
    } else {
      std::fprintf(stderr, "convert-tardis: unknown option '%s'\n", argv[i - 1]);
      return false;
    }
  }

  if (options.input.empty()) {
    std::fputs("convert-tardis: an input file argument is required\n", stderr);
    return false;
  }
  if (!have_out) {
    std::fputs("convert-tardis: --out is required\n", stderr);
    return false;
  }
  return true;
}

bool has_csv_extension(std::string_view path) {
  return path.size() >= 4 && path.substr(path.size() - 4) == ".csv";
}

}  // namespace

int run_convert_tardis(int argc, char** argv) {
  ConvertOptions options;
  if (!parse_args(argc, argv, options)) {
    std::fputs(kUsage.data(), stderr);
    return 2;
  }

  const TardisOptions tardis_options{options.tick, options.lot, options.max_rows};
  TardisErrorInfo error;
  std::uint64_t rows = 0;
  std::uint64_t records = 0;

  if (has_csv_extension(options.out)) {
    std::vector<FlowRecord> converted;
    const bool ok = lob::flowfile::convert_tardis_file(
        options.input, tardis_options,
        [&converted](const FlowRecord& record) {
          converted.push_back(record);
          return true;
        },
        error, &rows, &records);
    if (!ok) {
      std::fprintf(stderr, "convert-tardis: %s\n", error.message.c_str());
      return 1;
    }
    FlowErrorInfo write_error;
    lob::flowfile::write_csv(options.out, converted, write_error);
    if (write_error) {
      std::fprintf(stderr, "convert-tardis: %s: %s\n", options.out.c_str(),
                   write_error.message.c_str());
      return 1;
    }
  } else {
    lob::flowfile::FlowWriter writer;
    FlowErrorInfo write_error;
    writer.open(options.out, write_error);
    if (write_error) {
      std::fprintf(stderr, "convert-tardis: %s: %s\n", options.out.c_str(),
                   write_error.message.c_str());
      return 1;
    }
    const bool ok = lob::flowfile::convert_tardis_file(
        options.input, tardis_options,
        [&writer, &write_error](const FlowRecord& record) {
          writer.write(record, write_error);
          return !write_error;
        },
        error, &rows, &records);
    if (!ok) {
      std::fprintf(stderr, "convert-tardis: %s\n", error.message.c_str());
      return 1;
    }
    writer.close(write_error);
    if (write_error) {
      std::fprintf(stderr, "convert-tardis: %s: %s\n", options.out.c_str(),
                   write_error.message.c_str());
      return 1;
    }
  }

  std::printf("rows: %llu\n", static_cast<unsigned long long>(rows));
  std::printf("records: %llu\n", static_cast<unsigned long long>(records));
  std::printf("out: %s\n", options.out.c_str());
  return 0;
}

}  // namespace lobsim
