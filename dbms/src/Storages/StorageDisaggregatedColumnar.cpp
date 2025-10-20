// Copyright 2023 PingCAP, Inc.
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

#include <Common/Exception.h>
#include <Common/Stopwatch.h>
#include <Common/ThreadManager.h>
#include <Core/NamesAndTypes.h>
#include <DataStreams/AddExtraTableIDColumnTransformAction.h>
#include <DataStreams/IBlockInputStream.h>
#include <DataTypes/IDataType.h>
#include <Flash/Coprocessor/DAGContext.h>
#include <Flash/Coprocessor/DAGExpressionAnalyzer.h>
#include <Flash/Coprocessor/DAGPipeline.h>
#include <Flash/Coprocessor/DAGQueryInfo.h>
#include <Flash/Coprocessor/FilterConditions.h>
#include <Flash/Coprocessor/GenSchemaAndColumn.h>
#include <Flash/Coprocessor/InterpreterUtils.h>
#include <Flash/Coprocessor/RequestUtils.h>
#include <IO/IOThreadPools.h>
#include <Interpreters/Context.h>
#include <Interpreters/SharedContexts/Disagg.h>
#include <Storages/DeltaMerge/ScanContext.h>
#include <Storages/KVStore/KVStore.h>
#include <Storages/KVStore/TMTContext.h>
#include <Storages/SelectQueryInfo.h>
#include <Storages/StorageDisaggregated.h>
#include <Storages/StorageDisaggregatedColumnar.h>
#include <Storages/StorageDisaggregatedHelpers.h>
#include <TiDB/Schema/TiDB.h>
#include <kvproto/kvrpcpb.pb.h>
#include <pingcap/coprocessor/Client.h>
#include <pingcap/kv/Backoff.h>
#include <pingcap/kv/Cluster.h>
#include <pingcap/kv/RegionCache.h>
#include <tipb/executor.pb.h>
#include <tipb/select.pb.h>

