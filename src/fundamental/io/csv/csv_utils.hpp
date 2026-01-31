#pragma once
#define CSV_IO_NO_THREAD
// ref project https://github.com/ben-strasser/fast-cpp-csv-parser
#include "csv.h"

#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string_view>
namespace Fundamental
{
namespace csv
{
using LineReader      = ::io::LineReader;
using trim_policy     = ::io::trim_chars<' ', '\t'>;
using quote_policy    = ::io::no_quote_escape<','>;
using overflow_policy = ::io::throw_on_overflow;
using comment_policy  = ::io::single_and_empty_line_comment<'#'>;
template <unsigned column_count>
using CSVReader = ::io::CSVReader<column_count, trim_policy, quote_policy, overflow_policy, comment_policy>;

constexpr std::size_t kDefaultCsvCacheBufLimitSize = 4 * 1024 * 1024;
constexpr std::size_t kDefaultCsvCacheRowsLimitCnt = 65535;
using CSVFlushCallback = std::function<void(std::string_view, std::size_t /*start row*/, std::size_t /*row cnts*/)>;
template <std::size_t column_count, char quote_char = ','>
struct CSVWriter {

    CSVWriter(const CSVFlushCallback& flush_cb,
              std::size_t cache_buf_limit_size  = kDefaultCsvCacheBufLimitSize,
              std::size_t cache_rows_limit_cnts = kDefaultCsvCacheRowsLimitCnt) :
    flush_cb_(flush_cb), cache_buf_limit_size_(cache_buf_limit_size), cache_rows_limit_cnts_(cache_rows_limit_cnts) {
    }
    ~CSVWriter() {
        flush_data();
    }
    template <typename... Args, typename = std::enable_if_t<sizeof...(Args) == column_count>>
    inline void write_row(Args&&... agrs) {
        write_row_imp(std::forward<Args>(agrs)...);
        ++current_row_cnts;
        if (current_row_cnts >= cache_rows_limit_cnts_) {
            flush_data();
            return;
        }
        // check cache buffer size
        if (cache_size() >= cache_buf_limit_size_) {
            flush_data();
            return;
        }
    }

    void flush_data() {
        if (current_row_cnts == 0) return;
        if (flush_cb_) {
#if __cplusplus > 201703L && _GLIBCXX_USE_CXX11_ABI
            auto data_view = out_buf_.view();
#else
            auto data = out_buf_.str();
            std::string_view data_view(data);
#endif
            flush_cb_(data_view, current_row_start_index, current_row_cnts);
        }
        current_row_start_index += current_row_cnts;
        current_row_cnts = 0;
        out_buf_.str("");
    }

    std::size_t row_cnt() const {
        return current_row_cnts;
    }

    std::size_t cache_size() const {
        return static_cast<std::size_t>(out_buf_.tellp());
    }

private:
    template <typename FirstArg, typename... Args>
    inline void write_row_imp(FirstArg&& first_arg, Args&&... agrs) {
        if constexpr (sizeof...(Args) > 0) {
            out_buf_ << std::forward<FirstArg>(first_arg) << quote_char;
            write_row_imp(std::forward<Args>(agrs)...);
        } else {
            write_row_end(std::forward<FirstArg>(first_arg));
        }
    }
    template <typename T>
    inline void write_row_end(T&& value) {
        out_buf_ << std::forward<T>(value) << '\n';
    }

private:
    CSVFlushCallback flush_cb_;
    const std::size_t cache_buf_limit_size_  = 0;
    const std::size_t cache_rows_limit_cnts_ = 0;
    std::size_t current_row_start_index      = 0;
    std::size_t current_row_cnts             = 0;
    mutable std::stringstream out_buf_;
};

template <typename... Args>
struct BatchCsvReaderWrap {
    using args_tuple_t = std::tuple<Args...>;
    template <class... ArgsCtor>
    explicit BatchCsvReaderWrap(ArgsCtor&&... args) : reader(std::forward<ArgsCtor>(args)...) {
    }
    template <typename... StringsList>
    void init_read_header(StringsList&&... args) {
        reader.read_header(::io::ignore_extra_column, std::forward<StringsList>(args)...);
    }

