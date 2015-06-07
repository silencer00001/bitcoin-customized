#include "omnicore/transactions.h"

#include "omnicore/errors.h"
#include "omnicore/omnicore.h"
#include "omnicore/encoding.h"
#include "omnicore/log.h"
#include "omnicore/script.h"

#include "base58.h"
#include "uint256.h"
#include "util.h"
#include "core_io.h"
#include "init.h"
#include "sync.h"
#include "pubkey.h"
#include "script/script.h"
#include "script/standard.h"
#include "primitives/transaction.h"
#include "coincontrol.h"
#include "wallet.h"

#include <stdint.h>
#include <string>
#include <utility>
#include <map>
#include <vector>

namespace mastercore
{

static int64_t SelectCoins(const std::string& fromAddress, CCoinControl& coinControl, int64_t additional) {
    CWallet* pwallet = pwalletMain;
    if (NULL == pwallet) {
        return 0;
    }

    int64_t n_max = (COIN * (20 * (0.0001))); // assume 20 kB max tx size at 0.0001 per kB
    int64_t n_total = 0; // total output funds collected

    // if referenceamount is set it is needed to be accounted for here too
    if (0 < additional) n_max += additional;

    int nHeight = GetHeight();
    LOCK2(cs_main, pwallet->cs_wallet);

    for (std::map<uint256, CWalletTx>::iterator it = pwallet->mapWallet.begin();
            it != pwallet->mapWallet.end(); ++it) {
        const uint256& wtxid = it->first;
        const CWalletTx* pcoin = &(*it).second;
        bool bIsMine;
        bool bIsSpent;

        if (pcoin->IsTrusted()) {
            const int64_t nAvailable = pcoin->GetAvailableCredit();

            if (!nAvailable)
                continue;

            for (unsigned int i = 0; i < pcoin->vout.size(); i++) {
                txnouttype whichType;
                if (!GetOutputType(pcoin->vout[i].scriptPubKey, whichType))
                    continue;

                if (!IsAllowedOutputType(whichType, nHeight))
                    continue;

                CTxDestination dest;
                if (!ExtractDestination(pcoin->vout[i].scriptPubKey, dest))
                    continue;

                bIsMine = IsMine(*pwallet, dest);
                bIsSpent = pwallet->IsSpent(wtxid, i);

                if (!bIsMine || bIsSpent)
                    continue;

                int64_t n = bIsSpent ? 0 : pcoin->vout[i].nValue;

                std::string sAddress = CBitcoinAddress(dest).ToString();
                if (msc_debug_tokens)
                    PrintToLog("%s:IsMine()=%s:IsSpent()=%s:%s: i=%d, nValue= %lu\n",
                        sAddress, bIsMine ? "yes" : "NO",
                        bIsSpent ? "YES" : "no", wtxid.ToString(), i, n);

                // only use funds from the sender's address
                if (fromAddress == sAddress) {
                    COutPoint outpt(wtxid, i);
                    coinControl.Select(outpt);

                    n_total += n;

                    if (n_max <= n_total)
                        break;
                }
            }
        }

        if (n_max <= n_total)
            break;
    }

    return n_total;
}

int64_t FeeThreshold()
{
    // based on 3x <200 byte outputs (change/reference/data) & total tx size of <2KB
    return 3 * minRelayTxFee.GetFee(200) + CWallet::minTxFee.GetFee(2000);
}

int64_t FeeCheck(const std::string& address)
{
    // check the supplied address against selectCoins to determine if sufficient fees for send
    CCoinControl coinControl;
    return SelectCoins(address, coinControl, 0);
}

// This function determines whether it is valid to use a Class C transaction for a given payload size
static bool UseEncodingClassC(size_t nDataSize)
{
    size_t nTotalSize = nDataSize + 2; // Marker "om"
    bool fDataEnabled = GetBoolArg("-datacarrier", true);
    int nBlockNow = GetHeight();
    if (!IsAllowedOutputType(TX_NULL_DATA, nBlockNow)) {
        fDataEnabled = false;
    }
    return nTotalSize <= nMaxDatacarrierBytes && fDataEnabled;
}

bool AddressToPubKey(const std::string& sender, CPubKey& pubKey)
{
    CWallet* pwallet = pwalletMain;
    if (!pwallet) { return false; }

    CKeyID keyID;
    CBitcoinAddress address(sender);

    if (address.IsScript()) {
        PrintToLog("%s() ERROR: redemption address %s must not be a script hash\n", __func__, sender);
        return false;
    }
    if (!address.GetKeyID(keyID)) {
        PrintToLog("%s() ERROR: redemption address %s is invalid\n", __func__, sender);
        return false;
    }
    if (!pwallet->GetPubKey(keyID, pubKey)) {
        PrintToLog("%s() ERROR: failed to retrieve public key for redemption address %s from wallet\n", __func__, sender);
        return false;
    }
    if (!pubKey.IsFullyValid()) {
        PrintToLog("%s() ERROR: retrieved invalid public key for redemption address %s\n", __func__, sender);
        return false;
    }

    return true;
}

int PrepareTransaction(const std::string& senderAddress, const std::string& receiverAddress, const std::string& redemptionAddress,
        int64_t referenceAmount, const std::vector<unsigned char>& data, std::vector<std::pair<CScript, int64_t> >& vecSend)
{
    // Determine the class to send the transaction via - default is Class C
    int omniTxClass = OMNI_CLASS_C;
    if (!UseEncodingClassC(data.size())) omniTxClass = OMNI_CLASS_B;

    // Encode the data outputs
    switch (omniTxClass) {
        case OMNI_CLASS_B: {
            CPubKey redeemingPubKey;
            std::string str = redemptionAddress;
            if (str.empty()) str = senderAddress; // TODO: this seems laborious
            if (!AddressToPubKey(str, redeemingPubKey)) return MP_REDEMP_BAD_VALIDATION;
            if (!OmniCore_Encode_ClassB(senderAddress, redeemingPubKey, data, vecSend)) return MP_ENCODING_ERROR;
        break; }
        case OMNI_CLASS_C:
            if (!OmniCore_Encode_ClassC(data, vecSend)) return MP_ENCODING_ERROR;
        break;
    }

    // Then add a pay-to-pubkey-hash output for the recipient (if needed)
    // Note: we do this last as we want this to be the highest vout
    if (!receiverAddress.empty()) {
        CScript scriptPubKey = GetScriptForDestination(CBitcoinAddress(receiverAddress).Get());
        vecSend.push_back(std::make_pair(scriptPubKey, std::max(referenceAmount, GetDustThreshold(scriptPubKey))));
    }

    return 0;
}

// This function requests the wallet create an Omni transaction using the supplied parameters and payload
int ClassAgnosticWalletTXBuilder(const std::string& senderAddress, const std::string& receiverAddress, const std::string& redemptionAddress,
        int64_t referenceAmount, const std::vector<unsigned char>& data, uint256& txid, std::string& rawHex, bool commit)
{
    CWallet* pwallet = pwalletMain;
    if (!pwallet) { return MP_ERR_WALLET_ACCESS; }

    // Prepare the transaction - first setup some vars
    CCoinControl coinControl;
    std::vector<std::pair<CScript, int64_t> > vecSend;
    CWalletTx wtxNew;
    int64_t nFeeRet = 0;
    std::string strFailReason;
    CReserveKey reserveKey(pwallet);

    // Select the inputs
    // TODO: check, if enough coins were selected
    if (!SelectCoins(senderAddress, coinControl, referenceAmount)) { return MP_INPUTS_INVALID; }
    if (!coinControl.HasSelected()) { return MP_ERR_INPUTSELECT_FAIL; }

    // Next, we set the change address to the sender
    coinControl.destChange = CBitcoinAddress(senderAddress).Get();

    // Encode the data outputs
    int rc = PrepareTransaction(senderAddress, receiverAddress, redemptionAddress, referenceAmount, data, vecSend);
    if (rc != 0) { return rc; }

    // Ask the wallet to create the transaction (note mining fee determined by Bitcoin Core params)
    if (!pwallet->CreateTransaction(vecSend, wtxNew, reserveKey, nFeeRet, strFailReason, &coinControl)) {
        PrintToLog("%s() ERROR: %s\n", __FUNCTION__, strFailReason);
        return MP_ERR_CREATE_TX;
    }

    // If this request is only to create, but not commit the transaction then display it and exit
    if (!commit) {
        rawHex = EncodeHexTx(wtxNew);
    } else {
        // Commit the transaction to the wallet and broadcast)
        PrintToLog("%s():%s; nFeeRet = %lu, line %d, file: %s\n", __FUNCTION__, wtxNew.ToString(), nFeeRet, __LINE__, __FILE__);
        if (!pwallet->CommitTransaction(wtxNew, reserveKey)) return MP_ERR_COMMIT_TX;
        txid = wtxNew.GetHash();
    }

    return 0;
}

static bool GetTransactionInputs(const std::vector<COutPoint>& txInputs, std::vector<CTxOut>& txOutputsRet)
{
    txOutputsRet.clear();
    txOutputsRet.reserve(txInputs.size());

    // Gather outputs for all transaction inputs  
    for (std::vector<COutPoint>::const_iterator it = txInputs.begin(); it != txInputs.end(); ++it) {
        const COutPoint& outpoint = *it;

        CTransaction prevTx;
        uint256 prevTxBlock;
        if (!GetTransaction(outpoint.hash, prevTx, prevTxBlock, false)) {
            PrintToConsole("%s() ERROR: get transaction failed\n", __func__);
            return false;
        }
        // Sanity check
        if (prevTx.vout.size() < outpoint.n) {
            PrintToConsole("%s() ERROR: first sanity check failed (0 < %d || %d < %d\n", __func__, outpoint.n, prevTx.vout.size(), outpoint.n);
            return false;
        }
        CTxOut prevTxOut = prevTx.vout[outpoint.n];
        txOutputsRet.push_back(prevTxOut);
        PrintToConsole("%s() pushed an output\n", __func__);
    }

    // Sanity check
    return (txInputs.size() == txOutputsRet.size());
}

static bool CheckTransactionInputs(const std::vector<CTxOut>& txInputs, std::string& strSender, int64_t& amountIn, int nHeight=9999999)
{
    amountIn = 0;
    strSender.clear();

    for (std::vector<CTxOut>::const_iterator it = txInputs.begin(); it != txInputs.end(); ++it) {
        const CScript& scriptPubKey = it->scriptPubKey;
        const int64_t amount = it->nValue;

        txnouttype whichType;
        if (!GetOutputType(scriptPubKey, whichType)) {
            PrintToConsole("%s() ERROR: failed to retrieve output type\n", __func__);
            return false;
        }

        if (!IsAllowedOutputType(whichType, nHeight)) {
            PrintToConsole("%s() ERROR: output type is not allowed\n", __func__);
            return false;
        }

        CTxDestination dest;
        if (!ExtractDestination(scriptPubKey, dest)) {
            PrintToConsole("%s() ERROR: failed to retrieve destination\n", __func__);
            return false;
        }

        CBitcoinAddress address(dest);
        if (strSender.empty()) {
            strSender = address.ToString();
        }

        if (strSender != address.ToString()) {
            PrintToConsole("%s() ERROR: inputs must refer to the same address\n", __func__);
            return false;
        }

        amountIn += amount;
    }

    return true;
}

int ClassAgnosticWalletTXBuilder(const std::vector<COutPoint>& txInputs, const std::string& receiverAddress,
        const std::vector<unsigned char>& payload, const CPubKey& pubKey, std::string& rawTxHex, int64_t txFee)
{
    
    int omniTxClass = OMNI_CLASS_C;
    if (!UseEncodingClassC(payload.size())) {
        omniTxClass = OMNI_CLASS_B;
    }
    
    PrintToConsole("%s(): using class %d\n", __func__, omniTxClass);

    std::vector<CTxOut> txOutputs;
    if (!GetTransactionInputs(txInputs, txOutputs)) {
        PrintToConsole("%s() ERROR: failed to get transaction inputs\n", __func__);
        return false;
    }

    std::string strSender;
    int64_t amountIn = 0;
    if (!CheckTransactionInputs(txOutputs, strSender, amountIn)) {
        PrintToConsole("%s() ERROR: failed to check transaction inputs\n", __func__);
        return false;
    }

    std::vector<std::pair<CScript, int64_t> > vecSend;
    switch (omniTxClass) {
        case OMNI_CLASS_B:
            if (!pubKey.IsFullyValid()) {
                PrintToConsole("%s() ERROR: public key for class B redemption is invalid\n", __func__);
                return MP_REDEMP_BAD_VALIDATION;
            }
            if (!OmniCore_Encode_ClassB(strSender, pubKey, payload, vecSend)) {
                PrintToConsole("%s() ERROR: failed to embed payload\n", __func__);
                return MP_ENCODING_ERROR;
            }
        break;
        case OMNI_CLASS_C:
            if (!OmniCore_Encode_ClassC(payload, vecSend)) {
                PrintToConsole("%s() ERROR: failed to embed payload\n", __func__);
                return MP_ENCODING_ERROR;
            }
        break;
    }

    if (!receiverAddress.empty()) {
        CScript scriptPubKey = GetScriptForDestination(CBitcoinAddress(receiverAddress).Get());
        vecSend.push_back(std::make_pair(scriptPubKey, GetDustThreshold(scriptPubKey)));
    }

    CMutableTransaction rawTx;
    for (std::vector<COutPoint>::const_iterator it = txInputs.begin(); it != txInputs.end(); ++it) {
        const COutPoint& outpoint = *it;
        CTxIn input(outpoint);
        rawTx.vin.push_back(input);
    }

    int64_t amountOut = 0;
    for (std::vector<std::pair<CScript, int64_t> >::const_iterator it = vecSend.begin(); it != vecSend.end(); ++it) {
        const CScript& scriptPubKey = it->first;
        const int64_t amount = it->second;
        CTxOut out(amount, scriptPubKey);
        rawTx.vout.push_back(out);
        amountOut += amount;
    }

    int64_t amountChange = amountIn - amountOut - txFee;
    if (amountChange < 0) {
        PrintToConsole("%s() ERROR: insufficient input amount\n", __func__);
    }

    CScript scriptChange = GetScriptForDestination(CBitcoinAddress(strSender).Get());
    CTxOut rawTxOut(amountChange, scriptChange);

    // Never create dust outputs; if we would, just
    // add the dust to the fee.
    if (!rawTxOut.IsDust(::minRelayTxFee)) {
        // Insert change txn at random position:
        std::vector<CTxOut>::iterator position = rawTx.vout.begin()+GetRandInt(rawTx.vout.size()+1);
        rawTx.vout.insert(position, rawTxOut);
    }

    rawTxHex = EncodeHexTx(rawTx);
    PrintToConsole("%s() result: %s\n", __func__, rawTxHex);

    return 0;
}


} // namespace mastercore
