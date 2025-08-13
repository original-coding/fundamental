//
// @author lightning1993 <469953258@qq.com> 2025/08
//
#pragma once
#include "endian_utils.hpp"
#include "parallel.hpp"

#include <cstdint>
#include <cstring>
#include <zlib.h>

#include <chrono>
#include <ctime>
#include <list>

namespace Fundamental
{
using check_sum_t = std::uint32_t;
enum class DeflateCheckSumType : std::uint32_t
{
    // raw
    CRC32_CHECK_T,
    // zlib format or raw
    ADLER32_CHECK_T
};

enum class DeflateOutputFormat : std::uint32_t
{
    RAW_DEFLATE_STREAM_FORMAT,
    ZLIB_DEFLATE_STREAM_FORMAT
};

struct DeflateConfig {
    constexpr static std::uint32_t kMinChunkSize = 1024;
    void CorrectParams() {
        if (output_format == DeflateOutputFormat::ZLIB_DEFLATE_STREAM_FORMAT) {
            check_sum_type = DeflateCheckSumType::ADLER32_CHECK_T;
        }
        if (chunk_size < kMinChunkSize) chunk_size = kMinChunkSize;
    }
    // estimate the deflated/compressed length of data corresponding to input_len
    std::size_t GuessCompressLen(std::size_t input_len) const {
        auto full_chunk_nums = input_len / chunk_size;
        auto remain_len      = input_len % chunk_size;
        std::size_t ret =
            full_chunk_nums > 0
                ? static_cast<std::size_t>(compressBound(static_cast<std::uint32_t>(chunk_size))) * full_chunk_nums
                : 0;
        ret += remain_len > 0 ? static_cast<std::size_t>(compressBound(static_cast<std::uint32_t>(remain_len))) : 0;
        return ret;
    }

    std::uint32_t GetDefaultCheckCode() const {
        switch (check_sum_type) {
        case DeflateCheckSumType::ADLER32_CHECK_T: return 1;
        default: return 0;
        }
    }

    DeflateCheckSumType check_sum_type = DeflateCheckSumType::ADLER32_CHECK_T;
    DeflateOutputFormat output_format  = DeflateOutputFormat::ZLIB_DEFLATE_STREAM_FORMAT;
    std::uint32_t chunk_size           = 128U * 1024; // 128k
    // inherited from zlib Z_DEFAULT_COMPRESSION=-1
    // 0 means no compress
    std::int32_t compress_level = 6; // [-1 - 9]

