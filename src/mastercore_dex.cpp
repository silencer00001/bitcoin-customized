// DEx & MetaDEx

#include "base58.h"
#include "rpcserver.h"
#include "init.h"
#include "util.h"
#include "wallet.h"

#include <stdint.h>
#include <string.h>

#include <map>
#include <set>

#include <fstream>
#include <algorithm>

#include <vector>

#include <utility>
#include <string>

#include <boost/assign/list_of.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/find.hpp>
#include <boost/algorithm/string/join.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/format.hpp>
#include <boost/filesystem.hpp>
#include "json/json_spirit_utils.h"
#include "json/json_spirit_value.h"

#include "leveldb/db.h"
#include "leveldb/write_batch.h"

#include <openssl/sha.h>

#include <boost/math/constants/constants.hpp>
#include <boost/multiprecision/cpp_int.hpp>
#include <boost/multiprecision/cpp_dec_float.hpp>

using boost::multiprecision::int128_t;
using boost::multiprecision::cpp_int;
using boost::multiprecision::cpp_dec_float;
using boost::multiprecision::cpp_dec_float_50;

using namespace std;
using namespace boost;
using namespace boost::assign;
using namespace json_spirit;
using namespace leveldb;

#include "mastercore.h"

using namespace mastercore;

#include "mastercore_dex.h"
#include "mastercore_tx.h"

extern int msc_debug_dex, msc_debug_metadex, msc_debug_metadex2;

md_CurrenciesMap mastercore::metadex;

md_PricesMap *get_Prices(unsigned int curr)
{
md_CurrenciesMap::iterator it = metadex.find(curr);

  if (it != metadex.end()) return &(it->second);

  return (md_PricesMap *) NULL;
}

md_Set *get_Indexes(md_PricesMap *p, double price)
{
md_PricesMap::iterator it = p->find(price);

  if (it != p->end()) return &(it->second);

  return (md_Set *) NULL;
}

// find the best match on the market
// INPUT: currency, descurr, desprice = of the new order being inserted
static bool MetaDExMatch(unsigned int curr, unsigned int descurr, const double desprice, bool bTrade, CMPMetaDEx *obj)
{
const CMPMetaDEx *p_match = NULL;
bool found = false;
string label;
md_PricesMap *prices = NULL;

  if (bTrade) label = "INVERSE";
  else label = "Straight";

  if (msc_debug_metadex) fprintf(mp_fp, "%s(curr=%u, descurr=%u, desprice= %12.11lf:%s), line %d, file: %s\n", __FUNCTION__, curr, descurr, desprice, label.c_str(), __LINE__, __FILE__);

  if (bTrade)
  {
    prices = get_Prices(descurr);
  }
  else
  {
    prices = get_Prices(curr);
  }

  // nothing for the desired currency exists in the market, sorry!
  if (!prices) return false;

  md_Set indexes;
  double price;

  // within the desired currency map walk iterate over the items looking at prices
  md_Set::iterator iitt;
  for (md_PricesMap::iterator my_it = prices->begin(); my_it != prices->end(); ++my_it)
  {
    price = (my_it->first);

    if (bTrade)
    {
      if (msc_debug_metadex2) fprintf(mp_fp, "comparing prices: desprice %20.10lf needs to be LESS THAN OR EQUAL TO %20.10lf\n", desprice, price);

      // is the desired price check satisfied?
      if (desprice > price) continue;
    }
    else
    {
      if (desprice != price) continue;
    }

    indexes = my_it->second;

    // TODO: verify that the match should really be the first object off the list...
    // ...

    for (md_Set::iterator iitt = indexes.begin(); iitt != indexes.end(); ++iitt)
    {
      p_match = &(*iitt);

      if (msc_debug_metadex) fprintf(mp_fp, "Looking at: %12.11lf (its curr= %u, its des curr= %u) = %s\n",
       price, p_match->getCurrency(), p_match->getDesCurrency(), p_match->ToString().c_str());

      // is the desired currency correct?
      if (bTrade)
      {
        if (p_match->getDesCurrency() != curr) continue;
      }
      else
      {
        if (p_match->getDesCurrency() != descurr) continue;
      }

      if (msc_debug_metadex) fprintf(mp_fp, "MATCH FOUND: %12.11lf = %s\n", price, p_match->ToString().c_str());

      found = true;
      break;  // matched!
    }

    if (found)
    {
      if (bTrade)
      {
        if (msc_debug_metadex) fprintf(mp_fp, "======= TRADED !!!!!!!!!!!!!!!!!!!!!\n");
      }
      else
      {
        // assume ADD
        if (msc_debug_metadex) fprintf(mp_fp, "++++++++ ADDED !!!!!!!!!!!!!!!!!!!!!\n");
      }
      break;
    }
  }

  return found;
}

