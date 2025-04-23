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
#include <Columns/ColumnsNumber.h>
#include <Poco/File.h>
#include <Storages/DeltaMerge/dtpb/dmfile.pb.h>
#include <Storages/DeltaMerge/tests/gtest_dm_fulltext_index_utils.h>
#include <benchmark/benchmark.h>

#include <filesystem>
#include <fstream>


namespace DB::DM::bench
{

class Dataset
{
public:
    explicit Dataset(std::string_view file_name)
    {
        auto dataset_directory = std::filesystem::path(__FILE__).parent_path().string() + "/bench_dataset";
        auto dataset_path = fmt::format("{}/{}", dataset_directory, file_name);

        if (!std::filesystem::exists(dataset_path))
        {
            throw Exception(fmt::format(
                "Benchmark cannot run because dataset file {} not found. See {}/README.md for setup instructions.",
                dataset_path,
                dataset_directory));
        }

        Poco::JSON::Parser parser;
        std::ifstream file(dataset_path);
        std::string line;
        while (std::getline(file, line))
        {
            Poco::Dynamic::Var result = parser.parse(line);
            const auto & obj = result.extract<Poco::JSON::Object::Ptr>();
            auto name = obj->getValue<std::string>("text");
            data.emplace_back(name);
        }
    }

    virtual ~Dataset() = default;

public:
    std::vector<std::string> getData() const { return data; }

protected:
    std::vector<std::string> data;
};

class DatasetArtCraftsAndSewing : public Dataset
{
public:
    DatasetArtCraftsAndSewing()
        : Dataset("Arts_Crafts_and_Sewing.jsonl")
    {}

    static const DatasetArtCraftsAndSewing & get()
    {
        static DatasetArtCraftsAndSewing dataset;
        return dataset;
    }
};

} // namespace DB::DM::bench
