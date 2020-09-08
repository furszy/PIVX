// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "masternode-budget.h"

#include "init.h"
#include "addrman.h"
#include "chainparams.h"
#include "fs.h"
#include "masternode-sync.h"
#include "masternode.h"
#include "masternodeman.h"
#include "netmessagemaker.h"
#include "net_processing.h"
#include "util.h"


CBudgetManager budget;

std::map<uint256, int64_t> askedForSourceProposalOrBudget;

int nSubmittedFinalBudget;

bool IsBudgetCollateralValid(const uint256& nTxCollateralHash, const uint256& nExpectedHash, std::string& strError, int64_t& nTime, int& nConf, bool fBudgetFinalization)
{
    CTransaction txCollateral;
    uint256 nBlockHash;
    if (!GetTransaction(nTxCollateralHash, txCollateral, nBlockHash, true)) {
        strError = strprintf("Can't find collateral tx %s", txCollateral.ToString());
        LogPrint(BCLog::MNBUDGET,"%s: %s\n", __func__, strError);
        return false;
    }

    if (txCollateral.vout.size() < 1) return false;
    if (txCollateral.nLockTime != 0) return false;

    CScript findScript;
    findScript << OP_RETURN << ToByteVector(nExpectedHash);

    bool foundOpReturn = false;
    for (const CTxOut &o : txCollateral.vout) {
        if (!o.scriptPubKey.IsNormalPaymentScript() && !o.scriptPubKey.IsUnspendable()) {
            strError = strprintf("Invalid Script %s", txCollateral.ToString());
            LogPrint(BCLog::MNBUDGET,"%s: %s\n", __func__, strError);
            return false;
        }
        if (fBudgetFinalization) {
            // Collateral for budget finalization
            // Note: there are still old valid budgets out there, but the check for the new 5 PIV finalization collateral
            //       will also cover the old 50 PIV finalization collateral.
            LogPrint(BCLog::MNBUDGET, "Final Budget: o.scriptPubKey(%s) == findScript(%s) ?\n", HexStr(o.scriptPubKey), HexStr(findScript));
            if (o.scriptPubKey == findScript) {
                LogPrint(BCLog::MNBUDGET, "Final Budget: o.nValue(%ld) >= BUDGET_FEE_TX(%ld) ?\n", o.nValue, BUDGET_FEE_TX);
                if(o.nValue >= BUDGET_FEE_TX) {
                    foundOpReturn = true;
                }
            }
        }
        else {
            // Collateral for normal budget proposal
            LogPrint(BCLog::MNBUDGET, "Normal Budget: o.scriptPubKey(%s) == findScript(%s) ?\n", HexStr(o.scriptPubKey), HexStr(findScript));
            if (o.scriptPubKey == findScript) {
                LogPrint(BCLog::MNBUDGET, "Normal Budget: o.nValue(%ld) >= PROPOSAL_FEE_TX(%ld) ?\n", o.nValue, PROPOSAL_FEE_TX);
                if(o.nValue >= PROPOSAL_FEE_TX) {
                    foundOpReturn = true;
                }
            }
        }
    }
    if (!foundOpReturn) {
        strError = strprintf("Couldn't find opReturn %s in %s", nExpectedHash.ToString(), txCollateral.ToString());
        LogPrint(BCLog::MNBUDGET,"%s: %s\n", __func__, strError);
        return false;
    }

    // RETRIEVE CONFIRMATIONS AND NTIME
    /*
        - nTime starts as zero and is passed-by-reference out of this function and stored in the external proposal
        - nTime is never validated via the hashing mechanism and comes from a full-validated source (the blockchain)
    */

    int conf = GetIXConfirmations(nTxCollateralHash);
    if (!nBlockHash.IsNull()) {
        BlockMap::iterator mi = mapBlockIndex.find(nBlockHash);
        if (mi != mapBlockIndex.end() && (*mi).second) {
            CBlockIndex* pindex = (*mi).second;
            if (chainActive.Contains(pindex)) {
                conf += chainActive.Height() - pindex->nHeight + 1;
                nTime = pindex->nTime;
            }
        }
    }

    nConf = conf;

    //if we're syncing we won't have swiftTX information, so accept 1 confirmation
    const int nRequiredConfs = Params().GetConsensus().nBudgetFeeConfirmations;
    if (conf >= nRequiredConfs) {
        return true;
    } else {
        strError = strprintf("Collateral requires at least %d confirmations - %d confirmations", nRequiredConfs, conf);
        LogPrint(BCLog::MNBUDGET,"%s: %s\n", __func__, strError);
        return false;
    }
}

void CBudgetManager::CheckOrphanVotes()
{
    std::string strError = "";
    {
        LOCK(cs_votes);
        for (auto it = mapOrphanProposalVotes.begin(); it != mapOrphanProposalVotes.end();) {
            if (UpdateProposal(it->second, nullptr, strError))
                it = mapOrphanProposalVotes.erase(it);
            else
                ++it;
        }
    }
    {
        LOCK(cs_finalizedvotes);
        for (auto it = mapOrphanFinalizedBudgetVotes.begin(); it != mapOrphanFinalizedBudgetVotes.end();) {
            if (UpdateFinalizedBudget(it->second, nullptr, strError))
                it = mapOrphanFinalizedBudgetVotes.erase(it);
            else
                ++it;
        }
    }
    LogPrint(BCLog::MNBUDGET,"%s: Done\n", __func__);
}

void CBudgetManager::SubmitFinalBudget()
{
    static int nSubmittedHeight = 0; // height at which final budget was submitted last time
    int nCurrentHeight = GetBestHeight();

    const int nBlocksPerCycle = Params().GetConsensus().nBudgetCycleBlocks;
    int nBlockStart = nCurrentHeight - nCurrentHeight % nBlocksPerCycle + nBlocksPerCycle;
    if (nSubmittedHeight >= nBlockStart){
        LogPrint(BCLog::MNBUDGET,"%s: nSubmittedHeight(=%ld) < nBlockStart(=%ld) condition not fulfilled.\n",
                __func__, nSubmittedHeight, nBlockStart);
        return;
    }

     // Submit final budget during the last 2 days (2880 blocks) before payment for Mainnet, about 9 minutes (9 blocks) for Testnet
    int finalizationWindow = ((nBlocksPerCycle / 30) * 2);

    if (Params().NetworkID() == CBaseChainParams::TESTNET) {
        // NOTE: 9 blocks for testnet is way to short to have any masternode submit an automatic vote on the finalized(!) budget,
        //       because those votes are only submitted/relayed once every 56 blocks in CFinalizedBudget::AutoCheck()

        finalizationWindow = 64; // 56 + 4 finalization confirmations + 4 minutes buffer for propagation
    }

    int nFinalizationStart = nBlockStart - finalizationWindow;

    int nOffsetToStart = nFinalizationStart - nCurrentHeight;

    if (nBlockStart - nCurrentHeight > finalizationWindow) {
        LogPrint(BCLog::MNBUDGET,"%s: Too early for finalization. Current block is %ld, next Superblock is %ld.\n", __func__, nCurrentHeight, nBlockStart);
        LogPrint(BCLog::MNBUDGET,"%s: First possible block for finalization: %ld. Last possible block for finalization: %ld. "
                "You have to wait for %ld block(s) until Budget finalization will be possible\n", __func__, nFinalizationStart, nBlockStart, nOffsetToStart);
        return;
    }

    std::vector<CBudgetProposal*> vBudgetProposals = budget.GetBudget();
    std::string strBudgetName = "main";
    std::vector<CTxBudgetPayment> vecTxBudgetPayments;

    for (auto & vBudgetProposal : vBudgetProposals) {
        CTxBudgetPayment txBudgetPayment;
        txBudgetPayment.nProposalHash = vBudgetProposal->GetHash();
        txBudgetPayment.payee = vBudgetProposal->GetPayee();
        txBudgetPayment.nAmount = vBudgetProposal->GetAllotted();
        vecTxBudgetPayments.push_back(txBudgetPayment);
    }

    if (vecTxBudgetPayments.size() < 1) {
        LogPrint(BCLog::MNBUDGET,"%s: Found No Proposals For Period\n", __func__);
        return;
    }

    CFinalizedBudgetBroadcast tempBudget(strBudgetName, nBlockStart, vecTxBudgetPayments, UINT256_ZERO);
    const uint256& budgetHash = tempBudget.GetHash();
    if (HaveSeenFinalizedBudget(budgetHash)) {
        LogPrint(BCLog::MNBUDGET,"%s: Budget already exists - %s\n", __func__, budgetHash.ToString());
        nSubmittedHeight = nCurrentHeight;
        return; //already exists
    }

    //create fee tx
    CTransaction tx;
    uint256 txidCollateral;

    if (!mapCollateralTxids.count(budgetHash)) {
        CWalletTx wtx;
        // Get our change address
        CReserveKey keyChange(pwalletMain);
        if (!pwalletMain->CreateBudgetFeeTX(wtx, budgetHash, keyChange, true)) {
            LogPrint(BCLog::MNBUDGET,"%s: Can't make collateral transaction\n", __func__);
            return;
        }

        // Send the tx to the network. Do NOT use SwiftTx, locking might need too much time to propagate, especially for testnet
        const CWallet::CommitResult& res = pwalletMain->CommitTransaction(wtx, keyChange, g_connman.get(), "NO-ix");
        if (res.status != CWallet::CommitStatus::OK)
            return;
        tx = (CTransaction)wtx;
        txidCollateral = tx.GetHash();
        mapCollateralTxids.emplace(budgetHash, txidCollateral);
    } else {
        txidCollateral = mapCollateralTxids[budgetHash];
    }

    //create the proposal incase we're the first to make it
    CFinalizedBudgetBroadcast finalizedBudgetBroadcast(strBudgetName, nBlockStart, vecTxBudgetPayments, txidCollateral);

    // check
    int nConf = 0;
    int64_t nTime = 0;
    std::string strError = "";
    if (!IsBudgetCollateralValid(txidCollateral, finalizedBudgetBroadcast.GetHash(), strError, nTime, nConf, true)) {
        LogPrint(BCLog::MNBUDGET,"%s: Invalid Collateral for finalized budget - %s \n", __func__, strError);
        return;
    }

    if (!finalizedBudgetBroadcast.UpdateValid(nCurrentHeight)) {
        LogPrint(BCLog::MNBUDGET,"%s: Invalid finalized budget - %s \n", __func__, finalizedBudgetBroadcast.IsInvalidReason());
        return;
    }

    AddSeenFinalizedBudget(finalizedBudgetBroadcast);
    finalizedBudgetBroadcast.Relay();
    AddFinalizedBudget(finalizedBudgetBroadcast);
    nSubmittedHeight = nCurrentHeight;
    LogPrint(BCLog::MNBUDGET,"%s: Done! %s\n", __func__, finalizedBudgetBroadcast.GetHash().ToString());
}

//
// CBudgetDB
//

CBudgetDB::CBudgetDB()
{
    pathDB = GetDataDir() / "budget.dat";
    strMagicMessage = "MasternodeBudget";
}

