// Copyright 2022 PingCAP, Ltd.
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

#include <Common/nocopyable.h>
#include <Flash/Coprocessor/DAGExpressionAnalyzer.h>
#include <Flash/Coprocessor/DAGPipeline.h>
#include <Flash/Coprocessor/FilterConditions.h>
#include <Flash/Coprocessor/RemoteRequest.h>
#include <Flash/Coprocessor/TiDBTableScan.h>
#include <Flash/Pipeline/Exec/PipelineExecBuilder.h>
#include <Storages/DeltaMerge/Remote/DisaggSnapshot_fwd.h>
#include <Storages/RegionQueryInfo.h>
#include <Storages/SelectQueryInfo.h>
#include <Storages/TableLockHolder.h>
#include <Storages/Transaction/LearnerRead.h>
#include <Storages/Transaction/RegionException.h>
#include <Storages/Transaction/TMTStorages.h>
#include <Storages/Transaction/Types.h>
#include <pingcap/coprocessor/Client.h>

#include <vector>

namespace DB
{
class TMTContext;
using TablesRegionInfoMap = std::unordered_map<Int64, std::reference_wrapper<const RegionInfoMap>>;
/// DAGStorageInterpreter encapsulates operations around storage during interprete stage.
/// It's only intended to be used by DAGQueryBlockInterpreter.
/// After DAGStorageInterpreter::execute some of its members will be transferred to DAGQueryBlockInterpreter.
class DAGStorageInterpreter
{
public:
    DAGStorageInterpreter(
        Context & context_,
        const TiDBTableScan & table_scan,
        const FilterConditions & filter_conditions_,
        size_t max_streams_);

    DISALLOW_MOVE(DAGStorageInterpreter);

    void execute(DAGPipeline & pipeline);

    /// Members will be transferred to DAGQueryBlockInterpreter after execute

    std::unique_ptr<DAGExpressionAnalyzer> analyzer;

private:
    struct StorageWithStructureLock
    {
        ManageableStoragePtr storage;
        TableStructureLockHolder lock;
    };
    LearnerReadSnapshot doCopLearnerRead();

    LearnerReadSnapshot doBatchCopLearnerRead();

    bool checkRetriableForBatchCopOrMPP(
        const TableID & table_id,
        const SelectQueryInfo & query_info,
        const RegionException & e,
        int num_allow_retry);
    DM::Remote::DisaggPhysicalTableReadSnapshotPtr
    buildLocalStreamsForPhysicalTable(
        const TableID & table_id,
        const SelectQueryInfo & query_info,
        DAGPipeline & pipeline,
        size_t max_block_size);
    void buildLocalStreams(DAGPipeline & pipeline, size_t max_block_size);

    std::unordered_map<TableID, StorageWithStructureLock> getAndLockStorages(Int64 query_schema_version);

    std::tuple<Names, NamesAndTypes, std::vector<ExtraCastAfterTSMode>> getColumnsForTableScan();

    std::vector<RemoteRequest> buildRemoteRequests(const DM::ScanContextPtr & scan_context);

    TableLockHolders releaseAlterLocks();

    std::unordered_map<TableID, SelectQueryInfo> generateSelectQueryInfos();

    DAGContext & dagContext() const;

    void recordProfileStreams(DAGPipeline & pipeline, const String & key);

    std::vector<pingcap::coprocessor::CopTask> buildCopTasks(const std::vector<RemoteRequest> & remote_requests);
    void buildRemoteStreams(const std::vector<RemoteRequest> & remote_requests, DAGPipeline & pipeline);

    void executeCastAfterTableScan(
        size_t remote_read_streams_start_index,
        DAGPipeline & pipeline);

    void prepare();

    void executeImpl(DAGPipeline & pipeline);

private:
    std::vector<ExtraCastAfterTSMode> is_need_add_cast_column;
    /// it shouldn't be hash map because duplicated region id may occur if merge regions to retry of dag.
    RegionRetryList region_retry_from_local_region;

    /// passed from caller, doesn't change during DAGStorageInterpreter's lifetime

    Context & context;
    const TiDBTableScan & table_scan;
    const FilterConditions & filter_conditions;
    const size_t max_streams;
    LoggerPtr log;

    /// derived from other members, doesn't change during DAGStorageInterpreter's lifetime

    const TableID logical_table_id;
    TMTContext & tmt;

    /// Intermediate variables shared by multiple member functions

    std::unique_ptr<MvccQueryInfo> mvcc_query_info;
    // We need to validate regions snapshot after getting streams from storage.
    LearnerReadSnapshot learner_read_snapshot;
    /// Table from where to read data, if not subquery.
    /// Hold read lock on both `alter_lock` and `drop_lock` until the local input streams are created.
    /// We need an immutable structure to build the TableScan operator and create snapshot input streams
    /// of storage. After the input streams created, the `alter_lock` can be released so that reading
    /// won't block DDL operations.
    std::unordered_map<TableID, StorageWithStructureLock> storages_with_structure_lock;
    ManageableStoragePtr storage_for_logical_table;
    Names required_columns;
    NamesAndTypes source_columns;
    // For generated column, just need a placeholder, and TiDB will fill this column.
    std::vector<std::tuple<UInt64, String, DataTypePtr>> generated_column_infos;
};

} // namespace DB
