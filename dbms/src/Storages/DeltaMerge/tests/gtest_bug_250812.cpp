// Copyright 2025 PingCAP, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <Columns/ColumnArray.h>
#include <Columns/ColumnNullable.h>
#include <Columns/ColumnsNumber.h>
#include <Common/Checksum.h>
#include <Core/ColumnWithTypeAndName.h>
#include <IO/Buffer/ReadBufferFromFile.h>
#include <IO/Buffer/WriteBufferFromFile.h>
#include <IO/Compression/CompressedReadBuffer.h>
#include <IO/Compression/CompressedReadBufferFromFile.h>
#include <IO/Compression/CompressedWriteBuffer.h>
#include <IO/Compression/CompressionMethod.h>
#include <IO/FileProvider/ChecksumWriteBufferBuilder.h>
#include <IO/FileProvider/WriteBufferFromWritableFileBuilder.h>
#include <Interpreters/Context.h>
#include <Storages/DeltaMerge/DMChecksumConfig.h>
#include <Storages/DeltaMerge/DMContext.h>
#include <Storages/DeltaMerge/File/DMFileBlockInputStream.h>
#include <Storages/DeltaMerge/File/DMFileBlockOutputStream.h>
#include <Storages/DeltaMerge/RowKeyRange.h>
#include <Storages/DeltaMerge/ScanContext.h>
#include <Storages/DeltaMerge/tests/DMTestEnv.h>
#include <TestUtils/FunctionTestUtils.h>
#include <TestUtils/InputStreamTestUtils.h>
#include <TestUtils/TiFlashStorageTestBasic.h>
#include <gtest/gtest.h>

namespace DB::DM::tests
{

class DMFileBug250812Test : public DB::base::TiFlashStorageTestBasic
{
public:
    DMFileBug250812Test() = default;

protected:
    void SetUp() override
    {
        TiFlashStorageTestBasic::SetUp();

        // Configure compression and checksum settings
        auto & settings = db_context->getSettingsRef();
        settings.dt_compression_method = CompressionMethod::LZ4;
        settings.dt_checksum_algorithm = ChecksumAlgo::XXH3;

        // Create DMFile with explicit checksum configuration
        auto checksum_config = DMChecksumConfig(
            {}, // embedded_checksum
            TIFLASH_DEFAULT_CHECKSUM_FRAME_SIZE, // checksum_frame_length
            ChecksumAlgo::XXH3, // checksum_algorithm
            {} // debug_info (will use defaults)
        );

        dm_file = DMFile::create(1, getTemporaryPath(), std::make_optional<DMChecksumConfig>(checksum_config));
    }

    void TearDown() override
    {
        dm_file.reset();
        if (dmf2)
            dmf2.reset();
        TiFlashStorageTestBasic::TearDown();
    }