void mastercore::MetaDEx_debug_print3()
{
  printf("<<<\n");
  for (md_CurrenciesMap::iterator my_it = metadex.begin(); my_it != metadex.end(); ++my_it)
  {
    unsigned int curr = my_it->first;

    printf(" ## currency: %u\n", curr);
    md_PricesMap & prices = my_it->second;

    for (md_PricesMap::iterator it = prices.begin(); it != prices.end(); ++it)
    {
      double price = (it->first);
      md_Set & indexes = (it->second);

      for (md_Set::iterator it = indexes.begin(); it != indexes.end(); ++it)
      {
      CMPMetaDEx obj = *it;

        printf("%20.10lf = %s\n", price, obj.ToString().c_str());
      }
    }
  }
  printf(">>>\n");
}

void CMPMetaDEx::Set0(const string &sa, int b, unsigned int c, uint64_t nValue, unsigned int cd, uint64_t ad, const uint256 &tx, unsigned int i)
{
  addr = sa;
  block = b;
  txid = tx;
  currency = c;
  amount_original = nValue;
  desired_currency = cd;
  desired_amount_original = ad;

  idx = i;
}

CMPMetaDEx::CMPMetaDEx(const string &addr, int b, unsigned int c, uint64_t nValue, unsigned int cd, uint64_t ad, const uint256 &tx, unsigned int i)
{
  Set0(addr, b,c,nValue,cd,ad,tx,i);
}

std::string CMPMetaDEx::ToString() const
{
  return strprintf("%34s in %d/%03u, txid: %s, trade #%u %s for #%u %s",
   addr.c_str(), block, idx, txid.ToString().c_str(),
   currency, FormatMP(currency, amount_original), desired_currency, FormatMP(desired_currency, desired_amount_original));
}

// check to see if such a sell offer exists
bool mastercore::DEx_offerExists(const string &seller_addr, unsigned int curr)
{
//  if (msc_debug_dex) fprintf(mp_fp, "%s()\n", __FUNCTION__);
const string combo = STR_SELLOFFER_ADDR_CURR_COMBO(seller_addr);
OfferMap::iterator my_it = my_offers.find(combo);

  return !(my_it == my_offers.end());
}

// getOffer may replace DEx_offerExists() in the near future
// TODO: locks are needed around map's insert & erase
CMPOffer *mastercore::DEx_getOffer(const string &seller_addr, unsigned int curr)
{
  if (msc_debug_dex) fprintf(mp_fp, "%s(%s, %u)\n", __FUNCTION__, seller_addr.c_str(), curr);
const string combo = STR_SELLOFFER_ADDR_CURR_COMBO(seller_addr);
OfferMap::iterator my_it = my_offers.find(combo);

  if (my_it != my_offers.end()) return &(my_it->second);

  return (CMPOffer *) NULL;
}

// TODO: locks are needed around map's insert & erase
CMPAccept *mastercore::DEx_getAccept(const string &seller_addr, unsigned int curr, const string &buyer_addr)
{
  if (msc_debug_dex) fprintf(mp_fp, "%s(%s, %u, %s)\n", __FUNCTION__, seller_addr.c_str(), curr, buyer_addr.c_str());
const string combo = STR_ACCEPT_ADDR_CURR_ADDR_COMBO(seller_addr, buyer_addr);
AcceptMap::iterator my_it = my_accepts.find(combo);

  if (my_it != my_accepts.end()) return &(my_it->second);

  return (CMPAccept *) NULL;
}

