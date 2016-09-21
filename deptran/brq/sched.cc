

#include "sched.h"
#include "commo.h"
#include "../rcc/dtxn.h"

using namespace rococo;


map<txnid_t, RccDTxn*> BrqSched::Aggregate(RccGraph &graph) {
  // aggregate vertexes
  map<txnid_t, RccDTxn*> index;
  for (auto& pair: graph.vertex_index()) {
    RccDTxn* rhs_v = pair.second;
    verify(pair.first == rhs_v->id());
    RccDTxn* vertex = AggregateVertex(rhs_v);
    RccDTxn& dtxn = *vertex;
    if (dtxn.epoch_ == 0) {
      dtxn.epoch_ = epoch_mgr_.curr_epoch_;
    }
    epoch_mgr_.AddToEpoch(dtxn.epoch_, dtxn.tid_, dtxn.IsDecided());
    verify(vertex->id() == pair.second->id());
    verify(vertex_index().count(vertex->id()) > 0);
    index[vertex->id()] = vertex;
  }
  // aggregate edges.
  RebuildEdgePointer(index);

#ifdef DEBUG_CODE
  verify(index.size() == graph.vertex_index_.size());
  for (auto& pair: index) {
    txnid_t txnid = pair.first;
    RccVertex* v = pair.second;
    verify(v->parents_.size() == v->incoming_.size());
    auto sz = v->parents_.size();
    if (v->Get().status() >= TXN_CMT)
      RccSched::__DebugCheckParentSetSize(txnid, sz);
  }

  for (auto& pair: graph.vertex_index_) {
    auto txnid = pair.first;
    RccVertex* rhs_v = pair.second;
    auto lhs_v = FindV(txnid);
    verify(lhs_v != nullptr);
    // TODO, check the Sccs are the same.
    if (rhs_v->Get().status() >= TXN_DCD) {
      verify(lhs_v->Get().status() >= TXN_DCD);
      if (!AllAncCmt(rhs_v))
        continue;
      RccScc& rhs_scc = graph.FindSCC(rhs_v);
      for (RccVertex* rhs_vv : rhs_scc) {
        verify(rhs_vv->Get().status() >= TXN_DCD);
        RccVertex* lhs_vv = FindV(rhs_vv->id());
        verify(lhs_vv != nullptr);
        verify(lhs_vv->Get().status() >= TXN_DCD);
        verify(lhs_vv->GetParentSet() == rhs_vv->GetParentSet());
      }
      if (!AllAncCmt(lhs_v)) {
        continue;
      }
      RccScc& lhs_scc = FindSCC(lhs_v);
      for (RccVertex* lhs_vv : rhs_scc) {
        verify(lhs_vv->Get().status() >= TXN_DCD);
        RccVertex* rhs_vv = graph.FindV(lhs_v->id());
        verify(rhs_vv != nullptr);
        verify(rhs_vv->Get().status() >= TXN_DCD);
        verify(rhs_vv->GetParentSet() == rhs_vv->GetParentSet());
      }

      auto lhs_sz = lhs_scc.size();
      auto rhs_sz = rhs_scc.size();
      if (lhs_sz != rhs_sz) {
        // TODO
        for (auto& vv : rhs_scc) {
          auto vvv = FindV(vv->id());
          verify(vvv->Get().status() >= TXN_DCD);
        }
        verify(0);
      }
//      verify(sz >= rhs_sz);
//      verify(sz == rhs_sz);
      for (auto vv : rhs_scc) {
        bool r = std::any_of(lhs_scc.begin(),
                             lhs_scc.end(),
                             [vv] (RccVertex* vvv) -> bool {
                               return vvv->id() == vv->id();
                             }
        );
        verify(r);
      }
    }

    if (lhs_v->Get().status() >= TXN_DCD && AllAncCmt(lhs_v)) {
      RccScc& scc = FindSCC(lhs_v);
      for (auto vv : scc) {
        auto s = vv->Get().status();
        verify(s >= TXN_DCD);
      }
    }
  }

#endif
//  this->BuildEdgePointer(graph, index);
  return index;
}



void BrqSched::OnPreAccept(const txnid_t txn_id,
                           const vector<SimpleCommand>& cmds,
                           const RccGraph& graph,
                           int32_t* res,
                           RccGraph* res_graph,
                           function<void()> callback) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
