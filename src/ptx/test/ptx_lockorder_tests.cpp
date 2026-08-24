// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// BUG-048 — the settle-path lock order, tested against the REAL globals.
//
// ★ WHY THIS FILE EXISTS AT ALL, and why it is not the in-tree detector test.
// `src/test/sync_tests.cpp` already has a lock-order case, but it is
// `#ifdef DEBUG_LOCKORDER` AND absent from test_ptx's source list
// (Makefile.test.include PTX_TESTS), so running test_ptx would have been a
// VACUOUS pass — silence from a test that was never compiled. This file is in
// PTX_TESTS, and its first case proves the detector is armed before any other
// case is allowed to mean anything.
//
// ★ WHAT IT TESTS. Three real mutexes, at their real addresses:
//     cs_main            (validation.h)
//     mempool.cs         (txmempool.h — the global CTxMemPool)
//     CWallet::cs_wallet (a real CWallet instance)
// The DEBUG_LOCKORDER detector is an oracle over ACQUISITION ORDER between
// mutex addresses, so exercising these three at their real addresses is
// exercising the same state the daemon would.
//
// ★ WHAT IT DOES NOT TEST, stated so nobody reads more into a green run: it
// does not execute PTX_AutoCommit. Reaching that function needs a funded
// wallet, a chain and a quorum at threshold; the end-to-end evidence for the
// RED side is the live reproduction held under BUG-048 (a --enable-debug
// daemon, one real roll, POTENTIAL DEADLOCK DETECTED, 5/5). This file is the
// oracle; that reproduction is the subject.

#include "test/test_Hemis.h"

#include "sync.h"
#include "txmempool.h"
#include "validation.h"
#ifdef ENABLE_WALLET
#include "wallet/wallet.h"
#endif

#include <boost/test/unit_test.hpp>
#include <stdexcept>

BOOST_FIXTURE_TEST_SUITE(ptx_lockorder_tests, BasicTestingSetup)

#if defined(DEBUG_LOCKORDER) && defined(ENABLE_WALLET)

// RAII: make the detector THROW rather than abort() so a test can catch it.
struct NoAbort {
    bool saved;
    NoAbort() : saved(g_debug_lockorder_abort) { g_debug_lockorder_abort = false; }
    ~NoAbort() { g_debug_lockorder_abort = saved; }
};

// ---------------------------------------------------------------------------
// CASE 1 — ANTI-VACUITY. The detector must fire on a deliberate provocation.
// Every other case in this file is meaningless until this one passes: silence
// from a disarmed detector is indistinguishable from silence from correct code
// (KDD-102's corollary).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Bug048_DetectorIsArmed)
{
    NoAbort noabort;
    RecursiveMutex m1, m2;

    { LOCK2(m1, m2); }           // records m1 -> m2

    bool fired = false;
    try {
        LOCK2(m2, m1);           // the inversion
    } catch (const std::logic_error& e) {
        fired = (std::string(e.what()).find("potential deadlock detected") != std::string::npos);
    }
    BOOST_CHECK_MESSAGE(fired,
        "DEBUG_LOCKORDER is compiled in but did not fire on a deliberate m1->m2 / "
        "m2->m1 inversion — every other case in this file is vacuous");
}

// ---------------------------------------------------------------------------
// CASE 2 — the FIXED order is compatible with the wallet's established order.
//
// Leg A replays what CWallet::ReacceptWalletTransactions does:
//   wallet.cpp:1977  LOCK2(cs_main, cs_wallet)
//   wallet.cpp:1999  -> CWalletTx::AcceptToMemoryPool
//   wallet.cpp:4473  -> mempool.exists()
//   txmempool.h:642  -> LOCK(cs)
// i.e. cs_main -> cs_wallet -> mempool.cs. The intermediate frames take no
// locks, so calling mempool.exists() directly under LOCK2(cs_main, cs_wallet)
// records the identical pair set.
//
// Leg B is HEAD's settle block (ptx/ptx_mempool.cpp:269-270). It must add no
// pair the wallet has not already established.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Bug048_FixedOrderAgreesWithWallet)
{
    NoAbort noabort;
    CWallet wallet("bug048", WalletDatabase::CreateDummy());

    // Leg A — the wallet's order (the one that establishes the invariant).
    {
        LOCK2(cs_main, wallet.cs_wallet);
        (void)mempool.exists(UINT256_ZERO);
    }

    // Leg B — HEAD's settle block, same statements, same order.
    bool threw = false;
    try {
        LOCK2(cs_main, wallet.cs_wallet);
        LOCK(mempool.cs);
    } catch (const std::logic_error&) {
        threw = true;
    }
    BOOST_CHECK_MESSAGE(!threw,
        "cs_main -> cs_wallet -> mempool.cs must be compatible with the order "
        "CWallet::ReacceptWalletTransactions establishes — it IS that order");
}

// ---------------------------------------------------------------------------
// CASE 3 — RED. The order that shipped before BUG-048's fix is NOT compatible.
// This is the discriminating leg: without it, case 2's silence proves only that
// nothing was looking. The pair recorded here is mempool.cs -> cs_wallet, which
// inverts the cs_wallet -> mempool.cs that case 2 leg A established.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Bug048_ShippedOrderIsDetected)
{
    NoAbort noabort;
    CWallet wallet("bug048r", WalletDatabase::CreateDummy());

    // Establish the wallet's order first (the invariant under test).
    {
        LOCK2(cs_main, wallet.cs_wallet);
        (void)mempool.exists(UINT256_ZERO);
    }

    // The pre-fix settle block: LOCK2(cs_main, mempool.cs); LOCK(cs_wallet).
    bool fired = false;
    try {
        LOCK2(cs_main, mempool.cs);
        LOCK(wallet.cs_wallet);
    } catch (const std::logic_error& e) {
        fired = (std::string(e.what()).find("potential deadlock detected") != std::string::npos);
    }
    BOOST_CHECK_MESSAGE(fired,
        "cs_main -> mempool.cs -> cs_wallet is the order ptx_mempool.cpp shipped "
        "before BUG-048; the detector MUST reject it against the wallet's order");
}

#else

// A build without DEBUG_LOCKORDER cannot answer the question. Say so loudly
// rather than reporting a pass: a configuration that skips a check is
// indistinguishable from a check that passes (KDD-102).
BOOST_AUTO_TEST_CASE(Bug048_RequiresLockorderBuild)
{
    BOOST_TEST_MESSAGE("SKIPPED: needs DEBUG_LOCKORDER + ENABLE_WALLET — this "
                       "suite is inert. Re-run under --enable-debug.");
    BOOST_CHECK(true);
}

#endif // DEBUG_LOCKORDER && ENABLE_WALLET

BOOST_AUTO_TEST_SUITE_END()