// returns 0 if everything is OK
int mastercore::DEx_offerCreate(string seller_addr, unsigned int curr, uint64_t nValue, int block, uint64_t amount_desired, uint64_t fee, unsigned char btl, const uint256 &txid, uint64_t *nAmended)
{
//  if (msc_debug_dex) fprintf(mp_fp, "%s()\n", __FUNCTION__);
int rc = DEX_ERROR_SELLOFFER;

  // sanity check our params are OK
  if ((!btl) || (!amount_desired)) return (DEX_ERROR_SELLOFFER -101); // time limit or amount desired empty

  if (DEx_getOffer(seller_addr, curr)) return (DEX_ERROR_SELLOFFER -10);  // offer already exists

  const string combo = STR_SELLOFFER_ADDR_CURR_COMBO(seller_addr);

  if (msc_debug_dex)
   fprintf(mp_fp, "%s(%s|%s), nValue=%lu)\n", __FUNCTION__, seller_addr.c_str(), combo.c_str(), nValue);

  const uint64_t balanceReallyAvailable = getMPbalance(seller_addr, curr, MONEY);

  // if offering more than available -- put everything up on sale
  if (nValue > balanceReallyAvailable)
  {
  double BTC;

    // AND we must also re-adjust the BTC desired in this case...
    BTC = amount_desired * balanceReallyAvailable;
    BTC /= (double)nValue;
    amount_desired = rounduint64(BTC);

    nValue = balanceReallyAvailable;

    if (nAmended) *nAmended = nValue;
  }

  if (update_tally_map(seller_addr, curr, - nValue, MONEY)) // subtract from what's available
  {
    update_tally_map(seller_addr, curr, nValue, SELLOFFER_RESERVE); // put in reserve

    my_offers.insert(std::make_pair(combo, CMPOffer(block, nValue, curr, amount_desired, fee, btl, txid)));

    rc = 0;
  }

  return rc;
}

// returns 0 if everything is OK
int mastercore::DEx_offerDestroy(const string &seller_addr, unsigned int curr)
{
//  if (msc_debug_dex) fprintf(mp_fp, "%s()\n", __FUNCTION__);
const uint64_t amount = getMPbalance(seller_addr, curr, SELLOFFER_RESERVE);

  if (!DEx_offerExists(seller_addr, curr)) return (DEX_ERROR_SELLOFFER -11); // offer does not exist

  const string combo = STR_SELLOFFER_ADDR_CURR_COMBO(seller_addr);

  OfferMap::iterator my_it;

  my_it = my_offers.find(combo);

  if (amount)
  {
    update_tally_map(seller_addr, curr, amount, MONEY);   // give money back to the seller from SellOffer-Reserve
    update_tally_map(seller_addr, curr, - amount, SELLOFFER_RESERVE);
  }

  // delete the offer
  my_offers.erase(my_it);

  if (msc_debug_dex)
   fprintf(mp_fp, "%s(%s|%s)\n", __FUNCTION__, seller_addr.c_str(), combo.c_str());

  return 0;
}

// returns 0 if everything is OK
int mastercore::DEx_offerUpdate(const string &seller_addr, unsigned int curr, uint64_t nValue, int block, uint64_t desired, uint64_t fee, unsigned char btl, const uint256 &txid, uint64_t *nAmended)
{
//  if (msc_debug_dex) fprintf(mp_fp, "%s()\n", __FUNCTION__);
int rc = DEX_ERROR_SELLOFFER;

  fprintf(mp_fp, "%s(%s, %d)\n", __FUNCTION__, seller_addr.c_str(), curr);

  if (!DEx_offerExists(seller_addr, curr)) return (DEX_ERROR_SELLOFFER -12); // offer does not exist

  rc = DEx_offerDestroy(seller_addr, curr);

  if (!rc)
  {
    rc = DEx_offerCreate(seller_addr, curr, nValue, block, desired, fee, btl, txid, nAmended);
  }

  return rc;
}

