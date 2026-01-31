#include "csv_utils.hpp"
#include <gtest/gtest.h>
#include <iostream>
using namespace Fundamental::csv;

TEST(csv, basic_test) {
    std::size_t data_cnt       = 100;
    std::string schema1        = "id";
    std::string schema2        = "name";
    std::string schema3        = "age";
    std::string invalid_schema = "invalid_s";
    std::string test_outfile_buf;
    {
        std::size_t current_finished_index = 0;
        // generate csv file data
        CSVFlushCallback cb = [&](std::string_view data, std::size_t start_index, std::size_t row) {
            EXPECT_EQ(current_finished_index, start_index + row);
            test_outfile_buf += data;
        };
        CSVWriter<3> writer(cb);
        ++current_finished_index;
        writer.write_row(schema1, schema2, schema3);
        EXPECT_EQ(writer.row_cnt(), 1);
        writer.flush_data();
        EXPECT_TRUE(!test_outfile_buf.empty());
        for (std::size_t i = 0; i < data_cnt; ++i) {
            ++current_finished_index;
            writer.write_row(i, std::string("name") + std::to_string(i), (i % 70) + 3);
            writer.flush_data();
        }
    }
    { // just read id
        CSVReader<1> reader("memory.csv", test_outfile_buf.data(), test_outfile_buf.data() + test_outfile_buf.size());
        reader.read_header(::io::ignore_extra_column, schema1);
        for (std::size_t i = 0; i < data_cnt; ++i) {
            std::size_t read_index = 0;
            EXPECT_TRUE(reader.read_row(read_index) && read_index == i);
        }
    }
    { // just read name
        CSVReader<1> reader("memory.csv", test_outfile_buf.data(), test_outfile_buf.data() + test_outfile_buf.size());
        reader.read_header(::io::ignore_extra_column, schema2);
        for (std::size_t i = 0; i < data_cnt; ++i) {
            std::string read_data;
            EXPECT_TRUE(reader.read_row(read_data) && read_data == (std::string("name") + std::to_string(i)));
        }
    }
    { // just read id with no existed schema
        {
            CSVReader<2> reader("memory.csv", test_outfile_buf.data(),
                                test_outfile_buf.data() + test_outfile_buf.size());
            EXPECT_ANY_THROW(reader.read_header(::io::ignore_extra_column, schema1, invalid_schema));
        }
        CSVReader<2> reader("memory.csv", test_outfile_buf.data(), test_outfile_buf.data() + test_outfile_buf.size());
        EXPECT_NO_THROW(
            reader.read_header(::io::ignore_extra_column | ::io::ignore_missing_column, schema1, invalid_schema));
        for (std::size_t i = 0; i < data_cnt; ++i) {
            std::size_t read_index = 0;
            std::string read_invalid_str;
            EXPECT_TRUE(reader.read_row(read_index, read_invalid_str) && read_index == i && read_invalid_str.empty());
        }
    }
    std::string copy_outfile_buf;
    {
        CSVFlushCallback cb = [&](std::string_view data, std::size_t start_index, std::size_t row) {
            copy_outfile_buf += data;
        };
        CSVReader<3> reader("memory.csv", test_outfile_buf.data(), test_outfile_buf.data() + test_outfile_buf.size());
        reader.read_header(::io::ignore_no_column, schema1, schema2, schema3);

        CSVWriter<3> writer(cb);
        writer.write_row(schema1, schema2, schema3);
        for (std::size_t i = 0; i < data_cnt; ++i) {
            std::string data1;
            std::string data2;
            std::string data3;
            EXPECT_TRUE(reader.read_row(data1, data2, data3));
            writer.write_row(std::move(data1), std::move(data2), std::move(data3));
        }
    }
    EXPECT_EQ(copy_outfile_buf, test_outfile_buf);
}

TEST(csv, write_cache_test) {
    { // row flush test
        CSVFlushCallback cb = [](std::string_view, std::size_t, std::size_t row) { EXPECT_EQ(row, 1); };
        CSVWriter<1> writer(cb, kDefaultCsvCacheBufLimitSize, 1);
        for (std::size_t i = 0; i < 10; ++i) {
            writer.write_row(i);
        }
    }
    { // buffer flush test
        std::size_t test_buffer_size = 2;
        CSVFlushCallback cb          = [&](std::string_view data, std::size_t, std::size_t row) {
            EXPECT_EQ(row, 1);
            EXPECT_GE(data.size(), test_buffer_size);
        };
        CSVWriter<1> writer(cb, test_buffer_size, 100000);
        for (std::size_t i = 0; i < 10; ++i) {
            writer.write_row(std::string(test_buffer_size, 'a'));
        }
    }
}

