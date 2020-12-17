// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin Core developers
// Copyright (c) 2017-2019 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "validationinterface.h"
#include "scheduler.h"
#include "sync.h"
#include "util.h"

#include <list>
#include <atomic>

#include <unordered_map>
#include <boost/signals2/signal.hpp>

//! The MainSignalsInstance manages a list of shared_ptr<CValidationInterface>
//! callbacks.
//!
//! A std::unordered_map is used to track what callbacks are currently
//! registered, and a std::list is to used to store the callbacks that are
//! currently registered as well as any callbacks that are just unregistered
//! and about to be deleted when they are done executing.
struct MainSignalsInstance {
private:
    Mutex m_mutex;
    //! List entries consist of a callback pointer and reference count. The
    //! count is equal to the number of current executions of that entry, plus 1
    //! if it's registered. It cannot be 0 because that would imply it is
    //! unregistered and also not being executed (so shouldn't exist).
    struct ListEntry { std::shared_ptr<CValidationInterface> callbacks; int count = 1; };
    std::list<ListEntry> m_list GUARDED_BY(m_mutex);
    std::unordered_map<CValidationInterface*, std::list<ListEntry>::iterator> m_map GUARDED_BY(m_mutex);

public:
    // We are not allowed to assume the scheduler only runs in one thread,
    // but must ensure all callbacks happen in-order, so we end up creating
    // our own queue here :(
    SingleThreadedSchedulerClient m_schedulerClient;

    explicit MainSignalsInstance(CScheduler *pscheduler) : m_schedulerClient(pscheduler) {}

    void Register(std::shared_ptr<CValidationInterface> callbacks)
    {
        LOCK(m_mutex);
        auto inserted = m_map.emplace(callbacks.get(), m_list.end());
        if (inserted.second) inserted.first->second = m_list.emplace(m_list.end());
        inserted.first->second->callbacks = std::move(callbacks);
    }

    void Unregister(CValidationInterface* callbacks)
    {
        LOCK(m_mutex);
        auto it = m_map.find(callbacks);
        if (it != m_map.end()) {
            if (!--it->second->count) m_list.erase(it->second);
            m_map.erase(it);
        }
    }

    //! Clear unregisters every previously registered callback, erasing every
    //! map entry. After this call, the list may still contain callbacks that
    //! are currently executing, but it will be cleared when they are done
    //! executing.
    void Clear()
    {
        LOCK(m_mutex);
        for (auto it = m_list.begin(); it != m_list.end();) {
            it = --it->count ? std::next(it) : m_list.erase(it);
        }
        m_map.clear();
    }

    template<typename F> void Iterate(F&& f)
    {
        WAIT_LOCK(m_mutex, lock);
        for (auto it = m_list.begin(); it != m_list.end();) {
            ++it->count;
            {
                REVERSE_LOCK(lock);
                f(*it->callbacks);
            }
            it = --it->count ? std::next(it) : m_list.erase(it);
        }
    }
};

static CMainSignals g_signals;

void CMainSignals::RegisterBackgroundSignalScheduler(CScheduler& scheduler) {
    assert(!m_internals);
    m_internals.reset(new MainSignalsInstance(&scheduler));
}

void CMainSignals::UnregisterBackgroundSignalScheduler() {
    m_internals.reset(nullptr);
}

void CMainSignals::FlushBackgroundCallbacks() {
    if (m_internals) {
        m_internals->m_schedulerClient.EmptyQueue();
    }
}

CMainSignals& GetMainSignals()
{
    return g_signals;
}

void RegisterValidationInterface(CValidationInterface* pwalletIn)
{
    std::shared_ptr<CValidationInterface> sharedValidation = std::make_shared<CValidationInterface>(*pwalletIn);
    g_signals.m_internals->Register(std::move(sharedValidation));
}

void UnregisterValidationInterface(CValidationInterface* pwalletIn)
{
    if (g_signals.m_internals) {
        g_signals.m_internals->Unregister(pwalletIn);
    }
}

void UnregisterAllValidationInterfaces()
{
    if (!g_signals.m_internals) {
        return;
    }
    g_signals.m_internals->Clear();
}

void CMainSignals::UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork, bool fInitialDownload) {
    m_internals->Iterate([&](CValidationInterface& callbacks) { callbacks.UpdatedBlockTip(pindexNew, pindexFork, fInitialDownload); });
}

void CMainSignals::SyncTransaction(const CTransaction& tx, const CBlockIndex* pindex, int posInBlock) {
    m_internals->Iterate([&](CValidationInterface& callbacks) { callbacks.SyncTransaction(tx, pindex, posInBlock); });
}

void CMainSignals::NotifyTransactionLock(const CTransaction& tx) {
    m_internals->Iterate([&](CValidationInterface& callbacks) { callbacks.NotifyTransactionLock(tx); });
}

void CMainSignals::UpdatedTransaction(const uint256& hash) {
    m_internals->Iterate([&](CValidationInterface& callbacks) { callbacks.UpdatedTransaction(hash); });
}

void CMainSignals::SetBestChain(const CBlockLocator& locator) {
    m_internals->Iterate([&](CValidationInterface& callbacks) { callbacks.SetBestChain(locator); });
}

void CMainSignals::Broadcast(CConnman* connman) {
    m_internals->Iterate([&](CValidationInterface& callbacks) { callbacks.Broadcast(connman); });
}

void CMainSignals::BlockChecked(const CBlock& block, const CValidationState& state) {
    m_internals->Iterate([&](CValidationInterface& callbacks) { callbacks.BlockChecked(block, state); });
}

void CMainSignals::BlockFound(const uint256& hash) {
    m_internals->Iterate([&](CValidationInterface& callbacks) { callbacks.BlockFound(hash); });
}

void CMainSignals::ChainTip(const CBlockIndex* pindex, const CBlock* block, Optional<SaplingMerkleTree> tree) {
    m_internals->Iterate([&](CValidationInterface& callbacks) { callbacks.ChainTip(pindex, block, tree); });
}