// returns 0 if everything is OK
int mastercore::DEx_acceptCreate(const string &buyer, const string &seller, int curr, uint64_t nValue, int block, uint64_t fee_paid, uint64_t *nAmended)
{
int rc = DEX_ERROR_ACCEPT - 10;
OfferMap::iterator my_it;
const string selloffer_combo = STR_SELLOFFER_ADDR_CURR_COMBO(seller);
const string accept_combo = STR_ACCEPT_ADDR_CURR_ADDR_COMBO(seller, buyer);
uint64_t nActualAmount = getMPbalance(seller, curr, SELLOFFER_RESERVE);

  my_it = my_offers.find(selloffer_combo);

  if (my_it == my_offers.end()) return DEX_ERROR_ACCEPT -15;

  CMPOffer &offer = my_it->second;

  if (msc_debug_dex) fprintf(mp_fp, "%s(offer: %s)\n", __FUNCTION__, offer.getHash().GetHex().c_str());

  // here we ensure the correct BTC fee was paid in this acceptance message, per spec
  if (fee_paid < offer.getMinFee())
  {
    fprintf(mp_fp, "ERROR: fee too small -- the ACCEPT is rejected! (%lu is smaller than %lu)\n", fee_paid, offer.getMinFee());
    return DEX_ERROR_ACCEPT -105;
  }

  fprintf(mp_fp, "%s(%s) OFFER FOUND\n", __FUNCTION__, selloffer_combo.c_str());

  // the older accept is the valid one: do not accept any new ones!
  if (DEx_getAccept(seller, curr, buyer))
  {
    fprintf(mp_fp, "%s() ERROR: an accept from this same seller for this same offer is already open !!!!!\n", __FUNCTION__);
    return DEX_ERROR_ACCEPT -205;
  }

  if (nActualAmount > nValue)
  {
    nActualAmount = nValue;

    if (nAmended) *nAmended = nActualAmount;
  }

  // TODO: think if we want to save nValue -- as the amount coming off the wire into the object or not
  if (update_tally_map(seller, curr, - nActualAmount, SELLOFFER_RESERVE))
  {
    if (update_tally_map(seller, curr, nActualAmount, ACCEPT_RESERVE))
    {
      // insert into the map !
      my_accepts.insert(std::make_pair(accept_combo, CMPAccept(nActualAmount, block,
       offer.getBlockTimeLimit(), offer.getCurrency(), offer.getOfferAmountOriginal(), offer.getBTCDesiredOriginal(), offer.getHash() )));

      rc = 0;
    }
  }

  return rc;
}

// this function is called by handler_block() for each Accept that has expired
// this function is also called when the purchase has been completed (the buyer bought everything he was allocated)
//
// returns 0 if everything is OK
int mastercore::DEx_acceptDestroy(const string &buyer, const string &seller, int curr, bool bForceErase)
{
int rc = DEX_ERROR_ACCEPT - 20;
CMPOffer *p_offer = DEx_getOffer(seller, curr);
CMPAccept *p_accept = DEx_getAccept(seller, curr, buyer);
bool bReturnToMoney; // return to MONEY of the seller, otherwise return to SELLOFFER_RESERVE
const string accept_combo = STR_ACCEPT_ADDR_CURR_ADDR_COMBO(seller, buyer);

  if (!p_accept) return rc; // sanity check

  const uint64_t nActualAmount = p_accept->getAcceptAmountRemaining();

  // if the offer is gone ACCEPT_RESERVE should go back to MONEY
  if (!p_offer)
  {
    bReturnToMoney = true;
  }
  else
  {
    fprintf(mp_fp, "%s() HASHES: offer=%s, accept=%s\n", __FUNCTION__, p_offer->getHash().GetHex().c_str(), p_accept->getHash().GetHex().c_str());

    // offer exists, determine whether it's the original offer or some random new one
    if (p_offer->getHash() == p_accept->getHash())
    {
      // same offer, return to SELLOFFER_RESERVE
      bReturnToMoney = false;
    }
    else
    {
      // old offer is gone !
      bReturnToMoney = true;
    }
  }

  if (bReturnToMoney)
  {
    if (update_tally_map(seller, curr, - nActualAmount, ACCEPT_RESERVE))
    {
      update_tally_map(seller, curr, nActualAmount, MONEY);
      rc = 0;
    }
  }
  else
  {
    // return to SELLOFFER_RESERVE
    if (update_tally_map(seller, curr, - nActualAmount, ACCEPT_RESERVE))
    {
      update_tally_map(seller, curr, nActualAmount, SELLOFFER_RESERVE);
      rc = 0;
    }
  }

  // can only erase when is NOT called from an iterator loop
  if (bForceErase)
  {
  const AcceptMap::iterator my_it = my_accepts.find(accept_combo);

    if (my_accepts.end() !=my_it) my_accepts.erase(my_it);
  }

  return rc;
}