    // internal data
    std::uint32_t precompute_shift = 0;
};

class ZUtils {
public:
    static std::int32_t GetLastErrorCode() {
        return s_last_code;
    }
    static bool InflateBinary(const void* srcBuf, std::size_t inputLen, void* dstBuf, std::size_t* outputLen) {
        s_last_code = Z_ERRNO;
        if (!dstBuf || !outputLen) return false;
        unsigned long dstBufferSize = (unsigned long)*outputLen;
        *outputLen                  = 0;
        if (dstBufferSize == 0) return false;
        s_last_code = uncompress(static_cast<std::uint8_t*>(dstBuf), &dstBufferSize,
                                 static_cast<const std::uint8_t*>(srcBuf), (std::uint32_t)inputLen);
        if (s_last_code != Z_OK) {
            return false;
        }
        *outputLen = dstBufferSize;
        return true;
    }
    static bool DeflateBinary(const void* srcBuf,
                              std::size_t inputLen,
                              void* dstBuf,
                              std::size_t* outputLen,
                              std::int32_t level = Z_DEFAULT_COMPRESSION) {
        s_last_code = Z_ERRNO;
        if (!dstBuf || !outputLen) return false;
        unsigned long dstBufferSize = (unsigned long)*outputLen;
        *outputLen                  = 0;
        if (dstBufferSize == 0) return false;
        s_last_code = compress2(static_cast<std::uint8_t*>(dstBuf), &dstBufferSize,
                                static_cast<const std::uint8_t*>(srcBuf), (std::uint32_t)inputLen, level);
        if (s_last_code != Z_OK) {
            return false;
        }
        *outputLen = dstBufferSize;
        return true;
    }
    template <typename executor_t = DefaultParallelExecutor>
    static std::tuple<bool, check_sum_t> ParallelDeflateBinary(const void* srcBuf,
                                                               std::size_t inputLen,
                                                               void* dstBuf,
                                                               std::size_t* outputLen,
                                                               const executor_t& executor = {},
                                                               DeflateConfig config       = {});

private:
    inline static thread_local std::int32_t s_last_code = Z_OK;
};
namespace deflate_internal
{
template <typename executor_t>
struct ParrallelDeflateContext {
    constexpr static check_sum_t kAdlerBase = 65521U;
    constexpr static check_sum_t kLow16Mask = 0xffff;
    // CRC-32 polynomial, reflected.
    constexpr static check_sum_t kCRC_POLY = 0xedb88320;
    // Table of x^2^n modulo p(x).
    constexpr static const check_sum_t k_x2n_table[] = {
        0x40000000, 0x20000000, 0x08000000, 0x00800000, 0x00008000, 0xedb88320, 0xb1e6b092, 0xa06a2517,
        0xed627dae, 0x88d14467, 0xd7bbfe6a, 0xec447f11, 0x8e7ea170, 0x6427800e, 0x4d47bae0, 0x09fe548f,
        0x83852d0f, 0x30362f1a, 0x7b5a9cc3, 0x31fec169, 0x9fec022a, 0x6c8dedc4, 0x15d6874d, 0x5fde7a4e,
        0xbad90e37, 0x2e4e5eef, 0x4eaba214, 0xa8a472c0, 0x429a969e, 0x148d302a, 0xc40ba6d0, 0xc4e22c3c
    };
    ParrallelDeflateContext(const void* srcBuf,
                            std::size_t inputLen,
                            void* dstBuf,
                            std::size_t* outputLen,
                            const executor_t& executor,
                            const DeflateConfig& config) :
    src_buf(static_cast<const std::uint8_t*>(srcBuf)), input_len(inputLen), dst_buf(static_cast<std::uint8_t*>(dstBuf)),
    output_max_len(*outputLen), executor(executor), config(config) {
    }
    static check_sum_t adler32_comb(check_sum_t adler1, check_sum_t adler2, size_t len2) {
        check_sum_t sum1;
        check_sum_t sum2;

        // the derivation of this formula is left as an exercise for the reader
        auto rem = static_cast<check_sum_t>(len2 % kAdlerBase);
        sum1     = adler1 & kLow16Mask;
        sum2     = (rem * sum1) % kAdlerBase;
        sum1 += (adler2 & kLow16Mask) + kAdlerBase - 1;
        sum2 += ((adler1 >> 16) & kLow16Mask) + ((adler2 >> 16) & kLow16Mask) + kAdlerBase - rem;
        if (sum1 >= kAdlerBase) sum1 -= kAdlerBase;
        if (sum1 >= kAdlerBase) sum1 -= kAdlerBase;
        if (sum2 >= (kAdlerBase << 1)) sum2 -= (kAdlerBase << 1);
        if (sum2 >= kAdlerBase) sum2 -= kAdlerBase;
        return sum1 | (sum2 << 16);
    }
    // Combine two crc-32's or two adler-32's (copied from zlib 1.2.3 so that pigz
    // can be compatible with older versions of zlib).

    // We copy the combination routines from zlib here, in order to avoid linkage
    // issues with the zlib 1.2.3 builds on Sun, Ubuntu, and others.