bool CBudgetDB::Write(const CBudgetManager& objToSave)
{
    int64_t nStart = GetTimeMillis();

    // serialize, checksum data up to that point, then append checksum
    CDataStream ssObj(SER_DISK, CLIENT_VERSION);
    ssObj << strMagicMessage;                   // masternode cache file specific magic message
    ssObj << FLATDATA(Params().MessageStart()); // network specific magic number
    ssObj << objToSave;
    uint256 hash = Hash(ssObj.begin(), ssObj.end());
    ssObj << hash;

    // open output file, and associate with CAutoFile
    FILE* file = fsbridge::fopen(pathDB, "wb");
    CAutoFile fileout(file, SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("%s : Failed to open file %s", __func__, pathDB.string());

    // Write and commit header, data
    try {
        fileout << ssObj;
    } catch (const std::exception& e) {
        return error("%s : Serialize or I/O error - %s", __func__, e.what());
    }
    fileout.fclose();

    LogPrint(BCLog::MNBUDGET,"Written info to budget.dat  %dms\n", GetTimeMillis() - nStart);

    return true;
}

CBudgetDB::ReadResult CBudgetDB::Read(CBudgetManager& objToLoad, bool fDryRun)
{
    int64_t nStart = GetTimeMillis();
    // open input file, and associate with CAutoFile
    FILE* file = fsbridge::fopen(pathDB, "rb");
    CAutoFile filein(file, SER_DISK, CLIENT_VERSION);
    if (filein.IsNull()) {
        error("%s : Failed to open file %s", __func__, pathDB.string());
        return FileError;
    }

    // use file size to size memory buffer
    int fileSize = fs::file_size(pathDB);
    int dataSize = fileSize - sizeof(uint256);
    // Don't try to resize to a negative number if file is small
    if (dataSize < 0)
        dataSize = 0;
    std::vector<unsigned char> vchData;
    vchData.resize(dataSize);
    uint256 hashIn;

    // read data and checksum from file
    try {
        filein.read((char*)&vchData[0], dataSize);
        filein >> hashIn;
    } catch (const std::exception& e) {
        error("%s : Deserialize or I/O error - %s", __func__, e.what());
        return HashReadError;
    }
    filein.fclose();

    CDataStream ssObj(vchData, SER_DISK, CLIENT_VERSION);

    // verify stored checksum matches input data
    uint256 hashTmp = Hash(ssObj.begin(), ssObj.end());
    if (hashIn != hashTmp) {
        error("%s : Checksum mismatch, data corrupted", __func__);
        return IncorrectHash;
    }


    unsigned char pchMsgTmp[4];
    std::string strMagicMessageTmp;
    try {
        // de-serialize file header (masternode cache file specific magic message) and ..
        ssObj >> strMagicMessageTmp;

        // ... verify the message matches predefined one
        if (strMagicMessage != strMagicMessageTmp) {
            error("%s : Invalid masternode cache magic message", __func__);
            return IncorrectMagicMessage;
        }


        // de-serialize file header (network specific magic number) and ..
        ssObj >> FLATDATA(pchMsgTmp);

        // ... verify the network matches ours
        if (memcmp(pchMsgTmp, Params().MessageStart(), sizeof(pchMsgTmp))) {
            error("%s : Invalid network magic number", __func__);
            return IncorrectMagicNumber;
        }

        // de-serialize data into CBudgetManager object
        ssObj >> objToLoad;
    } catch (const std::exception& e) {
        objToLoad.Clear();
        error("%s : Deserialize or I/O error - %s", __func__, e.what());
        return IncorrectFormat;
    }

    LogPrint(BCLog::MNBUDGET,"Loaded info from budget.dat  %dms\n", GetTimeMillis() - nStart);
    LogPrint(BCLog::MNBUDGET,"%s\n", objToLoad.ToString());
    if (!fDryRun) {
        LogPrint(BCLog::MNBUDGET,"Budget manager - cleaning....\n");
        objToLoad.CheckAndRemove();
        LogPrint(BCLog::MNBUDGET,"Budget manager - result: %s\n", objToLoad.ToString());
    }

    return Ok;
}

void DumpBudgets()
{
    int64_t nStart = GetTimeMillis();

    CBudgetDB budgetdb;
    CBudgetManager tempBudget;

    LogPrint(BCLog::MNBUDGET,"Verifying budget.dat format...\n");
    CBudgetDB::ReadResult readResult = budgetdb.Read(tempBudget, true);
    // there was an error and it was not an error on file opening => do not proceed
    if (readResult == CBudgetDB::FileError)
        LogPrint(BCLog::MNBUDGET,"Missing budgets file - budget.dat, will try to recreate\n");
    else if (readResult != CBudgetDB::Ok) {
        LogPrint(BCLog::MNBUDGET,"Error reading budget.dat: ");
        if (readResult == CBudgetDB::IncorrectFormat)
            LogPrint(BCLog::MNBUDGET,"magic is ok but data has invalid format, will try to recreate\n");
        else {
            LogPrint(BCLog::MNBUDGET,"file format is unknown or invalid, please fix it manually\n");
            return;
        }
    }
    LogPrint(BCLog::MNBUDGET,"Writting info to budget.dat...\n");
    budgetdb.Write(budget);

    LogPrint(BCLog::MNBUDGET,"Budget dump finished  %dms\n", GetTimeMillis() - nStart);
}

void CBudgetManager::SetBudgetProposalsStr(CFinalizedBudget& finalizedBudget) const
{
    const std::vector<uint256>& vHashes = finalizedBudget.GetProposalsHashes();
    std::string strProposals = "";
    {
        LOCK(cs_proposals);
        for (const uint256& hash: vHashes) {
            const std::string token = (mapProposals.count(hash) ? mapProposals.at(hash).GetName() : hash.ToString());
            strProposals += (strProposals == "" ? "" : ", ") + token;
        }
    }
    finalizedBudget.SetProposalsStr(strProposals);
}

std::string CBudgetManager::GetFinalizedBudgetStatus(const uint256& nHash) const
{
    CFinalizedBudget fb;
    if (!GetFinalizedBudget(nHash, fb))
        return strprintf("ERROR: cannot find finalized budget %s\n", nHash.ToString());

    std::string retBadHashes = "";
    std::string retBadPayeeOrAmount = "";
    int nBlockStart = fb.GetBlockStart();
    int nBlockEnd = fb.GetBlockEnd();

    for (int nBlockHeight = nBlockStart; nBlockHeight <= nBlockEnd; nBlockHeight++) {
        CTxBudgetPayment budgetPayment;
        if (!fb.GetBudgetPaymentByBlock(nBlockHeight, budgetPayment)) {
            LogPrint(BCLog::MNBUDGET,"%s: Couldn't find budget payment for block %lld\n", __func__, nBlockHeight);
            continue;
        }

        CBudgetProposal bp;
        if (!GetProposal(budgetPayment.nProposalHash, bp)) {
            retBadHashes += (retBadHashes == "" ? "" : ", ") + budgetPayment.nProposalHash.ToString();
            continue;
        }

        if (bp.GetPayee() != budgetPayment.payee || bp.GetAmount() != budgetPayment.nAmount) {
            retBadPayeeOrAmount += (retBadPayeeOrAmount == "" ? "" : ", ") + budgetPayment.nProposalHash.ToString();
        }
    }

    if (retBadHashes == "" && retBadPayeeOrAmount == "") return "OK";

    if (retBadHashes != "") retBadHashes = "Unknown proposal(s) hash! Check this proposal(s) before voting: " + retBadHashes;
    if (retBadPayeeOrAmount != "") retBadPayeeOrAmount = "Budget payee/nAmount doesn't match our proposal(s)! "+ retBadPayeeOrAmount;

    return retBadHashes + " -- " + retBadPayeeOrAmount;
}

bool CBudgetManager::AddFinalizedBudget(CFinalizedBudget& finalizedBudget)
{
    const uint256& nHash = finalizedBudget.GetHash();
    if (WITH_LOCK(cs_budgets, return mapFinalizedBudgets.count(nHash))) {
        LogPrint(BCLog::MNBUDGET,"%s: finalized budget %s already added\n", __func__, nHash.ToString());
        return false;
    }

    if (!finalizedBudget.UpdateValid(GetBestHeight())) {
        LogPrint(BCLog::MNBUDGET,"%s: invalid finalized budget - %s\n", __func__, finalizedBudget.IsInvalidReason());
        return false;
    }

    SetBudgetProposalsStr(finalizedBudget);
    WITH_LOCK(cs_budgets, mapFinalizedBudgets.emplace(nHash, finalizedBudget); );
    return true;
}

bool CBudgetManager::AddProposal(CBudgetProposal& budgetProposal)
{
    LOCK(cs_proposals);
    if (!budgetProposal.UpdateValid(GetBestHeight())) {
        LogPrint(BCLog::MNBUDGET,"%s: invalid budget proposal - %s\n", __func__, budgetProposal.IsInvalidReason());
        return false;
    }

    const uint256& hash = budgetProposal.GetHash();

    if (mapProposals.count(hash)) {
        return false;
    }

    mapProposals.emplace(hash, budgetProposal);
    LogPrint(BCLog::MNBUDGET,"%s: proposal %s added\n", __func__, budgetProposal.GetName());
    return true;
}

void CBudgetManager::CheckAndRemove()
{
    int nCurrentHeight = GetBestHeight();
    std::map<uint256, CFinalizedBudget> tmpMapFinalizedBudgets;
    std::map<uint256, CBudgetProposal> tmpMapProposals;

    {
        LOCK(cs_budgets);
        LogPrint(BCLog::MNBUDGET, "%s: mapFinalizedBudgets cleanup - size before: %d\n", __func__, mapFinalizedBudgets.size());
        for (auto& it: mapFinalizedBudgets) {
            CFinalizedBudget* pfinalizedBudget = &(it.second);
            if (!pfinalizedBudget->UpdateValid(nCurrentHeight)) {
                LogPrint(BCLog::MNBUDGET,"%s: Invalid finalized budget: %s\n", __func__, pfinalizedBudget->IsInvalidReason());
            } else {
                LogPrint(BCLog::MNBUDGET,"%s: Found valid finalized budget: %s %s\n", __func__,
                          pfinalizedBudget->GetName(), pfinalizedBudget->GetFeeTXHash().ToString());
                pfinalizedBudget->CheckAndVote();
                tmpMapFinalizedBudgets.emplace(pfinalizedBudget->GetHash(), *pfinalizedBudget);
            }
        }
        // Remove invalid entries by overwriting complete map
        mapFinalizedBudgets.swap(tmpMapFinalizedBudgets);
        LogPrint(BCLog::MNBUDGET, "%s: mapFinalizedBudgets cleanup - size after: %d\n", __func__, mapFinalizedBudgets.size());
    }

    {
        LOCK(cs_proposals);
        LogPrint(BCLog::MNBUDGET, "%s: mapProposals cleanup - size before: %d\n", __func__, mapProposals.size());
        for (auto& it: mapProposals) {
            CBudgetProposal* pbudgetProposal = &(it.second);
            if (!pbudgetProposal->UpdateValid(nCurrentHeight)) {
                LogPrint(BCLog::MNBUDGET,"%s: Invalid budget proposal - %s\n", __func__, pbudgetProposal->IsInvalidReason());
            } else {
                 LogPrint(BCLog::MNBUDGET,"%s: Found valid budget proposal: %s %s\n", __func__,
                          pbudgetProposal->GetName(), pbudgetProposal->GetFeeTXHash().ToString());
                 tmpMapProposals.emplace(pbudgetProposal->GetHash(), *pbudgetProposal);
            }
        }
        // Remove invalid entries by overwriting complete map
        mapProposals.swap(tmpMapProposals);
        LogPrint(BCLog::MNBUDGET, "%s: mapProposals cleanup - size after: %d\n", __func__, mapProposals.size());
    }

}
const CFinalizedBudget* CBudgetManager::GetBudgetWithHighestVoteCount(int chainHeight) const
{
    LOCK(cs_budgets);
    int highestVoteCount = 0;
    const CFinalizedBudget* pHighestBudget = nullptr;
    for (const auto& it: mapFinalizedBudgets) {
        const CFinalizedBudget* pfinalizedBudget = &(it.second);
        int voteCount = pfinalizedBudget->GetVoteCount();
        if (voteCount > highestVoteCount &&
            chainHeight >= pfinalizedBudget->GetBlockStart() &&
            chainHeight <= pfinalizedBudget->GetBlockEnd()) {
            pHighestBudget = pfinalizedBudget;
            highestVoteCount = voteCount;
        }
    }
    return pHighestBudget;
}

int CBudgetManager::GetHighestVoteCount(int chainHeight) const
{
    const CFinalizedBudget* pbudget = GetBudgetWithHighestVoteCount(chainHeight);
    return (pbudget ? pbudget->GetVoteCount() : -1);
}

bool CBudgetManager::GetPayeeAndAmount(int chainHeight, CScript& payeeRet, CAmount& nAmountRet) const
{
    const CFinalizedBudget* pfb = GetBudgetWithHighestVoteCount(chainHeight);
    return pfb && pfb->GetPayeeAndAmount(chainHeight, payeeRet, nAmountRet);
}

void CBudgetManager::FillBlockPayee(CMutableTransaction& txNew, bool fProofOfStake) const
{
    int chainHeight = GetBestHeight();
    if (chainHeight <= 0) return;

    CScript payee;
    CAmount nAmount = 0;

    if (!GetPayeeAndAmount(chainHeight + 1, payee, nAmount))
        return;

    CAmount blockValue = GetBlockValue(chainHeight + 1);

    if (fProofOfStake) {
        unsigned int i = txNew.vout.size();
        txNew.vout.resize(i + 1);
        txNew.vout[i].scriptPubKey = payee;
        txNew.vout[i].nValue = nAmount;
    } else {
        //miners get the full amount on these blocks
        txNew.vout[0].nValue = blockValue;
        txNew.vout.resize(2);

        //these are super blocks, so their value can be much larger than normal
        txNew.vout[1].scriptPubKey = payee;
        txNew.vout[1].nValue = nAmount;
    }

    CTxDestination address;
    ExtractDestination(payee, address);
    LogPrint(BCLog::MNBUDGET,"%s: Budget payment to %s for %lld\n", __func__, EncodeDestination(address), nAmount);
}

CFinalizedBudget* CBudgetManager::FindFinalizedBudget(const uint256& nHash)
{
    AssertLockHeld(cs_budgets);

    if (mapFinalizedBudgets.count(nHash))
        return &mapFinalizedBudgets[nHash];

    return NULL;
}

const CBudgetProposal* CBudgetManager::FindProposalByName(const std::string& strProposalName) const
{
    LOCK(cs_proposals);

    int64_t nYesCountMax = std::numeric_limits<int64_t>::min();
    const CBudgetProposal* pbudgetProposal = nullptr;

    for (const auto& it: mapProposals) {
        const CBudgetProposal& proposal = it.second;
        int64_t nYesCount = proposal.GetYeas() - proposal.GetNays();
        if (proposal.GetName() == strProposalName && nYesCount > nYesCountMax) {
            pbudgetProposal = &proposal;
            nYesCountMax = nYesCount;
        }
    }

    return pbudgetProposal;
}

CBudgetProposal* CBudgetManager::FindProposal(const uint256& nHash)
{
    AssertLockHeld(cs_proposals);

    if (mapProposals.count(nHash))
        return &mapProposals[nHash];

    return NULL;
}

bool CBudgetManager::GetProposal(const uint256& nHash, CBudgetProposal& bp) const
{
    LOCK(cs_proposals);
    if (mapProposals.count(nHash)) {
        bp = mapProposals.at(nHash);
        return true;
    }
    return false;
}

bool CBudgetManager::GetFinalizedBudget(const uint256& nHash, CFinalizedBudget& fb) const
{
    LOCK(cs_budgets);
    if (mapFinalizedBudgets.count(nHash)) {
        fb = mapFinalizedBudgets.at(nHash);
        return true;
    }
    return false;
}

bool CBudgetManager::IsBudgetPaymentBlock(int nBlockHeight, int& nHighestCount, int& nFivePercent) const
{
    nHighestCount = GetHighestVoteCount(nBlockHeight);
    nFivePercent = mnodeman.CountEnabled(ActiveProtocol()) / 20;

    LogPrint(BCLog::MNBUDGET,"%s: nHighestCount: %lli, 5%% of Masternodes: %lli.\n",
            __func__, nHighestCount, nFivePercent);

    // If budget doesn't have 5% of the network votes, then we should pay a masternode instead
    return (nHighestCount > nFivePercent);
}

bool CBudgetManager::IsBudgetPaymentBlock(int nBlockHeight) const
{
    int nHighestCount, nFivePercent;
    return IsBudgetPaymentBlock(nBlockHeight, nHighestCount, nFivePercent);
}

TrxValidationStatus CBudgetManager::IsTransactionValid(const CTransaction& txNew, int nBlockHeight) const
{
    int nHighestCount = 0, nFivePercent = 0;
    if (!IsBudgetPaymentBlock(nBlockHeight, nHighestCount, nFivePercent)) {
        // If budget doesn't have 5% of the network votes, then we should pay a masternode instead
        return TrxValidationStatus::InValid;
    }

    // check the highest finalized budgets (+/- 10% to assist in consensus)
    bool fThreshold = false;
    int nCountThreshold = nHighestCount - 2 * nFivePercent;
    {
        LOCK(cs_budgets);
        for (const auto& it: mapFinalizedBudgets) {
            const CFinalizedBudget* pfb = &(it.second);
            const int nVoteCount = pfb->GetVoteCount();
            LogPrint(BCLog::MNBUDGET,"%s: checking (%s): votes %d (threshold %d)\n", __func__, pfb->GetProposalsStr(), nVoteCount, nCountThreshold);
            if (nVoteCount > nCountThreshold) {
                fThreshold = true;
                if (pfb->IsTransactionValid(txNew, nBlockHeight) == TrxValidationStatus::Valid) {
                    return TrxValidationStatus::Valid;
                }
                // tx not valid. keep looking.
                LogPrint(BCLog::MNBUDGET, "%s: ignoring budget. Out of range or tx not valid.\n", __func__);
            }
        }
    }

    // If not enough masternodes autovoted for any of the finalized budgets or if none of the txs
    // are valid, we should pay a masternode instead
    return fThreshold ? TrxValidationStatus::InValid : TrxValidationStatus::VoteThreshold;
}

std::vector<CBudgetProposal*> CBudgetManager::GetAllProposals()
{
    LOCK(cs_proposals);

    std::vector<CBudgetProposal*> vBudgetProposalRet;

    for (auto& it: mapProposals) {
        CBudgetProposal* pbudgetProposal = &(it.second);
        pbudgetProposal->CleanAndRemove();
        vBudgetProposalRet.push_back(pbudgetProposal);
    }

    std::sort(vBudgetProposalRet.begin(), vBudgetProposalRet.end(), CBudgetProposal::PtrHigherYes);

    return vBudgetProposalRet;
}

//Need to review this function
std::vector<CBudgetProposal*> CBudgetManager::GetBudget()
{
    LOCK(cs_proposals);

    int nHeight = GetBestHeight();
    if (nHeight <= 0)
        return std::vector<CBudgetProposal*>();

    // ------- Sort budgets by net Yes Count
    std::vector<CBudgetProposal*> vBudgetPorposalsSort;
    for (auto& it: mapProposals) {
        it.second.CleanAndRemove();
        vBudgetPorposalsSort.push_back(&it.second);
    }
    std::sort(vBudgetPorposalsSort.begin(), vBudgetPorposalsSort.end(), CBudgetProposal::PtrHigherYes);

    // ------- Grab The Budgets In Order
    std::vector<CBudgetProposal*> vBudgetProposalsRet;
    CAmount nBudgetAllocated = 0;

    const int nBlocksPerCycle = Params().GetConsensus().nBudgetCycleBlocks;
    int nBlockStart = nHeight - nHeight % nBlocksPerCycle + nBlocksPerCycle;
    int nBlockEnd = nBlockStart + nBlocksPerCycle - 1;
    int mnCount = mnodeman.CountEnabled(ActiveProtocol());
    CAmount nTotalBudget = GetTotalBudget(nBlockStart);

    for (CBudgetProposal* pbudgetProposal: vBudgetPorposalsSort) {
        LogPrint(BCLog::MNBUDGET,"%s: Processing Budget %s\n", __func__, pbudgetProposal->GetName());
        //prop start/end should be inside this period
        if (pbudgetProposal->IsPassing(nBlockStart, nBlockEnd, mnCount)) {
            LogPrint(BCLog::MNBUDGET,"%s:  -   Check 1 passed: valid=%d | %ld <= %ld | %ld >= %ld | Yeas=%d Nays=%d Count=%d | established=%d\n",
                    __func__, pbudgetProposal->IsValid(), pbudgetProposal->GetBlockStart(), nBlockStart, pbudgetProposal->GetBlockEnd(),
                    nBlockEnd, pbudgetProposal->GetYeas(), pbudgetProposal->GetNays(), mnCount / 10, pbudgetProposal->IsEstablished());

            if (pbudgetProposal->GetAmount() + nBudgetAllocated <= nTotalBudget) {
                pbudgetProposal->SetAllotted(pbudgetProposal->GetAmount());
                nBudgetAllocated += pbudgetProposal->GetAmount();
                vBudgetProposalsRet.push_back(pbudgetProposal);
                LogPrint(BCLog::MNBUDGET,"%s:  -     Check 2 passed: Budget added\n", __func__);
            } else {
                pbudgetProposal->SetAllotted(0);
                LogPrint(BCLog::MNBUDGET,"%s:  -     Check 2 failed: no amount allotted\n", __func__);
            }

        } else {
            LogPrint(BCLog::MNBUDGET,"%s:  -   Check 1 failed: valid=%d | %ld <= %ld | %ld >= %ld | Yeas=%d Nays=%d Count=%d | established=%d\n",
                    __func__, pbudgetProposal->IsValid(), pbudgetProposal->GetBlockStart(), nBlockStart, pbudgetProposal->GetBlockEnd(),
                    nBlockEnd, pbudgetProposal->GetYeas(), pbudgetProposal->GetNays(), mnodeman.CountEnabled(ActiveProtocol()) / 10,
                    pbudgetProposal->IsEstablished());
        }

    }

    return vBudgetProposalsRet;
}

std::vector<CFinalizedBudget*> CBudgetManager::GetFinalizedBudgets()
{
    LOCK(cs_budgets);

    std::vector<CFinalizedBudget*> vFinalizedBudgetsRet;

    // ------- Grab The Budgets In Order
    for (auto& it: mapFinalizedBudgets) {
        vFinalizedBudgetsRet.push_back(&(it.second));
    }
    std::sort(vFinalizedBudgetsRet.begin(), vFinalizedBudgetsRet.end(), CFinalizedBudget::PtrGreater);

    return vFinalizedBudgetsRet;
}

std::string CBudgetManager::GetRequiredPaymentsString(int nBlockHeight)
{
    LOCK(cs_budgets);

    std::string ret = "unknown-budget";

    std::map<uint256, CFinalizedBudget>::iterator it = mapFinalizedBudgets.begin();
    while (it != mapFinalizedBudgets.end()) {
        CFinalizedBudget* pfinalizedBudget = &((*it).second);
        if (nBlockHeight >= pfinalizedBudget->GetBlockStart() && nBlockHeight <= pfinalizedBudget->GetBlockEnd()) {
            CTxBudgetPayment payment;
            if (pfinalizedBudget->GetBudgetPaymentByBlock(nBlockHeight, payment)) {
                if (ret == "unknown-budget") {
                    ret = payment.nProposalHash.ToString();
                } else {
                    ret += ",";
                    ret += payment.nProposalHash.ToString();
                }
            } else {
                LogPrint(BCLog::MNBUDGET,"%s:  Couldn't find budget payment for block %d\n", __func__, nBlockHeight);
            }
        }

        ++it;
    }

    return ret;
}

CAmount CBudgetManager::GetTotalBudget(int nHeight)
{
    if (Params().NetworkID() == CBaseChainParams::TESTNET) {
        CAmount nSubsidy = 500 * COIN;
        return ((nSubsidy / 100) * 10) * 146;
    }

    //get block value and calculate from that
    CAmount nSubsidy = 0;
    const Consensus::Params& consensus = Params().GetConsensus();
    const bool isPoSActive = consensus.NetworkUpgradeActive(nHeight, Consensus::UPGRADE_POS);
    if (nHeight >= 151200 && !isPoSActive) {
        nSubsidy = 50 * COIN;
    } else if (isPoSActive && nHeight <= 302399) {
        nSubsidy = 50 * COIN;
    } else if (nHeight <= 345599 && nHeight >= 302400) {
        nSubsidy = 45 * COIN;
    } else if (nHeight <= 388799 && nHeight >= 345600) {
        nSubsidy = 40 * COIN;
    } else if (nHeight <= 431999 && nHeight >= 388800) {
        nSubsidy = 35 * COIN;
    } else if (nHeight <= 475199 && nHeight >= 432000) {
        nSubsidy = 30 * COIN;
    } else if (nHeight <= 518399 && nHeight >= 475200) {
        nSubsidy = 25 * COIN;
    } else if (nHeight <= 561599 && nHeight >= 518400) {
        nSubsidy = 20 * COIN;
    } else if (nHeight <= 604799 && nHeight >= 561600) {
        nSubsidy = 15 * COIN;
    } else if (nHeight <= 647999 && nHeight >= 604800) {
        nSubsidy = 10 * COIN;
    } else if (consensus.NetworkUpgradeActive(nHeight, Consensus::UPGRADE_ZC_V2)) {
        nSubsidy = 10 * COIN;
    } else {
        nSubsidy = 5 * COIN;
    }

    // Amount of blocks in a months period of time (using 1 minutes per) = (60*24*30)
    if (nHeight <= 172800) {
        return 648000 * COIN;
    } else {
        return ((nSubsidy / 100) * 10) * 1440 * 30;
    }
}

void CBudgetManager::AddSeenProposal(const CBudgetProposalBroadcast& prop)
{
    LOCK(cs_proposals);
    mapSeenProposals.emplace(prop.GetHash(), prop);
}

void CBudgetManager::AddSeenProposalVote(const CBudgetVote& vote)
{
    LOCK(cs_votes);
    mapSeenProposalVotes.emplace(vote.GetHash(), vote);
}

void CBudgetManager::AddSeenFinalizedBudget(const CFinalizedBudgetBroadcast& bud)
{
    LOCK(cs_budgets);
    mapSeenFinalizedBudgets.emplace(bud.GetHash(), bud);
}

void CBudgetManager::AddSeenFinalizedBudgetVote(const CFinalizedBudgetVote& vote)
{
    LOCK(cs_finalizedvotes);
    mapSeenFinalizedBudgetVotes.emplace(vote.GetHash(), vote);
}


CDataStream CBudgetManager::GetProposalVoteSerialized(const uint256& voteHash) const
{
    LOCK(cs_votes);
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss.reserve(1000);
    ss << mapSeenProposalVotes.at(voteHash);
    return ss;
}

CDataStream CBudgetManager::GetProposalSerialized(const uint256& propHash) const
{
    LOCK(cs_proposals);
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss.reserve(1000);
    ss << mapSeenProposals.at(propHash);
    return ss;
}

CDataStream CBudgetManager::GetFinalizedBudgetVoteSerialized(const uint256& voteHash) const
{
    LOCK(cs_finalizedvotes);
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss.reserve(1000);
    ss << mapSeenFinalizedBudgetVotes.at(voteHash);
    return ss;
}

CDataStream CBudgetManager::GetFinalizedBudgetSerialized(const uint256& budgetHash) const
{
    LOCK(cs_budgets);
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss.reserve(1000);
    ss << mapSeenFinalizedBudgets.at(budgetHash);
    return ss;
}

bool CBudgetManager::AddAndRelayProposalVote(const CBudgetVote& vote, std::string& strError)
{
    if (UpdateProposal(vote, nullptr, strError)) {
        AddSeenProposalVote(vote);
        vote.Relay();
        return true;
    }
    return false;
}

void CBudgetManager::NewBlock(int height)
{
    SetBestHeight(height);

    if (masternodeSync.RequestedMasternodeAssets <= MASTERNODE_SYNC_BUDGET) return;

    if (strBudgetMode == "suggest") { //suggest the budget we see
        SubmitFinalBudget();
    }

    int nCurrentHeight = GetBestHeight();
    //this function should be called 1/14 blocks, allowing up to 100 votes per day on all proposals
    if (nCurrentHeight % 14 != 0) return;

    // incremental sync with our peers
    if (masternodeSync.IsSynced()) {
        LogPrint(BCLog::MNBUDGET,"%s:  incremental sync started\n", __func__);
        if (rand() % 1440 == 0) {
            ClearSeen();
            ResetSync();
        }

        CBudgetManager* manager = this;
        g_connman->ForEachNode([manager](CNode* pnode){
            if (pnode->nVersion >= ActiveProtocol())
                manager->Sync(pnode, UINT256_ZERO, true);
        });
        MarkSynced();
    }

    CheckAndRemove();

    //remove invalid votes once in a while (we have to check the signatures and validity of every vote, somewhat CPU intensive)

    LogPrint(BCLog::MNBUDGET,"%s:  askedForSourceProposalOrBudget cleanup - size: %d\n", __func__, askedForSourceProposalOrBudget.size());
    for (auto it = askedForSourceProposalOrBudget.begin(); it !=  askedForSourceProposalOrBudget.end(); ) {
        if (it->second <= GetTime() - (60 * 60 * 24)) {
            it = askedForSourceProposalOrBudget.erase(it);
        } else {
            it++;
        }
    }
    {
        TRY_LOCK(cs_proposals, fBudgetNewBlock);
        if (!fBudgetNewBlock) return;
        LogPrint(BCLog::MNBUDGET,"%s:  mapProposals cleanup - size: %d\n", __func__, mapProposals.size());
        for (auto& it: mapProposals) {
            it.second.CleanAndRemove();
        }
        LogPrint(BCLog::MNBUDGET,"%s:  vecImmatureProposals cleanup - size: %d\n", __func__, vecImmatureProposals.size());
        std::vector<CBudgetProposalBroadcast>::iterator it = vecImmatureProposals.begin();
        while (it != vecImmatureProposals.end()) {
            std::string strError = "";
            int nConf = 0;
            const uint256& nHash = it->GetHash();
            if (!IsBudgetCollateralValid(it->GetFeeTXHash(), nHash, strError, it->nTime, nConf)) {
                ++it;
                continue;
            }
            if (!it->UpdateValid(nCurrentHeight)) {
                LogPrint(BCLog::MNBUDGET,"mprop (immature) - invalid budget proposal - %s\n", it->IsInvalidReason());
                it = vecImmatureProposals.erase(it);
                continue;
            }
            LogPrint(BCLog::MNBUDGET,"mprop (immature) - new budget - %s\n", nHash.ToString());
            CBudgetProposal budgetProposal(*it);
            if (AddProposal(budgetProposal)) {
                it->Relay();
            }
            it = vecImmatureProposals.erase(it);
        }
    }
    {
        TRY_LOCK(cs_budgets, fBudgetNewBlock);
        if (!fBudgetNewBlock) return;
        LogPrint(BCLog::MNBUDGET,"%s:  mapFinalizedBudgets cleanup - size: %d\n", __func__, mapFinalizedBudgets.size());
        for (auto& it: mapFinalizedBudgets) {
            it.second.CleanAndRemove();
        }
        LogPrint(BCLog::MNBUDGET,"%s:  vecImmatureFinalizedBudgets cleanup - size: %d\n", __func__, vecImmatureFinalizedBudgets.size());
        std::vector<CFinalizedBudgetBroadcast>::iterator it = vecImmatureFinalizedBudgets.begin();
        while (it != vecImmatureFinalizedBudgets.end()) {
            std::string strError = "";
            int nConf = 0;
            const uint256& nHash = it->GetHash();
            if (!IsBudgetCollateralValid(it->GetFeeTXHash(), nHash, strError, it->nTime, nConf, true)) {
                ++it;
                continue;
            }
            if (!it->UpdateValid(nCurrentHeight)) {
                LogPrint(BCLog::MNBUDGET,"fbs (immature) - invalid finalized budget - %s\n", it->IsInvalidReason());
                it = vecImmatureFinalizedBudgets.erase(it);
                continue;
            }
            LogPrint(BCLog::MNBUDGET,"fbs (immature) - new finalized budget - %s\n", nHash.ToString());
            CFinalizedBudget finalizedBudget(*it);
            if (AddFinalizedBudget(finalizedBudget)) {
                it->Relay();
            }
            it = vecImmatureFinalizedBudgets.erase(it);
        }
    }
    LogPrint(BCLog::MNBUDGET,"%s:  PASSED\n", __func__);
}

void CBudgetManager::ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
    // lite mode is not supported
    if (fLiteMode) return;
    if (!masternodeSync.IsBlockchainSynced()) return;

    int nCurrentHeight = GetBestHeight();

    if (strCommand == NetMsgType::BUDGETVOTESYNC) { //Masternode vote sync
        uint256 nProp;
        vRecv >> nProp;

        if (Params().NetworkID() == CBaseChainParams::MAIN) {
            if (nProp.IsNull()) {
                if (pfrom->HasFulfilledRequest("budgetvotesync")) {
                    LogPrint(BCLog::MNBUDGET,"mnvs - peer already asked me for the list\n");
                    LOCK(cs_main);
                    Misbehaving(pfrom->GetId(), 20);
                    return;
                }
                pfrom->FulfilledRequest("budgetvotesync");
            }
        }

        Sync(pfrom, nProp);
        LogPrint(BCLog::MNBUDGET, "mnvs - Sent Masternode votes to peer %i\n", pfrom->GetId());
    }

    if (strCommand == NetMsgType::BUDGETPROPOSAL) { //Masternode Proposal
        CBudgetProposalBroadcast budgetProposalBroadcast;
        vRecv >> budgetProposalBroadcast;

        if (HaveSeenProposal(budgetProposalBroadcast.GetHash())) {
            masternodeSync.AddedBudgetItem(budgetProposalBroadcast.GetHash());
            return;
        }

        std::string strError = "";
        int nConf = 0;
        const uint256& nHash = budgetProposalBroadcast.GetHash();
        const uint256& nFeeTXHash = budgetProposalBroadcast.GetFeeTXHash();
        if (!IsBudgetCollateralValid(nFeeTXHash, nHash, strError, budgetProposalBroadcast.nTime, nConf)) {
            LogPrint(BCLog::MNBUDGET,"Proposal FeeTX is not valid - %s - %s\n", nFeeTXHash.ToString(), strError);
            if (nConf >= 1) {
                LOCK(cs_proposals);
                vecImmatureProposals.push_back(budgetProposalBroadcast);
            }
            return;
        }

        AddSeenProposal(budgetProposalBroadcast);

        if (!budgetProposalBroadcast.UpdateValid(nCurrentHeight)) {
            LogPrint(BCLog::MNBUDGET,"mprop - invalid budget proposal - %s\n", budgetProposalBroadcast.IsInvalidReason());
            return;
        }

        CBudgetProposal budgetProposal(budgetProposalBroadcast);
        if (AddProposal(budgetProposal)) {
            budgetProposalBroadcast.Relay();
        }
        masternodeSync.AddedBudgetItem(nHash);

        LogPrint(BCLog::MNBUDGET,"mprop - new budget - %s\n", nHash.ToString());

        //We might have active votes for this proposal that are valid now
        CheckOrphanVotes();
    }

    if (strCommand == NetMsgType::BUDGETVOTE) { // Budget Vote
        CBudgetVote vote;
        vRecv >> vote;
        vote.SetValid(true);

        if (HaveSeenProposalVote(vote.GetHash())) {
            masternodeSync.AddedBudgetItem(vote.GetHash());
            return;
        }

        const CTxIn& voteVin = vote.GetVin();
        CMasternode* pmn = mnodeman.Find(voteVin);
        if (pmn == NULL) {
            LogPrint(BCLog::MNBUDGET,"mvote - unknown masternode - vin: %s\n", voteVin.ToString());
            mnodeman.AskForMN(pfrom, voteVin);
            return;
        }

        AddSeenProposalVote(vote);

        if (!vote.CheckSignature()) {
            if (masternodeSync.IsSynced()) {
                LogPrintf("mvote - signature invalid\n");
                LOCK(cs_main);
                Misbehaving(pfrom->GetId(), 20);
            }
            // it could just be a non-synced masternode
            mnodeman.AskForMN(pfrom, voteVin);
            return;
        }

        std::string strError = "";
        if (UpdateProposal(vote, pfrom, strError)) {
            vote.Relay();
            masternodeSync.AddedBudgetItem(vote.GetHash());
        }

        LogPrint(BCLog::MNBUDGET,"mvote - new budget vote for budget %s - %s\n", vote.GetProposalHash().ToString(),  vote.GetHash().ToString());
    }

    if (strCommand == NetMsgType::FINALBUDGET) { //Finalized Budget Suggestion
        CFinalizedBudgetBroadcast finalizedBudgetBroadcast;
        vRecv >> finalizedBudgetBroadcast;

        if (HaveSeenFinalizedBudget(finalizedBudgetBroadcast.GetHash())) {
            masternodeSync.AddedBudgetItem(finalizedBudgetBroadcast.GetHash());
            return;
        }

        std::string strError = "";
        int nConf = 0;
        const uint256& nHash = finalizedBudgetBroadcast.GetHash();
        const uint256& nFeeTXHash = finalizedBudgetBroadcast.GetFeeTXHash();
        if (!IsBudgetCollateralValid(nFeeTXHash, nHash, strError, finalizedBudgetBroadcast.nTime, nConf, true)) {
            LogPrint(BCLog::MNBUDGET,"fbs - Finalized Budget FeeTX is not valid - %s - %s\n", nFeeTXHash.ToString(), strError);
            if (nConf >= 1) {
                LOCK(cs_budgets);
                vecImmatureFinalizedBudgets.push_back(finalizedBudgetBroadcast);
            }
            return;
        }

        AddSeenFinalizedBudget(finalizedBudgetBroadcast);

        if (!finalizedBudgetBroadcast.UpdateValid(nCurrentHeight)) {
            LogPrint(BCLog::MNBUDGET,"fbs - invalid finalized budget - %s\n", finalizedBudgetBroadcast.IsInvalidReason());
            return;
        }

        LogPrint(BCLog::MNBUDGET,"fbs - new finalized budget - %s\n", nHash.ToString());

        CFinalizedBudget finalizedBudget(finalizedBudgetBroadcast);
        if (AddFinalizedBudget(finalizedBudget)) {
            finalizedBudgetBroadcast.Relay();
        }
        masternodeSync.AddedBudgetItem(nHash);

        //we might have active votes for this budget that are now valid
        CheckOrphanVotes();
    }

    if (strCommand == NetMsgType::FINALBUDGETVOTE) { //Finalized Budget Vote
        CFinalizedBudgetVote vote;
        vRecv >> vote;
        vote.SetValid(true);

        if (HaveSeenFinalizedBudgetVote(vote.GetHash())) {
            masternodeSync.AddedBudgetItem(vote.GetHash());
            return;
        }

        const CTxIn& voteVin = vote.GetVin();
        CMasternode* pmn = mnodeman.Find(voteVin);
        if (pmn == NULL) {
            LogPrint(BCLog::MNBUDGET, "fbvote - unknown masternode - vin: %s\n", voteVin.prevout.hash.ToString());
            mnodeman.AskForMN(pfrom, voteVin);
            return;
        }

        AddSeenFinalizedBudgetVote(vote);

        if (!vote.CheckSignature()) {
            if (masternodeSync.IsSynced()) {
                LogPrintf("fbvote - signature from masternode %s invalid\n", HexStr(pmn->pubKeyMasternode));
                LOCK(cs_main);
                Misbehaving(pfrom->GetId(), 20);
            }
            // it could just be a non-synced masternode
            mnodeman.AskForMN(pfrom, voteVin);
            return;
        }

        std::string strError = "";
        if (UpdateFinalizedBudget(vote, pfrom, strError)) {
            vote.Relay();
            masternodeSync.AddedBudgetItem(vote.GetHash());

            LogPrint(BCLog::MNBUDGET,"fbvote - new finalized budget vote - %s from masternode %s\n", vote.GetHash().ToString(), HexStr(pmn->pubKeyMasternode));
        } else {
            LogPrint(BCLog::MNBUDGET,"fbvote - rejected finalized budget vote - %s from masternode %s - %s\n", vote.GetHash().ToString(), HexStr(pmn->pubKeyMasternode), strError);
        }
    }
}

