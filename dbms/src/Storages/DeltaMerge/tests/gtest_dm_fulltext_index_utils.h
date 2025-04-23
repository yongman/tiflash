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

#pragma once

#include <Core/ColumnWithTypeAndName.h>
#include <Interpreters/Context.h>
#include <Interpreters/sortBlock.h>
#include <Storages/DeltaMerge/DMContext.h>
#include <Storages/DeltaMerge/File/DMFileBlockInputStream.h>
#include <Storages/DeltaMerge/File/DMFileBlockOutputStream.h>
#include <Storages/DeltaMerge/File/DMFileLocalIndexWriter.h>
#include <Storages/DeltaMerge/Index/FullTextIndex/Stream/Ctx.h>
#include <Storages/DeltaMerge/Index/FullTextIndex/Stream/InputStream.h>
#include <Storages/DeltaMerge/Index/LocalIndexInfo.h>
#include <Storages/DeltaMerge/ScanContext.h>
#include <Storages/DeltaMerge/StoragePool/StoragePool.h>
#include <Storages/DeltaMerge/tests/DMTestEnv.h>
#include <Storages/DeltaMerge/tests/gtest_dm_delta_merge_store_test_basic.h>
#include <Storages/DeltaMerge/tests/gtest_segment_util.h>
#include <Storages/PathPool.h>
#include <TestUtils/FunctionTestUtils.h>
#include <TestUtils/InputStreamTestUtils.h>
#include <TestUtils/TiFlashStorageTestBasic.h>
#include <TiDB/Decode/DatumCodec.h>
#include <TiDB/Schema/TiDB.h>


namespace DB::DM::tests
{

class FullTextIndexTestUtils
{
public:
    static constexpr ColumnID fts_column_id = 130;
    static constexpr const char * fts_column_name = "body";
    static constexpr IndexID fts_index_id = 42;

    static ColumnDefine cdFts()
    {
        return ColumnDefine(fts_column_id, fts_column_name, ::DB::tests::typeFromString("String"));
    }

    static ColumnWithTypeAndName colInt64(std::string_view sequence, const String & name = "", Int64 column_id = 0)
    {
        auto data = genSequence<Int64>(sequence);
        return ::DB::tests::createColumn<Int64>(data, name, column_id);
    }

    /// Create a column with values like "word_1", "word_2", "word_3", ...
    static ColumnWithTypeAndName colString(std::string_view sequence, const String & name = "", Int64 column_id = 0)
    {
        auto data = genSequence<Int64>(sequence);
        std::vector<String> column_data;
        column_data.reserve(data.size());
        for (auto & v : data)
            column_data.push_back(fmt::format("word_{}", v));
        return ::DB::tests::createColumn<String>(column_data, name, column_id);
    }

    struct FtsQueryInfoTopKOptions
    {
        String query;
        UInt32 top_k;
        Int64 column_id = 130; // fts_column_id
        Int64 index_id = 42; // fts_index_id
        String tokenizer = "STANDARD_V1";
    };

    static FTSQueryInfoPtr ftsQueryInfoTopK(FtsQueryInfoTopKOptions options)
    {
        auto fts_query_info = std::make_shared<tipb::FTSQueryInfo>();
        fts_query_info->set_query_type(tipb::FTSQueryType::FTSQueryTypeTopK);
        fts_query_info->set_index_id(options.index_id);
        auto * column_info = fts_query_info->add_columns();
        column_info->set_column_id(options.column_id);
        column_info->set_tp(TiDB::TP::TypeString);
        column_info->set_flag(TiDB::ColumnFlagNotNull);
        fts_query_info->set_top_k(options.top_k);
        fts_query_info->set_query_text(options.query);
        fts_query_info->set_query_tokenizer(options.tokenizer);
        return fts_query_info;
    }