TEST(csv, batch_reader_test) {
    std::size_t batch_size = 10000;
    std::size_t total_size = 2 * batch_size + 1;
    std::string dst_data;
    CSVFlushCallback cb = [&](std::string_view data, std::size_t, std::size_t) { dst_data += data; };
    {
        CSVWriter<1> writer(cb);
        writer.write_row("id");
        for (std::size_t i = 0; i < total_size; ++i) {
            writer.write_row(i);
            writer.flush_data();
        }
    }
    {
        BatchCsvReaderWrap<std::size_t> batcher_reader("memory.csv", dst_data.data(),
                                                       dst_data.data() + dst_data.size());
        EXPECT_NO_THROW(batcher_reader.init_read_header("id"));
        {
            auto b1 = batcher_reader.read(batch_size);
            EXPECT_EQ(b1.size(), batch_size);
            b1.clear();
            EXPECT_EQ(batcher_reader.read(b1, batch_size), batch_size);
        }
        EXPECT_EQ(batcher_reader.read(batch_size).size(), 1);
    }
}

TEST(csv, memory_reader_test) {
    std::size_t batch_size = 1000000;
    std::size_t total_size = 2 * batch_size + 1;
    std::string dst_data;
    bool is_eof                 = false;
    std::size_t total_read_size = 0;
    std::unique_ptr<BatchCsvReaderWrap<std::size_t>> batcher_reader;

    bool has_init       = false;
    CSVFlushCallback cb = [&](std::string_view data, std::size_t, std::size_t) {
        dst_data += data;
        // remove last '\n'
        if (is_eof) dst_data.resize(dst_data.size() - 1);
        if (dst_data.size() >= MemoryStreamReader::kMinInitBufferSize || is_eof) {
            batcher_reader = std::make_unique<BatchCsvReaderWrap<std::size_t>>(
                "memory.csv", std::make_unique<MemoryStreamReader>(dst_data, is_eof));
        }
        if (!batcher_reader) return;
        if (!has_init) {
            EXPECT_NO_THROW(batcher_reader->init_read_header("id"));
            has_init = true;
        }
        {

            while (true) {
                auto b1 = batcher_reader->read(batch_size);
                if (b1.empty()) break;
                for (auto& item : b1) {
                    EXPECT_EQ(std::get<0>(item), total_read_size);
                    ++total_read_size;
                }
            }
        }
    };
    {
        CSVWriter<1> writer(cb);
        writer.write_row("id");
        for (std::size_t i = 0; i < total_size; ++i) {
            writer.write_row(i);
        }
        is_eof = true;
    }
    EXPECT_EQ(total_read_size, total_size);
}

TEST(csv, memory_reader_exception_test) {
    std::string dst_data;
    bool is_eof = false;
    std::unique_ptr<BatchCsvReaderWrap<std::string>> batcher_reader;
    std::string data_str(MemoryStreamReader::kMaxLineLength, 'a');
    bool has_init       = false;
    CSVFlushCallback cb = [&](std::string_view data, std::size_t, std::size_t) {
        dst_data += data;
        // remove last '\n'
        if (is_eof) dst_data.resize(dst_data.size() - 1);
        if (dst_data.size() >= MemoryStreamReader::kMinInitBufferSize || is_eof) {
            batcher_reader = std::make_unique<BatchCsvReaderWrap<std::string>>(
                "memory.csv", std::make_unique<MemoryStreamReader>(dst_data, is_eof));
        }
        if (!batcher_reader) return;
        if (!has_init) {
            EXPECT_NO_THROW(batcher_reader->init_read_header("id"));
            has_init = true;
        }
        { // read success
            auto b1 = batcher_reader->read(1);
            EXPECT_TRUE(b1.size() == 1 && std::get<0>(b1.front()) == data_str);
        }
        { // read exception
            EXPECT_ANY_THROW(batcher_reader->read(1));
        }
    };
    {
        CSVWriter<1> writer(cb);
        writer.write_row("id");
        for (std::size_t i = 0; i < 2; ++i) {
            writer.write_row(data_str + std::string(i, 'c'));
        }
        is_eof = true;
    }
}

TEST(csv, write_file_sink) {
    {
        // invalid use case
        Fundamental::csv::CsvWriterFileSink sink(true);
        EXPECT_ANY_THROW(sink.wrapper_flush_func()("1", 1, 1));
        EXPECT_FALSE(sink.Init("."));
        EXPECT_ANY_THROW(sink.wrapper_flush_func()("1", 1, 1));
    }
}
int main(int argc, char** argv) {

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}