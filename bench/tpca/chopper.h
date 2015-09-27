#pragma once

#include "coordinator.h"
#include "./bench/tpca/piece.h"

namespace rococo {

class TpcaPaymentChopper : public TxnChopper {

public:

    TpcaPaymentChopper() {
    }

    virtual void init(TxnRequest &req) ;

    virtual bool start_callback(const std::vector<int> &pi, int res, BatchStartArgsHelper &bsah) {
        return false;
    }

    virtual bool start_callback(int pi, int res, const std::vector<mdb::Value> &output) { return false; }

    virtual bool is_read_only() { return false; }

    virtual void retry() {
        n_started_ = 0;
        n_prepared_ = 0;
        n_finished_ = 0;
        status_ = {0, 0, 0};
        commit_.store(true);
        proxies_.clear();
        n_try_ ++;
    }

    virtual ~TpcaPaymentChopper() {}

};

} // namespace rococo