// incoming BTC payment for the offer
// TODO: verify proper partial payment handling
int mastercore::DEx_payment(uint256 txid, unsigned int vout, string seller, string buyer, uint64_t BTC_paid, int blockNow, uint64_t *nAmended)
{
//  if (msc_debug_dex) fprintf(mp_fp, "%s()\n", __FUNCTION__);

int rc = DEX_ERROR_PAYMENT;
CMPAccept *p_accept;
int curr;

curr = MASTERCOIN_CURRENCY_MSC; //test for MSC accept first
p_accept = DEx_getAccept(seller, curr, buyer);

  if (!p_accept) 
  {
    curr = MASTERCOIN_CURRENCY_TMSC; //test for TMSC accept second
    p_accept = DEx_getAccept(seller, curr, buyer); 
  }

  if (msc_debug_dex) fprintf(mp_fp, "%s(%s, %s)\n", __FUNCTION__, seller.c_str(), buyer.c_str());

  if (!p_accept) return (DEX_ERROR_PAYMENT -1);  // there must be an active Accept for this payment

  const double BTC_desired_original = p_accept->getBTCDesiredOriginal();
  const double offer_amount_original = p_accept->getOfferAmountOriginal();

  if (0==(double)BTC_desired_original) return (DEX_ERROR_PAYMENT -2);  // divide by 0 protection

  double perc_X = (double)BTC_paid/BTC_desired_original;
  double Purchased = offer_amount_original * perc_X;

  uint64_t units_purchased = rounduint64(Purchased);

  const uint64_t nActualAmount = p_accept->getAcceptAmountRemaining();  // actual amount desired, in the Accept

  if (msc_debug_dex)
   fprintf(mp_fp, "BTC_desired= %30.20lf , offer_amount=%30.20lf , perc_X= %30.20lf , Purchased= %30.20lf , units_purchased= %lu\n",
   BTC_desired_original, offer_amount_original, perc_X, Purchased, units_purchased);

  // if units_purchased is greater than what's in the Accept, the buyer gets only what's in the Accept
  if (nActualAmount < units_purchased)
  {
    units_purchased = nActualAmount;

    if (nAmended) *nAmended = units_purchased;
  }

  if (update_tally_map(seller, curr, - units_purchased, ACCEPT_RESERVE))
  {
      update_tally_map(buyer, curr, units_purchased, MONEY);
      rc = 0;
      bool bValid = true;
      p_txlistdb->recordPaymentTX(txid, bValid, blockNow, vout, curr, units_purchased, buyer, seller);

      fprintf(mp_fp, "#######################################################\n");
  }

  // reduce the amount of units still desired by the buyer and if 0 must destroy the Accept
  if (p_accept->reduceAcceptAmountRemaining_andIsZero(units_purchased))
  {
  const uint64_t selloffer_reserve = getMPbalance(seller, curr, SELLOFFER_RESERVE);
  const uint64_t accept_reserve = getMPbalance(seller, curr, ACCEPT_RESERVE);

    DEx_acceptDestroy(buyer, seller, curr, true);

    // delete the Offer object if there is nothing in its Reserves -- everything got puchased and paid for
    if ((0 == selloffer_reserve) && (0 == accept_reserve))
    {
      DEx_offerDestroy(seller, curr);
    }
  }

  return rc;
}

unsigned int eraseExpiredAccepts(int blockNow)
{
unsigned int how_many_erased = 0;
AcceptMap::iterator my_it = my_accepts.begin();

  while (my_accepts.end() != my_it)
  {
    // my_it->first = key
    // my_it->second = value

    CMPAccept &mpaccept = my_it->second;

    if ((blockNow - mpaccept.block) >= (int) mpaccept.getBlockTimeLimit())
    {
      fprintf(mp_fp, "%s() FOUND EXPIRED ACCEPT, erasing: blockNow=%d, offer block=%d, blocktimelimit= %d\n",
       __FUNCTION__, blockNow, mpaccept.block, mpaccept.getBlockTimeLimit());

      // extract the seller, buyer & currency from the Key
      std::vector<std::string> vstr;
      boost::split(vstr, my_it->first, boost::is_any_of("-+"), token_compress_on);
      string seller = vstr[0];
      int currency = atoi(vstr[1]);
      string buyer = vstr[2];

      DEx_acceptDestroy(buyer, seller, currency);

      my_accepts.erase(my_it++);

      ++how_many_erased;
    }
    else my_it++;

  }

  return how_many_erased;
}

// bSell = true when selling property for MSC/TMSC
int mastercore::MetaDEx_Phase1(const string &addr, unsigned int property, bool bSell, const uint256 &txid, unsigned int idx)
{
  fprintf(mp_fp, "%s(%s, bSell=%s), line %d, file: %s\n", __FUNCTION__, addr.c_str(), bSell ? "YES":"NO", __LINE__, __FILE__);

  return 0;
}