void CBudgetManager::SetSynced(bool synced)
{
    {
        LOCK(cs_proposals);
        for (const auto& it: mapSeenProposals) {
            CBudgetProposal* pbudgetProposal = FindProposal(it.first);
            if (pbudgetProposal && pbudgetProposal->IsValid()) {
                //mark votes
                pbudgetProposal->SetSynced(synced);
            }
        }
    }
    {
        LOCK(cs_budgets);
        for (const auto& it: mapSeenFinalizedBudgets) {
            CFinalizedBudget* pfinalizedBudget = FindFinalizedBudget(it.first);
            if (pfinalizedBudget && pfinalizedBudget->IsValid()) {
                //mark votes
                pfinalizedBudget->SetSynced(synced);
            }
        }
    }
}

void CBudgetManager::Sync(CNode* pfrom, const uint256& nProp, bool fPartial)
{
    CNetMsgMaker msgMaker(pfrom->GetSendVersion());
    int nInvCount = 0;
    {
        LOCK(cs_proposals);
        for (auto& it: mapSeenProposals) {
            CBudgetProposal* pbudgetProposal = FindProposal(it.first);
            if (pbudgetProposal && pbudgetProposal->IsValid() && (nProp.IsNull() || it.first == nProp)) {
                pfrom->PushInventory(CInv(MSG_BUDGET_PROPOSAL, it.second.GetHash()));
                nInvCount++;
                pbudgetProposal->SyncVotes(pfrom, fPartial, nInvCount);
            }
        }
    }
    g_connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::SYNCSTATUSCOUNT, MASTERNODE_SYNC_BUDGET_PROP, nInvCount));
    LogPrint(BCLog::MNBUDGET, "%s: sent %d items\n", __func__, nInvCount);

    nInvCount = 0;
    {
        LOCK(cs_budgets);
        for (auto& it: mapSeenFinalizedBudgets) {
            CFinalizedBudget* pfinalizedBudget = FindFinalizedBudget(it.first);
            if (pfinalizedBudget && pfinalizedBudget->IsValid() && (nProp.IsNull() || it.first == nProp)) {
                pfrom->PushInventory(CInv(MSG_BUDGET_FINALIZED, it.second.GetHash()));
                nInvCount++;
                pfinalizedBudget->SyncVotes(pfrom, fPartial, nInvCount);
            }
        }
    }
    g_connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::SYNCSTATUSCOUNT, MASTERNODE_SYNC_BUDGET_FIN, nInvCount));
    LogPrint(BCLog::MNBUDGET, "%s: sent %d items\n", __func__, nInvCount);
}