    // Return a(x) multiplied by b(x) modulo p(x), where p(x) is the CRC
    // polynomial, reflected. For speed, this requires that a not be zero.
    static check_sum_t multmodp(check_sum_t a, check_sum_t b) {
        check_sum_t m = static_cast<check_sum_t>(1) << 31;
        check_sum_t p = 0;
        for (;;) {
            if (a & m) {
                p ^= b;
                if ((a & (m - 1)) == 0) break;
            }
            m >>= 1;
            b = b & 1 ? (b >> 1) ^ kCRC_POLY : b >> 1;
        }
        return p;
    }
    // Return x^(n*2^k) modulo p(x).
    static check_sum_t x2nmodp(size_t n, unsigned k) {
        check_sum_t p = (check_sum_t)1 << 31; // x^0 == 1
        while (n) {
            if (n & 1) p = multmodp(k_x2n_table[k & 31], p);
            n >>= 1;
            k++;
        }
        return p;
    }
    // This uses the pre-computed g.shift value most of the time. Only the last
    // combination requires a new x2nmodp() calculation.
    check_sum_t crc32_comb(check_sum_t crc1, check_sum_t crc2, size_t len2) {
        return multmodp(len2 == config.chunk_size ? config.precompute_shift : x2nmodp(len2, 3), crc1) ^ crc2;
    }

    check_sum_t caculate_check_sum(check_sum_t check, const std::uint8_t* data, std::size_t dataLen) {
        switch (config.check_sum_type) {
        case DeflateCheckSumType::ADLER32_CHECK_T:
            return static_cast<check_sum_t>(adler32(static_cast<unsigned long>(check), data, static_cast<uInt>(dataLen)));
        case DeflateCheckSumType::CRC32_CHECK_T:
            return static_cast<check_sum_t>(crc32(static_cast<unsigned long>(check), data, static_cast<uInt>(dataLen)));
        default: return 0;
        }
    }

    check_sum_t combine_check_sum(check_sum_t check1, check_sum_t check2, std::size_t dataLen2) {
        switch (config.check_sum_type) {
        case DeflateCheckSumType::ADLER32_CHECK_T: return adler32_comb(check1, check2, dataLen2);
        case DeflateCheckSumType::CRC32_CHECK_T: return crc32_comb(check1, check2, dataLen2);
        default: return 0;
        }
    }
    void process_deflate();
    void write_header() {
        switch (config.output_format) {
        case DeflateOutputFormat::ZLIB_DEFLATE_STREAM_FORMAT: {
            if (!check_outbuf(sizeof(std::uint16_t))) break;
            //  write zlib header
            int level = config.compress_level;
            std::uint32_t header =
                (0x78 << 8) + // deflate, 32K window
                (level >= 9                                     ? 3 << 6
                 : level == 1                                   ? 0 << 6
                 : level >= 6 || level == Z_DEFAULT_COMPRESSION ? 1 << 6
                                                                : 2 << 6); // optional compression level clue
            header += 31 - (header % 31);                                  // make it a multiple of 31

            // write 2 bytes with big endian
            std::uint16_t header_big_endian =
                Fundamental::host_value_convert<std::uint16_t, Fundamental::Endian::BigEndian>(
                    static_cast<std::uint16_t>(header));
            constexpr std::size_t kZlibHeaderSize = sizeof(std::uint16_t);
            std::memcpy(dst_buf + current_data_size, &header_big_endian, kZlibHeaderSize);
            current_data_size += kZlibHeaderSize;
        } break;

        default: break;
        }
    }

    void write_trailer() {
        switch (config.output_format) {
        case DeflateOutputFormat::ZLIB_DEFLATE_STREAM_FORMAT: {
            if (!check_outbuf(sizeof(check_sum_t))) break;
            // write 4 bytes with big endian
            check_sum_t trailer_big_endian =
                Fundamental::host_value_convert<check_sum_t, Fundamental::Endian::BigEndian>(final_check_sum);
            constexpr std::size_t kZlibTrailerSize = sizeof(check_sum_t);
            std::memcpy(dst_buf + current_data_size, &trailer_big_endian, kZlibTrailerSize);
            current_data_size += kZlibTrailerSize;
        } break;
        default: break;
        }
    }
    bool check_outbuf(std::size_t need_len) {
        if (need_len + current_data_size > output_max_len) {
            bool expected_status = false;
            if (has_any_error.compare_exchange_strong(expected_status, true, std::memory_order::memory_order_relaxed)) {
                last_error_code = Z_BUF_ERROR;
            }
            return false;
        }
        return true;
    }
    //
    const std::uint8_t* const src_buf;
    std::size_t input_len;
    std::uint8_t* const dst_buf;
    std::size_t output_max_len = 0;
    const executor_t& executor;
    const DeflateConfig& config;