int mastercore::MetaDEx_Create(const string &sender_addr, unsigned int curr, uint64_t amount, int block, unsigned int currency_desired, uint64_t amount_desired, const uint256 &txid, unsigned int idx)
{
int rc = METADEX_ERROR -1;
bool bPhase1Seller = true; // seller (property for MSC) or buyer (property for MSC); only applies to phase 1 code
bool bAdded = false;
bool bTraded = false;

  if (msc_debug_metadex) fprintf(mp_fp, "%s(%s, %u, %lu)\n", __FUNCTION__, sender_addr.c_str(), curr, amount);

  // MetaDEx implementation phase 1 check
  if ((curr != MASTERCOIN_CURRENCY_MSC) && (currency_desired != MASTERCOIN_CURRENCY_MSC) &&
   (curr != MASTERCOIN_CURRENCY_TMSC) && (currency_desired != MASTERCOIN_CURRENCY_TMSC))
  {
    return METADEX_ERROR -800;
  }

  if ((MASTERCOIN_CURRENCY_MSC == curr) || (MASTERCOIN_CURRENCY_TMSC == curr)) bPhase1Seller = false;

  const string combo = STR_SELLOFFER_ADDR_CURR_COMBO(sender_addr);

  (void) MetaDEx_Phase1(sender_addr, bPhase1Seller ? curr:currency_desired, bPhase1Seller, txid, idx);

  if (update_tally_map(sender_addr, curr, - amount, MONEY)) // subtract from what's available
  {
    update_tally_map(sender_addr, curr, amount, SELLOFFER_RESERVE); // put in reserve

  // --------------------------------
  {
    // store the data into the temp MetaDEx object here
    CMPMetaDEx mdex(sender_addr, block, curr, amount, currency_desired, amount_desired, txid, idx);

    // given the currency & the price find the proper place for insertion
    // price simulated with 'double' for now...
    double price = (double)amount / (double)amount_desired;

    // TODO: reconsider for boost::multiprecision
    // FIXME
    if (0 >= price)
    {
      // do not work with 0 prices
      return METADEX_ERROR -66;
    }

    // check matches (to ADD and to TRADE)
    bTraded = MetaDExMatch(curr, currency_desired, (1/price), true, &mdex); // inverse price match to TRADE
    bAdded  = MetaDExMatch(curr, currency_desired, price, false, &mdex); // straight match to ADD

    if (!bAdded)
    {
    md_PricesMap temp_prices, *p_prices = get_Prices(curr);
    md_Set temp_indexes, *p_indexes = NULL;

    std::pair<md_Set::iterator,bool> ret;

    if (p_prices)
    {
      p_indexes = get_Indexes(p_prices, price);
    }

    if (!p_indexes) p_indexes = &temp_indexes;

    {
      ret = p_indexes->insert(mdex);

      if (false == ret.second)
      {
        printf("%s() ERROR: ALREADY EXISTS, line %d, file: %s\n", __FUNCTION__, __LINE__, __FILE__);
        fprintf(mp_fp, "%s() ERROR: ALREADY EXISTS, line %d, file: %s\n", __FUNCTION__, __LINE__, __FILE__);
      }
      else
      {
        if (msc_debug_metadex) fprintf(mp_fp, "INSERTED: %12.11lf = %s\n", price, mdex.ToString().c_str());
      }
    }

    if (!p_prices) p_prices = &temp_prices;

    (*p_prices)[price] = *p_indexes;

    metadex[curr] = *p_prices;
    }
  }

  // --------------------------------

    rc = 0;
  }

  return rc;
}

// returns 0 if everything is OK
int mastercore::MetaDEx_Destroy(const string &sender_addr, unsigned int curr)
{
  if (msc_debug_metadex) fprintf(mp_fp, "%s(%s, %u)\n", __FUNCTION__, sender_addr.c_str(), curr);

  return 0;
}

bool MetaDEx_compare::operator()(const CMPMetaDEx &lhs, const CMPMetaDEx &rhs) const
{
  if (lhs.getBlock() == rhs.getBlock()) return lhs.getIdx() < rhs.getIdx();
  else return lhs.getBlock() < rhs.getBlock();
}