bool CBudgetManager::UpdateProposal(const CBudgetVote& vote, CNode* pfrom, std::string& strError)
{
    LOCK(cs_proposals);

    const uint256& nProposalHash = vote.GetProposalHash();
    if (!mapProposals.count(nProposalHash)) {
        if (pfrom) {
            // only ask for missing items after our syncing process is complete --
            //   otherwise we'll think a full sync succeeded when they return a result
            if (!masternodeSync.IsSynced()) return false;

            LogPrint(BCLog::MNBUDGET,"%s: Unknown proposal %d, asking for source proposal\n", __func__, nProposalHash.ToString());
            WITH_LOCK(cs_votes, mapOrphanProposalVotes[nProposalHash] = vote; );

            if (!askedForSourceProposalOrBudget.count(nProposalHash)) {
                g_connman->PushMessage(pfrom, CNetMsgMaker(pfrom->GetSendVersion()).Make(NetMsgType::BUDGETVOTESYNC, nProposalHash));
                askedForSourceProposalOrBudget[nProposalHash] = GetTime();
            }
        }

        strError = "Proposal not found!";
        return false;
    }


    return mapProposals[nProposalHash].AddOrUpdateVote(vote, strError);
}

bool CBudgetManager::UpdateFinalizedBudget(CFinalizedBudgetVote& vote, CNode* pfrom, std::string& strError)
{
    LOCK(cs_budgets);

    const uint256& nBudgetHash = vote.GetBudgetHash();
    if (!mapFinalizedBudgets.count(nBudgetHash)) {
        if (pfrom) {
            // only ask for missing items after our syncing process is complete --
            //   otherwise we'll think a full sync succeeded when they return a result
            if (!masternodeSync.IsSynced()) return false;

            LogPrint(BCLog::MNBUDGET,"%s: Unknown Finalized Proposal %s, asking for source budget\n", __func__, nBudgetHash.ToString());
            WITH_LOCK(cs_finalizedvotes, mapOrphanFinalizedBudgetVotes[nBudgetHash] = vote; );

            if (!askedForSourceProposalOrBudget.count(nBudgetHash)) {
                g_connman->PushMessage(pfrom, CNetMsgMaker(pfrom->GetSendVersion()).Make(NetMsgType::BUDGETVOTESYNC, nBudgetHash));
                askedForSourceProposalOrBudget[nBudgetHash] = GetTime();
            }
        }

        strError = "Finalized Budget " + nBudgetHash.ToString() +  " not found!";
        return false;
    }
    LogPrint(BCLog::MNBUDGET,"%s: Finalized Proposal %s added\n", __func__, nBudgetHash.ToString());
    return mapFinalizedBudgets[nBudgetHash].AddOrUpdateVote(vote, strError);
}