    // runtime data
    std::atomic_bool has_any_error = false;
    std::int32_t last_error_code   = Z_OK;
    check_sum_t final_check_sum    = 0;
    std::size_t current_data_size  = 0;
};

template <typename executor_t>
inline void ParrallelDeflateContext<executor_t>::process_deflate() {
    write_header();
    struct defalate_finish_status {
        std::size_t input_len = 0;
        check_sum_t check     = 0;
        std::vector<std::uint8_t> out_buf;
    };
    final_check_sum = config.GetDefaultCheckCode();
    std::vector<defalate_finish_status> finish_cache;
    Fundamental::ParallelRunEventsHandler parallel_handler;
    parallel_handler.notify_parallel_groups = [&](std::size_t nums) { finish_cache.resize(nums); };
    parallel_handler.notify_subtask_joined  = [&](std::size_t index) {
        defalate_finish_status task_item = std::move(finish_cache[index]);
        if (has_any_error.load()) return;
        if (!check_outbuf(task_item.out_buf.size())) return;
        final_check_sum = combine_check_sum(final_check_sum, task_item.check, task_item.input_len);
        std::memcpy(dst_buf + current_data_size, task_item.out_buf.data(), task_item.out_buf.size());
        current_data_size += task_item.out_buf.size();
    };
    auto task_process_func = [&](std::size_t start_offet, std::size_t len, std::size_t index) {
        if (has_any_error) return;
        z_stream strm;
        defalate_finish_status& sub_task_status = finish_cache[index];
        std::int32_t finish_code                = Z_OK;
        do {

            sub_task_status.check     = config.GetDefaultCheckCode();
            sub_task_status.input_len = len;

            strm.zalloc = Z_NULL;
            strm.zfree  = Z_NULL;
            strm.opaque = Z_NULL;
            finish_code = deflateInit2(&strm, config.compress_level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
            if (finish_code != Z_OK) break;
            deflateReset(&strm);
            strm.next_in  = const_cast<std::uint8_t*>(src_buf) + start_offet;
            strm.avail_in = static_cast<decltype(strm.avail_in)>(sub_task_status.input_len);

            sub_task_status.out_buf.resize(config.GuessCompressLen(sub_task_status.input_len));
            strm.avail_out = static_cast<decltype(strm.avail_out)>(sub_task_status.out_buf.size());
            strm.next_out  = sub_task_status.out_buf.data();
            // generate check
            sub_task_status.check =
                caculate_check_sum(sub_task_status.check, src_buf + start_offet, sub_task_status.input_len);
            if (index + 1 < finish_cache.size()) {
                finish_code = deflate(&strm, Z_SYNC_FLUSH);
            } else {
                auto code = deflate(&strm, Z_FINISH);
                if (code != Z_STREAM_END) {
                    finish_code = Z_ERRNO;
                }
            }
            sub_task_status.out_buf.resize(sub_task_status.out_buf.size() - strm.avail_out);
        } while (0);
        deflateEnd(&strm);
        if (finish_code != Z_OK) {
            bool expected_status = false;
            if (has_any_error.compare_exchange_strong(expected_status, true, std::memory_order::memory_order_relaxed)) {
                last_error_code = finish_code;
            }
        }
    };
    Fundamental::ParallelRun(static_cast<std::size_t>(0), input_len, task_process_func, config.chunk_size, executor,
                             parallel_handler);
    write_trailer();
}

} // namespace deflate_internal
template <typename executor_t>
inline std::tuple<bool, check_sum_t> ZUtils::ParallelDeflateBinary(const void* srcBuf,
                                                                   std::size_t inputLen,
                                                                   void* dstBuf,
                                                                   std::size_t* outputLen,
                                                                   const executor_t& executor,
                                                                   DeflateConfig config) {
    s_last_code = Z_ERRNO;
    do {
        if (!outputLen || !dstBuf) break;
        auto need_len = config.GuessCompressLen(inputLen);
        if (need_len > *outputLen) break;
        config.CorrectParams();
        // precompute
        config.precompute_shift = deflate_internal::ParrallelDeflateContext<executor_t>::x2nmodp(config.chunk_size, 3);
        deflate_internal::ParrallelDeflateContext<executor_t> context(srcBuf, inputLen, dstBuf, outputLen, executor,
                                                                      config);
        try {
            *outputLen = 0;
            context.process_deflate();
            *outputLen = context.current_data_size;
        } catch (...) {
            break;
        }
        if (context.has_any_error) {
            s_last_code = context.last_error_code;
        }
        return std::make_tuple(!context.has_any_error.load(), context.final_check_sum);
    } while (0);

    return std::make_tuple(false, 0);
}

struct EntryCompressInfo {
    constexpr static std::uint16_t kBasicDecompressNeedVersion = 0x14;
    constexpr static std::uint32_t kDefaultFilePermission      = static_cast<std::uint32_t>((0100644 << 16) | 0x20);
    // zlib version num
    std::uint16_t compressZipVersion = kBasicDecompressNeedVersion;
    // 8 means deflate 0 means no compress
    std::uint16_t compressType = 8;
    //
    bool useSpecifiedTimeStamp = false;
    // high 16 bit unix permission low 16 bit dos type
    std::uint32_t permissions = kDefaultFilePermission;
    EntryCompressInfo();
};

struct ZipFileEntry;
using ZipCustomWriteFunction = std::function<void(const std::uint8_t*, std::uint32_t)>;

class ZipWriter {
    struct ZipFileEntry {
        std::string path;
        std::uint32_t srcCRC32;
        std::uint64_t length;
        std::uint64_t rawLength;

