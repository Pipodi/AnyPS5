#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/MissAhead.hpp"
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using AgcDriver::DriverDetail::MissAheadLedger;
using AgcDriver::DriverDetail::MissAheadPlan;
using AgcDriver::DriverDetail::MissAheadRange;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void CheckPlan() {
    MissAheadPlan plan(3);
    Require(plan.Visit() && plan.Visit() && plan.Visit(), "three visits fit the cap");
    Require(!plan.Visit(), "a fourth visit exceeds the cap");
    Require(plan.Visited() == 3, "visits are counted up to the cap");
    Require(plan.Admit(7), "a new variant key is admitted");
    Require(!plan.Admit(7), "a repeated variant key is deduplicated");
    Require(plan.Admit(8), "another key is admitted");
    MissAheadPlan none(0);
    Require(!none.Visit(), "a zero cap visits nothing");
}

void CheckRange() {
    MissAheadRange range;
    Require(!range.Covered(1, 0, 0), "nothing is covered before a scan");
    range.Record(1, 0, 100);
    Require(range.Covered(1, 0, 40), "a miss inside the scanned range is covered");
    Require(!range.Covered(1, 0, 100), "a miss at the scan end starts a new scan");
    Require(!range.Covered(1, 0, 140), "a miss past the scan end starts a new scan");
    Require(!range.Covered(2, 0, 40), "another submission is not covered");
    Require(!range.Covered(1, 1, 40), "another queue is not covered");
    range.Record(1, 0, 200);
    Require(range.Covered(1, 0, 150), "the recorded range moves with the scan");
}

void CheckLedger() {
    MissAheadLedger ledger;
    Require(!ledger.Active(), "a new ledger is idle");
    Require(!ledger.Used(1), "an unknown key is not a use");
    ledger.Started(1);
    ledger.Started(2);
    ledger.Started(3);
    Require(ledger.Active(), "a started build activates the ledger");
    ledger.Finished(1);
    Require(ledger.Used(1), "a finished build is used");
    Require(ledger.Used(2), "a building build is used");
    Require(!ledger.Used(2), "a build is used once");
    const auto totals = ledger.Snapshot();
    Require(totals.started == 3 && totals.hit == 1 && totals.waited == 1, "totals split used builds by whether they had finished");
    Require(totals.Unused() == 1, "the remaining build is unused");
    for (std::uint64_t key = 100; key < 100 + MissAheadLedger::Retained + 10; ++key) ledger.Started(key);
    Require(!ledger.Used(3), "the retained set is bounded");
}

}

int main() {
    try {
        CheckPlan();
        CheckRange();
        CheckLedger();
        std::cout << "miss ahead tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