CBudgetProposal::CBudgetProposal()
{
    strProposalName = "unknown";
    nBlockStart = 0;
    nBlockEnd = 0;
    nAmount = 0;
    nTime = 0;
    fValid = true;
    strInvalid = "";
}

CBudgetProposal::CBudgetProposal(std::string strProposalNameIn, std::string strURLIn, int nBlockStartIn, int nBlockEndIn, CScript addressIn, CAmount nAmountIn, uint256 nFeeTXHashIn)
{
    strProposalName = strProposalNameIn;
    strURL = strURLIn;
    nBlockStart = nBlockStartIn;
    nBlockEnd = nBlockEndIn;
    address = addressIn;
    nAmount = nAmountIn;
    nFeeTXHash = nFeeTXHashIn;
    fValid = true;
    strInvalid = "";
}

CBudgetProposal::CBudgetProposal(const CBudgetProposal& other)
{
    strProposalName = other.strProposalName;
    strURL = other.strURL;
    nBlockStart = other.nBlockStart;
    nBlockEnd = other.nBlockEnd;
    address = other.address;
    nAmount = other.nAmount;
    nTime = other.nTime;
    nFeeTXHash = other.nFeeTXHash;
    mapVotes = other.mapVotes;
    fValid = true;
    strInvalid = "";
}

void CBudgetProposal::SyncVotes(CNode* pfrom, bool fPartial, int& nInvCount) const
{
    for (const auto& it: mapVotes) {
        const CBudgetVote& vote = it.second;
        if (vote.IsValid() && (!fPartial || !vote.IsSynced())) {
            pfrom->PushInventory(CInv(MSG_BUDGET_VOTE, vote.GetHash()));
            nInvCount++;
        }
    }
}