//  Log_info("on preaccept: %llx par: %d", txn_id, (int)partition_id_);
//  if (RandomGenerator::rand(1, 2000) <= 1)
//    Log_info("on pre-accept graph size: %d", graph.size());
  verify(txn_id > 0);
  verify(cmds[0].root_id_ == txn_id);
  Aggregate(const_cast<RccGraph&>(graph));
  TriggerCheckAfterAggregation(const_cast<RccGraph&>(graph));
  // TODO FIXME
  // add interference based on cmds.
  RccDTxn *dtxn = (RccDTxn *) GetOrCreateDTxn(txn_id);
//  TxnInfo& tinfo = dtxn->tv_->Get();
  if (dtxn->status() < TXN_CMT) {
    if (dtxn->phase_ < PHASE_RCC_DISPATCH && dtxn->status() < TXN_CMT) {
      for (auto& c: cmds) {
        map<int32_t, Value> output;
        dtxn->DispatchExecute(c, res, &output);
      }
    }
  } else {
    if (dtxn->dreqs_.size() == 0) {
      for (auto& c: cmds) {
        dtxn->dreqs_.push_back(c);
      }
    }
  }
  verify(!dtxn->fully_dispatched);
  dtxn->fully_dispatched = true;
  MinItfrGraph(txn_id, res_graph, false, 1);
  if (dtxn->status() >= TXN_CMT) {
    waitlist_.insert(dtxn);
    verify(dtxn->epoch_ > 0);
  }
  *res = SUCCESS;
  callback();
}

void BrqSched::OnPreAcceptWoGraph(const txnid_t txn_id,
                                  const vector<SimpleCommand>& cmds,
                                  int32_t* res,
                                  RccGraph* res_graph,
                                  function<void()> callback) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
//  Log_info("on preaccept: %llx par: %d", txn_id, (int)partition_id_);
//  if (RandomGenerator::rand(1, 2000) <= 1)
//    Log_info("on pre-accept graph size: %d", graph.size());
  verify(txn_id > 0);
  verify(cmds[0].root_id_ == txn_id);
//  dep_graph_->Aggregate(const_cast<RccGraph&>(graph));
//  TriggerCheckAfterAggregation(const_cast<RccGraph&>(graph));
  // TODO FIXME
  // add interference based on cmds.
  RccDTxn *dtxn = (RccDTxn *) GetOrCreateDTxn(txn_id);
  RccDTxn& tinfo = *dtxn;
  if (tinfo.status() < TXN_CMT) {
    if (dtxn->phase_ < PHASE_RCC_DISPATCH && tinfo.status() < TXN_CMT) {
      for (auto& c: cmds) {
        map<int32_t, Value> output;
        dtxn->DispatchExecute(c, res, &output);
      }
    }
  } else {
    if (dtxn->dreqs_.size() == 0) {
      for (auto& c: cmds) {
        dtxn->dreqs_.push_back(c);
      }
    }
  }
  verify(!tinfo.fully_dispatched);
  tinfo.fully_dispatched = true;
  MinItfrGraph(txn_id, res_graph, false, 1);
  if (tinfo.status() >= TXN_CMT) {
    waitlist_.insert(dtxn);
    verify(dtxn->epoch_ > 0);
  }
  *res = SUCCESS;
  callback();
}


void BrqSched::OnAccept(const txnid_t txn_id,
                        const ballot_t& ballot,
                        const RccGraph& graph,
                        int32_t* res,
                        function<void()> callback) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  // TODO
  *res = SUCCESS;
  callback();
}