    std::vector<args_tuple_t> read(std::size_t batch_read_size = 65536) {
        std::vector<args_tuple_t> ret;
        read(ret, batch_read_size);
        return ret;
    }

    std::size_t /*row nums*/ read(std::vector<args_tuple_t>& out_vec, std::size_t batch_read_size = 65536) {
        std::size_t row_nums = 0;
        while (row_nums < batch_read_size) {
            if (!std::apply([this](Args&... args) -> bool { return reader.read_row(args...); },
                            out_vec.emplace_back())) {
                out_vec.pop_back();
                break;
            }
            ++row_nums;
        }
        return row_nums;
    }

    CSVReader<sizeof...(Args)> reader;
};

struct CsvWriterSinkInterface {
    virtual void flush_data(std::string_view, std::size_t /*start row*/, std::size_t /*row cnts*/) = 0;
    CSVFlushCallback wrapper_flush_func() {
        return [this](std::string_view data, std::size_t row, std::size_t row_cnt) { flush_data(data, row, row_cnt); };
    }
};

class CsvWriterFileSink : public CsvWriterSinkInterface {
public:
    CsvWriterFileSink(bool open_file_in_real_time = false) : open_file_in_real_time_(open_file_in_real_time) {
    }
    ~CsvWriterFileSink() {
    }
    bool Init(const std::string& file_name, bool is_append_mode = false) {
        file.close();
        file_name_                   = file_name;
        std::ios_base::openmode mode = is_append_mode ? std::ios::app : std::ios::trunc;
        mode |= (std::ios::binary | std::ios::out);
        file.open(file_name, mode);
        auto ret = file.operator bool();
        if (ret && open_file_in_real_time_) {
            file.close();
        }
        return ret;
    }

private:
    void flush_data(std::string_view data, std::size_t, std::size_t) override {
        if (open_file_in_real_time_) {
            file.open(file_name_, std::ios::binary | std::ios::out | std::ios::app);
        }
        if (!file) throw std::runtime_error(file_name_ + " open failed " + std::system_category().message(errno));
        auto origin_offset = static_cast<std::size_t>(file.tellp());
        file.write(data.data(), data.size());
        auto current_offset = static_cast<std::size_t>(file.tellp());
        if ((origin_offset + data.size()) != current_offset) {
            std::string throw_msg = file_name_ + " write failed " + " old offset:";
            throw_msg += std::to_string(origin_offset);
            throw_msg += " want write bytes:" + std::to_string(data.size());
            throw_msg += " current offset:" + std::to_string(current_offset);
            throw std::runtime_error(std::move(throw_msg));
        }
        if (open_file_in_real_time_) {
            file.close();
        }
    }

private:
    const bool open_file_in_real_time_ = false;
    std::string file_name_;

    std::ofstream file;
};

class MemoryStreamReader : public ::io::ByteSourceBase {
public:
    // csv read will read this data for init read
    constexpr static std::size_t kMinInitBufferSize = 2 * (1 << 20);
    constexpr static std::size_t kMaxLineLength     = (1 << 20) - 1;

public:
    explicit MemoryStreamReader(std::string& ref_str, bool& is_eof) : ref_str_(ref_str), is_eof_(is_eof) {
    }
    int read(char* buffer, int size) override {
        auto convert_size        = static_cast<std::size_t>(size);
        std::size_t search_start = convert_size > ref_str_.size() ? ref_str_.size() : convert_size;
        auto lf_pos              = ref_str_.rfind('\n', search_start - 1);
        std::size_t read_size    = 0;
        if (lf_pos == std::string::npos) {
            if (is_eof_) {
                read_size = search_start;
            } else if (convert_size < ref_str_.size()) {
                read_size = convert_size;
            }
            // otherwise remain for next read
        } else {
            read_size = search_start;
        }
        if (read_size > 0) {
            std::memcpy(buffer, ref_str_.data(), read_size);
            ref_str_ = ref_str_.substr(read_size);
        }
        return static_cast<int>(read_size);
    }

private:
    std::string& ref_str_;
    bool& is_eof_;
};

}; // namespace csv
}; // namespace Fundamental