bool CBudgetProposal::IsHeavilyDownvoted()
{
    if (GetNays() - GetYeas() > mnodeman.CountEnabled(ActiveProtocol()) / 10) {
        strInvalid = "Proposal " + strProposalName + ": Active removal";
        return true;
    }
    return false;
}

bool CBudgetProposal::CheckStartEnd()
{
    if (nBlockStart < 0) {
        strInvalid = "Invalid Proposal";
        return false;
    }

    if (nBlockEnd < nBlockStart) {
        strInvalid = "Proposal " + strProposalName + ": Invalid nBlockEnd (end before start)";
        return false;
    }

    int nProposalEnd = nBlockStart + (Params().GetConsensus().nBudgetCycleBlocks + 1) * GetTotalPaymentCount();
    if (nBlockEnd != nProposalEnd) {
        strInvalid = "Proposal " + strProposalName + ": Invalid nBlockEnd (mismatch with payments count)";
        return false;
    }

    return true;
}

bool CBudgetProposal::CheckAmount(const CAmount& nTotalBudget)
{
    // check minimum amount
    if (nAmount < 10 * COIN) {
        strInvalid = "Proposal " + strProposalName + ": Invalid nAmount (too low)";
        return false;
    }

    // check maximum amount
    // can only pay out 10% of the possible coins (min value of coins)
    if (nAmount > nTotalBudget) {
        strInvalid = "Proposal " + strProposalName + ": Invalid nAmount (too high)";
        return false;
    }

    return true;
}

bool CBudgetProposal::CheckAddress()
{
    if (address == CScript()) {
        strInvalid = "Proposal " + strProposalName + ": Invalid Payment Address (null)";
        return false;
    }

    /*
        TODO: There might be an issue with multisig in the coinbase on mainnet, we will add support for it in a future release.
    */
    if (address.IsPayToScriptHash()) {
        strInvalid = "Proposal " + strProposalName + ": Multisig is not currently supported.";
        return false;
    }

    return true;
}

bool CBudgetProposal::IsWellFormed(const CAmount& nTotalBudget)
{
    return CheckStartEnd() && CheckAmount(nTotalBudget) && CheckAddress();
}

bool CBudgetProposal::IsExpired(int nCurrentHeight)
{
    if (nBlockEnd < nCurrentHeight) {
        strInvalid = "Proposal " + strProposalName + ": Proposal expired";
        return true;
    }
    return false;
}

bool CBudgetProposal::UpdateValid(int nCurrentHeight, bool fCheckCollateral)
{
    fValid = false;

    if (IsHeavilyDownvoted()) {
        return false;
    }

    // Checks that don't change. !TODO: remove from here, they should be done only once.
    if (!IsWellFormed(budget.GetTotalBudget(nBlockStart))) {
        return false;
    }

    if (IsExpired(nCurrentHeight)) {
        return false;
    }

    if (fCheckCollateral) {
        int nConf = 0;
        std::string strError;
        if (!IsBudgetCollateralValid(nFeeTXHash, GetHash(), strError, nTime, nConf)) {
            strInvalid = "Proposal " + strProposalName + ": Invalid collateral (" + strError + ")";
            return false;
        }
    }

    fValid = true;
    strInvalid.clear();
    return true;
}

bool CBudgetProposal::IsEstablished() const
{
    return nTime < GetAdjustedTime() - Params().GetConsensus().nProposalEstablishmentTime;
}

bool CBudgetProposal::IsPassing(int nBlockStartBudget, int nBlockEndBudget, int mnCount) const
{
    if (!fValid)
        return false;

    if (this->nBlockStart > nBlockStartBudget)
        return false;

    if (this->nBlockEnd < nBlockEndBudget)
        return false;

    if (GetYeas() - GetNays() <= mnCount / 10)
        return false;

    if (!IsEstablished())
        return false;

    return true;
}

bool CBudgetProposal::AddOrUpdateVote(const CBudgetVote& vote, std::string& strError)
{
    std::string strAction = "New vote inserted:";
    const uint256& hash = vote.GetVin().prevout.GetHash();
    const int64_t voteTime = vote.GetTime();

    if (mapVotes.count(hash)) {
        const int64_t& oldTime = mapVotes[hash].GetTime();
        if (oldTime > voteTime) {
            strError = strprintf("new vote older than existing vote - %s\n", vote.GetHash().ToString());
            LogPrint(BCLog::MNBUDGET, "%s: %s\n", __func__, strError);
            return false;
        }
        if (voteTime - oldTime < BUDGET_VOTE_UPDATE_MIN) {
            strError = strprintf("time between votes is too soon - %s - %lli sec < %lli sec\n",
                    vote.GetHash().ToString(), voteTime - oldTime, BUDGET_VOTE_UPDATE_MIN);
            LogPrint(BCLog::MNBUDGET, "%s: %s\n", __func__, strError);
            return false;
        }
        strAction = "Existing vote updated:";
    }

    if (voteTime > GetTime() + (60 * 60)) {
        strError = strprintf("new vote is too far ahead of current time - %s - nTime %lli - Max Time %lli\n", vote.GetHash().ToString(), voteTime, GetTime() + (60 * 60));
        LogPrint(BCLog::MNBUDGET, "%s: %s\n", __func__, strError);
        return false;
    }

    mapVotes[hash] = vote;
    LogPrint(BCLog::MNBUDGET, "%s: %s %s\n", __func__, strAction.c_str(), vote.GetHash().ToString().c_str());

    return true;
}

UniValue CBudgetProposal::GetVotesArray() const
{
    UniValue ret(UniValue::VARR);
    for (const auto& it: mapVotes) {
        ret.push_back(it.second.ToJSON());
    }
    return ret;
}

void CBudgetProposal::SetSynced(bool synced)
{
    for (auto& it: mapVotes) {
        CBudgetVote& vote = it.second;
        if (synced) {
            if (vote.IsValid()) vote.SetSynced(true);
        } else {
            vote.SetSynced(false);
        }
    }
}

// If masternode voted for a proposal, but is now invalid -- remove the vote
void CBudgetProposal::CleanAndRemove()
{
    std::map<uint256, CBudgetVote>::iterator it = mapVotes.begin();

    while (it != mapVotes.end()) {
        CMasternode* pmn = mnodeman.Find((*it).second.GetVin());
        (*it).second.SetValid(pmn != nullptr);
        ++it;
    }
}

double CBudgetProposal::GetRatio() const
{
    int yeas = GetYeas();
    int nays = GetNays();

    if (yeas + nays == 0) return 0.0f;

    return ((double)(yeas) / (double)(yeas + nays));
}

int CBudgetProposal::GetVoteCount(CBudgetVote::VoteDirection vd) const
{
    int ret = 0;
    for (const auto& it : mapVotes) {
        const CBudgetVote& vote = it.second;
        if (vote.GetDirection() == vd && vote.IsValid())
            ret++;
    }
    return ret;
}

int CBudgetProposal::GetBlockStartCycle() const
{
    //end block is half way through the next cycle (so the proposal will be removed much after the payment is sent)
    return GetBlockCycle(nBlockStart);
}

int CBudgetProposal::GetBlockCycle(int nHeight)
{
    return nHeight - nHeight % Params().GetConsensus().nBudgetCycleBlocks;
}

int CBudgetProposal::GetBlockEndCycle() const
{
    // Right now single payment proposals have nBlockEnd have a cycle too early!
    // switch back if it break something else
    // end block is half way through the next cycle (so the proposal will be removed much after the payment is sent)
    // return nBlockEnd - GetBudgetPaymentCycleBlocks() / 2;

    // End block is half way through the next cycle (so the proposal will be removed much after the payment is sent)
    return nBlockEnd;

}

int CBudgetProposal::GetTotalPaymentCount() const
{
    return (GetBlockEndCycle() - GetBlockStartCycle()) / Params().GetConsensus().nBudgetCycleBlocks;
}

int CBudgetProposal::GetRemainingPaymentCount(int nCurrentHeight) const
{
    // If this budget starts in the future, this value will be wrong
    int nPayments = (GetBlockEndCycle() - GetBlockCycle(nCurrentHeight)) / Params().GetConsensus().nBudgetCycleBlocks - 1;
    // Take the lowest value
    return std::min(nPayments, GetTotalPaymentCount());
}

inline bool CBudgetProposal::PtrHigherYes(CBudgetProposal* a, CBudgetProposal* b)
{
    const int netYes_a = a->GetYeas() - a->GetNays();
    const int netYes_b = b->GetYeas() - b->GetNays();

    if (netYes_a == netYes_b) return a->GetFeeTXHash() > b->GetFeeTXHash();

    return netYes_a > netYes_b;
}

CBudgetProposalBroadcast::CBudgetProposalBroadcast(std::string strProposalNameIn, std::string strURLIn, int nPaymentCount, CScript addressIn, CAmount nAmountIn, int nBlockStartIn, uint256 nFeeTXHashIn)
{
    strProposalName = strProposalNameIn;
    strURL = strURLIn;

    nBlockStart = nBlockStartIn;

    const int nBlocksPerCycle = Params().GetConsensus().nBudgetCycleBlocks;
    int nCycleStart = nBlockStart - nBlockStart % nBlocksPerCycle;

    // Right now single payment proposals have nBlockEnd have a cycle too early!
    // switch back if it break something else
    // calculate the end of the cycle for this vote, add half a cycle (vote will be deleted after that block)
    // nBlockEnd = nCycleStart + GetBudgetPaymentCycleBlocks() * nPaymentCount + GetBudgetPaymentCycleBlocks() / 2;

    // Calculate the end of the cycle for this vote, vote will be deleted after next cycle
    nBlockEnd = nCycleStart + (nBlocksPerCycle + 1)  * nPaymentCount;

    address = addressIn;
    nAmount = nAmountIn;

    nFeeTXHash = nFeeTXHashIn;
}

void CBudgetProposalBroadcast::Relay()
{
    CInv inv(MSG_BUDGET_PROPOSAL, GetHash());
    g_connman->RelayInv(inv);
}

CBudgetVote::CBudgetVote() :
        CSignedMessage(),
        fValid(true),
        fSynced(false),
        nProposalHash(UINT256_ZERO),
        nVote(VOTE_ABSTAIN),
        nTime(0),
        vin()
{ }

CBudgetVote::CBudgetVote(CTxIn vinIn, uint256 nProposalHashIn, VoteDirection nVoteIn) :
        CSignedMessage(),
        fValid(true),
        fSynced(false),
        nProposalHash(nProposalHashIn),
        nVote(nVoteIn),
        vin(vinIn)
{
    nTime = GetAdjustedTime();
}

void CBudgetVote::Relay() const
{
    CInv inv(MSG_BUDGET_VOTE, GetHash());
    g_connman->RelayInv(inv);
}

uint256 CBudgetVote::GetHash() const
{
    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << vin;
    ss << nProposalHash;
    ss << (int) nVote;
    ss << nTime;
    return ss.GetHash();
}

std::string CBudgetVote::GetStrMessage() const
{
    return vin.prevout.ToStringShort() + nProposalHash.ToString() +
            std::to_string(nVote) + std::to_string(nTime);
}

UniValue CBudgetVote::ToJSON() const
{
    UniValue bObj(UniValue::VOBJ);
    bObj.pushKV("mnId", vin.prevout.hash.ToString());
    bObj.pushKV("nHash", vin.prevout.GetHash().ToString());
    bObj.pushKV("Vote", GetVoteString());
    bObj.pushKV("nTime", nTime);
    bObj.pushKV("fValid", fValid);
    return bObj;
}

CFinalizedBudget::CFinalizedBudget() :
        fAutoChecked(false),
        fValid(true),
        strInvalid(),
        mapVotes(),
        strBudgetName(""),
        nBlockStart(0),
        vecBudgetPayments(),
        nFeeTXHash(),
        strProposals(""),
        nTime(0)
{ }