        std::uint64_t offset;
        std::uint32_t modifyDate;
        EntryCompressInfo compressInfo;
    };
    constexpr static std::uint32_t ZIP_DIR_HEADER_SIG = 0x04034b50;

public:
    ZipWriter(const ZipCustomWriteFunction& flushCB = nullptr);
    ~ZipWriter();
    // disable move and copy
    ZipWriter(ZipWriter&&)                 = delete;
    ZipWriter(const ZipWriter&)            = delete;
    ZipWriter& operator=(ZipWriter&&)      = delete;
    ZipWriter& operator=(const ZipWriter&) = delete;

    void SetZipCustomWriteFunction(const ZipCustomWriteFunction& flushCB);
    // add a zip entry to list
    void AddFile(std::string fileName,
                 const std::uint8_t* data,
                 std::uint64_t dataLen,
                 std::uint64_t rawDataLen,
                 std::uint32_t srcCrc,
                 EntryCompressInfo info     = EntryCompressInfo {},
                 std::uint32_t timestampSec = 0);

    // you can call this function to force flush cache buffer
    // when you  had set ZipCustomWriteFunction,this function will be called
    // after this function is called,cache data will be clear;
    void Flush();
    // get final zip data
    [[nodiscard]] std::vector<std::uint8_t>& Filnalize();
    // reset the status,release all  memory
    void Reset();

private:
    void WriteEntryRecords();
    void WriteEntryInfo(const ZipFileEntry& entry);
    template <typename NumberType>
    void Write(NumberType number);
    void Write(const std::uint8_t* data, std::uint64_t dataSize);
    void EnlargeCache(std::size_t needSpace);
    void AllocateSpace(std::string& fileName, std::uint32_t dataLen);

private:
    static unsigned long time2dos(time_t t = 0) {

        unsigned long dos;

        if (t == 0) t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        struct tm timeNow;
        struct tm* tm = &timeNow;
#ifdef _WIN32
        ::localtime_s(tm, &t);
#else
        ::localtime_r(&t, tm);
#endif
        if (tm->tm_year < 80 || tm->tm_year > 207) return 0;
        dos = (unsigned long)(tm->tm_year - 80) << 25;
        dos += (unsigned long)(tm->tm_mon + 1) << 21;
        dos += (unsigned long)tm->tm_mday << 16;
        dos += (unsigned long)tm->tm_hour << 11;
        dos += (unsigned long)tm->tm_min << 5;
        dos += (unsigned long)(tm->tm_sec + 1) >> 1; // round to even seconds
        return dos;
    }

private:
    bool has_write_final_records = false;
    std::list<ZipFileEntry> m_entrylist;
    std::uint64_t m_fileOffset         = 0;
    std::size_t m_cacheSize            = 0;
    ZipCustomWriteFunction m_writeFunc = nullptr;
    std::vector<std::uint8_t> m_cacheData;
};
template <typename NumberType>
inline void ZipWriter::Write(NumberType number) {
    NumberType little_endian_value =
        Fundamental::host_value_convert<NumberType, Fundamental::Endian::LittleEndian>(number);
    Write((const std::uint8_t*)&little_endian_value, sizeof(little_endian_value));
}

ZipWriter::ZipWriter(const ZipCustomWriteFunction& flushCB) : m_writeFunc(flushCB) {
}

ZipWriter::~ZipWriter() {
    Reset();
}

void ZipWriter::SetZipCustomWriteFunction(const ZipCustomWriteFunction& flushCB) {
    m_writeFunc = flushCB;
}

void ZipWriter::AddFile(std::string fileName,
                        const std::uint8_t* data,
                        std::uint64_t dataLen,
                        std::uint64_t rawDataLen,
                        std::uint32_t crc,
                        EntryCompressInfo info,
                        std::uint32_t timeStampSec) {
    if (has_write_final_records) throw std::runtime_error("zip has been finalized");
    // Convert the date-time to a zip file timestamp (2-second resolution).
    std::uint32_t modifyDate = timeStampSec;
    if (!info.useSpecifiedTimeStamp) modifyDate = time2dos();
    std::uint64_t nowfileOffset = m_fileOffset;

    ZipFileEntry entry;
    entry.path         = fileName;
    entry.offset       = nowfileOffset;
    entry.length       = dataLen;
    entry.modifyDate   = modifyDate;
    entry.srcCRC32     = crc;
    entry.rawLength    = rawDataLen;
    entry.compressInfo = info;
    m_entrylist.emplace_back(std::move(entry));
    AllocateSpace(fileName, static_cast<std::uint32_t>(dataLen));

    // local file header filed
    Write(ZIP_DIR_HEADER_SIG); // 4
    // decompress version
    Write(EntryCompressInfo::kBasicDecompressNeedVersion); // 6
    // normal mark
    Write((std::uint16_t)0x0000); // 8
    // write compress type
    Write((std::uint16_t)info.compressType); // 10
    // modify date
    Write((std::uint32_t)modifyDate); // 14
    // crc32 for uncompress data
    Write((std::uint32_t)crc); // 18
    // compress len
    Write((std::uint32_t)dataLen); // 22
    // decompress len
    Write((std::uint32_t)rawDataLen); // 26
    // file name len
    Write((std::uint16_t)fileName.length()); // 28
    // extern field size
    Write((std::uint16_t)0x00); // 30

    Write((std::uint8_t*)fileName.c_str(), (std::uint64_t)fileName.length());
    // TODO add zip64 for large file
    //  write actual data
    Write(data, dataLen);
    Flush();
}

void ZipWriter::Flush() {
    if (m_writeFunc && m_cacheSize > 0) {
        m_writeFunc(m_cacheData.data(), static_cast<std::uint32_t>(m_cacheSize));
        m_cacheSize = 0;
    }
}

std::vector<std::uint8_t>& ZipWriter::Filnalize() {
    if (!has_write_final_records) {
        WriteEntryRecords();
        m_cacheData.resize(m_cacheSize);
        has_write_final_records = true;
    }

    return m_cacheData;
}

void ZipWriter::Reset() {
    m_entrylist.clear();
    m_fileOffset = 0;
    m_cacheData.clear();
    m_cacheData.shrink_to_fit();
    m_cacheSize             = 0;
    has_write_final_records = false;
}

void ZipWriter::WriteEntryInfo(const ZipFileEntry& entry) {
    constexpr static std::uint32_t header = 0x02014b50;
    Write(header);
    Write((std::uint16_t)entry.compressInfo.compressZipVersion);
    Write(EntryCompressInfo::kBasicDecompressNeedVersion);
    Write((std::uint16_t)0x00);
    Write((std::uint16_t)entry.compressInfo.compressType);
    Write(entry.modifyDate);
    Write(entry.srcCRC32);

    // compress len
    Write((std::uint32_t)entry.length);
    // decompress len
    Write((std::uint32_t)entry.rawLength);

    Write((std::uint16_t)entry.path.length());
    // extern field len
    Write((std::uint16_t)0x00);
    // comment filed len
    Write((std::uint16_t)0x00);
    // disk num
    Write((std::uint16_t)0x00);
    // file internal property
    Write((std::uint16_t)0x00);
    // file external property
    Write(entry.compressInfo.permissions);
    Write((std::uint32_t)entry.offset);
    Write((std::uint8_t*)entry.path.c_str(), entry.path.length());

    // TODO add zip64 for large file
}
void ZipWriter::Write(const uint8_t* data, std::uint64_t dataSize) {
    EnlargeCache(static_cast<std::size_t>(dataSize));
    // append to end
    std::memcpy(m_cacheData.data() + m_cacheSize, data, static_cast<std::size_t>(dataSize));
    m_fileOffset += dataSize;
    m_cacheSize += static_cast<std::size_t>(dataSize);
}
void ZipWriter::EnlargeCache(std::size_t needSpace) {
    auto target_size = m_cacheSize + needSpace;
    if (target_size > m_cacheData.size()) {
        m_cacheData.resize(target_size);
    }
}

void ZipWriter::WriteEntryRecords() {
    std::uint64_t nowfileOffset = m_fileOffset;
    for (auto& entry : m_entrylist) {
        WriteEntryInfo(entry);
    }
    std::uint64_t endfileOffset = m_fileOffset;

    std::uint64_t DirectorySizeInBytes    = endfileOffset - nowfileOffset;
    constexpr static std::uint32_t header = 0x06054b50;
    Write(header);
    // disk num
    Write((std::uint16_t)0x00);
    // disk num central start
    Write((std::uint16_t)0x00);
    // entry nums
    Write((std::uint16_t)m_entrylist.size());
    // disk num central start
    Write((std::uint16_t)m_entrylist.size());
    // record entry size
    Write((std::uint32_t)DirectorySizeInBytes);
    // record entry start
    Write((std::uint32_t)nowfileOffset);
    // comment len
    Write((std::uint16_t)0x00);
    // TODO add zip64 for large file
    Flush();
}

void ZipWriter::AllocateSpace(std::string& fileName, std::uint32_t dataLen) {
    std::uint32_t totalSize = 0;
    // AddFile
    totalSize +=
        sizeof(std::uint32_t) * 5 + sizeof(std::uint16_t) * 5 + static_cast<std::uint32_t>(fileName.size()) + dataLen;
    // WriteEntryInfo
    for (auto& entry : m_entrylist) {
        totalSize += sizeof(std::uint32_t) * 5 + sizeof(std::uint16_t) * 9;
        totalSize += sizeof(entry.modifyDate) + sizeof(entry.srcCRC32);
        totalSize += static_cast<std::uint32_t>(entry.path.length());
    }
    // WriteEntryRecords
    totalSize += sizeof(std::uint32_t) * 3 + sizeof(std::uint16_t) * 5;
    EnlargeCache(totalSize);
}

EntryCompressInfo::EntryCompressInfo() :
compressZipVersion(kBasicDecompressNeedVersion), compressType(8), useSpecifiedTimeStamp(false) {
}
} // namespace Fundamental
