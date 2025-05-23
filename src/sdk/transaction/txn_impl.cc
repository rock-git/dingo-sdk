// Copyright (c) 2023 dingodb.com, Inc. All Rights Reserved
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

#include "sdk/transaction/txn_impl.h"

#include <fmt/format.h>

#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/logging.h"
#include "dingosdk/client.h"
#include "dingosdk/status.h"
#include "fmt/core.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "sdk/common/common.h"
#include "sdk/common/helper.h"
#include "sdk/common/param_config.h"
#include "sdk/rpc/store_rpc.h"
#include "sdk/transaction/txn_buffer.h"
#include "sdk/transaction/txn_common.h"
#include "sdk/utils/async_util.h"

namespace dingodb {
namespace sdk {

Transaction::TxnImpl::TxnImpl(const ClientStub& stub, const TransactionOptions& options)
    : stub_(stub), options_(options), state_(kInit), buffer_(new TxnBuffer()) {}

Status Transaction::TxnImpl::Begin() {
  pb::meta::TsoTimestamp tso;
  Status ret = stub_.GetAdminTool()->GetCurrentTsoTimeStamp(tso);
  if (ret.ok()) {
    start_tso_ = tso;
    start_ts_ = Tso2Timestamp(start_tso_);
    state_ = kActive;
  }
  return ret;
}

std::unique_ptr<TxnGetRpc> Transaction::TxnImpl::PrepareTxnGetRpc(const std::shared_ptr<Region>& region) const {
  auto rpc = std::make_unique<TxnGetRpc>();
  rpc->MutableRequest()->set_start_ts(start_ts_);
  FillRpcContext(*rpc->MutableRequest()->mutable_context(), region->RegionId(), region->Epoch(),
                 TransactionIsolation2IsolationLevel(options_.isolation));
  return std::move(rpc);
}

Status Transaction::TxnImpl::DoTxnGet(const std::string& key, std::string& value) {
  std::shared_ptr<Region> region;
  Status ret = stub_.GetMetaCache()->LookupRegionByKey(key, region);
  if (!ret.IsOK()) {
    return ret;
  }

  std::unique_ptr<TxnGetRpc> rpc = PrepareTxnGetRpc(region);
  rpc->MutableRequest()->set_key(key);

  int retry = 0;
  while (true) {
    DINGO_RETURN_NOT_OK(LogAndSendRpc(stub_, *rpc, region));

    const auto* response = rpc->Response();
    if (response->has_txn_result()) {
      ret = CheckTxnResultInfo(response->txn_result());
    }

    if (ret.ok()) {
      break;
    } else if (ret.IsTxnLockConflict()) {
      ret = stub_.GetTxnLockResolver()->ResolveLock(response->txn_result().locked(), start_ts_);
      if (!ret.ok()) {
        break;
      }
    } else {
      DINGO_LOG(WARNING) << "unexpect txn get rpc response, status:" << ret.ToString()
                         << " response:" << response->ShortDebugString();
      break;
    }

    if (NeedRetryAndInc(retry)) {
      // TODO: set txn retry ms
      DINGO_LOG(INFO) << "try to delay:" << FLAGS_txn_op_delay_ms << "ms";
      DelayRetry(FLAGS_txn_op_delay_ms);
    } else {
      break;
    }
  }

  if (ret.ok()) {
    const auto* response = rpc->Response();
    if (response->value().empty()) {
      ret = Status::NotFound(fmt::format("key:{} not found", key));
    } else {
      value = response->value();
    }
  }

  return ret;
}

Status Transaction::TxnImpl::Get(const std::string& key, std::string& value) {
  TxnMutation mutation;
  Status ret = buffer_->Get(key, mutation);
  if (ret.ok()) {
    switch (mutation.type) {
      case kPut:
        value = mutation.value;
        return Status::OK();
      case kDelete:
        return Status::NotFound("");
      case kPutIfAbsent:
        // NOTE: directy return is ok?
        value = mutation.value;
        return Status::OK();
      default:
        CHECK(false) << "unknow mutation type, mutation:" << mutation.ToString();
    }
  }

  return DoTxnGet(key, value);
}

void Transaction::TxnImpl::ProcessTxnBatchGetSubTask(TxnSubTask* sub_task) {
  auto* rpc = CHECK_NOTNULL(dynamic_cast<TxnBatchGetRpc*>(sub_task->rpc));

  Status res;
  int retry = 0;
  while (true) {
    res = LogAndSendRpc(stub_, *rpc, sub_task->region);

    if (!res.ok()) {
      break;
    }

    const auto* response = rpc->Response();
    if (response->has_txn_result()) {
      res = CheckTxnResultInfo(response->txn_result());
    }

    if (res.ok()) {
      break;
    } else if (res.IsTxnLockConflict()) {
      res = stub_.GetTxnLockResolver()->ResolveLock(response->txn_result().locked(), start_ts_);
      if (!res.ok()) {
        break;
      }
    } else {
      DINGO_LOG(WARNING) << "unexpect txn batch get rpc response, status:" << res.ToString()
                         << " response:" << response->ShortDebugString();
      break;
    }

    if (NeedRetryAndInc(retry)) {
      // TODO: set txn retry ms
      DINGO_LOG(INFO) << "try to delay:" << FLAGS_txn_op_delay_ms << "ms";
      DelayRetry(FLAGS_txn_op_delay_ms);
    } else {
      break;
    }
  }

  if (res.ok()) {
    for (const auto& kv : rpc->Response()->kvs()) {
      if (!kv.value().empty()) {
        sub_task->result_kvs.push_back({kv.key(), kv.value()});
      } else {
        DINGO_LOG(DEBUG) << "Ignore kv key:" << kv.key() << " because value is empty";
      }
    }
  }

  sub_task->status = res;
}

std::unique_ptr<TxnBatchGetRpc> Transaction::TxnImpl::PrepareTxnBatchGetRpc(
    const std::shared_ptr<Region>& region) const {
  auto rpc = std::make_unique<TxnBatchGetRpc>();
  rpc->MutableRequest()->set_start_ts(start_ts_);
  FillRpcContext(*rpc->MutableRequest()->mutable_context(), region->RegionId(), region->Epoch(),
                 TransactionIsolation2IsolationLevel(options_.isolation));
  return std::move(rpc);
}

// TODO: return not found keys
Status Transaction::TxnImpl::DoTxnBatchGet(const std::vector<std::string>& keys, std::vector<KVPair>& kvs) {
  auto meta_cache = stub_.GetMetaCache();
  std::unordered_map<int64_t, std::shared_ptr<Region>> region_id_to_region;
  std::unordered_map<int64_t, std::vector<std::string>> region_keys;

  for (const auto& key : keys) {
    std::shared_ptr<Region> tmp;
    Status got = meta_cache->LookupRegionByKey(key, tmp);
    if (!got.IsOK()) {
      return got;
    }
    auto iter = region_id_to_region.find(tmp->RegionId());
    if (iter == region_id_to_region.end()) {
      region_id_to_region.emplace(std::make_pair(tmp->RegionId(), tmp));
    }

    region_keys[tmp->RegionId()].push_back(key);
  }

  std::vector<TxnSubTask> sub_tasks;
  std::vector<std::unique_ptr<TxnBatchGetRpc>> rpcs;

  for (const auto& entry : region_keys) {
    auto region_id = entry.first;
    auto iter = region_id_to_region.find(region_id);
    CHECK(iter != region_id_to_region.end());
    auto region = iter->second;

    auto rpc = PrepareTxnBatchGetRpc(region);

    for (const auto& key : entry.second) {
      auto* fill = rpc->MutableRequest()->add_keys();
      *fill = key;
    }

    sub_tasks.emplace_back(rpc.get(), region);
    rpcs.push_back(std::move(rpc));
  }

  DCHECK_EQ(rpcs.size(), region_keys.size());
  DCHECK_EQ(rpcs.size(), sub_tasks.size());

  // parallel execute sub task
  ParallelExecutor::Execute(sub_tasks.size(), [&sub_tasks, this](uint32_t i) {
    Transaction::TxnImpl::ProcessTxnBatchGetSubTask(&sub_tasks[i]);
  });

  Status result;
  std::vector<KVPair> tmp_kvs;
  for (auto& state : sub_tasks) {
    if (!state.status.IsOK()) {
      DINGO_LOG(WARNING) << "Fail txn_batch_get_sub_task, rpc: " << state.rpc->Method()
                         << " send to region: " << state.region->RegionId() << " status: " << state.status.ToString();
      if (result.ok()) {
        // only return first fail status
        result = state.status;
      }
    } else {
      tmp_kvs.insert(tmp_kvs.end(), std::make_move_iterator(state.result_kvs.begin()),
                     std::make_move_iterator(state.result_kvs.end()));
    }
  }

  kvs = std::move(tmp_kvs);
  return result;
}

Status Transaction::TxnImpl::BatchGet(const std::vector<std::string>& keys, std::vector<KVPair>& kvs) {
  std::vector<std::string> not_found;
  std::vector<KVPair> to_return;
  Status ret;
  for (const auto& key : keys) {
    TxnMutation mutation;
    ret = buffer_->Get(key, mutation);
    if (ret.IsOK()) {
      switch (mutation.type) {
        case kPut:
          to_return.push_back({key, mutation.value});
          continue;
        case kDelete:
          continue;
        case kPutIfAbsent:
          // NOTE: use this value is ok?
          to_return.push_back({key, mutation.value});
          continue;
        default:
          CHECK(false) << "unknow mutation type, mutation:" << mutation.ToString();
      }
    } else {
      CHECK(ret.IsNotFound());
      not_found.push_back(key);
    }
  }

  if (!not_found.empty()) {
    std::vector<KVPair> batch_get;
    ret = DoTxnBatchGet(not_found, batch_get);
    to_return.insert(to_return.end(), std::make_move_iterator(batch_get.begin()),
                     std::make_move_iterator(batch_get.end()));
  }

  kvs = std::move(to_return);

  return ret;
}

Status Transaction::TxnImpl::Put(const std::string& key, const std::string& value) { return buffer_->Put(key, value); }

Status Transaction::TxnImpl::BatchPut(const std::vector<KVPair>& kvs) { return buffer_->BatchPut(kvs); }

Status Transaction::TxnImpl::PutIfAbsent(const std::string& key, const std::string& value) {
  return buffer_->PutIfAbsent(key, value);
}

Status Transaction::TxnImpl::BatchPutIfAbsent(const std::vector<KVPair>& kvs) { return buffer_->BatchPutIfAbsent(kvs); }

Status Transaction::TxnImpl::Delete(const std::string& key) { return buffer_->Delete(key); }

Status Transaction::TxnImpl::BatchDelete(const std::vector<std::string>& keys) { return buffer_->BatchDelete(keys); }

Status Transaction::TxnImpl::ProcessScanState(ScanState& scan_state, uint64_t limit, std::vector<KVPair>& out_kvs) {
  int mutations_offset = 0;
  auto& local_mutations = scan_state.local_mutations;
  while (scan_state.pending_offset < scan_state.pending_kvs.size()) {
    auto& kv = scan_state.pending_kvs[scan_state.pending_offset];
    ++scan_state.pending_offset;

    if (mutations_offset >= local_mutations.size()) {
      out_kvs.push_back(std::move(kv));
      if (out_kvs.size() == limit) {
        return Status::OK();
      }
      continue;
    }

    const auto& mutation = local_mutations[mutations_offset];
    if (kv.key == mutation.key) {
      if (mutation.type == TxnMutationType::kDelete) {
        continue;

      } else if (mutation.type == TxnMutationType::kPut) {
        out_kvs.push_back({std::move(kv.key), mutation.value});

      } else {
        CHECK(false) << "unknow mutation type, mutation:" << mutation.ToString();
      }

      ++mutations_offset;

    } else if (kv.key < mutation.key) {
      out_kvs.push_back(std::move(kv));

    } else {
      do {
        if (mutation.type == TxnMutationType::kPutIfAbsent) {
          out_kvs.push_back({std::move(kv.key), mutation.value});
        }

        ++mutations_offset;

        if (out_kvs.size() == limit) {
          return Status::OK();
        }

      } while (mutations_offset < local_mutations.size() && kv.key > local_mutations[mutations_offset].key);
    }

    if (out_kvs.size() == limit) {
      return Status::OK();
    }
  }

  return Status::OK();
}

Status Transaction::TxnImpl::Scan(const std::string& start_key, const std::string& end_key, uint64_t limit,
                                  std::vector<KVPair>& out_kvs) {
  if (start_key.empty() || end_key.empty()) {
    return Status::InvalidArgument("start_key and end_key must not empty");
  }

  if (start_key >= end_key) {
    return Status::InvalidArgument("end_key must greater than start_key");
  }

  DINGO_LOG(INFO) << fmt::format("scan range [{}, {}), limit:{}", StringToHex(start_key), StringToHex(end_key), limit);

  auto meta_cache = stub_.GetMetaCache();
  // check whether region exist
  std::shared_ptr<Region> region;
  Status status = meta_cache->LookupRegionBetweenRange(start_key, end_key, region);
  if (!status.IsOK()) {
    DINGO_LOG(WARNING) << fmt::format("lookup region fail between [{},{}) {}.", StringToHex(start_key),
                                      StringToHex(end_key), status.ToString());
    return status;
  }

  // get or create scan state
  std::string state_key = start_key + end_key;
  auto it = scan_states_.find(state_key);
  if (it == scan_states_.end()) {
    ScanState scan_state = {.next_key = start_key};
    CHECK(buffer_->Range(start_key, end_key, scan_state.local_mutations).ok());

    scan_states_.emplace(std::make_pair(state_key, std::move(scan_state)));
    it = scan_states_.find(state_key);
  }
  auto& scan_state = it->second;

  if (scan_state.pending_offset < scan_state.pending_kvs.size()) {
    ProcessScanState(scan_state, limit, out_kvs);
    if (!out_kvs.empty()) {
      scan_state.next_key = out_kvs.back().key;
    }
    if (out_kvs.size() == limit) {
      return Status::OK();
    }
  }

  // loop scan
  while (scan_state.next_key < end_key) {
    DINGO_LOG(INFO) << fmt::format("scan next_key:{} end_key:{}", StringToHex(scan_state.next_key),
                                   StringToHex(end_key));

    auto scanner = scan_state.scanner;
    if (scanner == nullptr) {
      // get region
      RegionPtr region;
      Status status = meta_cache->LookupRegionBetweenRange(scan_state.next_key, end_key, region);
      if (!status.IsOK()) {
        DINGO_LOG(INFO) << fmt::format("lookup region fail, range[{}, {}) {}.", StringToHex(start_key),
                                       StringToHex(end_key), status.ToString());

        if (status.IsNotFound()) {
          scan_state.next_key = end_key;
          continue;
        }
        return status;
      }

      std::string amend_start_key =
          scan_state.next_key <= region->Range().start_key() ? region->Range().start_key() : scan_state.next_key;
      std::string amend_end_key = end_key <= region->Range().end_key() ? end_key : region->Range().end_key();

      DINGO_LOG(INFO) << fmt::format("scan region: {} range [{}, {})", region->RegionId(), StringToHex(amend_start_key),
                                     StringToHex(amend_end_key));

      ScannerOptions scan_options(stub_, region, amend_start_key, amend_end_key, options_, start_ts_);
      CHECK(stub_.GetTxnRegionScannerFactory()->NewRegionScanner(scan_options, scanner).IsOK());
      CHECK(scanner->Open().ok());

      scan_state.scanner = scanner;
    }

    while (scanner->HasMore()) {
      std::vector<KVPair> scan_kvs;
      status = scanner->NextBatch(scan_kvs);
      if (!status.IsOK()) {
        DINGO_LOG(ERROR) << fmt::format("next batch fail, region({}) {}.", region->RegionId(), status.ToString());
        return status;
      }
      if (scan_kvs.empty()) {
        CHECK(!scanner->HasMore()) << "scan_kvs is empty, so scanner should not has more.";
        break;
      }

      CHECK(scan_state.pending_offset == scan_state.pending_kvs.size()) << "pending_kvs is not empty.";

      scan_state.pending_kvs = std::move(scan_kvs);
      scan_state.pending_offset = 0;

      ProcessScanState(scan_state, limit, out_kvs);
      if (!out_kvs.empty()) {
        scan_state.next_key = out_kvs.back().key;
      }
      if (out_kvs.size() == limit) {
        return Status::OK();
      }
    }

    auto region = scanner->GetRegion();
    CHECK(region != nullptr) << "region should not nullptr.";
    scan_state.next_key = region->Range().end_key();
    scanner->Close();
    scan_state.scanner = nullptr;
  }

  scan_states_.erase(state_key);

  return Status::OK();
}

std::unique_ptr<TxnPrewriteRpc> Transaction::TxnImpl::PrepareTxnPrewriteRpc(
    const std::shared_ptr<Region>& region) const {
  auto rpc = std::make_unique<TxnPrewriteRpc>();

  rpc->MutableRequest()->set_start_ts(start_ts_);
  FillRpcContext(*rpc->MutableRequest()->mutable_context(), region->RegionId(), region->Epoch(),
                 TransactionIsolation2IsolationLevel(options_.isolation));

  std::string pk = buffer_->GetPrimaryKey();
  rpc->MutableRequest()->set_primary_lock(pk);
  rpc->MutableRequest()->set_txn_size(buffer_->MutationsSize());

  // FIXME: set ttl
  rpc->MutableRequest()->set_lock_ttl(INT64_MAX);

  return std::move(rpc);
}

void Transaction::TxnImpl::CheckAndLogPreCommitPrimaryKeyResponse(
    const pb::store::TxnPrewriteResponse* response) const {
  std::string pk = buffer_->GetPrimaryKey();
  auto txn_result_size = response->txn_result_size();
  if (0 == txn_result_size) {
    DINGO_LOG(DEBUG) << "success pre_commit_primary_key:" << StringToHex(pk);
  } else if (1 == txn_result_size) {
    const auto& txn_result = response->txn_result(0);
    DINGO_LOG(INFO) << "lock or confict pre_commit_primary_key:" << StringToHex(pk)
                    << " txn_result:" << txn_result.ShortDebugString();
  } else {
    DINGO_LOG(FATAL) << "unexpected pre_commit_primary_key response txn_result_size size: " << txn_result_size
                     << ", response:" << response->ShortDebugString();
  }
}

Status Transaction::TxnImpl::TryResolveTxnPrewriteLockConflict(const pb::store::TxnPrewriteResponse* response) const {
  Status ret;
  std::string pk = buffer_->GetPrimaryKey();
  for (const auto& txn_result : response->txn_result()) {
    ret = CheckTxnResultInfo(txn_result);

    if (ret.ok()) {
      continue;
    } else if (ret.IsTxnLockConflict()) {
      Status resolve = stub_.GetTxnLockResolver()->ResolveLock(txn_result.locked(), start_ts_);
      if (!resolve.ok()) {
        DINGO_LOG(WARNING) << "fail resolve lock pk:" << StringToHex(pk) << ", status:" << ret.ToString()
                           << " txn_result:" << txn_result.ShortDebugString();
        ret = resolve;
      }
    } else if (ret.IsTxnWriteConflict()) {
      DINGO_LOG(WARNING) << "write conflict pk:" << StringToHex(pk) << ", status:" << ret.ToString()
                         << " txn_result:" << txn_result.ShortDebugString();
      return ret;
    } else {
      DINGO_LOG(WARNING) << "unexpect txn pre commit rpc response, status:" << ret.ToString()
                         << " response:" << response->ShortDebugString();
    }
  }

  return ret;
}

Status Transaction::TxnImpl::PreCommitPrimaryKey(bool is_one_pc) {
  std::string pk = buffer_->GetPrimaryKey();

  std::shared_ptr<Region> region;
  Status ret = stub_.GetMetaCache()->LookupRegionByKey(pk, region);
  if (!ret.IsOK()) {
    return ret;
  }

  std::unique_ptr<TxnPrewriteRpc> rpc = PrepareTxnPrewriteRpc(region);
  TxnMutation mutation;
  CHECK(buffer_->Get(pk, mutation).ok());
  TxnMutation2MutationPB(mutation, rpc->MutableRequest()->add_mutations());

  if (is_one_pc) {
    rpc->MutableRequest()->set_try_one_pc(true);
    for (const auto& [key, mutation] : buffer_->Mutations()) {
      if (key != pk) {
        TxnMutation2MutationPB(mutation, rpc->MutableRequest()->add_mutations());
      }
    }
  }

  int retry = 0;
  while (true) {
    DINGO_RETURN_NOT_OK(LogAndSendRpc(stub_, *rpc, region));

    const auto* response = rpc->Response();
    CheckAndLogPreCommitPrimaryKeyResponse(response);

    ret = TryResolveTxnPrewriteLockConflict(response);

    if (ret.ok()) {
      break;
    } else if (ret.IsTxnWriteConflict()) {
      // no need retry
      // TODO: should we change txn state?
      DINGO_LOG(WARNING) << "write conflict, txn need abort and restart, pre_commit_primary:" << pk;
      break;
    }

    if (NeedRetryAndInc(retry)) {
      // TODO: set txn retry ms
      DINGO_LOG(INFO) << "try to delay:" << FLAGS_txn_op_delay_ms << "ms";
      DelayRetry(FLAGS_txn_op_delay_ms);
    } else {
      break;
    }
  }

  return ret;
}

void Transaction::TxnImpl::ProcessTxnPrewriteSubTask(TxnSubTask* sub_task) {
  auto* rpc = CHECK_NOTNULL(dynamic_cast<TxnPrewriteRpc*>(sub_task->rpc));
  std::string pk = buffer_->GetPrimaryKey();
  Status ret;
  int retry = 0;
  while (true) {
    ret = LogAndSendRpc(stub_, *rpc, sub_task->region);
    if (!ret.ok()) {
      break;
    }

    const auto* response = rpc->Response();
    ret = TryResolveTxnPrewriteLockConflict(response);

    if (ret.ok()) {
      break;
    } else if (ret.IsTxnWriteConflict()) {
      // no need retry
      // TODO: should we change txn state?
      DINGO_LOG(WARNING) << "write conflict, txn need abort and restart, pre_commit_primary:" << StringToHex(pk);
      break;
    }
    if (NeedRetryAndInc(retry)) {
      // TODO: set txn retry ms
      DINGO_LOG(INFO) << "try to delay:" << FLAGS_txn_op_delay_ms << "ms";
      DelayRetry(FLAGS_txn_op_delay_ms);
    } else {
      // TODO: maybe set ret as meaningful status
      break;
    }
  }

  sub_task->status = ret;
}

static bool IsOneRegionTxn(std::shared_ptr<MetaCache> meta_cache, TxnBuffer& buffer) {
  uint64_t region_id = 0;
  for (const auto& [key, mutation] : buffer.Mutations()) {
    RegionPtr region;
    Status s = meta_cache->LookupRegionByKey(key, region);
    if (!s.IsOK()) {
      return false;
    }

    if (region_id == 0) {
      region_id = region->RegionId();
    } else if (region_id != region->RegionId()) {
      return false;
    }
  }

  return true;
}

// TODO: process AlreadyExist if mutaion is PutIfAbsent
Status Transaction::TxnImpl::PreCommit() {
  struct RegionTxnMutation {
    RegionPtr region;
    std::vector<TxnMutation> mutations;
  };

  state_ = kPreCommitting;

  if (buffer_->IsEmpty()) {
    state_ = kPreCommitted;
    return Status::OK();
  }

  auto meta_cache = stub_.GetMetaCache();

  // check whether one region txn, if true, use try_one_pc
  is_one_pc_ = IsOneRegionTxn(meta_cache, *buffer_);

  DINGO_LOG(INFO) << fmt::format("is_one_pc: {}", is_one_pc_);

  DINGO_RETURN_NOT_OK(PreCommitPrimaryKey(is_one_pc_));

  if (is_one_pc_) {
    state_ = kCommitted;
    return Status::OK();
  }

  // TODO: start heartbeat

  // group mutations by region
  std::string pk = buffer_->GetPrimaryKey();
  std::unordered_map<int64_t, RegionTxnMutation> region_mutation_map;
  for (const auto& [key, mutation] : buffer_->Mutations()) {
    if (key == pk) {
      continue;
    }

    RegionPtr region;
    Status s = meta_cache->LookupRegionByKey(key, region);
    if (!s.IsOK()) {
      return s;
    }

    auto iter = region_mutation_map.find(region->RegionId());
    if (iter == region_mutation_map.end()) {
      region_mutation_map.emplace(std::make_pair(region->RegionId(), RegionTxnMutation{region, {mutation}}));
    } else {
      iter->second.mutations.push_back(mutation);
    }
  }

  // generate rpcs task
  std::vector<TxnSubTask> sub_tasks;
  std::vector<std::unique_ptr<TxnPrewriteRpc>> rpcs;
  for (const auto& [region_id, region_mutation] : region_mutation_map) {
    auto region = region_mutation.region;

    dingodb::pb::store::TxnPrewriteRequest txn_prewrite_request;
    for (const auto& mutation : region_mutation.mutations) {
      TxnMutation2MutationPB(mutation, txn_prewrite_request.add_mutations());
    }

    auto rpc = PrepareTxnPrewriteRpc(region);
    for (const auto& mutation : txn_prewrite_request.mutations()) {
      auto* request = rpc->MutableRequest();
      *request->add_mutations() = mutation;

      if (request->mutations_size() == FLAGS_txn_max_batch_count) {
        sub_tasks.emplace_back(rpc.get(), region);
        rpcs.push_back(std::move(rpc));
        rpc = PrepareTxnPrewriteRpc(region);
      }
    }

    if (rpc != nullptr && rpc->Request()->mutations_size() > 0) {
      sub_tasks.emplace_back(rpc.get(), region);
      rpcs.push_back(std::move(rpc));
    }
  }

  DCHECK_EQ(rpcs.size(), sub_tasks.size());

  // parallel execute sub task
  ParallelExecutor::Execute(sub_tasks.size(), [&sub_tasks, this](uint32_t i) {
    Transaction::TxnImpl::ProcessTxnPrewriteSubTask(&sub_tasks[i]);
  });

  // check execute result
  Status result;
  for (auto& state : sub_tasks) {
    if (!state.status.IsOK()) {
      DINGO_LOG(WARNING) << fmt::format("prewrite part fail, region({}) {} {}.", state.region->RegionId(),
                                        state.rpc->Method(), state.status.ToString());
      if (result.ok()) {
        // only return first fail status
        result = state.status;
      }
    }
  }

  if (result.ok()) {
    state_ = kPreCommitted;
  }

  return result;
}

std::unique_ptr<TxnCommitRpc> Transaction::TxnImpl::PrepareTxnCommitRpc(const std::shared_ptr<Region>& region) const {
  auto rpc = std::make_unique<TxnCommitRpc>();
  FillRpcContext(*rpc->MutableRequest()->mutable_context(), region->RegionId(), region->Epoch(),
                 TransactionIsolation2IsolationLevel(options_.isolation));

  rpc->MutableRequest()->set_start_ts(start_ts_);
  rpc->MutableRequest()->set_commit_ts(commit_ts_);

  return std::move(rpc);
}

Status Transaction::TxnImpl::ProcessTxnCommitResponse(const pb::store::TxnCommitResponse* response,
                                                      bool is_primary) const {
  std::string pk = buffer_->GetPrimaryKey();
  DINGO_LOG(DEBUG) << fmt::format("commit response, start_ts({}) pk({}) response({}).", start_ts_, StringToHex(pk),
                                  response->ShortDebugString());

  if (response->has_txn_result()) {
    const auto& txn_result = response->txn_result();
    if (txn_result.has_locked()) {
      const auto& lock_info = txn_result.locked();
      DINGO_LOG(FATAL) << fmt::format("txn lock confilict, start_ts({}) pk({}) response({}).", start_ts_,
                                      StringToHex(pk), response->ShortDebugString());
    }

    if (txn_result.has_txn_not_found()) {
      DINGO_LOG(FATAL) << fmt::format("txn not found, start_ts({}) pk({}) response({}).", start_ts_, StringToHex(pk),
                                      response->ShortDebugString());
    }

    if (txn_result.has_write_conflict()) {
      const auto& write_conflict = txn_result.write_conflict();
      if (!is_primary) {
        DINGO_LOG(FATAL) << fmt::format("txn write conlict, start_ts({}) pk({}) response({}).", start_ts_,
                                        StringToHex(pk), response->ShortDebugString());
      }
      return Status::TxnRolledBack("");
    }
  }

  return Status::OK();
}

Status Transaction::TxnImpl::CommitPrimaryKey() {
  std::string pk = buffer_->GetPrimaryKey();
  std::shared_ptr<Region> region;
  Status ret = stub_.GetMetaCache()->LookupRegionByKey(pk, region);
  if (!ret.IsOK()) {
    return ret;
  }

  std::unique_ptr<TxnCommitRpc> rpc = PrepareTxnCommitRpc(region);
  auto* fill = rpc->MutableRequest()->add_keys();
  *fill = pk;

  DINGO_RETURN_NOT_OK(LogAndSendRpc(stub_, *rpc, region));

  const auto* response = rpc->Response();
  return ProcessTxnCommitResponse(response, true);
}

void Transaction::TxnImpl::ProcessTxnCommitSubTask(TxnSubTask* sub_task) {
  auto* rpc = CHECK_NOTNULL(dynamic_cast<TxnCommitRpc*>(sub_task->rpc));
  std::string pk = buffer_->GetPrimaryKey();
  Status ret;

  ret = LogAndSendRpc(stub_, *rpc, sub_task->region);
  if (!ret.ok()) {
    sub_task->status = ret;
    return;
  }

  const auto* response = rpc->Response();
  ret = ProcessTxnCommitResponse(response, true);
  sub_task->status = ret;
}

Status Transaction::TxnImpl::Commit() {
  if (state_ == kCommitted) {
    return Status::OK();
  } else if (state_ != kPreCommitted) {
    return Status::IllegalState(fmt::format("forbid commit, txn state is:{}, expect:{}", TransactionState2Str(state_),
                                            TransactionState2Str(kPreCommitted)));
  }

  if (buffer_->IsEmpty()) {
    state_ = kCommitted;
    return Status::OK();
  }

  state_ = kCommitting;

  pb::meta::TsoTimestamp tso;
  DINGO_RETURN_NOT_OK(stub_.GetAdminTool()->GetCurrentTsoTimeStamp(tso));
  commit_tso_ = tso;
  commit_ts_ = Tso2Timestamp(commit_tso_);
  CHECK(commit_ts_ > start_ts_) << "commit_ts:" << commit_ts_ << " must greater than start_ts:" << start_ts_
                                << ", commit_tso:" << commit_tso_.ShortDebugString()
                                << ", start_tso:" << start_tso_.ShortDebugString();
  // TODO: if commit primary key and find txn is rolled back, should we rollback all the mutation?
  Status ret = CommitPrimaryKey();
  if (!ret.ok()) {
    if (ret.IsTxnRolledBack()) {
      state_ = kRollbackted;
    } else {
      DINGO_LOG(INFO) << "unexpect commit primary key status:" << ret.ToString();
    }
  } else {
    state_ = kCommitted;

    {
      // we commit primary key is success, and then we try best to commit other keys, if fail we ignore
      auto meta_cache = stub_.GetMetaCache();
      std::unordered_map<int64_t, std::shared_ptr<Region>> region_id_to_region;
      std::unordered_map<int64_t, std::vector<std::string>> region_commit_keys;

      std::string pk = buffer_->GetPrimaryKey();
      for (const auto& mutaion_entry : buffer_->Mutations()) {
        if (mutaion_entry.first == pk) {
          continue;
        }

        std::shared_ptr<Region> tmp;
        Status got = meta_cache->LookupRegionByKey(mutaion_entry.first, tmp);
        if (!got.IsOK()) {
          continue;
        }

        auto iter = region_id_to_region.find(tmp->RegionId());
        if (iter == region_id_to_region.end()) {
          region_id_to_region.emplace(std::make_pair(tmp->RegionId(), tmp));
        }

        region_commit_keys[tmp->RegionId()].push_back(mutaion_entry.second.key);
      }

      std::vector<TxnSubTask> sub_tasks;
      std::vector<std::unique_ptr<TxnCommitRpc>> rpcs;
      for (const auto& entry : region_commit_keys) {
        auto region_id = entry.first;
        auto iter = region_id_to_region.find(region_id);
        CHECK(iter != region_id_to_region.end());
        auto region = iter->second;

        std::unique_ptr<TxnCommitRpc> rpc = PrepareTxnCommitRpc(region);

        uint32_t tmp_count = 0;
        for (const auto& key : entry.second) {
          rpc->MutableRequest()->add_keys(key);
          tmp_count++;

          if (tmp_count == FLAGS_txn_max_batch_count) {
            sub_tasks.emplace_back(rpc.get(), region);
            rpcs.push_back(std::move(rpc));
            tmp_count = 0;
            rpc = PrepareTxnCommitRpc(region);
          }
        }

        if (tmp_count > 0) {
          sub_tasks.emplace_back(rpc.get(), region);
          rpcs.push_back(std::move(rpc));
        }
      }

      // DCHECK_EQ(rpcs.size(), region_commit_keys.size());
      DCHECK_EQ(rpcs.size(), sub_tasks.size());

      // parallel execute sub task
      ParallelExecutor::Execute(sub_tasks.size(), [&sub_tasks, this](uint32_t i) {
        Transaction::TxnImpl::ProcessTxnCommitSubTask(&sub_tasks[i]);
      });

      for (auto& state : sub_tasks) {
        // ignore
        if (!state.status.IsOK()) {
          DINGO_LOG(INFO) << fmt::format("commit fail, region({}) {} {}.", state.region->RegionId(),
                                         state.rpc->Method(), state.status.ToString());
        }
      }
    }
  }

  return ret;
}

std::unique_ptr<TxnBatchRollbackRpc> Transaction::TxnImpl::PrepareTxnBatchRollbackRpc(
    const std::shared_ptr<Region>& region) const {
  auto rpc = std::make_unique<TxnBatchRollbackRpc>();
  FillRpcContext(*rpc->MutableRequest()->mutable_context(), region->RegionId(), region->Epoch(),
                 TransactionIsolation2IsolationLevel(options_.isolation));
  rpc->MutableRequest()->set_start_ts(start_ts_);
  return std::move(rpc);
}

void Transaction::TxnImpl::CheckAndLogTxnBatchRollbackResponse(
    const pb::store::TxnBatchRollbackResponse* response) const {
  if (response->has_txn_result()) {
    std::string pk = buffer_->GetPrimaryKey();
    const auto& txn_result = response->txn_result();
    DINGO_LOG(WARNING) << fmt::format("rollback fail, start_ts({}) pk({}) result({}).", start_ts_, StringToHex(pk),
                                      txn_result.ShortDebugString());
  }
}

void Transaction::TxnImpl::ProcessBatchRollbackSubTask(TxnSubTask* sub_task) {
  auto* rpc = CHECK_NOTNULL(dynamic_cast<TxnBatchRollbackRpc*>(sub_task->rpc));
  std::string pk = buffer_->GetPrimaryKey();
  Status ret;

  ret = LogAndSendRpc(stub_, *rpc, sub_task->region);
  if (!ret.ok()) {
    sub_task->status = ret;
    return;
  }

  const auto* response = rpc->Response();
  CheckAndLogTxnBatchRollbackResponse(response);
  if (response->has_txn_result()) {
    const auto& txn_result = response->txn_result();
    if (txn_result.has_locked()) {
      sub_task->status = Status::TxnLockConflict("");
      return;
    }
  }

  sub_task->status = Status::OK();
}

Status Transaction::TxnImpl::Rollback() {
  struct RegionRollbackKeys {
    RegionPtr region;
    std::vector<std::string> keys;
  };

  // TODO: client txn status maybe inconsistence with server
  // so we should check txn status first and then take action
  // TODO: maybe support rollback when txn is active
  if (state_ != kRollbacking && state_ != kPreCommitting && state_ != kPreCommitted) {
    return Status::IllegalState(fmt::format("forbid rollback, txn state is:{}", TransactionState2Str(state_)));
  }

  auto meta_cache = stub_.GetMetaCache();
  std::string pk = buffer_->GetPrimaryKey();

  state_ = kRollbacking;
  {
    // rollback primary key
    RegionPtr region;
    Status ret = meta_cache->LookupRegionByKey(pk, region);
    if (!ret.IsOK()) {
      return ret;
    }

    std::unique_ptr<TxnBatchRollbackRpc> rpc = PrepareTxnBatchRollbackRpc(region);
    *rpc->MutableRequest()->add_keys() = pk;
    if (is_one_pc_) {
      for (const auto& [key, _] : buffer_->Mutations()) {
        if (key != pk) {
          *rpc->MutableRequest()->add_keys() = key;
        }
      }
    }

    DINGO_RETURN_NOT_OK(LogAndSendRpc(stub_, *rpc, region));

    const auto* response = rpc->Response();
    CheckAndLogTxnBatchRollbackResponse(response);
    if (response->has_txn_result()) {
      // TODO: which state should we transfer to ?
      const auto& txn_result = response->txn_result();
      if (txn_result.has_locked()) {
        return Status::TxnLockConflict(txn_result.locked().ShortDebugString());
      }
    }
  }
  state_ = kRollbackted;
  if (is_one_pc_) {
    return Status::OK();
  }

  {
    // we rollback primary key is success, and then we try best to rollback other keys, if fail we ignore

    std::unordered_map<int64_t, RegionRollbackKeys> region_rollback_map;
    for (const auto& [key, mutaion] : buffer_->Mutations()) {
      if (key == pk) {
        continue;
      }

      RegionPtr region;
      Status got = meta_cache->LookupRegionByKey(key, region);
      if (!got.IsOK()) {
        continue;
      }

      auto iter = region_rollback_map.find(region->RegionId());
      if (iter == region_rollback_map.end()) {
        region_rollback_map.emplace(std::make_pair(region->RegionId(), RegionRollbackKeys{region, {key}}));
      } else {
        iter->second.keys.push_back(key);
      }
    }

    if (region_rollback_map.empty()) {
      return Status::OK();
    }

    std::vector<TxnSubTask> sub_tasks;
    std::vector<std::unique_ptr<TxnBatchRollbackRpc>> rpcs;
    for (const auto& [region_id, region_rollback] : region_rollback_map) {
      auto region = region_rollback.region;

      std::unique_ptr<TxnBatchRollbackRpc> rpc = PrepareTxnBatchRollbackRpc(region);
      for (const auto& key : region_rollback.keys) {
        *rpc->MutableRequest()->add_keys() = key;
      }
      sub_tasks.emplace_back(rpc.get(), region);
      rpcs.push_back(std::move(rpc));
    }

    DCHECK_EQ(rpcs.size(), region_rollback_map.size());
    DCHECK_EQ(rpcs.size(), sub_tasks.size());

    // parallel execute sub task
    ParallelExecutor::Execute(sub_tasks.size(), [&sub_tasks, this](uint32_t i) {
      Transaction::TxnImpl::ProcessBatchRollbackSubTask(&sub_tasks[i]);
    });

    for (auto& state : sub_tasks) {
      // ignore
      if (!state.status.IsOK()) {
        DINGO_LOG(INFO) << fmt::format("rollback fail, region({}) {} {}.", state.region->RegionId(),
                                       state.rpc->Method(), state.status.ToString());
      }
    }
  }

  return Status::OK();
}

bool Transaction::TxnImpl::NeedRetryAndInc(int& times) {
  bool retry = times < FLAGS_txn_op_max_retry;
  times++;
  return retry;
}

void Transaction::TxnImpl::DelayRetry(int64_t delay_ms) { (void)usleep(delay_ms * 1000); }

}  // namespace sdk
}  // namespace dingodb