CFinalizedBudget::CFinalizedBudget(const CFinalizedBudget& other) :
        fAutoChecked(false),
        fValid(true),
        strInvalid(),
        mapVotes(other.mapVotes),
        strBudgetName(other.strBudgetName),
        nBlockStart(other.nBlockStart),
        vecBudgetPayments(other.vecBudgetPayments),
        nFeeTXHash(other.nFeeTXHash),
        strProposals(other.strProposals),
        nTime(other.nTime)
{ }

bool CFinalizedBudget::AddOrUpdateVote(const CFinalizedBudgetVote& vote, std::string& strError)
{
    const uint256& hash = vote.GetVin().prevout.GetHash();
    const int64_t voteTime = vote.GetTime();
    std::string strAction = "New vote inserted:";

    if (mapVotes.count(hash)) {
        const int64_t oldTime = mapVotes[hash].GetTime();
        if (oldTime > voteTime) {
            strError = strprintf("new vote older than existing vote - %s\n", vote.GetHash().ToString());
            LogPrint(BCLog::MNBUDGET, "%s: %s\n", __func__, strError);
            return false;
        }
        if (voteTime - oldTime < BUDGET_VOTE_UPDATE_MIN) {
            strError = strprintf("time between votes is too soon - %s - %lli sec < %lli sec\n",
                    vote.GetHash().ToString(), voteTime - oldTime, BUDGET_VOTE_UPDATE_MIN);
            LogPrint(BCLog::MNBUDGET, "%s: %s\n", __func__, strError);
            return false;
        }
        strAction = "Existing vote updated:";
    }

    if (voteTime > GetTime() + (60 * 60)) {
        strError = strprintf("new vote is too far ahead of current time - %s - nTime %lli - Max Time %lli\n",
                vote.GetHash().ToString(), voteTime, GetTime() + (60 * 60));
        LogPrint(BCLog::MNBUDGET, "%s: %s\n", __func__, strError);
        return false;
    }

    mapVotes[hash] = vote;
    LogPrint(BCLog::MNBUDGET, "%s: %s %s\n", __func__, strAction.c_str(), vote.GetHash().ToString().c_str());
    return true;
}

UniValue CFinalizedBudget::GetVotesObject() const
{
    UniValue ret(UniValue::VOBJ);
    for (const auto& it: mapVotes) {
        const CFinalizedBudgetVote& vote = it.second;
        ret.pushKV(vote.GetVin().prevout.ToStringShort(), vote.ToJSON());
    }
    return ret;
}

void CFinalizedBudget::SetSynced(bool synced)
{
    for (auto& it: mapVotes) {
        CFinalizedBudgetVote& vote = it.second;
        if (synced) {
            if (vote.IsValid()) vote.SetSynced(true);
        } else {
            vote.SetSynced(false);
        }
    }
}

// Sort budget proposals by hash
struct sortProposalsByHash  {
    bool operator()(const CBudgetProposal* left, const CBudgetProposal* right)
    {
        return (left->GetHash() < right->GetHash());
    }
};

// Check finalized budget and vote on it if correct. Masternodes only
void CFinalizedBudget::CheckAndVote()
{
    if (!fMasterNode || fAutoChecked) {
        LogPrint(BCLog::MNBUDGET,"%s: fMasterNode=%d fAutoChecked=%d\n", __func__, fMasterNode, fAutoChecked);
        return;
    }

    if (activeMasternode.vin == nullopt) {
        LogPrint(BCLog::MNBUDGET,"%s: Active Masternode not initialized.\n", __func__);
        return;
    }

    // Do this 1 in 4 blocks -- spread out the voting activity
    // -- this function is only called every fourteenth block, so this is really 1 in 56 blocks
    if (rand() % 4 != 0) {
        LogPrint(BCLog::MNBUDGET,"%s: waiting\n", __func__);
        return;
    }

    fAutoChecked = true; //we only need to check this once

    if (strBudgetMode == "auto") //only vote for exact matches
    {
        std::vector<CBudgetProposal*> vBudgetProposals = budget.GetBudget();

        // We have to resort the proposals by hash (they are sorted by votes here) and sort the payments
        // by hash (they are not sorted at all) to make the following tests deterministic
        // We're working on copies to avoid any side-effects by the possibly changed sorting order

        // Sort copy of proposals by hash (descending)
        std::vector<CBudgetProposal*> vBudgetProposalsSortedByHash(vBudgetProposals);
        std::sort(vBudgetProposalsSortedByHash.begin(), vBudgetProposalsSortedByHash.end(), CBudgetProposal::PtrGreater);

        // Sort copy payments by hash (descending)
        std::vector<CTxBudgetPayment> vecBudgetPaymentsSortedByHash(vecBudgetPayments);
        std::sort(vecBudgetPaymentsSortedByHash.begin(), vecBudgetPaymentsSortedByHash.end(), std::greater<CTxBudgetPayment>());

        for (unsigned int i = 0; i < vecBudgetPaymentsSortedByHash.size(); i++) {
            LogPrint(BCLog::MNBUDGET,"%s: Budget-Payments - nProp %d %s\n", __func__, i, vecBudgetPaymentsSortedByHash[i].nProposalHash.ToString());
            LogPrint(BCLog::MNBUDGET,"%s: Budget-Payments - Payee %d %s\n", __func__, i, HexStr(vecBudgetPaymentsSortedByHash[i].payee));
            LogPrint(BCLog::MNBUDGET,"%s: Budget-Payments - nAmount %d %lli\n", __func__, i, vecBudgetPaymentsSortedByHash[i].nAmount);
        }

        for (unsigned int i = 0; i < vBudgetProposalsSortedByHash.size(); i++) {
            LogPrint(BCLog::MNBUDGET,"%s: Budget-Proposals - nProp %d %s\n", __func__, i, vBudgetProposalsSortedByHash[i]->GetHash().ToString());
            LogPrint(BCLog::MNBUDGET,"%s: Budget-Proposals - Payee %d %s\n", __func__, i, HexStr(vBudgetProposalsSortedByHash[i]->GetPayee()));
            LogPrint(BCLog::MNBUDGET,"%s: Budget-Proposals - nAmount %d %lli\n", __func__, i, vBudgetProposalsSortedByHash[i]->GetAmount());
        }

        if (vBudgetProposalsSortedByHash.size() == 0) {
            LogPrint(BCLog::MNBUDGET,"%s: No Budget-Proposals found, aborting\n", __func__);
            return;
        }

        if (vBudgetProposalsSortedByHash.size() != vecBudgetPaymentsSortedByHash.size()) {
            LogPrint(BCLog::MNBUDGET,"%s: Budget-Proposal length (%ld) doesn't match Budget-Payment length (%ld).\n", __func__,
                      vBudgetProposalsSortedByHash.size(), vecBudgetPaymentsSortedByHash.size());
            return;
        }

        for (unsigned int i = 0; i < vecBudgetPaymentsSortedByHash.size(); i++) {
            if (i > vBudgetProposalsSortedByHash.size() - 1) {
                LogPrint(BCLog::MNBUDGET,"%s: Proposal size mismatch, i=%d > (vBudgetProposals.size() - 1)=%d\n",
                        __func__, i, vBudgetProposalsSortedByHash.size() - 1);
                return;
            }

            if (vecBudgetPaymentsSortedByHash[i].nProposalHash != vBudgetProposalsSortedByHash[i]->GetHash()) {
                LogPrint(BCLog::MNBUDGET,"%s: item #%d doesn't match %s %s\n", __func__,
                        i, vecBudgetPaymentsSortedByHash[i].nProposalHash.ToString(), vBudgetProposalsSortedByHash[i]->GetHash().ToString());
                return;
            }

            // if(vecBudgetPayments[i].payee != vBudgetProposals[i]->GetPayee()){ -- triggered with false positive
            if (HexStr(vecBudgetPaymentsSortedByHash[i].payee) != HexStr(vBudgetProposalsSortedByHash[i]->GetPayee())) {
                LogPrint(BCLog::MNBUDGET,"%s: item #%d payee doesn't match %s %s\n", __func__,
                        i, HexStr(vecBudgetPaymentsSortedByHash[i].payee), HexStr(vBudgetProposalsSortedByHash[i]->GetPayee()));
                return;
            }

            if (vecBudgetPaymentsSortedByHash[i].nAmount != vBudgetProposalsSortedByHash[i]->GetAmount()) {
                LogPrint(BCLog::MNBUDGET,"%s: item #%d payee doesn't match %lli %lli\n", __func__,
                        i, vecBudgetPaymentsSortedByHash[i].nAmount, vBudgetProposalsSortedByHash[i]->GetAmount());
                return;
            }
        }

        LogPrint(BCLog::MNBUDGET,"%s: Finalized Budget Matches! Submitting Vote.\n", __func__);
        SubmitVote();
    }
}

// Remove votes from masternodes which are not valid/existent anymore
void CFinalizedBudget::CleanAndRemove()
{
    std::map<uint256, CFinalizedBudgetVote>::iterator it = mapVotes.begin();

    while (it != mapVotes.end()) {
        CMasternode* pmn = mnodeman.Find((*it).second.GetVin());
        (*it).second.SetValid(pmn != nullptr);
        ++it;
    }
}

CAmount CFinalizedBudget::GetTotalPayout() const
{
    CAmount ret = 0;

    for (auto & vecBudgetPayment : vecBudgetPayments) {
        ret += vecBudgetPayment.nAmount;
    }

    return ret;
}

std::vector<uint256> CFinalizedBudget::GetProposalsHashes() const
{
    std::vector<uint256> vHashes;
    for (const CTxBudgetPayment& budgetPayment : vecBudgetPayments) {
        vHashes.push_back(budgetPayment.nProposalHash);
    }
    return vHashes;
}

void CFinalizedBudget::SyncVotes(CNode* pfrom, bool fPartial, int& nInvCount) const
{
    for (const auto& it: mapVotes) {
        const CFinalizedBudgetVote& vote = it.second;
        if (vote.IsValid() && (!fPartial || !vote.IsSynced())) {
            pfrom->PushInventory(CInv(MSG_BUDGET_FINALIZED_VOTE, vote.GetHash()));
            nInvCount++;
        }
    }
}

bool CFinalizedBudget::UpdateValid(int nCurrentHeight, bool fCheckCollateral)
{
    fValid = false;
    // All(!) finalized budgets have the name "main", so get some additional information about them
    std::string strProposals = GetProposalsStr();

    const int nBlocksPerCycle = Params().GetConsensus().nBudgetCycleBlocks;
    // Must be the correct block for payment to happen (once a month)
    if (nBlockStart % nBlocksPerCycle != 0) {
        strInvalid = "Invalid BlockStart";
        return false;
    }

    // The following 2 checks check the same (basically if vecBudgetPayments.size() > 100)
    if (GetBlockEnd() - nBlockStart > 100) {
        strInvalid = "Invalid BlockEnd";
        return false;
    }
    if ((int)vecBudgetPayments.size() > 100) {
        strInvalid = "Invalid budget payments count (too many)";
        return false;
    }
    if (strBudgetName == "") {
        strInvalid = "Invalid Budget Name";
        return false;
    }
    if (nBlockStart == 0) {
        strInvalid = "Budget " + strBudgetName + " (" + strProposals + ") Invalid BlockStart == 0";
        return false;
    }
    if (nFeeTXHash.IsNull()) {
        strInvalid = "Budget " + strBudgetName  + " (" + strProposals + ") Invalid FeeTx == 0";
        return false;
    }

    // Can only pay out 10% of the possible coins (min value of coins)
    if (GetTotalPayout() > budget.GetTotalBudget(nBlockStart)) {
        strInvalid = "Budget " + strBudgetName + " (" + strProposals + ") Invalid Payout (more than max)";
        return false;
    }

    std::string strError2 = "";
    if (fCheckCollateral) {
        int nConf = 0;
        if (!IsBudgetCollateralValid(nFeeTXHash, GetHash(), strError2, nTime, nConf, true)) {
            {
                strInvalid = "Budget " + strBudgetName + " (" + strProposals + ") Invalid Collateral : " + strError2;
                return false;
            }
        }
    }

    // Remove obsolete finalized budgets after some time
    int nBlockStart = nCurrentHeight - nCurrentHeight % nBlocksPerCycle + nBlocksPerCycle;

    // Remove budgets where the last payment (from max. 100) ends before 2 budget-cycles before the current one
    int nMaxAge = nBlockStart - (2 * nBlocksPerCycle);

    if (GetBlockEnd() < nMaxAge) {
        strInvalid = strprintf("Budget " + strBudgetName + " (" + strProposals + ") (ends at block %ld) too old and obsolete", GetBlockEnd());
        return false;
    }

    fValid = true;
    strInvalid.clear();
    return true;
}