namespace DB
{
namespace ErrorCodes
{
extern const int COLUMNAR_SNAPSHOT_ERROR;
} // namespace ErrorCodes

bool StorageDisaggregated::isReadColumnar()
{
    return context.getSharedContextDisagg()->use_columnar;
}

BlockInputStreams StorageDisaggregated::readThroughProxy(const Context & context, unsigned num_streams)
{
    DAGPipeline pipeline;
    const UInt64 start_ts = sender_target_mpp_task_id.gather_id.query_id.start_ts;
    auto [remote_table_ranges, region_num] = buildRemoteTableRanges();
    auto read_proxy_tasks = RNProxyReadTask::buildProxyReadTaskWithBackoff(
        log,
        context,
        start_ts,
        table_scan,
        filter_conditions,
        remote_table_ranges,
        num_streams);
    for (auto & task : read_proxy_tasks)
    {
        auto streams = task->getInputStreams();
        pipeline.streams.insert(pipeline.streams.end(), streams.begin(), streams.end());
    }
    NamesAndTypes source_columns;
    source_columns.reserve(table_scan.getColumnSize());
    const auto & stream_header = pipeline.firstStream()->getHeader();
    for (const auto & col : stream_header)
    {
        source_columns.emplace_back(col.name, col.type);
    }
    analyzer = std::make_unique<DAGExpressionAnalyzer>(std::move(source_columns), context);

    // Handle duration type column
    extraCast(*analyzer, pipeline);
    // Handle filter
    filterConditionsWithPushedDownFilters(*analyzer, pipeline);
    return pipeline.streams;
}


void StorageDisaggregated::readThroughProxy(
    PipelineExecutorContext & exec_context,
    PipelineExecGroupBuilder & group_builder,
    const Context & context,
    unsigned num_streams)
{
    const UInt64 start_ts = sender_target_mpp_task_id.gather_id.query_id.start_ts;
    auto [remote_table_ranges, region_num] = buildRemoteTableRanges();
    auto read_proxy_tasks = RNProxyReadTask::buildProxyReadTaskWithBackoff(
        log,
        context,
        start_ts,
        table_scan,
        filter_conditions,
        remote_table_ranges,
        num_streams);
    auto [column_defines, extra_table_id_index] = genColumnDefinesForDisaggregatedRead(table_scan);
    for (auto & task : read_proxy_tasks)
    {
        group_builder.addConcurrency(RNProxySourceOp::create({
            .context = context,
            .debug_tag = log->identifier(),
            .exec_context = exec_context,
            .columns_to_read = *column_defines,
            .task = task,
            .extra_table_id_index = extra_table_id_index,
        }));
    }

    NamesAndTypes source_columns;
    auto header = group_builder.getCurrentHeader();
    source_columns.reserve(header.columns());
    for (const auto & col : header)
        source_columns.emplace_back(col.name, col.type);
    analyzer = std::make_unique<DAGExpressionAnalyzer>(std::move(source_columns), context);

    // Handle duration type column
    extraCast(exec_context, group_builder, *analyzer);
    // Handle filter
    filterConditionsWithPushedDownFilters(exec_context, group_builder, *analyzer);
}

void StorageDisaggregated::filterConditionsWithPushedDownFilters(
    DAGExpressionAnalyzer & analyzer,
    DAGPipeline & pipeline)
{
    FilterConditions conditions(filter_conditions.executor_id, filter_conditions.conditions);
    conditions.conditions.MergeFrom(table_scan.getPushedDownFilters());
    if (conditions.hasValue())
    {
        ::DB::executePushedDownFilter(conditions, analyzer, log, pipeline);
        auto & profile_streams = context.getDAGContext()->getProfileStreamsMap()[conditions.executor_id];
        pipeline.transform([&profile_streams](auto & stream) { profile_streams.push_back(stream); });
    }
}

void StorageDisaggregated::filterConditionsWithPushedDownFilters(
    PipelineExecutorContext & exec_context,
    PipelineExecGroupBuilder & group_builder,
    DAGExpressionAnalyzer & analyzer)
{
    FilterConditions conditions(filter_conditions.executor_id, filter_conditions.conditions);
    conditions.conditions.MergeFrom(table_scan.getPushedDownFilters());
    if (conditions.hasValue())
    {
        ::DB::executePushedDownFilter(exec_context, group_builder, conditions, analyzer, log);
        context.getDAGContext()->addOperatorProfileInfos(conditions.executor_id, group_builder.getCurProfileInfos());
    }
}

// RNProxyReaderPtr
RNProxyReaderPtr RNProxyReader::createProxyReader(
    const LoggerPtr & log,
    const Context & context,
    RegionID region_id,
    RegionVersion region_ver,
    UInt64 region_conf_ver,
    const std::vector<std::tuple<TableID, pingcap::coprocessor::KeyRanges>> & physical_table_ranges,
    UInt64 start_ts,
    const TiDBTableScan & table_scan,
    const FilterConditions & filter_conditions,
    std::mutex & output_lock)
{
    auto table_scan_pb = table_scan.getTableScanPB();
    auto table_scan_data = table_scan_pb->SerializeAsString();
    BaseBuffView table_scan_view = BaseBuffView{table_scan_data.data(), table_scan_data.size()};
    auto conditions = filter_conditions.conditions;
    // Copy pushed down filters to filter_conditions to make filterConditions works properly.
    // Proxy columnar reader use pushed down filters to reduce packs load from disk and has no
    // guarantee to filter all useless data, so we rely on the filterConditions to filter data.
    String tables_range_data;
    for (const auto & [physical_table_id, ranges] : physical_table_ranges)
    {
        tables_range_data.append(reinterpret_cast<const char *>(&physical_table_id), sizeof(physical_table_id));

        String ranges_data;
        for (const auto & range : ranges)
        {
            tipb::KeyRange range_pb;
            range_pb.set_low(range.start_key);
            range_pb.set_high(range.end_key);
            auto data = range_pb.SerializeAsString();
            uint32_t len = data.size();
            ranges_data.append(reinterpret_cast<const char *>(&len), sizeof(len));
            ranges_data.append(data.data(), data.size());
        }
        uint32_t ranges_data_size = ranges_data.size();
        tables_range_data.append(reinterpret_cast<const char *>(&ranges_data_size), sizeof(ranges_data_size));
        tables_range_data.append(ranges_data.data(), ranges_data.size());
    }
    BaseBuffView tables_range_view = BaseBuffView{tables_range_data.data(), tables_range_data.size()};
    String filter_conditions_data;
    for (const auto & condition : conditions)
    {
        auto data = condition.SerializeAsString();
        uint32_t len = data.size();
        filter_conditions_data.append(reinterpret_cast<const char *>(&len), sizeof(len));
        filter_conditions_data.append(data.data(), data.size());
    }
    tipb::TableInfo table_info;
    bool is_partition_scan = table_scan.isPartitionTableScan();
    if (is_partition_scan)
    {
        for (const auto & column : table_scan_pb->partition_table_scan().columns())
        {
            *table_info.add_columns() = column;
        }
    }
    else
    {
        for (const auto & column : table_scan_pb->tbl_scan().columns())
        {
            *table_info.add_columns() = column;
        }
    }
    auto table_info_data = table_info.SerializeAsString();
    BaseBuffView columns = BaseBuffView{table_info_data.data(), table_info_data.size()};
    BaseBuffView filter_conditions_view = BaseBuffView{filter_conditions_data.data(), filter_conditions_data.size()};
    auto ann_query_info_pb = table_scan.getANNQueryInfo();
    auto ann_query_info_data = ann_query_info_pb.SerializeAsString();
    BaseBuffView ann_query_info_view = BaseBuffView{ann_query_info_data.data(), ann_query_info_data.size()};
    const Context & global_ctx = context.getGlobalContext();
    auto * cluster = global_ctx.getTMTContext().getKVCluster();
    const TiFlashRaftProxyHelper * proxy_helper = global_ctx.getTMTContext().getKVStore()->getProxyHelper();
    ColumnarReaderPtr columnar_reader = proxy_helper->cloud_storage_engine_interfaces.fn_get_columnar_reader(
        region_id,
        region_ver,
        start_ts,
        std::move(tables_range_view),
        std::move(columns),
        std::move(table_scan_view),
        std::move(filter_conditions_view),
        std::move(ann_query_info_view),
        proxy_helper->proxy_ptr);
    if (columnar_reader.error_type == ColumnarReaderErrorType::RegionError)
    {
        auto error_msg = String(columnar_reader.error.buff.data, columnar_reader.error.buff.len);
        errorpb::Error region_error;
        region_error.ParseFromString(error_msg);
        auto region_ver_id = pingcap::kv::RegionVerID(region_id, region_conf_ver, region_ver);
        // Refresh region cache and throw an exception for retrying.
        if (region_error.has_epoch_not_match())
        {
            RegionException::UnavailableRegions unavailable_regions;
            String region_id_ver; // region_id:region_ver:conf_ver
            std::unordered_set<RegionID> retry_regions;
            for (const auto & region : region_error.epoch_not_match().current_regions())
            {
                unavailable_regions.insert(region.id());
                retry_regions.insert(region.id());
                region_id_ver = std::to_string(region.id()) + ":" + std::to_string(region_ver) + ":"
                    + std::to_string(region.region_epoch().conf_ver());
            }
            auto _guard = std::lock_guard(output_lock);
            cluster->region_cache->dropRegion(region_ver_id);
            LOG_WARNING(
                log,
                "create columnar reader failed region_id={}, epoch not match {}",
                std::to_string(region_id),
                region_ver_id.toString());
            throw RegionException(
                std::move(unavailable_regions),
                RegionException::RegionReadStatus::EPOCH_NOT_MATCH,
                region_id_ver.c_str());
        }
        else
        {
            RegionException::UnavailableRegions unavailable_regions;
            std::unordered_set<RegionID> retry_regions;
            auto err_region_id = 0;
            if (region_error.has_region_not_found())
            {
                err_region_id = region_error.region_not_found().region_id();
                unavailable_regions.insert(err_region_id);
                retry_regions.insert(err_region_id);
                LOG_WARNING(
                    log,
                    "create columnar reader failed region_id={}, region not found {}",
                    std::to_string(region_id),
                    std::to_string(err_region_id));
            }
            else
            {
                LOG_WARNING(
                    log,
                    "create columnar reader failed region_id={}, {}",
                    std::to_string(region_id),
                    region_error.ShortDebugString());
            }
            auto _guard = std::lock_guard(output_lock);
            cluster->region_cache->dropRegion(region_ver_id);
            throw RegionException(
                std::move(unavailable_regions),
                RegionException::RegionReadStatus::NOT_FOUND,
                std::to_string(region_id).c_str());
        }
    }
    else if (columnar_reader.error_type == ColumnarReaderErrorType::LockedError)
    {
        auto error_msg = String(columnar_reader.error.buff.data, columnar_reader.error.buff.len);
        kvrpcpb::LockInfo lock_info;
        lock_info.ParseFromString(error_msg);
        // Try to resolve locks.
        pingcap::kv::Backoffer bo(pingcap::kv::copNextMaxBackoff);
        std::vector<uint64_t> pushed;
        std::vector<pingcap::kv::LockPtr> locks{std::make_shared<pingcap::kv::Lock>(lock_info)};
        auto _guard = std::lock_guard(output_lock);
        auto before_expired = cluster->lock_resolver->resolveLocks(bo, start_ts, locks, pushed);
        LOG_WARNING(log, "Finished resolve locks, before_expired={}", before_expired);
        throw Exception("lock error", ErrorCodes::COLUMNAR_SNAPSHOT_ERROR);
    }
    else if (columnar_reader.error_type == ColumnarReaderErrorType::PdClientError)
    {
        LOG_WARNING(log, "create columnar reader failed, pd client error");
        throw Exception("pd client error", ErrorCodes::COLUMNAR_SNAPSHOT_ERROR);
    }
    else if (columnar_reader.error_type != ColumnarReaderErrorType::OK)
    {
        LOG_WARNING(log, "create columnar reader, other error_type {}", uint8_t(columnar_reader.error_type));
        throw Exception("unknown error type", ErrorCodes::COLUMNAR_SNAPSHOT_ERROR);
    }

    if (columnar_reader.error_type != ColumnarReaderErrorType::OK)
    {
        RustGcHelper::instance().gcRustPtr(columnar_reader.error.inner.ptr, columnar_reader.error.inner.type);
    }

    // Create input stream.
    auto [column_defines, extra_table_id_index] = genColumnDefinesForDisaggregatedRead(table_scan);
    BlockInputStreamPtr input_stream = RNProxyInputStream::create({
        .context = context,
        .debug_tag = log->identifier(),
        .columns_to_read = *column_defines,
        .reader = columnar_reader,
        .extra_table_id_index = extra_table_id_index,
        .executor_id = table_scan.getTableScanExecutorID(),
    });
    return std::make_shared<RNProxyReader>(input_stream);
}

// RNProxyReadTask
std::vector<RNProxyReadTaskPtr> RNProxyReadTask::buildProxyReadTaskWithBackoff(
    const LoggerPtr & log,
    const Context & context,
    UInt64 start_ts,
    const TiDBTableScan & table_scan,
    const FilterConditions & filter_conditions,
    const std::vector<RemoteTableRange> & remote_table_ranges,
    unsigned num_streams)
{
    std::vector<RNProxyReadTaskPtr> tasks;
    pingcap::kv::Backoffer bo(pingcap::kv::copNextMaxBackoff);
    while (true)
    {
        try
        {
            tasks = RNProxyReadTask::buildProxyReadTask(
                log,
                context,
                start_ts,
                table_scan,
                filter_conditions,
                remote_table_ranges,
                num_streams);
            break;
        }
        catch (RegionException & e)
        {
            LOG_WARNING(log, "buildProxyReadTask failed, backoff and retry, {}", e.message());
            bo.backoff(pingcap::kv::boRegionMiss, pingcap::Exception(e.message(), e.code()));
        }
        catch (Exception & e)
        {
            if (e.code() != ErrorCodes::COLUMNAR_SNAPSHOT_ERROR)
                throw;
            LOG_WARNING(log, "buildProxyReadTask failed, backoff and retry, {}", e.message());
            bo.backoff(pingcap::kv::boRegionMiss, pingcap::Exception(e.message(), e.code()));
        }
    }
    return tasks;
}

std::vector<RNProxyReadTaskPtr> RNProxyReadTask::buildProxyReadTask(
    const LoggerPtr & log,
    const Context & context,
    UInt64 start_ts,
    const TiDBTableScan & table_scan,
    const FilterConditions & filter_conditions,
    const std::vector<RemoteTableRange> & remote_table_ranges,
    unsigned num_streams)
{
    auto * dag_context = context.getDAGContext();
    auto scan_context = std::make_shared<DM::ScanContext>(dag_context->getResourceGroupName());
    dag_context->scan_context_map[table_scan.getTableScanExecutorID()] = scan_context;

    std::vector<RNProxyReadTaskPtr> tasks;
    // Collect all regions in the table scan.
    std::unordered_map<uint64_t, std::vector<std::tuple<TableID, pingcap::coprocessor::KeyRanges>>>
        all_remote_regions_by_region;
    std::unordered_map<uint64_t, pingcap::kv::RegionVerID> region_ver_ids;

    std::vector<UInt64> physical_table_ids;
    std::vector<pingcap::coprocessor::KeyRanges> ranges_for_each_physical_table;
    physical_table_ids.reserve(remote_table_ranges.size());
    ranges_for_each_physical_table.reserve(remote_table_ranges.size());
    for (const auto & remote_table_range : remote_table_ranges)
    {
        physical_table_ids.emplace_back(remote_table_range.first);
        ranges_for_each_physical_table.emplace_back(remote_table_range.second);
    }
    pingcap::kv::Cluster * cluster = context.getTMTContext().getKVCluster();
    pingcap::kv::Backoffer bo(pingcap::kv::copBuildTaskMaxBackoff);
    auto & region_cache = cluster->region_cache;
    for (auto idx = 0; idx < int(ranges_for_each_physical_table.size()); idx++)
    {
        const auto physical_table_id = physical_table_ids[idx];
        const auto ranges = ranges_for_each_physical_table[idx];
        const auto locations = pingcap::coprocessor::details::splitKeyRangesByLocations(region_cache, bo, ranges);
        for (const auto & location : locations)
        {
            all_remote_regions_by_region[location.location.region.id].push_back(
                std::make_tuple(physical_table_id, location.ranges));
            region_ver_ids[location.location.region.id] = location.location.region;
            LOG_DEBUG(
                log,
                "buildProxyReadTask, physical_table_id={}, region_ver_id={}",
                physical_table_id,
                location.location.region.toString());
        }
    }
    unsigned region_num = all_remote_regions_by_region.size();
    unsigned physical_table_num = physical_table_ids.size();
    unsigned real_num_streams = std::min(num_streams, region_num);
    // Regions per RNProxyReader, it should be ceil of region_num / real_num_streams.
    // `regions_per_reader` is the ceil of the division, so the concurrency may be less than `real_num_streams`.
    unsigned regions_per_reader = (region_num + real_num_streams - 1) / real_num_streams;
    LOG_INFO(
        log,
        "region_num={}, table_num={}, num_streams={}, real_num_streams={}, regions_per_reader={}",
        region_num,
        physical_table_num,
        num_streams,
        real_num_streams,
        regions_per_reader);
    unsigned reader_idx = 0;
    std::vector<RNProxyReaderPtr> all_readers;
    std::mutex output_lock;
    auto thread_manager = newThreadManager();

    for (const auto & [region_id, physical_table_ranges] : all_remote_regions_by_region)
    {
        auto region_ver = region_ver_ids[region_id].ver;
        auto region_conf_ver = region_ver_ids[region_id].conf_ver;
        thread_manager->schedule(
            true,
            "createProxyReader",
            [log,
             &context,
             region_id,
             region_ver,
             region_conf_ver,
             physical_table_ranges,
             start_ts,
             &table_scan,
             &filter_conditions,
             &output_lock,
             &all_readers] {
                LOG_INFO(
                    log,
                    "create proxy reader for tables in region, region_id={}, table_num={}",
                    region_id,
                    physical_table_ranges.size());
                auto reader_ptr = RNProxyReader::createProxyReader(
                    log,
                    context,
                    region_id,
                    region_ver,
                    region_conf_ver,
                    physical_table_ranges,
                    start_ts,
                    table_scan,
                    filter_conditions,
                    output_lock);
                {
                    std::lock_guard lock(output_lock);
                    all_readers.push_back(reader_ptr);
                }
            });
    }

    thread_manager->wait();

    std::vector<RNProxyReaderPtr> readers;
    for (auto & reader : all_readers)
    {
        ++reader_idx;
        readers.push_back(reader);
        if (reader_idx == regions_per_reader)
        {
            reader_idx = 0;
            tasks.push_back(std::make_shared<RNProxyReadTask>(std::move(readers)));
            readers.clear();
        }
    }

    if (!readers.empty())
    {
        tasks.push_back(std::make_shared<RNProxyReadTask>(std::move(readers)));
    }

    return tasks;
}

BlockInputStreams RNProxyReadTask::getInputStreams() const
{
    BlockInputStreams streams;
    streams.reserve(proxy_readers.size());
    for (const auto & reader : proxy_readers)
    {
        streams.push_back(reader->getInputStream());
    }
    return streams;
}

// RNProxyInputStream
RNProxyInputStream::~RNProxyInputStream()
{
    LOG_INFO(
        log,
        "Finished reading remote snapshot through proxy, rows={} bytes={} read_cost={:.3f}s deserialize_cost={:.3f}s",
        action.totalRows(),
        total_bytes,
        duration_read_sec,
        duration_deserialize_sec);
    auto * dag_context = context.getDAGContext();
    auto scan_context = dag_context->scan_context_map[executor_id];
    scan_context->user_read_bytes += total_bytes;
    RustGcHelper::instance().gcRustPtr(reader.inner.ptr, reader.inner.type);
}

Block RNProxyInputStream::read(FilterPtr & res_filter, bool return_filter)
{
    return readImpl(res_filter, return_filter);
}

Block RNProxyInputStream::readImpl()
{
    FilterPtr filter_ignored;
    return readImpl(filter_ignored, false);
}

Block RNProxyInputStream::readImpl([[maybe_unused]] FilterPtr & res_filter, [[maybe_unused]] bool return_filter)
{
    if (done)
        return {};
    const Context & global_ctx = context.getGlobalContext();
    const TiFlashRaftProxyHelper * proxy_helper = global_ctx.getTMTContext().getKVStore()->getProxyHelper();
    Stopwatch w{CLOCK_MONOTONIC_COARSE};
    UInt64 rows = proxy_helper->cloud_storage_engine_interfaces.fn_read_block(reader, batch_size);
    duration_read_sec += w.elapsedSecondsFromLastTime();
    LOG_DEBUG(log, "Read {} rows from proxy", rows);
    if (rows == 0)
        return {};

    Block header = action.getHeader();
    const ColumnsWithTypeAndName col_type_and_name = header.getColumnsWithTypeAndName();
    // Construct block from proxy column data.
    MutableColumns columns = header.cloneEmptyColumns();
    for (UInt32 i = 0; i < col_type_and_name.size(); ++i)
    {
        LOG_DEBUG(
            log,
            "Read column id={} name={} type={}",
            col_type_and_name[i].column_id,
            col_type_and_name[i].name,
            col_type_and_name[i].type->getName());
        // Read column data from proxy
        Int64 col_id = col_type_and_name[i].column_id;
        if (col_id == TiDBPkColumnID)
        {
            RustStrWithView col_data = proxy_helper->cloud_storage_engine_interfaces.fn_read_handle(reader);
            String col_data_str(col_data.buff.data, col_data.buff.len);
            // Deserialize column data to column
            ReadBufferFromString buf(col_data_str);
            auto & col = *columns[i];
            col_type_and_name[i].type->deserializeBinaryBulkWithMultipleStreams(
                col,
                [&](const IDataType::SubstreamPath &) { return &buf; },
                rows,
                -1.0, // avg_value_size_hint set to -1 to indicate Decimal format from proxy
                true,
                {});
            RustGcHelper::instance().gcRustPtr(col_data.inner.ptr, col_data.inner.type);
        }
        else if (col_id == ExtraTableIDColumnID)
        {
            continue;
        }
        else
        {
            RustStrWithView col_data = proxy_helper->cloud_storage_engine_interfaces.fn_read_column(reader, col_id);
            String col_data_str(col_data.buff.data, col_data.buff.len);
            // Deserialize column data to column
            ReadBufferFromString buf(col_data_str);
            auto & col = *columns[i];
            col_type_and_name[i].type->deserializeBinaryBulkWithMultipleStreams(
                col,
                [&](const IDataType::SubstreamPath &) { return &buf; },
                rows,
                -1.0, // avg_value_size_hint set to -1 to indicate Decimal format from proxy
                true,
                {});
            LOG_DEBUG(log, "Read column data done, col size={}", col.size());
            RustGcHelper::instance().gcRustPtr(col_data.inner.ptr, col_data.inner.type);
        }
    }
    duration_deserialize_sec += w.elapsedSecondsFromLastTime();

    Block block = header.cloneWithColumns(std::move(columns));
    LOG_DEBUG(log, "Read block rows={}, structure={}", block.rows(), block.dumpStructure());
    block.checkNumberOfRows();
    action.transform(block, table_id);
    total_bytes += block.bytes();
    return block;
}

// RNProxySourceOp
void RNProxySourceOp::operateSuffixImpl()
{
    UNUSED(context);
    LOG_INFO(log, "Finished reading proxy snapshots, rows={} cost={:.3f}s", total_rows, duration_read_sec);
}

void RNProxySourceOp::operatePrefixImpl()
{
    LOG_INFO(log, "Begin reading proxy snapshots");
}

OperatorStatus RNProxySourceOp::readImpl(Block & block)
{
    if unlikely (done)
    {
        block = {};
        return OperatorStatus::HAS_OUTPUT;
    }

    if (t_block.has_value())
    {
        std::swap(block, t_block.value());
        t_block.reset();
        return OperatorStatus::HAS_OUTPUT;
    }

    return current_reader_idx < 0 ? OperatorStatus::IO_IN : awaitImpl();
}

OperatorStatus RNProxySourceOp::awaitImpl()
{
    if unlikely (done || t_block.has_value())
    {
        return OperatorStatus::HAS_OUTPUT;
    }

    if unlikely (current_reader_idx < 0)
    {
        current_reader_idx = 0;
    }

    return OperatorStatus::IO_IN;
}

OperatorStatus RNProxySourceOp::executeIOImpl()
{
    if unlikely (done || t_block.has_value())
    {
        return OperatorStatus::HAS_OUTPUT;
    }

    if unlikely (current_reader_idx < 0)
    {
        return awaitImpl();
    }

    FilterPtr filter_ignored = nullptr;
    Stopwatch w{CLOCK_MONOTONIC_COARSE};
    Block block = task->getProxyReaders()[current_reader_idx]->getInputStream()->read(filter_ignored, false);
    duration_read_sec += w.elapsedSeconds();
    if likely (block && block.rows() > 0)
    {
        total_rows += block.rows();
        t_block.emplace(std::move(block));
        return OperatorStatus::HAS_OUTPUT;
    }
    else
    {
        if (current_reader_idx == Int32(task->getProxyReaders().size() - 1))
        {
            done = true;
        }
        else if (current_reader_idx < Int32(task->getProxyReaders().size() - 1))
        {
            ++current_reader_idx;
        }
        // Current stream is drained, try to read from next stream.
        return awaitImpl();
    }
}

} // namespace DB