    Context & dbContext() { return *db_context; }

protected:
    DMFilePtr dm_file;
    DMFilePtr dmf2;
};

TEST_F(DMFileBug250812Test, WriteReadNullableVectorColumn)
try
{
    // Verify that compression and checksum settings are configured correctly
    ASSERT_EQ(dbContext().getSettingsRef().dt_compression_method.get(), CompressionMethod::LZ4);
    ASSERT_EQ(dbContext().getSettingsRef().dt_checksum_algorithm.get(), ChecksumAlgo::XXH3);
    ASSERT_TRUE(dm_file->getConfiguration().has_value());
    ASSERT_EQ(dm_file->getConfiguration()->getChecksumAlgorithm(), ChecksumAlgo::XXH3);


    // Define columns: ID (Int64, not null) and embedding (Array(Float32), nullable)
    auto cols = std::make_shared<ColumnDefines>();

    // ID column (not null)
    ColumnDefine id_cd(1, "id", std::make_shared<DataTypeInt64>());
    cols->emplace_back(id_cd);

    // Embedding column (nullable vector with 1536 dimensions)
    ColumnDefine embedding_cd(
        2,
        "embedding",
        std::make_shared<DataTypeNullable>(std::make_shared<DataTypeArray>(std::make_shared<DataTypeFloat32>())));
    cols->emplace_back(embedding_cd);

    const size_t total_rows = 1000;
    const size_t filled_embedding_rows = 100;
    const size_t embedding_dimensions = 1536;

    // Prepare data in batches to avoid memory issues
    const size_t batch_size = 50;
    const size_t num_batches = total_rows / batch_size;

    {
        auto stream = std::make_shared<DMFileBlockOutputStream>(dbContext(), dm_file, *cols);
        stream->writePrefix();

        for (size_t batch = 0; batch < num_batches; ++batch)
        {
            size_t batch_start = batch * batch_size;
            size_t batch_end = (batch + 1) * batch_size;

            Block block;

            // Create ID column data (fully filled)
            std::vector<Int64> id_data;
            id_data.reserve(batch_size);
            for (size_t i = batch_start; i < batch_end; ++i)
            {
                id_data.push_back(static_cast<Int64>(i));
            }
            block.insert(DB::tests::createColumn<Int64>(id_data, id_cd.name, id_cd.id));

            // Create embedding column data (nullable, only first 100 rows filled)
            // Use a simpler approach similar to existing tests
            std::vector<std::optional<Array>> embedding_data;
            embedding_data.reserve(batch_size);

            for (size_t i = batch_start; i < batch_end; ++i)
            {
                if (i < filled_embedding_rows)
                {
                    // Create a 1536-dimension vector with values based on row index
                    Array vec;
                    vec.reserve(embedding_dimensions);
                    for (size_t dim = 0; dim < embedding_dimensions; ++dim)
                    {
                        // Use a simple pattern: row_id + dimension_index * 0.001
                        vec.push_back(static_cast<Float64>(i + dim * 0.001));
                    }
                    embedding_data.push_back(vec);
                }
                else
                {
                    // Null value for rows beyond the first 100
                    embedding_data.push_back(std::nullopt);
                }
            }

            // Create nullable array column using the tuple approach
            auto embedding_col = DB::tests::createColumn<Nullable<Array>>(
                std::make_tuple(std::make_shared<DataTypeFloat32>()),
                embedding_data);
            embedding_col.name = embedding_cd.name;
            embedding_col.column_id = embedding_cd.id;
            block.insert(embedding_col);

            stream->write(block, DMFileBlockOutputStream::BlockProperty{0, 0, 0, 0});
        }

        stream->writeSuffix();
    }

    // Read back and verify the data
    {
        DMFileBlockInputStreamBuilder builder(dbContext());
        builder.setRowsThreshold(100);
        auto stream = builder.build(
            dm_file,
            *cols,
            RowKeyRanges{RowKeyRange::newAll(false, 1)},
            std::make_shared<ScanContext>());

        size_t total_rows_read = 0;
        size_t non_null_embedding_count = 0;
        size_t null_embedding_count = 0;

        while (Block block = stream->read())
        {
            if (!block)
                break;

            ASSERT_EQ(block.columns(), 2);

            auto id_column = block.getByName("id").column;
            auto embedding_column = block.getByName("embedding").column;

            // Verify column types
            ASSERT_TRUE(id_column->isNumeric());
            ASSERT_TRUE(embedding_column->isColumnNullable());

            const auto * nullable_embedding = static_cast<const ColumnNullable *>(embedding_column.get());
            auto nested_column = nullable_embedding->getNestedColumnPtr();
            auto null_map = nullable_embedding->getNullMapColumnPtr();

            // Check if nested column is array type
            ASSERT_TRUE(typeid_cast<const ColumnArray *>(nested_column.get()) != nullptr);

            for (size_t i = 0; i < block.rows(); ++i)
            {
                // Verify ID values
                Int64 id_value = id_column->getInt(i);
                ASSERT_EQ(id_value, static_cast<Int64>(total_rows_read + i));

                // Verify embedding values
                bool is_null = null_map->getUInt(i) != 0;
                if (total_rows_read + i < filled_embedding_rows)
                {
                    // Should not be null for first 100 rows
                    ASSERT_FALSE(is_null) << "Row " << (total_rows_read + i) << " should not be null";

                    if (!is_null)
                    {
                        const auto * array_column = static_cast<const ColumnArray *>(nested_column.get());

                        // Verify array size (should be 1536 dimensions)
                        size_t array_size = array_column->sizeAt(non_null_embedding_count);
                        ASSERT_EQ(array_size, embedding_dimensions)
                            << "Row " << (total_rows_read + i) << " has wrong embedding dimension";

                        non_null_embedding_count++;
                    }
                }
                else
                {
                    // Should be null for rows beyond first 100
                    ASSERT_TRUE(is_null) << "Row " << (total_rows_read + i) << " should be null";
                    null_embedding_count++;
                }
            }

            total_rows_read += block.rows();
        }

        // Final verification for first DMFile
        ASSERT_EQ(total_rows_read, total_rows);
        ASSERT_EQ(non_null_embedding_count, filled_embedding_rows);
        ASSERT_EQ(null_embedding_count, total_rows - filled_embedding_rows);

        std::cout << "Successfully verified first DMFile with:" << std::endl;
        std::cout << "  Total rows: " << total_rows_read << std::endl;
        std::cout << "  Non-null embeddings: " << non_null_embedding_count << std::endl;
        std::cout << "  Null embeddings: " << null_embedding_count << std::endl;
    }
}
CATCH


TEST_F(DMFileBug250812Test, CompressedSeekableReaderBufferTest)
try
{
    // Create a temporary file for testing
    const std::string temp_file_path = "/tmp/tiflash_compressed_seek_test.dat";
    // Test data - create multiple blocks with different patterns
    std::vector<std::string> test_blocks;

    test_blocks = {
        std::string(1500, 'A') + "BLOCK0_END",
        std::string(800, 'B') + "BLOCK1_END",
        "", // Block 2 is empty
        "", // Block 3 is empty
    };

    std::vector<size_t> block_compressed_offsets;
    std::vector<size_t> block_decompressed_sizes;

    // Write compressed data to file
    {
        auto plain_file = ChecksumWriteBufferBuilder::build(
            true,
            dbContext().getFileProvider(),
            temp_file_path,
            EncryptionPath(temp_file_path, temp_file_path),
            false,
            dbContext().getWriteLimiter(),
            dbContext().getSettingsRef().dt_checksum_algorithm.get(),
            dbContext().getSettingsRef().dt_checksum_frame_size.get(),
            /*flags*/
            -1,
            /*mode*/ 0666,
            1048576);
        auto compressed_buf
            = CompressedWriteBuffer<>::build(*plain_file, CompressionSettings(CompressionMethod::LZ4), false);

        for (const auto & block_data : test_blocks)
        {
            // Record the compressed file offset before writing this block
            block_compressed_offsets.push_back(plain_file->count());
            block_decompressed_sizes.push_back(block_data.size());

            // Write the block data
            compressed_buf->write(block_data.data(), block_data.size());
            compressed_buf->next(); // Force compression of this block
        }
    }

    std::cout << "Created compressed file with " << test_blocks.size() << " blocks" << std::endl;
    for (size_t i = 0; i < block_compressed_offsets.size(); ++i)
    {
        std::cout << "Block " << i << ": compressed_offset=" << block_compressed_offsets[i]
                  << ", decompressed_size=" << block_decompressed_sizes[i] << std::endl;
    }


    auto compressed_in = CompressedReadBufferFromFileBuilder::build(
        dbContext().getFileProvider(),
        temp_file_path,
        EncryptionPath(temp_file_path, temp_file_path),
        dbContext().getSettingsRef().dt_checksum_frame_size.get(),
        dbContext().getReadLimiter(),
        dbContext().getSettingsRef().dt_checksum_algorithm.get(),
        dbContext().getSettingsRef().dt_checksum_frame_size.get());

    // 1. Check seek + read
    for (size_t i = 0; i < test_blocks.size(); ++i)
    {
        // Seek to the start of each block
        compressed_in->seek(block_compressed_offsets[i], 0);

        // Read the data
        std::string read_data;
        read_data.resize(block_decompressed_sizes[i]);
        compressed_in->readBig(read_data.data(), block_decompressed_sizes[i]);

        // Verify the data matches
        ASSERT_EQ(read_data, test_blocks[i]) << "Block " << i << " data mismatch";
    }

    // 2. Seek to block 2 again
    {
        const size_t target_block = 2;
        compressed_in->seek(block_compressed_offsets[target_block], 0);
        std::string read_data;
        read_data.resize(100);
        compressed_in->readBig(read_data.data(), 0);
    }
}
CATCH

} // namespace DB::DM::tests
