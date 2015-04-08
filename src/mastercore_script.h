#ifndef MASTERCOIN_SCRIPT_H
#define MASTERCOIN_SCRIPT_H

class CScript;

#include "script/standard.h" // for CTxDestination + txnouttypetxnouttype

#include <boost/variant.hpp>

#include <string>
#include <vector>

bool GetScriptPushes(const CScript& scriptIn, std::vector<std::string>& vstrRet, bool fSkipFirst = false);

bool SafeSolver(const CScript& scriptPubKey, txnouttype& typeRet, std::vector<std::vector<unsigned char> >& vSolutionsRet);
bool SafeExtractDestination(const CScript& scriptPubKey, CTxDestination& addressRet);
bool SafeExtractDestinations(const CScript& scriptPubKey, txnouttype& typeRet, std::vector<CTxDestination>& addressRet, int& nRequiredRet);

#endif // MASTERCOIN_SCRIPT_H