    static LocalIndexInfosPtr indexInfo(
        TiDB::FullTextIndexDefinition definition = TiDB::FullTextIndexDefinition{
            .parser_type = "STANDARD_V1",
        })
    {
        const LocalIndexInfos index_infos = LocalIndexInfos{
            LocalIndexInfo(fts_index_id, fts_column_id, std::make_shared<TiDB::FullTextIndexDefinition>(definition)),
        };
        return std::make_shared<LocalIndexInfos>(index_infos);
    }

    static auto wrapFTSStream(
        const FullTextIndexStreamCtxPtr & ctx,
        const SkippableBlockInputStreamPtr & inner,
        const BitmapFilterPtr & filter)
    {
        auto stream = ConcatSkippableBlockInputStream<false>::create(
            /* inputs */ {inner},
            /* rows */ {filter->size()},
            /* ScanContext */ nullptr);
        return FullTextIndexInputStream::create(ctx, filter, stream);
    }
};


class FullTextIndexDMFileTestBase
    : public FullTextIndexTestUtils
    , public DB::base::TiFlashStorageTestBasic
{
public:
    void SetUp() override
    {
        TiFlashStorageTestBasic::SetUp();

        parent_path = TiFlashStorageTestBasic::getTemporaryPath();
        path_pool = std::make_shared<StoragePathPool>(db_context->getPathPool().withTable("test", "t1", false));
        storage_pool = std::make_shared<StoragePool>(*db_context, NullspaceID, /*ns_id*/ 100, *path_pool, "test.t1");
        auto delegator = path_pool->getStableDiskDelegator();
        auto paths = delegator.listPaths();
        RUNTIME_CHECK(paths.size() == 1);
        dm_file = DMFile::create(
            1,
            paths[0],
            std::make_optional<DMChecksumConfig>(),
            128 * 1024,
            16 * 1024 * 1024,
            DMFileFormat::V3);

        DB::tests::TiFlashTestEnv::disableS3Config();

        reload();
    }

    // Update dm_context.
    void reload()
    {
        TiFlashStorageTestBasic::reload();

        *path_pool = db_context->getPathPool().withTable("test", "t1", false);
        dm_context = DMContext::createUnique(
            *db_context,
            path_pool,
            storage_pool,
            /*min_version_*/ 0,
            NullspaceID,
            /*physical_table_id*/ 100,
            /*pk_col_id*/ enable_column_cache ? EXTRA_HANDLE_COLUMN_ID : 0,
            false,
            1,
            db_context->getSettingsRef());
    }

    DMFilePtr restoreDMFile()
    {
        auto dmfile_parent_path = dm_file->parentPath();
        auto dmfile = DMFile::restore(
            dbContext().getFileProvider(),
            dm_file->fileId(),
            dm_file->pageId(),
            dmfile_parent_path,
            DMFileMeta::ReadMode::all(),
            /* meta_version= */ 0);
        auto delegator = path_pool->getStableDiskDelegator();
        delegator.addDTFile(dm_file->fileId(), dmfile->getBytesOnDisk(), dmfile_parent_path);
        return dmfile;
    }

    Context & dbContext() { return *db_context; }

protected:
    std::unique_ptr<DMContext> dm_context;
    /// all these var live as ref in dm_context
    std::shared_ptr<StoragePathPool> path_pool;
    std::shared_ptr<StoragePool> storage_pool;

public:
    String parent_path;
    DMFilePtr dm_file = nullptr;

public:
    bool test_no_fts_column = false;
    bool enable_column_cache = false;

    static ColumnDefinesPtr getWriteColumns()
    {
        auto cols = DMTestEnv::getDefaultColumns(DMTestEnv::PkType::HiddenTiDBRowID, /*add_nullable*/ true);
        cols->emplace_back(cdFts());
        return cols;
    }

    void writeDMFile(const std::vector<String> & data)
    {
        const auto fts_cd = cdFts();
        Block block = DMTestEnv::prepareSimpleWriteBlockWithNullable(0, data.size());
        block.insert(createColumn<String>(data, fts_cd.name, fts_cd.id));
        auto stream = std::make_shared<DMFileBlockOutputStream>(dbContext(), dm_file, *getWriteColumns());
        stream->writePrefix();
        stream->write(block, DMFileBlockOutputStream::BlockProperty{0, 0, 0, 0});
        stream->writeSuffix();
    }

    DMFilePtr buildIndex(TiDB::FullTextIndexDefinition definition)
    {
        auto build_info = DMFileLocalIndexWriter::getLocalIndexBuildInfo(indexInfo(definition), {dm_file});
        DMFileLocalIndexWriter iw(DMFileLocalIndexWriter::Options{
            .path_pool = path_pool,
            .index_infos = build_info.indexes_to_build,
            .dm_files = {dm_file},
            .dm_context = *dm_context,
        });
        auto new_dmfiles = iw.build();
        assert(new_dmfiles.size() == 1);
        return new_dmfiles[0];
    }

    BlockInputStreamPtr searchFromDMFile(FtsQueryInfoTopKOptions options, BitmapFilterPtr bitmap_filter)
    {
        auto read_cols = std::make_shared<ColumnDefines>();
        read_cols->emplace_back(getExtraHandleColumnDefine(/* common_handle */ false));
        if (!test_no_fts_column)
            read_cols->emplace_back(cdFts());
        read_cols->emplace_back(FullTextIndexStreamCtx::VIRTUAL_SCORE_CD);

        DMFileBlockInputStreamBuilder builder(dbContext());

        auto fts_idx_ctx = FullTextIndexStreamCtx::createForStableOnlyTests(ftsQueryInfoTopK(options), read_cols);
        auto stream = builder.setFtsIndexQuery(fts_idx_ctx)
                          .enableColumnCacheLongTerm(enable_column_cache ? EXTRA_HANDLE_COLUMN_ID : 0)
                          .build(
                              dm_file,
                              *read_cols,
                              RowKeyRanges{RowKeyRange::newAll(false, 1)},
                              std::make_shared<ScanContext>());
        return FullTextIndexTestUtils::wrapFTSStream( //
            fts_idx_ctx,
            stream,
            bitmap_filter);
    }

    BlockInputStreamPtr searchFromDMFile(FtsQueryInfoTopKOptions options, std::initializer_list<UInt8> mvcc_bitmap_init)
    {
        return searchFromDMFile(options, std::make_shared<BitmapFilter>(mvcc_bitmap_init));
    }

    Block searchFromDMFileAndSort(FtsQueryInfoTopKOptions options, std::initializer_list<UInt8> mvcc_bitmap_init)
    {
        auto stream = searchFromDMFile(options, mvcc_bitmap_init);
        stream->readPrefix();
        auto block = stream->read();
        RUNTIME_CHECK(!stream->read());
        stream->readSuffix();

        if (block)
        {
            SortDescription sort;
            sort.emplace_back(FullTextIndexStreamCtx::VIRTUAL_SCORE_CD.name, -1, 0);
            sortBlock(block, sort);
        }

        return block;
    }

    ColumnsWithTypeAndName createColumnData(const ColumnsWithTypeAndName & columns) const
    {
        RUNTIME_CHECK(columns.size() == 2);
        ColumnsWithTypeAndName ret{};
        ret.emplace_back(columns[0]);
        if (!test_no_fts_column)
            ret.emplace_back(columns[1]);
        return ret;
    }

    Strings createColumnNames() const
    {
        Strings ret{};
        ret.emplace_back(DMTestEnv::pk_name);
        if (!test_no_fts_column)
            ret.emplace_back(fts_column_name);
        return ret;
    }
};

/// Empty TestBody. This class can be used in benchmark suites for SetUp and TearDown.
class FullTextIndexDMFileTestBaseDummy : public FullTextIndexDMFileTestBase
{
public:
    void TestBody() override {}
};

} // namespace DB::DM::tests
