#include "mastercore_script.h"

#include "pubkey.h"
#include "script/script.h"
#include "script/standard.h" // for CTxDestination + txnouttype
#include "utilstrencodings.h"

#include <boost/foreach.hpp>
#include <boost/variant.hpp>

#include <string>
#include <vector>

// TODO: could be inlined
using std::multimap;
using std::vector;

typedef vector<unsigned char> valtype;

bool GetScriptPushes(const CScript& scriptIn, std::vector<std::string>& vstrRet, bool fSkipFirst)
{
    int count = 0;
    CScript::const_iterator pc = scriptIn.begin();
    while (pc < scriptIn.end())
    {
        opcodetype opcode;
        std::vector<unsigned char> data;
        if (!scriptIn.GetOp(pc, opcode, data))
            return false;
        if (0 <= opcode && opcode <= OP_PUSHDATA4)
            if (count++ || !fSkipFirst) vstrRet.push_back(HexStr(data));
    }

    return true;
}

/**
 * Return public keys or hashes from scriptPubKey, for 'standard' transaction types.
 */
bool SafeSolver(const CScript& scriptPubKey, txnouttype& typeRet, vector<vector<unsigned char> >& vSolutionsRet)
{
    // Templates
    static multimap<txnouttype, CScript> mTemplates;
    if (mTemplates.empty())
    {
        // Standard tx, sender provides pubkey, receiver adds signature
        mTemplates.insert(make_pair(TX_PUBKEY, CScript() << OP_PUBKEY << OP_CHECKSIG));

        // Bitcoin address tx, sender provides hash of pubkey, receiver provides signature and pubkey
        mTemplates.insert(make_pair(TX_PUBKEYHASH, CScript() << OP_DUP << OP_HASH160 << OP_PUBKEYHASH << OP_EQUALVERIFY << OP_CHECKSIG));

        // Sender provides N pubkeys, receivers provides M signatures
        mTemplates.insert(make_pair(TX_MULTISIG, CScript() << OP_SMALLINTEGER << OP_PUBKEYS << OP_SMALLINTEGER << OP_CHECKMULTISIG));

        // Empty, provably prunable, data-carrying output
        mTemplates.insert(make_pair(TX_NULL_DATA, CScript() << OP_RETURN << OP_SMALLDATA));
    }

    // Shortcut for pay-to-script-hash, which are more constrained than the other types:
    // it is always OP_HASH160 20 [20 byte hash] OP_EQUAL
    if (scriptPubKey.IsPayToScriptHash())
    {
        typeRet = TX_SCRIPTHASH;
        vector<unsigned char> hashBytes(scriptPubKey.begin()+2, scriptPubKey.begin()+22);
        vSolutionsRet.push_back(hashBytes);
        return true;
    }

    // Scan templates
    const CScript& script1 = scriptPubKey;
    BOOST_FOREACH(const PAIRTYPE(txnouttype, CScript)& tplate, mTemplates)
    {
        const CScript& script2 = tplate.second;
        vSolutionsRet.clear();

        opcodetype opcode1, opcode2;
        vector<unsigned char> vch1, vch2;

        // Compare
        CScript::const_iterator pc1 = script1.begin();
        CScript::const_iterator pc2 = script2.begin();
        while (true)
        {
            if (pc1 == script1.end() && pc2 == script2.end())
            {
                // Found a match
                typeRet = tplate.first;
                if (typeRet == TX_MULTISIG)
                {
                    // Additional checks for TX_MULTISIG:
                    unsigned char m = vSolutionsRet.front()[0];
                    unsigned char n = vSolutionsRet.back()[0];
                    if (m < 1 || n < 1 || m > n || vSolutionsRet.size()-2 != n)
                        return false;
                }
                return true;
            }
            if (!script1.GetOp(pc1, opcode1, vch1))
                break;
            if (!script2.GetOp(pc2, opcode2, vch2))
                break;

            // Template matching opcodes:
            if (opcode2 == OP_PUBKEYS)
            {
                while (vch1.size() >= 33 && vch1.size() <= 65)
                {
                    vSolutionsRet.push_back(vch1);
                    if (!script1.GetOp(pc1, opcode1, vch1))
                        break;
                }
                if (!script2.GetOp(pc2, opcode2, vch2))
                    break;
                // Normal situation is to fall through
                // to other if/else statements
            }

            if (opcode2 == OP_PUBKEY)
            {
                if (vch1.size() < 33 || vch1.size() > 65)
                    break;
                vSolutionsRet.push_back(vch1);
            }
            else if (opcode2 == OP_PUBKEYHASH)
            {
                if (vch1.size() != sizeof(uint160))
                    break;
                vSolutionsRet.push_back(vch1);
            }
            else if (opcode2 == OP_SMALLINTEGER)
            {   // Single-byte small integer pushed onto vSolutions
                if (opcode1 == OP_0 ||
                    (opcode1 >= OP_1 && opcode1 <= OP_16))
                {
                    char n = (char)CScript::DecodeOP_N(opcode1);
                    vSolutionsRet.push_back(valtype(1, n));
                }
                else
                    break;
            }
            else if (opcode2 == OP_SMALLDATA)
            {
                // No size limit enforced
            }
            else if (opcode1 != opcode2 || vch1 != vch2)
            {
                // Others must match exactly
                break;
            }
        }
    }

    vSolutionsRet.clear();
    typeRet = TX_NONSTANDARD;
    return false;
}

bool SafeExtractDestination(const CScript& scriptPubKey, CTxDestination& addressRet)
{
    vector<valtype> vSolutions;
    txnouttype whichType;
    if (!SafeSolver(scriptPubKey, whichType, vSolutions))
        return false;

    if (whichType == TX_PUBKEY)
    {
        CPubKey pubKey(vSolutions[0]);
        if (!pubKey.IsValid())
            return false;

        addressRet = pubKey.GetID();
        return true;
    }
    else if (whichType == TX_PUBKEYHASH)
    {
        addressRet = CKeyID(uint160(vSolutions[0]));
        return true;
    }
    else if (whichType == TX_SCRIPTHASH)
    {
        addressRet = CScriptID(uint160(vSolutions[0]));
        return true;
    }
    // Multisig txns have more than one address...
    return false;
}

bool SafeExtractDestinations(const CScript& scriptPubKey, txnouttype& typeRet, vector<CTxDestination>& addressRet, int& nRequiredRet)
{
    addressRet.clear();
    typeRet = TX_NONSTANDARD;
    vector<valtype> vSolutions;
    if (!SafeSolver(scriptPubKey, typeRet, vSolutions))
        return false;
    if (typeRet == TX_NULL_DATA){
        // This is data, not addresses
        return false;
    }

    if (typeRet == TX_MULTISIG)
    {
        nRequiredRet = vSolutions.front()[0];
        for (unsigned int i = 1; i < vSolutions.size()-1; i++)
        {
            CPubKey pubKey(vSolutions[i]);
            if (!pubKey.IsValid())
                continue;

            CTxDestination address = pubKey.GetID();
            addressRet.push_back(address);
        }

        if (addressRet.empty())
            return false;
    }
    else
    {
        nRequiredRet = 1;
        CTxDestination address;
        if (!SafeExtractDestination(scriptPubKey, address))
           return false;
        addressRet.push_back(address);
    }

    return true;
}