void BrqSched::OnCommit(const txnid_t cmd_id,
                        const RccGraph& graph,
                        int32_t* res,
                        TxnOutput* output,
                        const function<void()>& callback) {
  // TODO to support cascade abort
  std::lock_guard<std::recursive_mutex> lock(mtx_);
//  if (RandomGenerator::rand(1, 2000) <= 1)
//    Log_info("on commit graph size: %d", graph.size());
  *res = SUCCESS;
  // union the graph into dep graph
  RccDTxn *dtxn = (RccDTxn*) GetOrCreateDTxn(cmd_id);
  verify(dtxn != nullptr);
  RccDTxn& info = *dtxn;

  verify(dtxn->ptr_output_repy_ == nullptr);
  dtxn->ptr_output_repy_ = output;

  if (info.IsExecuted()) {
    verify(info.status() >= TXN_DCD);
    verify(info.graphs_for_inquire_.size() == 0);
    *res = SUCCESS;
    callback();
  } else if (info.IsAborted()) {
    verify(0);
    *res = REJECT;
    callback();
  } else {
//    Log_info("on commit: %llx par: %d", cmd_id, (int)partition_id_);
    dtxn->commit_request_received_ = true;
    dtxn->finish_reply_callback_ = [callback, res] (int r) {
      *res = r;
//      verify(r == SUCCESS);
      callback();
    };
    auto index = Aggregate(const_cast<RccGraph&> (graph));
    for (auto& pair: index) {
      verify(pair.second->epoch_ > 0);
    }
    TriggerCheckAfterAggregation(const_cast<RccGraph &>(graph));
    // fast path without check wait list?
//    if (graph.size() == 1) {
//      auto v = dep_graph_->FindV(cmd_id);
//      if (v->incoming_.size() == 0);
//      CheckInquired(v->Get());
//      Execute(v->Get());
//      return;
//    } else {
//      Log_debug("graph size on commit, %d", (int) graph.size());
////    verify(0);
//    }
  }

}


void BrqSched::OnCommitWoGraph(const txnid_t cmd_id,
                               int32_t* res,
                               TxnOutput* output,
                               const function<void()>& callback) {
  // TODO to support cascade abort
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  *res = SUCCESS;
  // union the graph into dep graph
  RccDTxn *dtxn = (RccDTxn*) GetOrCreateDTxn(cmd_id);
  verify(dtxn != nullptr);
  RccDTxn& info = *dtxn;

  verify(dtxn->ptr_output_repy_ == nullptr);
  dtxn->ptr_output_repy_ = output;

  if (info.IsExecuted()) {
    verify(info.status() >= TXN_DCD);
    verify(info.graphs_for_inquire_.size() == 0);
    *res = SUCCESS;
    callback();
  } else if (info.IsAborted()) {
    verify(0);
    *res = REJECT;
    callback();
  } else {
//    Log_info("on commit: %llx par: %d", cmd_id, (int)partition_id_);
    dtxn->commit_request_received_ = true;
    dtxn->finish_reply_callback_ = [callback, res] (int r) {
      *res = r;
//      verify(r == SUCCESS);
      callback();
    };
    UpgradeStatus(dtxn, TXN_CMT);
    waitlist_.insert(dtxn);
    verify(dtxn->epoch_ > 0);
    CheckWaitlist();
    // fast path without check wait list?
//    if (graph.size() == 1) {
//      auto v = dep_graph_->FindV(cmd_id);
//      if (v->incoming_.size() == 0);
//      CheckInquired(v->Get());
//      Execute(v->Get());
//      return;
//    } else {
//      Log_debug("graph size on commit, %d", (int) graph.size());
////    verify(0);
//    }
  }
}

int BrqSched::OnInquire(epoch_t epoch,
                        cmdid_t cmd_id,
                        RccGraph *graph,
                        const function<void()> &callback) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  // TODO check epoch, cannot be a too old one.
  RccDTxn *dtxn = (RccDTxn *) GetOrCreateDTxn(cmd_id);
  RccDTxn& info = *dtxn;
  //register an event, triggered when the status >= COMMITTING;
  verify (info.Involve(Scheduler::partition_id_));

  auto cb_wrapper = [callback, graph] () {
#ifdef DEBUG_CODE
    for (auto pair : graph->vertex_index_) {
      RccVertex* v = pair.second;
      TxnInfo& tinfo = v->Get();
      if (tinfo.status() >= TXN_CMT) {
//        Log_info("inquire ack, txnid: %llx, parent size: %d",
//                 pair.first, v->GetParentSet().size());
        RccSched::__DebugCheckParentSetSize(v->id(), v->parents_.size());
      }
    }
#endif
    callback();
  };

  if (info.status() >= TXN_CMT) {
    MinItfrGraph(cmd_id, graph, false, 1);
    cb_wrapper();
  } else {
    info.graphs_for_inquire_.push_back(graph);
    info.callbacks_for_inquire_.push_back(cb_wrapper);
    verify(info.graphs_for_inquire_.size() ==
        info.callbacks_for_inquire_.size());
    waitlist_.insert(dtxn);
    verify(dtxn->epoch_ > 0);
  }

}

BrqCommo* BrqSched::commo() {

  auto commo = dynamic_cast<BrqCommo*>(commo_);
  verify(commo != nullptr);
  return commo;
}
