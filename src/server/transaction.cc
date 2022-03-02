// Copyright 2021, Roman Gershman.  All rights reserved.
// See LICENSE for licensing terms.
//

#include "server/transaction.h"

#include <absl/strings/match.h>

#include "base/logging.h"
#include "server/command_registry.h"
#include "server/db_slice.h"
#include "server/engine_shard_set.h"

namespace dfly {

using namespace std;
using namespace util;

thread_local Transaction::TLTmpSpace Transaction::tmp_space;

namespace {

std::atomic_uint64_t op_seq{1};

[[maybe_unused]] constexpr size_t kTransSize = sizeof(Transaction);


}  // namespace

struct Transaction::FindFirstProcessor {
 public:
  FindFirstProcessor(TxId notify, unsigned size)
      : find_res_(size, OpStatus::KEY_NOTFOUND), notify_txid_(notify) {
  }

  void Find(Transaction* t);

  OpResult<Transaction::FindFirstResult> Process(Transaction* t);

 private:
  OpStatus RunInShard(Transaction* t, EngineShard* shard);

  // Holds Find results: (iterator to a found key, and its index in the passed arguments).
  // See DbSlice::FindFirst for more details.
  // spans all the shards for now.
  std::vector<OpResult<std::pair<MainIterator, unsigned>>> find_res_;
  TxId notify_txid_;
};

void Transaction::FindFirstProcessor::Find(Transaction* t) {
  VLOG(2) << "FindFirst::Find " << t->DebugId();

  t->Execute([this](auto* t, auto* s) { return RunInShard(t, s); }, false);
}

OpStatus Transaction::FindFirstProcessor::RunInShard(Transaction* t, EngineShard* shard) {
  if (notify_txid_ == kuint64max || shard->committed_txid() == notify_txid_) {
    // TODO: to add timestamp logic that provides consistency guarantees for blocking transactions.
    auto args = t->ShardArgsInShard(shard->shard_id());
    find_res_[shard->shard_id()] = shard->db_slice().FindFirst(t->db_index(), args);
  }
  return OpStatus::OK;
}

OpResult<Transaction::FindFirstResult> Transaction::FindFirstProcessor::Process(Transaction* t) {
  uint32_t min_arg_indx = UINT32_MAX;

  FindFirstResult result;
  for (size_t sid = 0; sid < find_res_.size(); ++sid) {
    const auto& fr = find_res_[sid];
    auto status = fr.status();
    if (status == OpStatus::KEY_NOTFOUND)
      continue;

    if (status == OpStatus::WRONG_TYPE) {
      return status;
    }

    DCHECK(fr && IsValid(fr->first));
    const auto& it_pos = fr.value();

    size_t arg_indx = t->ReverseArgIndex(sid, it_pos.second);
    if (arg_indx < min_arg_indx) {
      min_arg_indx = arg_indx;
      result.sid = sid;
      result.find_res = it_pos.first;
    }
  }

  if (result.sid == kInvalidSid) {
    return OpStatus::KEY_NOTFOUND;
  }
  return result;
}

IntentLock::Mode Transaction::Mode() const {
  return (cid_->opt_mask() & CO::READONLY) ? IntentLock::SHARED : IntentLock::EXCLUSIVE;
}

/**
 * @brief Construct a new Transaction:: Transaction object
 *
 * @param cid
 * @param ess
 * @param cs
 */
Transaction::Transaction(const CommandId* cid, EngineShardSet* ess) : cid_(cid), ess_(ess) {
  string_view cmd_name(cid_->name());
  if (cmd_name == "EXEC" || cmd_name == "EVAL" || cmd_name == "EVALSHA") {
    multi_.reset(new Multi);
    multi_->multi_opts = cid->opt_mask();

    if (cmd_name == "EVAL" || cmd_name == "EVALSHA") {
      multi_->incremental = false;  // we lock all the keys at once.
    }
  }
}

Transaction::~Transaction() {
  DVLOG(2) << "Transaction " << DebugId() << " destroyed";
}

/**
 *
 * There are 4 options that we consider here:
 * a. T spans a single shard and its not multi.
 *    unique_shard_id_ is predefined before the schedule() is called.
 *    In that case only a single thread will be scheduled and it will use shard_data[0] just becase
 *    shard_data.size() = 1. Coordinator thread can access any data because there is a
 *    schedule barrier between InitByArgs and RunInShard/IsArmedInShard functions.
 * b. T spans multiple shards and its not multi
 *    In that case multiple threads will be scheduled. Similarly they have a schedule barrier,
 *    and IsArmedInShard can read any variable from shard_data[x].
 * c. Trans spans a single shard and it's multi. shard_data has size of ess_.size.
 *    IsArmedInShard will check shard_data[x].
 * d. Trans spans multiple shards and it's multi. Similarly shard_data[x] will be checked.
 *    unique_shard_cnt_ and unique_shard_id_ are not accessed until shard_data[x] is armed, hence
 *    we have a barrier between coordinator and engine-threads. Therefore there should not be
 *    data races.
 *
 **/

void Transaction::InitByArgs(DbIndex index, CmdArgList args) {
  db_index_ = index;

  if (IsGlobal()) {
    unique_shard_cnt_ = ess_->size();
    shard_data_.resize(unique_shard_cnt_);
    return;
  }

  CHECK_GT(args.size(), 1U);  // first entry is the command name.
  DCHECK_EQ(unique_shard_cnt_, 0u);
  DCHECK(args_.empty());

  KeyIndex key_index = DetermineKeys(cid_, args);

  if (key_index.start == args.size()) {  // eval with 0 keys.
    CHECK(absl::StartsWith(cid_->name(), "EVAL"));
    return;
  }

  DCHECK_LT(key_index.start, args.size());
  DCHECK_GT(key_index.start, 0u);

  bool incremental_locking = multi_ && multi_->incremental;
  bool single_key = !multi_ && (key_index.start + key_index.step) >= key_index.end;

  if (single_key) {
    DCHECK_GT(key_index.step, 0u);

    shard_data_.resize(1);  // Single key optimization
    for (unsigned j = key_index.start; j < key_index.start + key_index.step; ++j) {
      args_.push_back(ArgS(args, j));
    }
    string_view key = args_.front();

    unique_shard_cnt_ = 1;
    unique_shard_id_ = Shard(key, ess_->size());

    return;
  }

  // Our shard_data is not sparse, so we must allocate for all threads :(
  shard_data_.resize(ess_->size());
  CHECK(key_index.step == 1 || key_index.step == 2);
  DCHECK(key_index.step == 1 || (args.size() % 2) == 1);

  // Reuse thread-local temporary storage. Since this code is atomic we can use it here.
  auto& shard_index = tmp_space.shard_cache;
  shard_index.resize(shard_data_.size());
  for (auto& v : shard_index) {
    v.Clear();
  }

  // TODO: to determine correctly locking mode for transactions, scripts
  // and regular commands.
  IntentLock::Mode mode = IntentLock::EXCLUSIVE;
  bool should_record_locks = false;
  if (multi_) {
    mode = Mode();
    tmp_space.uniq_keys.clear();
    DCHECK_LT(int(mode), 2);
    should_record_locks = incremental_locking || !multi_->locks_recorded;
  }

  for (unsigned i = key_index.start; i < key_index.end; ++i) {
    string_view key = ArgS(args, i);
    uint32_t sid = Shard(key, shard_data_.size());
    shard_index[sid].args.push_back(key);
    shard_index[sid].original_index.push_back(i - 1);

    if (should_record_locks && tmp_space.uniq_keys.insert(key).second) {
      multi_->locks[key].cnt[int(mode)]++;
    };

    if (key_index.step == 2) {  // value
      ++i;
      auto val = ArgS(args, i);
      shard_index[sid].args.push_back(val);
      shard_index[sid].original_index.push_back(i - 1);
    }
  }

  if (multi_) {
    multi_->locks_recorded = true;
  }

  args_.resize(key_index.end - key_index.start);
  reverse_index_.resize(args_.size());

  auto next_arg = args_.begin();
  auto rev_indx_it = reverse_index_.begin();

  // slice.arg_start/arg_count point to args_ array which is sorted according to shard of each key.
  // reverse_index_[i] says what's the original position of args_[i] in args.
  for (size_t i = 0; i < shard_data_.size(); ++i) {
    auto& sd = shard_data_[i];
    auto& si = shard_index[i];

    CHECK_LT(si.args.size(), 1u << 15);

    sd.arg_count = si.args.size();
    sd.arg_start = next_arg - args_.begin();

    // We reset the local_mask for incremental locking to allow locking of arguments
    // for each operation within the same transaction. For instant locking we lock at
    // the beginning all the keys so we must preserve the mask to avoid double locking.
    if (incremental_locking) {
      sd.local_mask = 0;
    }

    if (!sd.arg_count)
      continue;

    ++unique_shard_cnt_;
    unique_shard_id_ = i;
    uint32_t orig_indx = 0;
    for (size_t j = 0; j < si.args.size(); ++j) {
      *next_arg = si.args[j];
      *rev_indx_it = si.original_index[orig_indx];

      ++next_arg;
      ++orig_indx;
      ++rev_indx_it;
    }
  }

  CHECK(next_arg == args_.end());
  DVLOG(1) << "InitByArgs " << DebugId() << " " << args_.front();

  if (unique_shard_cnt_ == 1) {
    PerShardData* sd;
    if (multi_) {
      sd = &shard_data_[unique_shard_id_];
    } else {
      shard_data_.resize(1);
      sd = &shard_data_.front();
    }
    sd->arg_count = -1;
    sd->arg_start = -1;
  }

  // Validation.
  for (const auto& sd : shard_data_) {
    // sd.local_mask may be non-zero for multi transactions with instant locking.
    // Specifically EVALs may maintain state between calls.
    DCHECK_EQ(0, sd.local_mask & ARMED);
    if (!multi_) {
      DCHECK_EQ(TxQueue::kEnd, sd.pq_pos);
    }
  }
}

void Transaction::SetExecCmd(const CommandId* cid) {
  DCHECK(multi_);
  DCHECK(!cb_);

  // The order is important, we are Schedule() for multi transaction before overriding cid_.
  // TODO: The flow is ugly. I should introduce a proper interface for Multi transactions
  // like SetupMulti/TurndownMulti. We already have UnlockMulti that should be part of
  // TurndownMulti.

  if (txid_ == 0) {
    Schedule();
  }

  unique_shard_cnt_ = 0;
  args_.clear();
  cid_ = cid;
  cb_ = nullptr;
}

string Transaction::DebugId() const {
  return absl::StrCat(Name(), "@", txid_, "/", unique_shard_cnt_, " (", trans_id(this), ")");
}

// Runs in the dbslice thread. Returns true if transaction needs to be kept in the queue.
bool Transaction::RunInShard(EngineShard* shard) {
  DCHECK_GT(run_count_.load(memory_order_relaxed), 0u);
  CHECK(cb_) << DebugId();
  DCHECK_GT(txid_, 0u);

  // Unlike with regular transactions we do not acquire locks upon scheduling
  // because Scheduling is done before multi-exec batch is executed. Therefore we
  // lock keys right before the execution of each statement.

  DVLOG(1) << "RunInShard: " << DebugId() << " sid:" << shard->shard_id();

  unsigned idx = SidToId(shard->shard_id());
  auto& sd = shard_data_[idx];

  DCHECK(sd.local_mask & ARMED);
  sd.local_mask &= ~ARMED;

  DCHECK_EQ(sd.local_mask & (SUSPENDED_Q | EXPIRED_Q), 0);

  bool awaked_prerun = (sd.local_mask & AWAKED_Q) != 0;
  bool incremental_lock = multi_ && multi_->incremental;

  // For multi we unlock transaction (i.e. its keys) in UnlockMulti() call.
  // Therefore we differentiate between concluding, which says that this specific
  // runnable concludes current operation, and should_release which tells
  // whether we should unlock the keys. should_release is false for multi and
  // equal to concluding otherwise.
  bool should_release = (coordinator_state_ & COORD_EXEC_CONCLUDING) && !multi_;
  IntentLock::Mode mode = Mode();

  // We make sure that we lock exactly once for each (multi-hop) transaction inside
  // transactions that lock incrementally.
  if (incremental_lock && ((sd.local_mask & KEYLOCK_ACQUIRED) == 0)) {
    DCHECK(!awaked_prerun);  // we should not have blocking transaction inside multi block.

    sd.local_mask |= KEYLOCK_ACQUIRED;
    shard->db_slice().Acquire(mode, GetLockArgs(idx));
  }

  DCHECK(IsGlobal() || (sd.local_mask & KEYLOCK_ACQUIRED));

  /*************************************************************************/
  // Actually running the callback.
  OpStatus status = cb_(this, shard);
  /*************************************************************************/

  if (unique_shard_cnt_ == 1) {
    cb_ = nullptr;  // We can do it because only a single thread runs the callback.
    local_result_ = status;
  } else {
    CHECK_EQ(OpStatus::OK, status);
  }

  // at least the coordinator thread owns the reference.
  DCHECK_GE(use_count(), 1u);

  // we remove tx from tx-queue upon first invocation.
  // if it needs to run again it runs via a dedicated continuation_trans_ state in EngineShard.
  if (sd.pq_pos != TxQueue::kEnd) {
    shard->txq()->Remove(sd.pq_pos);
    sd.pq_pos = TxQueue::kEnd;
  }

  // If it's a final hop we should release the locks.
  if (should_release) {
    bool is_suspended = sd.local_mask & SUSPENDED_Q;

    if (IsGlobal()) {
      DCHECK(!awaked_prerun && !is_suspended);  // Global transactions can not be blocking.
      shard->shard_lock()->Release(Mode());
    } else {  // not global.
      KeyLockArgs largs = GetLockArgs(idx);

      // If a transaction has been suspended, we keep the lock so that future transaction
      // touching those keys will be ordered via TxQueue. It's necessary because we preserve
      // the atomicity of awaked transactions by halting the TxQueue.
      if (!is_suspended) {
        shard->db_slice().Release(mode, largs);
        sd.local_mask &= ~KEYLOCK_ACQUIRED;
      }
      sd.local_mask &= ~OUT_OF_ORDER;
      // It has 2 responsibilities.
      // 1: to go over potential wakened keys, verify them and activate watch queues.
      // 2: if this transaction was notified and finished running - to remove it from the head
      //    of the queue and notify the next one.
      shard->ProcessAwakened(awaked_prerun ? this : nullptr);
    }
  }

  CHECK_GE(DecreaseRunCnt(), 1u);
  // From this point on we can not access 'this'.

  return !should_release;  // keep
}

void Transaction::RunNoop(EngineShard* shard) {
  DVLOG(1) << "RunNoop " << DebugId();

  unsigned idx = SidToId(shard->shard_id());
  auto& sd = shard_data_[idx];
  DCHECK(sd.local_mask & ARMED);
  DCHECK(sd.local_mask & KEYLOCK_ACQUIRED);
  DCHECK(!multi_);
  DCHECK(!IsGlobal());

  sd.local_mask &= ~ARMED;

  if (unique_shard_cnt_ == 1) {
    cb_ = nullptr;
    local_result_ = OpStatus::OK;
  }

  if (coordinator_state_ & COORD_EXEC_CONCLUDING) {
    KeyLockArgs largs = GetLockArgs(idx);
    shard->db_slice().Release(Mode(), largs);
    sd.local_mask &= ~KEYLOCK_ACQUIRED;

    if (sd.local_mask & SUSPENDED_Q) {
      sd.local_mask |= EXPIRED_Q;
      shard->GCWatched(largs);
    }
  }
  // Decrease run count after we update all the data in the transaction object.
  CHECK_GE(DecreaseRunCnt(), 1u);
}

void Transaction::ScheduleInternal() {
  DCHECK_EQ(0u, txid_);
  DCHECK_EQ(0, coordinator_state_ & (COORD_SCHED | COORD_OOO));

  bool span_all = IsGlobal();
  bool single_hop = (coordinator_state_ & COORD_EXEC_CONCLUDING);

  uint32_t num_shards;
  std::function<bool(uint32_t)> is_active;

  // TODO: For multi-transactions we should be able to deduce mode() at run-time based
  // on the context. For regular multi-transactions we can actually inspect all commands.
  // For eval-like transactions - we can decided based on the command flavor (EVAL/EVALRO) or
  // auto-tune based on the static analysis (by identifying commands with hardcoded command names).
  IntentLock::Mode mode = Mode();

  if (span_all) {
    is_active = [](uint32_t) { return true; };
    num_shards = ess_->size();

    // Lock shards
    auto cb = [mode](EngineShard* shard) { shard->shard_lock()->Acquire(mode); };
    ess_->RunBriefInParallel(std::move(cb));
  } else {
    num_shards = unique_shard_cnt_;
    DCHECK_GT(num_shards, 0u);

    is_active = [&](uint32_t i) {
      return num_shards == 1 ? (i == unique_shard_id_) : shard_data_[i].arg_count > 0;
    };
  }

  while (true) {
    txid_ = op_seq.fetch_add(1, std::memory_order_relaxed);

    std::atomic_uint32_t lock_granted_cnt{0};
    std::atomic_uint32_t success{0};

    auto cb = [&](EngineShard* shard) {
      pair<bool, bool> res = ScheduleInShard(shard);
      success.fetch_add(res.first, memory_order_relaxed);
      lock_granted_cnt.fetch_add(res.second, memory_order_relaxed);
    };

    ess_->RunBriefInParallel(std::move(cb), is_active);

    if (success.load(memory_order_acquire) == num_shards) {
      // We allow out of order execution only for single hop transactions.
      // It might be possible to do it for multi-hop transactions as well but currently is
      // too complicated to reason about.
      if (single_hop && lock_granted_cnt.load(memory_order_relaxed) == num_shards) {
        // OOO can not happen with span-all transactions. We ensure it in ScheduleInShard when we
        // refuse to acquire locks for these transactions..
        DCHECK(!span_all);
        coordinator_state_ |= COORD_OOO;
      }
      DVLOG(1) << "Scheduled " << DebugId()
               << " OutOfOrder: " << bool(coordinator_state_ & COORD_OOO);
      coordinator_state_ |= COORD_SCHED;
      break;
    }

    DVLOG(1) << "Cancelling " << DebugId();

    auto cancel = [&](EngineShard* shard) {
      success.fetch_sub(CancelInShard(shard), memory_order_relaxed);
    };

    ess_->RunBriefInParallel(std::move(cancel), is_active);
    CHECK_EQ(0u, success.load(memory_order_relaxed));
  }

  if (IsOOO()) {
    for (auto& sd : shard_data_) {
      sd.local_mask |= OUT_OF_ORDER;
    }
  }
}

// Optimized "Schedule and execute" function for the most common use-case of a single hop
// transactions like set/mset/mget etc. Does not apply for more complicated cases like RENAME or
// BLPOP where a data must be read from multiple shards before performing another hop.
OpStatus Transaction::ScheduleSingleHop(RunnableType cb) {
  DCHECK(!cb_);

  cb_ = std::move(cb);

  // single hop -> concluding.
  coordinator_state_ |= (COORD_EXEC | COORD_EXEC_CONCLUDING);

  if (!multi_) {   // for non-multi transactions we schedule exactly once.
    DCHECK_EQ(0, coordinator_state_ & COORD_SCHED);
  }

  bool schedule_fast = (unique_shard_cnt_ == 1) && !IsGlobal() && !multi_;
  if (schedule_fast) {  // Single shard (local) optimization.
    // We never resize shard_data because that would affect MULTI transaction correctness.
    DCHECK_EQ(1u, shard_data_.size());

    shard_data_[0].local_mask |= ARMED;

    // memory_order_release because we do not want it to be reordered with shard_data writes
    // above.
    // IsArmedInShard() first checks run_count_ before accessing shard_data.
    run_count_.fetch_add(1, memory_order_release);

    // Please note that schedule_cb can not update any data on ScheduleSingleHop stack
    // since the latter can exit before ScheduleUniqueShard returns.
    // The problematic flow is as follows: ScheduleUniqueShard schedules into TxQueue and then
    // call PollExecute that runs the callback which calls DecreaseRunCnt.
    // As a result WaitForShardCallbacks below is unblocked.
    auto schedule_cb = [&] {
      bool run_eager = ScheduleUniqueShard(EngineShard::tlocal());
      if (run_eager) {
        // it's important to DecreaseRunCnt only for run_eager and after run_eager was assigned.
        // If DecreaseRunCnt were called before ScheduleUniqueShard finishes
        // then WaitForShardCallbacks below could exit before schedule_cb assigns return value
        // to run_eager and cause stack corruption.
        CHECK_GE(DecreaseRunCnt(), 1u);
      }
    };

    ess_->Add(unique_shard_id_, std::move(schedule_cb));  // serves as a barrier.
  } else {
    // Transaction spans multiple shards or it's global (like flushdb) or multi.
    if (!multi_)
      ScheduleInternal();
    ExecuteAsync();
  }

  DVLOG(1) << "ScheduleSingleHop before Wait " << DebugId() << " " << run_count_.load();
  WaitForShardCallbacks();
  DVLOG(1) << "ScheduleSingleHop after Wait " << DebugId();

  cb_ = nullptr;

  return local_result_;
}

// Runs in the coordinator fiber.
void Transaction::UnlockMulti() {
  VLOG(1) << "UnlockMulti " << DebugId();

  DCHECK(multi_);
  using KeyList = vector<pair<std::string_view, LockCnt>>;
  vector<KeyList> sharded_keys(ess_->size());

  // It's LE and not EQ because there may be callbacks in progress that increase use_count_.
  DCHECK_LE(1u, use_count());

  for (const auto& k_v : multi_->locks) {
    ShardId sid = Shard(k_v.first, sharded_keys.size());
    sharded_keys[sid].push_back(k_v);
  }

  auto cb = [&] {
    EngineShard* shard = EngineShard::tlocal();

    if (multi_->multi_opts & CO::GLOBAL_TRANS) {
      shard->shard_lock()->Release(IntentLock::EXCLUSIVE);
    }

    ShardId sid = shard->shard_id();
    for (const auto& k_v : sharded_keys[sid]) {
      auto release = [&](IntentLock::Mode mode) {
        if (k_v.second.cnt[mode]) {
          shard->db_slice().Release(mode, this->db_index_, k_v.first, k_v.second.cnt[mode]);
        }
      };

      release(IntentLock::SHARED);
      release(IntentLock::EXCLUSIVE);
    }

    auto& sd = shard_data_[SidToId(shard->shard_id())];

    // It does not have to be that all shards in multi transaction execute this tx.
    // Hence it could stay in the tx queue. We perform the necessary cleanup and remove it from
    // there.
    if (sd.pq_pos != TxQueue::kEnd) {
      DVLOG(1) << "unlockmulti: TxPopFront " << DebugId();

      TxQueue* txq = shard->txq();
      DCHECK(!txq->Empty());
      Transaction* trans = absl::get<Transaction*>(txq->Front());
      DCHECK(trans == this);
      txq->PopFront();
      sd.pq_pos = TxQueue::kEnd;
    }

    shard->ShutdownMulti(this);

    // notify awakened transactions.
    shard->ProcessAwakened(nullptr);
    shard->PollExecution("unlockmulti", nullptr);

    this->DecreaseRunCnt();
  };

  uint32_t prev = run_count_.fetch_add(shard_data_.size(), memory_order_relaxed);
  DCHECK_EQ(prev, 0u);

  for (ShardId i = 0; i < shard_data_.size(); ++i) {
    ess_->Add(i, cb);
  }
  WaitForShardCallbacks();
  DCHECK_GE(use_count(), 1u);

  VLOG(1) << "UnlockMultiEnd " << DebugId();
}

// Runs in coordinator thread.
void Transaction::Execute(RunnableType cb, bool conclude) {
  cb_ = std::move(cb);
  coordinator_state_ |= COORD_EXEC;

  if (conclude) {
    coordinator_state_ |= COORD_EXEC_CONCLUDING;
  } else {
    coordinator_state_ &= ~COORD_EXEC_CONCLUDING;
  }

  ExecuteAsync();

  DVLOG(1) << "Wait on Exec " << DebugId();
  WaitForShardCallbacks();
  DVLOG(1) << "Wait on Exec " << DebugId() << " completed";

  cb_ = nullptr;
}

// Runs in coordinator thread.
void Transaction::ExecuteAsync() {
  DVLOG(1) << "ExecuteAsync " << DebugId();

  DCHECK_GT(unique_shard_cnt_, 0u);
  DCHECK_GT(use_count_.load(memory_order_relaxed), 0u);

  // We do not necessarily Execute this transaction in 'cb' below. It well may be that it will be
  // executed by the engine shard once it has been armed and coordinator thread will finish the
  // transaction before engine shard thread stops accessing it. Therefore, we increase reference
  // by number of callbacks accessesing 'this' to allow callbacks to execute shard->Execute(this);
  // safely.
  use_count_.fetch_add(unique_shard_cnt_, memory_order_relaxed);

  bool is_global = IsGlobal();

  if (unique_shard_cnt_ == 1) {
    shard_data_[SidToId(unique_shard_id_)].local_mask |= ARMED;
  } else {
    for (ShardId i = 0; i < shard_data_.size(); ++i) {
      auto& sd = shard_data_[i];
      if (!is_global && sd.arg_count == 0)
        continue;
      DCHECK_LT(sd.arg_count, 1u << 15);
      sd.local_mask |= ARMED;
    }
  }

  uint32_t seq = seqlock_.load(memory_order_relaxed);

  // this fence prevents that a read or write operation before a release fence will be reordered
  // with a write operation after a release fence. Specifically no writes below will be reordered
  // upwards. Important, because it protects non-threadsafe local_mask from being accessed by
  // IsArmedInShard in other threads.
  run_count_.store(unique_shard_cnt_, memory_order_release);

  // We verify seq lock has the same generation number. See below for more info.
  auto cb = [seq, this] {
    EngineShard* shard = EngineShard::tlocal();

    uint16_t local_mask = GetLocalMask(shard->shard_id());

    // we use fetch_add with release trick to make sure that local_mask is loaded before
    // we load seq_after. We could gain similar result with "atomic_thread_fence(acquire)"
    uint32_t seq_after = seqlock_.fetch_add(0, memory_order_release);
    bool should_poll = (seq_after == seq) && (local_mask & ARMED);

    DVLOG(2) << "EngineShard::Exec " << DebugId() << " sid:" << shard->shard_id() << " "
             << run_count_.load(memory_order_relaxed) << ", should_poll: " << should_poll;

    // We verify that this callback is still relevant.
    // If we still have the same sequence number and local_mask is ARMED it means
    // the coordinator thread has not crossed WaitForShardCallbacks barrier.
    // Otherwise, this callback is redundant. We may still call PollExecution but
    // we should not pass this to it since it can be in undefined state for this callback.
    if (should_poll) {
      // shard->PollExecution(this) does not necessarily execute this transaction.
      // Therefore, everything that should be handled during the callback execution
      // should go into RunInShard.
      shard->PollExecution("exec_cb", this);
    }

    DVLOG(2) << "ptr_release " << DebugId() << " " << seq;
    intrusive_ptr_release(this);  // against use_count_.fetch_add above.
  };

  // IsArmedInShard is the protector of non-thread safe data.
  if (!is_global && unique_shard_cnt_ == 1) {
    ess_->Add(unique_shard_id_, std::move(cb));  // serves as a barrier.
  } else {
    for (ShardId i = 0; i < shard_data_.size(); ++i) {
      auto& sd = shard_data_[i];
      if (!is_global && sd.arg_count == 0)
        continue;
      ess_->Add(i, cb);  // serves as a barrier.
    }
  }
}

void Transaction::RunQuickie(EngineShard* shard) {
  DCHECK(!multi_);
  DCHECK_EQ(1u, shard_data_.size());
  DCHECK_EQ(0u, txid_);

  shard->IncQuickRun();

  auto& sd = shard_data_[0];
  DCHECK_EQ(0, sd.local_mask & (KEYLOCK_ACQUIRED | OUT_OF_ORDER));

  DVLOG(1) << "RunQuickSingle " << DebugId() << " " << shard->shard_id() << " " << args_[0];
  CHECK(cb_) << DebugId() << " " << shard->shard_id() << " " << args_[0];

  local_result_ = cb_(this, shard);

  sd.local_mask &= ~ARMED;
  cb_ = nullptr;  // We can do it because only a single shard runs the callback.
}

// runs in coordinator thread.
// Marks the transaction as expired but does not remove it from the waiting queue.
void Transaction::ExpireBlocking() {
  DVLOG(1) << "ExpireBlocking " << DebugId();
  DCHECK(!IsGlobal());

  run_count_.store(unique_shard_cnt_, memory_order_release);

  auto expire_cb = [this] {
    EngineShard* shard = EngineShard::tlocal();

    auto lock_args = GetLockArgs(shard->shard_id());
    shard->db_slice().Release(Mode(), lock_args);

    unsigned sd_idx = SidToId(shard->shard_id());
    auto& sd = shard_data_[sd_idx];
    sd.local_mask |= EXPIRED_Q;
    sd.local_mask &= ~KEYLOCK_ACQUIRED;

    // Need to see why I decided to call this.
    // My guess - probably to trigger the run of stalled transactions in case
    // this shard concurrently awoke this transaction and stalled the processing
    // of TxQueue.
    shard->PollExecution("expirecb", nullptr);

    CHECK_GE(DecreaseRunCnt(), 1u);
  };

  if (unique_shard_cnt_ == 1) {
    DCHECK_LT(unique_shard_id_, ess_->size());
    ess_->Add(unique_shard_id_, std::move(expire_cb));
  } else {
    for (ShardId i = 0; i < shard_data_.size(); ++i) {
      auto& sd = shard_data_[i];
      DCHECK_EQ(0, sd.local_mask & ARMED);
      if (sd.arg_count == 0)
        continue;
      ess_->Add(i, expire_cb);
    }
  }

  // Wait for all callbacks to conclude.
  WaitForShardCallbacks();
  DVLOG(1) << "ExpireBlocking finished " << DebugId();
}

const char* Transaction::Name() const {
  return cid_->name();
}

KeyLockArgs Transaction::GetLockArgs(ShardId sid) const {
  KeyLockArgs res;
  res.db_index = db_index_;
  res.key_step = cid_->key_arg_step();
  res.args = ShardArgsInShard(sid);

  return res;
}

// Runs within a engine shard thread.
// Optimized path that schedules and runs transactions out of order if possible.
// Returns true if was eagerly executed, false if it was scheduled into queue.
bool Transaction::ScheduleUniqueShard(EngineShard* shard) {
  DCHECK(!multi_);
  DCHECK_EQ(0u, txid_);
  DCHECK_EQ(1u, shard_data_.size());

  auto mode = Mode();
  auto lock_args = GetLockArgs(shard->shard_id());

  auto& sd = shard_data_.front();
  DCHECK_EQ(TxQueue::kEnd, sd.pq_pos);

  // Fast path - for uncontended keys, just run the callback.
  // That applies for single key operations like set, get, lpush etc.
  if (shard->db_slice().CheckLock(mode, lock_args)) {
    RunQuickie(shard);
    return true;
  }

  // we can do it because only a single thread writes into txid_ and sd.
  txid_ = op_seq.fetch_add(1, std::memory_order_relaxed);
  sd.pq_pos = shard->txq()->Insert(this);

  DCHECK_EQ(0, sd.local_mask & KEYLOCK_ACQUIRED);
  bool lock_acquired = shard->db_slice().Acquire(mode, lock_args);
  sd.local_mask |= KEYLOCK_ACQUIRED;
  DCHECK(!lock_acquired);  // Because CheckLock above failed.

  DVLOG(1) << "Rescheduling into TxQueue " << DebugId();

  shard->PollExecution("schedule_unique", nullptr);

  return false;
}

// This function should not block since it's run via RunBriefInParallel.
pair<bool, bool> Transaction::ScheduleInShard(EngineShard* shard) {
  // schedule_success, lock_granted.
  pair<bool, bool> result{false, false};

  if (shard->committed_txid() >= txid_) {
    return result;
  }

  TxQueue* txq = shard->txq();
  KeyLockArgs lock_args;
  IntentLock::Mode mode = Mode();

  bool spans_all = IsGlobal();
  bool lock_granted = false;
  ShardId sid = SidToId(shard->shard_id());

  auto& sd = shard_data_[sid];

  if (!spans_all) {
    bool shard_unlocked = shard->shard_lock()->Check(mode);
    lock_args = GetLockArgs(shard->shard_id());

    // we need to acquire the lock unrelated to shard_unlocked since we register into Tx queue.
    // All transactions in the queue must acquire the intent lock.
    lock_granted = shard->db_slice().Acquire(mode, lock_args) && shard_unlocked;
    sd.local_mask |= KEYLOCK_ACQUIRED;
    DVLOG(1) << "Lock granted " << lock_granted << " for trans " << DebugId();
  }

  if (!txq->Empty()) {
    // If the new transaction requires reordering of the pending queue (i.e. it comes before tail)
    // and some other transaction already locked its keys we can not reorder 'trans' because
    // that other transaction could have deduced that it can run OOO and eagerly execute. Hence, we
    // fail this scheduling attempt for trans.
    // However, when we schedule span-all transactions we can still reorder them. The reason is
    // before we start scheduling them we lock the shards and disable OOO.
    // We may record when they disable OOO via barrier_ts so if the queue contains transactions
    // that were only scheduled afterwards we know they are not free so we can still
    // reorder the queue. Currently, this optimization is disabled: barrier_ts < pq->HeadScore().
    bool to_proceed = lock_granted || txq->TailScore() < txid_;
    if (!to_proceed) {
      if (sd.local_mask & KEYLOCK_ACQUIRED) {  // rollback the lock.
        shard->db_slice().Release(mode, lock_args);
        sd.local_mask &= ~KEYLOCK_ACQUIRED;
      }

      return result;  // false, false
    }
  }

  result.second = lock_granted;
  result.first = true;

  TxQueue::Iterator it = txq->Insert(this);
  DCHECK_EQ(TxQueue::kEnd, sd.pq_pos);
  sd.pq_pos = it;

  DVLOG(1) << "Insert into tx-queue, sid(" << sid << ") " << DebugId() << ", qlen " << txq->size();

  return result;
}

bool Transaction::CancelInShard(EngineShard* shard) {
  ShardId idx = SidToId(shard->shard_id());
  auto& sd = shard_data_[idx];

  auto pos = sd.pq_pos;
  if (pos == TxQueue::kEnd)
    return false;

  sd.pq_pos = TxQueue::kEnd;

  TxQueue* pq = shard->txq();
  auto val = pq->At(pos);
  Transaction* trans = absl::get<Transaction*>(val);
  DCHECK(trans == this) << "Pos " << pos << ", pq size " << pq->size() << ", trans " << trans;
  pq->Remove(pos);

  if (sd.local_mask & KEYLOCK_ACQUIRED) {
    auto mode = Mode();
    auto lock_args = GetLockArgs(shard->shard_id());
    shard->db_slice().Release(mode, lock_args);
    sd.local_mask &= ~KEYLOCK_ACQUIRED;
  }
  return true;
}

// runs in engine-shard thread.
ArgSlice Transaction::ShardArgsInShard(ShardId sid) const {
  DCHECK(!args_.empty());
  DCHECK_NOTNULL(EngineShard::tlocal());

  // We can read unique_shard_cnt_  only because ShardArgsInShard is called after IsArmedInShard
  // barrier.
  if (unique_shard_cnt_ == 1) {
    return args_;
  }

  const auto& sd = shard_data_[sid];
  return ArgSlice{args_.data() + sd.arg_start, sd.arg_count};
}

size_t Transaction::ReverseArgIndex(ShardId shard_id, size_t arg_index) const {
  if (unique_shard_cnt_ == 1)
    return arg_index;

  return reverse_index_[shard_data_[shard_id].arg_start + arg_index];
}

bool Transaction::WaitOnWatch(const time_point& tp) {
  // Assumes that transaction is pending and scheduled. TODO: To verify it with state machine.
  VLOG(2) << "WaitOnWatch Start use_count(" << use_count() << ")";
  using namespace chrono;

  // wake_txid_.store(kuint64max, std::memory_order_relaxed);
  Execute([](auto* t, auto* shard) { return t->AddToWatchedShardCb(shard); }, true);
  coordinator_state_ |= COORD_BLOCKED;
  bool res = true;  // returns false if timeout occurs.

  auto wake_cb = [this] {
    return (coordinator_state_ & COORD_CANCELLED) ||
           notify_txid_.load(memory_order_relaxed) != kuint64max;
  };
  cv_status status = cv_status::no_timeout;

  if (tp == time_point::max()) {
    DVLOG(1) << "WaitOnWatch foreva " << DebugId();
    blocking_ec_.await(move(wake_cb));
    DVLOG(1) << "WaitOnWatch AfterWait";
  } else {
    DVLOG(1) << "WaitOnWatch TimeWait for "
             << duration_cast<milliseconds>(tp - time_point::clock::now()).count() << " ms";

    status = blocking_ec_.await_until(move(wake_cb), tp);

    DVLOG(1) << "WaitOnWatch await_until " << int(status);
  }

  if ((coordinator_state_ & COORD_CANCELLED) || status == cv_status::timeout) {
    ExpireBlocking();
    coordinator_state_ &= ~COORD_BLOCKED;
    return false;
  }

  // We were notified by a shard, so lets make sure that our notifications converged to a stable
  // form.
  if (unique_shard_cnt_ > 1) {
    run_count_.store(unique_shard_cnt_, memory_order_release);
    auto converge_cb = [this] {
      EngineShard* shard = EngineShard::tlocal();
      auto& sd = shard_data_[shard->shard_id()];

      TxId notify = notify_txid();
      if ((sd.local_mask & AWAKED_Q) || shard->HasResultConverged(notify)) {
        CHECK_GE(DecreaseRunCnt(), 1u);
        return;
      }
      shard->WaitForConvergence(notify, this);
    };

    for (ShardId i = 0; i < shard_data_.size(); ++i) {
      auto& sd = shard_data_[i];
      DCHECK_EQ(0, sd.local_mask & ARMED);
      if (sd.arg_count == 0)
        continue;
      ess_->Add(i, converge_cb);
    }

    // Wait for all callbacks to conclude.
    WaitForShardCallbacks();
    DVLOG(1) << "Convergence finished " << DebugId();
  }

  // Lift blocking mask.
  coordinator_state_ &= ~COORD_BLOCKED;

  return res;
}

void Transaction::UnregisterWatch() {
  auto cb = [](Transaction* t, EngineShard* shard) {
    t->RemoveFromWatchedShardCb(shard);
    return OpStatus::OK;
  };
  Execute(std::move(cb), true);
}

// Runs only in the shard thread.
OpStatus Transaction::AddToWatchedShardCb(EngineShard* shard) {
  ShardId sid = SidToId(shard->shard_id());

  auto& sd = shard_data_[sid];
  CHECK_EQ(0, sd.local_mask & SUSPENDED_Q);
  DCHECK_EQ(0, sd.local_mask & ARMED);

  auto args = ShardArgsInShard(shard->shard_id());
  for (auto s : args) {
    shard->AddWatched(s, this);
  }
  sd.local_mask |= SUSPENDED_Q;

  return OpStatus::OK;
}

// Runs only in the shard thread.
// Quadratic complexity in number of arguments and queue length.
bool Transaction::RemoveFromWatchedShardCb(EngineShard* shard) {
  ShardId sid = SidToId(shard->shard_id());
  auto& sd = shard_data_[sid];

  constexpr uint16_t kQueueMask =
      -Transaction::SUSPENDED_Q | Transaction::AWAKED_Q | Transaction::EXPIRED_Q;

  if ((sd.local_mask & kQueueMask) == 0)
    return false;

  sd.local_mask &= kQueueMask;

  // TODO: what if args have keys and values?
  auto args = ShardArgsInShard(shard->shard_id());
  for (auto s : args) {
    shard->RemovedWatched(s, this);
  }
  return true;
}

inline uint32_t Transaction::DecreaseRunCnt() {
  // to protect against cases where Transaction is destroyed before run_ec_.notify
  // finishes running. We can not put it inside the (res == 1) block because then it's too late.
  ::boost::intrusive_ptr guard(this);

  // We use release so that no stores will be reordered after.
  uint32_t res = run_count_.fetch_sub(1, std::memory_order_release);
  if (res == 1) {
    run_ec_.notify();
  }
  return res;
}

bool Transaction::IsGlobal() const {
  return (cid_->opt_mask() & CO::GLOBAL_TRANS) != 0;
}

// Runs only in the shard thread.
bool Transaction::NotifySuspended(TxId committed_txid, ShardId sid) {
  unsigned sd_id = SidToId(sid);
  auto& sd = shard_data_[sd_id];
  unsigned local_mask = sd.local_mask;
  CHECK_NE(0u, local_mask & SUSPENDED_Q);
  DVLOG(1) << "NotifyBlocked " << DebugId() << ", local_mask: " << local_mask;

  if (local_mask & Transaction::EXPIRED_Q) {
    return false;
  }

  if (local_mask & SUSPENDED_Q) {
    DCHECK_EQ(0u, local_mask & AWAKED_Q);

    sd.local_mask &= ~SUSPENDED_Q;
    sd.local_mask |= AWAKED_Q;

    TxId notify_id = notify_txid_.load(memory_order_relaxed);

    while (committed_txid < notify_id) {
      if (notify_txid_.compare_exchange_weak(notify_id, committed_txid, memory_order_relaxed)) {
        // if we improved notify_txid_ - break.
        blocking_ec_.notify();  // release barrier.
        break;
      }
    }
    return true;
  }

  CHECK(sd.local_mask & AWAKED_Q);
  return true;
}

void Transaction::BreakOnClose() {
  if (coordinator_state_ & COORD_BLOCKED) {
    coordinator_state_ |= COORD_CANCELLED;
    blocking_ec_.notify();
  }
}

auto Transaction::FindFirst() -> OpResult<FindFirstResult> {
  FindFirstProcessor processor(notify_txid_.load(memory_order_relaxed), ess_->size());

  processor.Find(this);

  return processor.Process(this);
}

}  // namespace dfly