// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Copyright (c) 2017-2019 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_VALIDATIONINTERFACE_H
#define BITCOIN_VALIDATIONINTERFACE_H

#include "optional.h"
#include "sapling/incrementalmerkletree.h"
#include "primitives/transaction.h"

class CBlock;
struct CBlockLocator;
class CBlockIndex;
class CConnman;
class CValidationInterface;
class CValidationState;
class uint256;
class CScheduler;
class CTxMemPool;
enum class MemPoolRemovalReason;

// These functions dispatch to one or all registered wallets

/** Register a wallet to receive updates from core */
void RegisterValidationInterface(CValidationInterface* pwalletIn);
/** Unregister a wallet from core */
void UnregisterValidationInterface(CValidationInterface* pwalletIn);
/** Unregister all wallets from core */
void UnregisterAllValidationInterfaces();

class CValidationInterface {
public:
    virtual ~CValidationInterface() = default;
protected:
    /** Notifies listeners of updated block chain tip */
    virtual void UpdatedBlockTip(const CBlockIndex *pindexNew, const CBlockIndex *pindexFork, bool fInitialDownload) {}
    virtual void TransactionAddedToMempool(const CTransactionRef &ptxn) {}
    /**
     * Notifies listeners of a transaction leaving mempool.
     *
     * This only fires for transactions which leave mempool because of expiry,
     * size limiting, reorg (changes in lock times/coinbase/coinstake maturity), or
     * replacement. This does not include any transactions which are included
     * in BlockConnectedDisconnected either in block->vtx or in txnConflicted.
     */
    virtual void TransactionRemovedFromMempool(const CTransactionRef &ptx) {}
    virtual void BlockConnected(const std::shared_ptr<const CBlock> &block, const CBlockIndex *pindex, const std::vector<CTransactionRef> &txnConflicted) {}
    virtual void BlockDisconnected(const std::shared_ptr<const CBlock> &block, int nBlockHeight) {}
    /** Notifies listeners of the new active block chain on-disk. */
    virtual void SetBestChain(const CBlockLocator &locator) {}
    /** Tells listeners to broadcast their data. */
    virtual void ResendWalletTransactions(CConnman* connman) {}
    virtual void BlockChecked(const CBlock&, const CValidationState&) {}
    friend void ::RegisterValidationInterface(CValidationInterface*);
    friend void ::UnregisterValidationInterface(CValidationInterface*);
    friend void ::UnregisterAllValidationInterfaces();
};

struct MainSignalsInstance;
class CMainSignals {
private:
    std::unique_ptr<MainSignalsInstance> m_internals;

    void MempoolEntryRemoved(CTransactionRef tx, MemPoolRemovalReason reason);

    friend void ::RegisterValidationInterface(CValidationInterface*);
    friend void ::UnregisterValidationInterface(CValidationInterface*);
    friend void ::UnregisterAllValidationInterfaces();

public:
    /** Register a CScheduler to give callbacks which should run in the background (may only be called once) */
    void RegisterBackgroundSignalScheduler(CScheduler& scheduler);
    /** Unregister a CScheduler to give callbacks which should run in the background - these callbacks will now be dropped! */
    void UnregisterBackgroundSignalScheduler();
    /** Call any remaining callbacks on the calling thread */
    void FlushBackgroundCallbacks();

    /** Register with mempool to call TransactionRemovedFromMempool callbacks */
    void RegisterWithMempoolSignals(CTxMemPool& pool);
    /** Unregister with mempool */
    void UnregisterWithMempoolSignals(CTxMemPool& pool);

    void UpdatedBlockTip(const CBlockIndex *, const CBlockIndex *, bool fInitialDownload);
    void TransactionAddedToMempool(const CTransactionRef &ptxn);
    void BlockConnected(const std::shared_ptr<const CBlock> &block, const CBlockIndex *pindex, const std::vector<CTransactionRef> &txnConflicted);
    void BlockDisconnected(const std::shared_ptr<const CBlock> &block, int nBlockHeight);
    void SetBestChain(const CBlockLocator &);
    void Broadcast(CConnman* connman);
    void BlockChecked(const CBlock&, const CValidationState&);
};

CMainSignals& GetMainSignals();

#endif // BITCOIN_VALIDATIONINTERFACE_H
