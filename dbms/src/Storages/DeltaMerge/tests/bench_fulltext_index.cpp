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

#include <Common/PerfControl.h>
#include <Storages/DeltaMerge/Index/FullTextIndex/Stream/InputStream.h>
#include <Storages/DeltaMerge/Index/FullTextIndex/Writer.h>
#include <Storages/DeltaMerge/tests/bench_fulltext_index_utils.h>
#include <TestUtils/TiFlashTestEnv.h>
#include <gtest/gtest.h>

namespace DB::DM::bench
{

static void FullTextIndexBuild(::benchmark::State & state)
try
{
    const auto & dataset = DatasetArtCraftsAndSewing::get();

    tests::FullTextIndexDMFileTestBaseDummy test;
    auto guard = test.overrideTestName("FullTextIndexBuild");
    test.SetUp();

    for (auto _ : state)
    {
        state.PauseTiming();
        test.writeDMFile(dataset.getData());
        test.dm_file = test.restoreDMFile();
        state.ResumeTiming();
        test.buildIndex(TiDB::FullTextIndexDefinition{
            .parser_type = "STANDARD_V1",
        });
    }
}
CATCH

BENCHMARK(FullTextIndexBuild);


class FullTextIndexFixture : public benchmark::Fixture
{
protected:
    std::optional<tests::FullTextIndexDMFileTestBaseDummy> test;
    BitmapFilterPtr bitmap_filter = nullptr;

public:
    void SetUp(const ::benchmark::State &) override
    {
        if (test.has_value())
            return;

        std::cerr << "Loading dataset..." << std::endl;
        const auto & dataset = DatasetArtCraftsAndSewing::get();
        std::cerr << "Load finished, got " << dataset.getData().size() << " rows." << std::endl;

        test.emplace();
        test->test_no_fts_column = true;
        test->enable_column_cache = true;

        auto guard = test->overrideTestName("FullTextRankTop10");
        test->SetUp();
        std::cerr << "Writing DMFile..." << std::endl;
        test->writeDMFile(dataset.getData());
        test->dm_file = test->restoreDMFile();
        std::cerr << "Building index..." << std::endl;
        test->dm_file = test->buildIndex(TiDB::FullTextIndexDefinition{
            .parser_type = "STANDARD_V1",
        });

        bitmap_filter = std::make_shared<BitmapFilter>(dataset.getData().size(), 1);

        std::cerr << "Prepare finished." << std::endl;
    }
};

BENCHMARK_F(FullTextIndexFixture, FullTextRankTop10)(benchmark::State & state)
try
{
    PerfControl::enable();
    SCOPE_EXIT({ PerfControl::disable(); });

    for (auto _ : state)
    {
        auto stream = test->searchFromDMFile( //
            {.query = "sewing machine", .top_k = 10},
            bitmap_filter);
        auto block = stream->read();
        RUNTIME_CHECK(block);
        RUNTIME_CHECK(block.rows() == 10);
        RUNTIME_CHECK(!stream->read());
    }
}
CATCH


} // namespace DB::DM::bench