bool CFinalizedBudget::IsPaidAlready(uint256 nProposalHash, int nBlockHeight) const
{
    // Remove budget-payments from former/future payment cycles
    std::map<uint256, int>::iterator it = mapPayment_History.begin();
    int nPaidBlockHeight = 0;
    uint256 nOldProposalHash;

    for(it = mapPayment_History.begin(); it != mapPayment_History.end(); /* No incrementation needed */ ) {
        nPaidBlockHeight = (*it).second;
        if((nPaidBlockHeight < GetBlockStart()) || (nPaidBlockHeight > GetBlockEnd())) {
            nOldProposalHash = (*it).first;
            LogPrint(BCLog::MNBUDGET, "%s: Budget Proposal %s, Block %d from old cycle deleted\n",
                    __func__, nOldProposalHash.ToString().c_str(), nPaidBlockHeight);
            it = mapPayment_History.erase(it);
        } else {
            ++it;
        }
    }

    // Now that we only have payments from the current payment cycle check if this budget was paid already
    if(mapPayment_History.count(nProposalHash) == 0) {
        // New proposal payment, insert into map for checks with later blocks from this cycle
        mapPayment_History.emplace(nProposalHash, nBlockHeight);
        LogPrint(BCLog::MNBUDGET, "%s: Budget Proposal %s, Block %d added to payment history\n",
                __func__, nProposalHash.ToString().c_str(), nBlockHeight);
        return false;
    }
    // This budget was paid already -> reject transaction so it gets paid to a masternode instead
    return true;
}

TrxValidationStatus CFinalizedBudget::IsTransactionValid(const CTransaction& txNew, int nBlockHeight) const
{
    const int nBlockEnd = GetBlockEnd();
    if (nBlockHeight > nBlockEnd) {
        LogPrint(BCLog::MNBUDGET,"%s: Invalid block - height: %d end: %d\n", __func__, nBlockHeight, nBlockEnd);
        return TrxValidationStatus::InValid;
    }
    if (nBlockHeight < nBlockStart) {
        LogPrint(BCLog::MNBUDGET,"%s: Invalid block - height: %d start: %d\n", __func__, nBlockHeight, nBlockStart);
        return TrxValidationStatus::InValid;
    }

    const int nCurrentBudgetPayment = nBlockHeight - nBlockStart;
    if (nCurrentBudgetPayment > (int)vecBudgetPayments.size() - 1) {
        LogPrint(BCLog::MNBUDGET,"%s: Invalid last block - current budget payment: %d of %d\n",
                __func__, nCurrentBudgetPayment + 1, (int)vecBudgetPayments.size());
        return TrxValidationStatus::InValid;
    }

    // Check if this proposal was paid already. If so, pay a masternode instead
    if(IsPaidAlready(vecBudgetPayments[nCurrentBudgetPayment].nProposalHash, nBlockHeight)) {
        LogPrint(BCLog::MNBUDGET,"%s: Double Budget Payment of %d for proposal %d detected. Paying a masternode instead.\n",
                __func__, vecBudgetPayments[nCurrentBudgetPayment].nAmount, vecBudgetPayments[nCurrentBudgetPayment].nProposalHash.GetHex());
        // No matter what we've found before, stop all checks here. In future releases there might be more than one budget payment
        // per block, so even if the first one was not paid yet this one disables all budget payments for this block.
        return TrxValidationStatus::DoublePayment;
    }

    // Search the payment
    const CScript& scriptExpected = vecBudgetPayments[nCurrentBudgetPayment].payee;
    const CAmount& amountExpected = vecBudgetPayments[nCurrentBudgetPayment].nAmount;
    // Budget payment is usually the last output of coinstake txes, iterate backwords
    for (auto out = txNew.vout.rbegin(); out != txNew.vout.rend(); ++out) {
        LogPrint(BCLog::MNBUDGET,"%s: nCurrentBudgetPayment=%d, payee=%s == out.scriptPubKey=%s, amount=%ld == out.nValue=%ld\n",
                __func__, nCurrentBudgetPayment, HexStr(scriptExpected), HexStr(out->scriptPubKey), amountExpected, out->nValue);
        if (scriptExpected == out->scriptPubKey && amountExpected == out->nValue) {
            // payment found
            LogPrint(BCLog::MNBUDGET,"%s: Found valid Budget Payment of %d for proposal %d\n",
                    __func__, amountExpected, vecBudgetPayments[nCurrentBudgetPayment].nProposalHash.GetHex());
            return TrxValidationStatus::Valid;
        }
    }

    // payment not found
    CTxDestination address1;
    ExtractDestination(scriptExpected, address1);
    LogPrint(BCLog::MNBUDGET,"%s: Missing required payment - %s: %d c: %d\n",
            __func__, EncodeDestination(address1), amountExpected, nCurrentBudgetPayment);
    return TrxValidationStatus::InValid;
}

bool CFinalizedBudget::GetBudgetPaymentByBlock(int64_t nBlockHeight, CTxBudgetPayment& payment) const
{
    int i = nBlockHeight - GetBlockStart();
    if (i < 0) return false;
    if (i > (int)vecBudgetPayments.size() - 1) return false;
    payment = vecBudgetPayments[i];
    return true;
}

bool CFinalizedBudget::GetPayeeAndAmount(int64_t nBlockHeight, CScript& payee, CAmount& nAmount) const
{
    int i = nBlockHeight - GetBlockStart();
    if (i < 0) return false;
    if (i > (int)vecBudgetPayments.size() - 1) return false;
    payee = vecBudgetPayments[i].payee;
    nAmount = vecBudgetPayments[i].nAmount;
    return true;
}

void CFinalizedBudget::SubmitVote()
{
    // function called only from initialized masternodes
    assert(fMasterNode && activeMasternode.vin != nullopt);

    std::string strError = "";
    CPubKey pubKeyMasternode;
    CKey keyMasternode;

    if (!CMessageSigner::GetKeysFromSecret(strMasterNodePrivKey, keyMasternode, pubKeyMasternode)) {
        LogPrint(BCLog::MNBUDGET,"%s: Error upon calling GetKeysFromSecret\n", __func__);
        return;
    }

    CFinalizedBudgetVote vote(*(activeMasternode.vin), GetHash());
    if (!vote.Sign(keyMasternode, pubKeyMasternode)) {
        LogPrint(BCLog::MNBUDGET,"%s: Failure to sign.", __func__);
        return;
    }

    if (budget.UpdateFinalizedBudget(vote, NULL, strError)) {
        LogPrint(BCLog::MNBUDGET,"%s: new finalized budget vote - %s\n", __func__, vote.GetHash().ToString());

        budget.AddSeenFinalizedBudgetVote(vote);
        vote.Relay();
    } else {
        LogPrint(BCLog::MNBUDGET,"%s: Error submitting vote - %s\n", __func__, strError);
    }
}

bool CFinalizedBudget::operator>(const CFinalizedBudget& other) const
{
    const int count = GetVoteCount();
    const int otherCount = other.GetVoteCount();

    if (count == otherCount) return GetFeeTXHash() > other.GetFeeTXHash();

    return count > otherCount;
}

CFinalizedBudgetBroadcast::CFinalizedBudgetBroadcast() :
        CFinalizedBudget()
{ }

CFinalizedBudgetBroadcast::CFinalizedBudgetBroadcast(const CFinalizedBudget& other) :
        CFinalizedBudget(other)
{ }

CFinalizedBudgetBroadcast::CFinalizedBudgetBroadcast(std::string strBudgetNameIn,
                                                     int nBlockStartIn,
                                                     const std::vector<CTxBudgetPayment>& vecBudgetPaymentsIn,
                                                     uint256 nFeeTXHashIn)
{
    strBudgetName = strBudgetNameIn;
    nBlockStart = nBlockStartIn;
    for (const CTxBudgetPayment& out : vecBudgetPaymentsIn)
        vecBudgetPayments.push_back(out);
    nFeeTXHash = nFeeTXHashIn;
}

void CFinalizedBudgetBroadcast::Relay()
{
    CInv inv(MSG_BUDGET_FINALIZED, GetHash());
    g_connman->RelayInv(inv);
}

CFinalizedBudgetVote::CFinalizedBudgetVote() :
        CSignedMessage(),
        fValid(true),
        fSynced(false),
        vin(),
        nBudgetHash(),
        nTime(0)
{ }

CFinalizedBudgetVote::CFinalizedBudgetVote(CTxIn vinIn, uint256 nBudgetHashIn) :
        CSignedMessage(),
        fValid(true),
        fSynced(false),
        vin(vinIn),
        nBudgetHash(nBudgetHashIn)
{
    nTime = GetAdjustedTime();
}

void CFinalizedBudgetVote::Relay() const
{
    CInv inv(MSG_BUDGET_FINALIZED_VOTE, GetHash());
    g_connman->RelayInv(inv);
}

uint256 CFinalizedBudgetVote::GetHash() const
{
    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << vin;
    ss << nBudgetHash;
    ss << nTime;
    return ss.GetHash();
}

UniValue CFinalizedBudgetVote::ToJSON() const
{
    UniValue bObj(UniValue::VOBJ);
    bObj.pushKV("nHash", vin.prevout.GetHash().ToString());
    bObj.pushKV("nTime", (int64_t) nTime);
    bObj.pushKV("fValid", fValid);
    return bObj;
}

std::string CFinalizedBudgetVote::GetStrMessage() const
{
    return vin.prevout.ToStringShort() + nBudgetHash.ToString() + std::to_string(nTime);
}

std::string CBudgetManager::ToString() const
{
    unsigned int nProposals = 0, nSeenProposals = 0;
    {
        LOCK(cs_proposals);
        nProposals = mapProposals.size();
        nSeenProposals = mapSeenProposals.size();
    }

    unsigned int nBudgets = 0, nSeenBudgets = 0;
    {
        LOCK(cs_budgets);
        nBudgets = mapFinalizedBudgets.size();
        nSeenBudgets = mapSeenFinalizedBudgets.size();
    }

    unsigned int nSeenVotes = 0, nOrphanVotes = 0;
    {
        LOCK(cs_votes);
        nSeenVotes = mapSeenProposalVotes.size();
        nOrphanVotes = mapOrphanProposalVotes.size();
    }

    unsigned int nSeenFinalizedVotes = 0, nOrphanFinalizedVotes = 0;
    {
        LOCK(cs_finalizedvotes);
        nSeenFinalizedVotes = mapSeenFinalizedBudgetVotes.size();
        nOrphanFinalizedVotes = mapOrphanFinalizedBudgetVotes.size();
    }

    return strprintf("Proposals: %d (seen: %d) - "
            "Finalized Budgets: %d (seen: %d) - "
            "Proposal Votes: %d (orphan: %d) - "
            "Finalized Budget Votes: %d (orphan: %d)",
            nProposals, nSeenProposals, nBudgets, nSeenBudgets,
            nSeenVotes, nOrphanVotes, nSeenFinalizedVotes, nOrphanFinalizedVotes);
}
