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

extern int msc_debug_dex, msc_debug_metadex, msc_debug_metadex2, msc_debug_metadex3;

md_PropertiesMap mastercore::metadex;

md_PricesMap *get_Prices(unsigned int prop)
{
md_PropertiesMap::iterator it = metadex.find(prop);

  if (it != metadex.end()) return &(it->second);

  return (md_PricesMap *) NULL;
}

md_Set *get_Indexes(md_PricesMap *p, XDOUBLE price)
{
md_PricesMap::iterator it = p->find(price);

  if (it != p->end()) return &(it->second);

  return (md_Set *) NULL;
}

enum MatchReturnType
{
  NOTHING             = 0,
  TRADED              = 1,
  TRADED_MOREINSELLER,
  TRADED_MOREINBUYER,
  ADDED,
  CANCELLED,
};

const string getTradeReturnType(MatchReturnType ret)
{
  switch (ret)
  {
    case NOTHING: return string("NOTHING");
    case TRADED: return string("TRADED");
    case TRADED_MOREINSELLER: return string("TRADED_MOREINSELLER");
    case TRADED_MOREINBUYER: return string("TRADED_MOREINBUYER");
    case ADDED: return string("ADDED");
    case CANCELLED: return string("CANCELLED");
    default: return string("* unknown *");
  }
}

bool operator==(XDOUBLE first, XDOUBLE second)
{
  return (first.str(INTERNAL_PRECISION_LEN, std::ios_base::fixed) == second.str(INTERNAL_PRECISION_LEN, std::ios_base::fixed));
}

bool operator!=(XDOUBLE first, XDOUBLE second)
{
  return !(first == second);
}

bool operator<=(XDOUBLE first, XDOUBLE second)
{
  return ((first.str(INTERNAL_PRECISION_LEN, std::ios_base::fixed) < second.str(INTERNAL_PRECISION_LEN, std::ios_base::fixed)) || (first == second));
}

bool operator>=(XDOUBLE first, XDOUBLE second)
{
  return ((first.str(INTERNAL_PRECISION_LEN, std::ios_base::fixed) > second.str(INTERNAL_PRECISION_LEN, std::ios_base::fixed)) || (first == second));
}

static void PriceCheck(const string &label, XDOUBLE left, XDOUBLE right, FILE *fp = stdout)
{
const bool bOK = (left == right);

  fprintf(fp, "PRICE CHECK %s: buyer = %s , inserted = %s : %s\n", label.c_str(),
   left.str(DISPLAY_PRECISION_LEN, std::ios_base::fixed).c_str(),
   right.str(DISPLAY_PRECISION_LEN, std::ios_base::fixed).c_str(), bOK ? "good":"PROBLEM!");
}

// find the best match on the market
// INPUT: property, desprop, desprice = of the new order being inserted; the new object being processed
// RETURN: 
static MatchReturnType x_Trade(CMPMetaDEx *newo)
{
const CMPMetaDEx *p_older = NULL;
md_PricesMap *prices = NULL;
const string buyer_addr = newo->getAddr();
const unsigned int prop = newo->getProperty();
const unsigned int desprop = newo->getDesProperty();
MatchReturnType NewReturn = NOTHING;
bool bBuyerSatisfied = false;
const XDOUBLE buyersprice = newo->getEffectivePrice();
const XDOUBLE desprice = (1/buyersprice); // inverse

  if (msc_debug_metadex)
  {
    fprintf(mp_fp, "%s(%s: prop=%u, desprop=%u, desprice= %s);newo: %s\n",
     __FUNCTION__, buyer_addr.c_str(), prop, desprop, desprice.str(DISPLAY_PRECISION_LEN, std::ios_base::fixed).c_str(), newo->ToString().c_str());

    mp_log( "%s(%s: prop=%u, desprop=%u, desprice= %s);newo: %s\n",
     __FUNCTION__, buyer_addr, prop, desprop, desprice.str(DISPLAY_PRECISION_LEN, std::ios_base::fixed), newo->ToString());
  }

  prices = get_Prices(desprop);

  // nothing for the desired property exists in the market, sorry!
  if (!prices)
  {
    fprintf(mp_fp, "%s()=%u:%s NOT FOUND ON THE MARKET\n", __FUNCTION__, NewReturn, getTradeReturnType(NewReturn).c_str());
    return NewReturn;
  }

  md_Set *indexes;
  XDOUBLE sellers_price;

  // within the desired property map (given one property) iterate over the items looking at prices
  for (md_PricesMap::iterator my_it = prices->begin(); my_it != prices->end(); ++my_it)
  { // check all prices
    sellers_price = (my_it->first);

      if (msc_debug_metadex2) fprintf(mp_fp, "comparing prices: desprice %s needs to be LESS THAN OR EQUAL TO %s\n",
       desprice.str(DISPLAY_PRECISION_LEN, std::ios_base::fixed).c_str(), sellers_price.str(DISPLAY_PRECISION_LEN, std::ios_base::fixed).c_str());

      // is the desired price check satisfied?
      if (desprice > sellers_price) continue;

    indexes = &(my_it->second);

    // at good (single) price level and property iterate over offers looking at all parameters to find the match
    md_Set::iterator iitt;
    for (iitt = indexes->begin(); iitt != indexes->end();)
    { // specific price, check all properties
      p_older = &(*iitt);

      if (msc_debug_metadex) fprintf(mp_fp, "Looking at existing: %s (its prop= %u, its des prop= %u) = %s\n",
       sellers_price.str(DISPLAY_PRECISION_LEN, std::ios_base::fixed).c_str(), p_older->getProperty(), p_older->getDesProperty(), p_older->ToString().c_str());

      // is the desired property correct?
        if (p_older->getDesProperty() != prop)
        {
          ++iitt;
          continue;
        }

      if (msc_debug_metadex) fprintf(mp_fp, "MATCH FOUND, Trade: %s = %s\n", sellers_price.str(DISPLAY_PRECISION_LEN, std::ios_base::fixed).c_str(), p_older->ToString().c_str());

      // if found

        // All Matched ! Trade now.
        // p_older is the old order pointer
        // newo is the new order pointer
        // the price in the older order is used
        const int64_t seller_amountOffered = p_older->getAmount();
        const int64_t buyer_amountWanted = newo->getAmountDesired();

        if (msc_debug_metadex) fprintf(mp_fp, "$$ trading using price: %s; amount offered= %ld, amount wanted= %ld\n",
         sellers_price.str(DISPLAY_PRECISION_LEN, std::ios_base::fixed).c_str(), seller_amountOffered, buyer_amountWanted);
        if (msc_debug_metadex) fprintf(mp_fp, "$$ old: %s\n", p_older->ToString().c_str());
        if (msc_debug_metadex) fprintf(mp_fp, "$$ new: %s\n", newo->ToString().c_str());

        int64_t buyer_amountGot = buyer_amountWanted;

        if (seller_amountOffered < buyer_amountWanted)
        {
          buyer_amountGot = seller_amountOffered;
        }

        XDOUBLE amount_paid = (XDOUBLE) buyer_amountGot / sellers_price;
        std::string str_amount_paid = amount_paid.str(INTERNAL_PRECISION_LEN, std::ios_base::fixed);
        std::string str_paid_int_part = str_amount_paid.substr(0, str_amount_paid.find_first_of("."));
        const int64_t paymentAmount = boost::lexical_cast<int64_t>( str_paid_int_part );

        const int64_t seller_amountLeft = seller_amountOffered - buyer_amountGot;
        const int64_t buyer_amountStillWanted = buyer_amountWanted - buyer_amountGot;

        if (msc_debug_metadex) fprintf(mp_fp, "$$ buyer_got= %ld, seller_left= %ld, buyer_still= %ld, payment= %ld\n",
         buyer_amountGot, seller_amountLeft, buyer_amountStillWanted, paymentAmount);

        XDOUBLE amount_left = (XDOUBLE) seller_amountLeft / sellers_price;
        std::string str_amount_left = amount_left.str(INTERNAL_PRECISION_LEN, std::ios_base::fixed);
        std::string str_left_int_part = str_amount_left.substr(0, str_amount_left.find_first_of("."));

        CMPMetaDEx seller_replacement = *p_older;

        seller_replacement.setAmount(seller_amountLeft, "seller_replacement");
        seller_replacement.setAmountDesired(boost::lexical_cast<int64_t>( str_left_int_part ), "seller_replacement");

        // transfer the payment property from buyer to seller
        // TODO: do something when failing here............
        // ...
        if (update_tally_map(newo->getAddr(), newo->getProperty(), - paymentAmount, BALANCE))
        {
          if (update_tally_map(p_older->getAddr(), p_older->getDesProperty(), paymentAmount, BALANCE))
          {
          }
        }

        // transfer the market (the one being sold) property from seller to buyer
        // TODO: do something when failing here............
        // ...
        if (update_tally_map(p_older->getAddr(), p_older->getProperty(), - buyer_amountGot, SELLOFFER_RESERVE))
        {
          update_tally_map(newo->getAddr(), newo->getDesProperty(), buyer_amountGot, BALANCE);
        }

        NewReturn = TRADED;

        XDOUBLE will_pay = (XDOUBLE) buyer_amountStillWanted * buyersprice;
        std::string str_will_pay = will_pay.str(INTERNAL_PRECISION_LEN, std::ios_base::fixed);
        std::string str_wanted_int_part = str_will_pay.substr(0, str_will_pay.find_first_of("."));

        newo->setAmount(boost::lexical_cast<int64_t>( str_wanted_int_part ), "buyer");
        newo->setAmountDesired(buyer_amountStillWanted, "buyer");

        if (0 < buyer_amountStillWanted)
        {
          NewReturn = TRADED_MOREINBUYER;

          PriceCheck(getTradeReturnType(NewReturn), buyersprice, newo->getEffectivePrice(), mp_fp);
        }
        else
        {
          bBuyerSatisfied = true;
        }

        if (0 < seller_amountLeft)  // done with all loops, update the seller, buyer is fully satisfied
        {
          NewReturn = TRADED_MOREINSELLER;
          bBuyerSatisfied = true;

          PriceCheck(getTradeReturnType(NewReturn), p_older->getEffectivePrice(), seller_replacement.getEffectivePrice(), mp_fp);
        }

        if (msc_debug_metadex) fprintf(mp_fp, "==== TRADED !!! %u=%s\n", NewReturn, getTradeReturnType(NewReturn).c_str());
        // record the trade in MPTradeList
        t_tradelistdb->recordTrade(p_older->getHash(), newo->getHash(), p_older->getAddr(), newo->getAddr(), p_older->getProperty(), newo->getProperty(), buyer_amountGot, paymentAmount, newo->getBlock());

      if (msc_debug_metadex) fprintf(mp_fp, "++ erased old: %s\n", iitt->ToString().c_str());
      // erase the old seller element
      indexes->erase(iitt++);

      if (bBuyerSatisfied)
      {
        // insert the updated one in place of the old
        if (0 < seller_replacement.getAmount())
        {
          fprintf(mp_fp, "++ inserting seller_replacement: %s\n", seller_replacement.ToString().c_str());
          indexes->insert(seller_replacement);
        }
        break;
      }
    } // specific price, check all properties

    if (bBuyerSatisfied) break;
  } // check all prices
  
  fprintf(mp_fp, "%s()=%u:%s\n", __FUNCTION__, NewReturn, getTradeReturnType(NewReturn).c_str());

  return NewReturn;
}

// find the best match on the market
// INPUT: property, desprop, desprice = of the new order being inserted; the new object being processed
// INPUT: CancelAtPrice will CANCEL the order once the exact match is found
// RETURN: 
static MatchReturnType x_Add(CMPMetaDEx *newo, bool bCancelAtPrice = false)
{
const CMPMetaDEx *p_older = NULL;
md_PricesMap *prices = NULL;
const string buyer_addr = newo->getAddr();
const unsigned int prop = newo->getProperty();
const unsigned int desprop = newo->getDesProperty();
const XDOUBLE buyersprice = newo->getEffectivePrice();
XDOUBLE price = 0;
MatchReturnType NewReturn = NOTHING;
bool found = false;

  if (msc_debug_metadex)
  {
    fprintf(mp_fp, "%s(%s: prop=%u, desprop=%u);newo: %s\n",
     __FUNCTION__, buyer_addr.c_str(), prop, desprop, newo->ToString().c_str());

    mp_log( "%s(%s: prop=%u, desprop=%u);newo: %s\n",
     __FUNCTION__, buyer_addr, prop, desprop, newo->ToString());
  }

  prices = get_Prices(prop);

  // nothing for the desired property exists in the market, sorry!
  if (!prices)
  {
    fprintf(mp_fp, "%s()=%u:%s NOTHING FOUND FOR THE PRICE\n", __FUNCTION__, NewReturn, getTradeReturnType(NewReturn).c_str());

    return NewReturn;
  }

  md_Set *indexes;

  // within the desired property map (given one property) iterate over the items looking at prices
  for (md_PricesMap::iterator my_it = prices->begin(); my_it != prices->end(); ++my_it)
  {
    price = (my_it->first);

    if (buyersprice != price) continue;

    indexes = &(my_it->second);

    // at good price level and property iterate over offers looking at all parameters to find the match
    md_Set::iterator iitt;
    for (iitt = indexes->begin(); iitt != indexes->end(); ++iitt)
    { // for iitt
      p_older = &(*iitt);

      if (msc_debug_metadex) fprintf(mp_fp, "Looking at: %s (its prop= %u, its des prop= %u) = %s\n",
       price.str(DISPLAY_PRECISION_LEN, std::ios_base::fixed).c_str(), p_older->getProperty(), p_older->getDesProperty(), p_older->ToString().c_str());

      // is the desired property correct?
      if ((p_older->getDesProperty() != desprop) || (p_older->getAddr() != buyer_addr))
      {
        continue;
      }

      CMPMetaDEx replacement = *p_older;

      if (msc_debug_metadex) fprintf(mp_fp, "MATCH FOUND, Add: %s = %s\n", price.str(DISPLAY_PRECISION_LEN, std::ios_base::fixed).c_str(), p_older->ToString().c_str());

      {
        // ADD, bump the replacement's amounts
        replacement.setAmount(newo->getAmount() + p_older->getAmount(), "ADDing");
        replacement.setAmountDesired(newo->getAmountDesired() + p_older->getAmountDesired(), "ADDing");

        // destroy the txid as a marker...
        replacement.nullTxid();

        NewReturn = ADDED;
      }

      if (msc_debug_metadex) fprintf(mp_fp, "++ erased old: %s\n", iitt->ToString().c_str());
      // erase the old element
      indexes->erase(iitt);

      if (bCancelAtPrice)
      {
        // move from reserve to main
        update_tally_map(p_older->getAddr(), p_older->getProperty(), - p_older->getAmount(), SELLOFFER_RESERVE);
        update_tally_map(p_older->getAddr(), p_older->getProperty(), p_older->getAmount(), BALANCE);

        NewReturn = CANCELLED;
      }
      // insert the updated one in place of the old
      else if (0 < replacement.getAmount())
      {
        if (msc_debug_metadex) fprintf(mp_fp, "==== ADDED !!! %s\n", replacement.ToString().c_str());
        fprintf(mp_fp, "++ inserting replacement: %s\n", replacement.ToString().c_str());
        indexes->insert(replacement);
      }

      newo->setAmount(0, "ADD-destroy");
      newo->setAmountDesired(0, "ADD-destroy");
      found = true;
      break;  // do not need to and can't iterate any more
    }

    if (found) break;
  }
  
  fprintf(mp_fp, "%s()=%u:%s\n", __FUNCTION__, NewReturn, getTradeReturnType(NewReturn).c_str());

  return NewReturn;
}

void mastercore::MetaDEx_debug_print(FILE *fp, bool bShowPriceLevel)
{
  fprintf(fp, "<<<\n");
  for (md_PropertiesMap::iterator my_it = metadex.begin(); my_it != metadex.end(); ++my_it)
  {
    unsigned int prop = my_it->first;

    fprintf(fp, " ## property: %u\n", prop);
    md_PricesMap & prices = my_it->second;

    for (md_PricesMap::iterator it = prices.begin(); it != prices.end(); ++it)
    {
      XDOUBLE price = (it->first);
      md_Set & indexes = (it->second);

      if (bShowPriceLevel) fprintf(fp, "  # Price Level: %s\n", price.str(DISPLAY_PRECISION_LEN, std::ios_base::fixed).c_str());

      for (md_Set::iterator it = indexes.begin(); it != indexes.end(); ++it)
      {
      CMPMetaDEx obj = *it;

        fprintf(fp, "%s= %s\n", price.str(DISPLAY_PRECISION_LEN, std::ios_base::fixed).c_str() , obj.ToString().c_str());
      }
    }
  }
  fprintf(fp, ">>>\n");
}

void CMPMetaDEx::Set(const string &sa, int b, unsigned int c, uint64_t nValue, unsigned int cd, uint64_t ad, const uint256 &tx, unsigned int i, unsigned char suba)
{
  addr = sa;
  block = b;
  txid = tx;
  property = c;
  amount= nValue;
  desired_property = cd;
  amount_desired = ad;
  idx = i;
  subaction = suba;
}

CMPMetaDEx::CMPMetaDEx(const string &addr, int b, unsigned int c, uint64_t nValue, unsigned int cd, uint64_t ad, const uint256 &tx, unsigned int i, unsigned char suba)
{
  Set(addr, b,c,nValue,cd,ad,tx,i,suba);
}

std::string CMPMetaDEx::ToString() const
{
  return strprintf("%s:%34s in %d/%03u, txid: %s, trade #%u %s for #%u %s",
   getEffectivePrice().str(DISPLAY_PRECISION_LEN, std::ios_base::fixed),
   addr.c_str(), block, idx, txid.ToString().substr(0,10).c_str(),
   property, FormatMP(property, amount), desired_property, FormatMP(desired_property, amount_desired));
}

// check to see if such a sell offer exists
bool mastercore::DEx_offerExists(const string &seller_addr, unsigned int prop)
{
//  if (msc_debug_dex) fprintf(mp_fp, "%s()\n", __FUNCTION__);
const string combo = STR_SELLOFFER_ADDR_PROP_COMBO(seller_addr);
OfferMap::iterator my_it = my_offers.find(combo);

  return !(my_it == my_offers.end());
}

// getOffer may replace DEx_offerExists() in the near future
// TODO: locks are needed around map's insert & erase
CMPOffer *mastercore::DEx_getOffer(const string &seller_addr, unsigned int prop)
{
  if (msc_debug_dex) fprintf(mp_fp, "%s(%s, %u)\n", __FUNCTION__, seller_addr.c_str(), prop);
const string combo = STR_SELLOFFER_ADDR_PROP_COMBO(seller_addr);
OfferMap::iterator my_it = my_offers.find(combo);

  if (my_it != my_offers.end()) return &(my_it->second);

  return (CMPOffer *) NULL;
}

// TODO: locks are needed around map's insert & erase
CMPAccept *mastercore::DEx_getAccept(const string &seller_addr, unsigned int prop, const string &buyer_addr)
{
  if (msc_debug_dex) fprintf(mp_fp, "%s(%s, %u, %s)\n", __FUNCTION__, seller_addr.c_str(), prop, buyer_addr.c_str());
const string combo = STR_ACCEPT_ADDR_PROP_ADDR_COMBO(seller_addr, buyer_addr);
AcceptMap::iterator my_it = my_accepts.find(combo);

  if (my_it != my_accepts.end()) return &(my_it->second);

  return (CMPAccept *) NULL;
}

// returns 0 if everything is OK
int mastercore::DEx_offerCreate(string seller_addr, unsigned int prop, uint64_t nValue, int block, uint64_t amount_des, uint64_t fee, unsigned char btl, const uint256 &txid, uint64_t *nAmended)
{
int rc = DEX_ERROR_SELLOFFER;

  // sanity check our params are OK
  if ((!btl) || (!amount_des)) return (DEX_ERROR_SELLOFFER -101); // time limit or amount desired empty

  if (DEx_getOffer(seller_addr, prop)) return (DEX_ERROR_SELLOFFER -10);  // offer already exists

  const string combo = STR_SELLOFFER_ADDR_PROP_COMBO(seller_addr);

  if (msc_debug_dex)
   fprintf(mp_fp, "%s(%s|%s), nValue=%lu)\n", __FUNCTION__, seller_addr.c_str(), combo.c_str(), nValue);

  const uint64_t balanceReallyAvailable = getMPbalance(seller_addr, prop, BALANCE);

  // if offering more than available -- put everything up on sale
  if (nValue > balanceReallyAvailable)
  {
  double BTC;

    // AND we must also re-adjust the BTC desired in this case...
    BTC = amount_des * balanceReallyAvailable;
    BTC /= (double)nValue;
    amount_des = rounduint64(BTC);

    nValue = balanceReallyAvailable;

    if (nAmended) *nAmended = nValue;
  }

  if (update_tally_map(seller_addr, prop, - nValue, BALANCE)) // subtract from what's available
  {
    update_tally_map(seller_addr, prop, nValue, SELLOFFER_RESERVE); // put in reserve

    my_offers.insert(std::make_pair(combo, CMPOffer(block, nValue, prop, amount_des, fee, btl, txid)));

    rc = 0;
  }

  return rc;
}

// returns 0 if everything is OK
int mastercore::DEx_offerDestroy(const string &seller_addr, unsigned int prop)
{
const uint64_t amount = getMPbalance(seller_addr, prop, SELLOFFER_RESERVE);

  if (!DEx_offerExists(seller_addr, prop)) return (DEX_ERROR_SELLOFFER -11); // offer does not exist

  const string combo = STR_SELLOFFER_ADDR_PROP_COMBO(seller_addr);

  OfferMap::iterator my_it;

  my_it = my_offers.find(combo);

  if (amount)
  {
    update_tally_map(seller_addr, prop, amount, BALANCE);   // give back to the seller from SellOffer-Reserve
    update_tally_map(seller_addr, prop, - amount, SELLOFFER_RESERVE);
  }

  // delete the offer
  my_offers.erase(my_it);

  if (msc_debug_dex)
   fprintf(mp_fp, "%s(%s|%s)\n", __FUNCTION__, seller_addr.c_str(), combo.c_str());

  return 0;
}

// returns 0 if everything is OK
int mastercore::DEx_offerUpdate(const string &seller_addr, unsigned int prop, uint64_t nValue, int block, uint64_t desired, uint64_t fee, unsigned char btl, const uint256 &txid, uint64_t *nAmended)
{
int rc = DEX_ERROR_SELLOFFER;

  fprintf(mp_fp, "%s(%s, %d)\n", __FUNCTION__, seller_addr.c_str(), prop);

  if (!DEx_offerExists(seller_addr, prop)) return (DEX_ERROR_SELLOFFER -12); // offer does not exist

  rc = DEx_offerDestroy(seller_addr, prop);

  if (!rc)
  {
    rc = DEx_offerCreate(seller_addr, prop, nValue, block, desired, fee, btl, txid, nAmended);
  }

  return rc;
}

// returns 0 if everything is OK
int mastercore::DEx_acceptCreate(const string &buyer, const string &seller, int prop, uint64_t nValue, int block, uint64_t fee_paid, uint64_t *nAmended)
{
int rc = DEX_ERROR_ACCEPT - 10;
OfferMap::iterator my_it;
const string selloffer_combo = STR_SELLOFFER_ADDR_PROP_COMBO(seller);
const string accept_combo = STR_ACCEPT_ADDR_PROP_ADDR_COMBO(seller, buyer);
uint64_t nActualAmount = getMPbalance(seller, prop, SELLOFFER_RESERVE);

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
  if (DEx_getAccept(seller, prop, buyer))
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
  if (update_tally_map(seller, prop, - nActualAmount, SELLOFFER_RESERVE))
  {
    if (update_tally_map(seller, prop, nActualAmount, ACCEPT_RESERVE))
    {
      // insert into the map !
      my_accepts.insert(std::make_pair(accept_combo, CMPAccept(nActualAmount, block,
       offer.getBlockTimeLimit(), offer.getProperty(), offer.getOfferAmountOriginal(), offer.getBTCDesiredOriginal(), offer.getHash() )));

      rc = 0;
    }
  }

  return rc;
}

// this function is called by handler_block() for each Accept that has expired
// this function is also called when the purchase has been completed (the buyer bought everything he was allocated)
//
// returns 0 if everything is OK
int mastercore::DEx_acceptDestroy(const string &buyer, const string &seller, int prop, bool bForceErase)
{
int rc = DEX_ERROR_ACCEPT - 20;
CMPOffer *p_offer = DEx_getOffer(seller, prop);
CMPAccept *p_accept = DEx_getAccept(seller, prop, buyer);
bool bReturnToMoney; // return to BALANCE of the seller, otherwise return to SELLOFFER_RESERVE
const string accept_combo = STR_ACCEPT_ADDR_PROP_ADDR_COMBO(seller, buyer);

  if (!p_accept) return rc; // sanity check

  const uint64_t nActualAmount = p_accept->getAcceptAmountRemaining();

  // if the offer is gone ACCEPT_RESERVE should go back to BALANCE
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
    if (update_tally_map(seller, prop, - nActualAmount, ACCEPT_RESERVE))
    {
      update_tally_map(seller, prop, nActualAmount, BALANCE);
      rc = 0;
    }
  }
  else
  {
    // return to SELLOFFER_RESERVE
    if (update_tally_map(seller, prop, - nActualAmount, ACCEPT_RESERVE))
    {
      update_tally_map(seller, prop, nActualAmount, SELLOFFER_RESERVE);
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
int prop;

prop = OMNI_PROPERTY_MSC; //test for MSC accept first
p_accept = DEx_getAccept(seller, prop, buyer);

  if (!p_accept) 
  {
    prop = OMNI_PROPERTY_TMSC; //test for TMSC accept second
    p_accept = DEx_getAccept(seller, prop, buyer); 
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

  if (update_tally_map(seller, prop, - units_purchased, ACCEPT_RESERVE))
  {
      update_tally_map(buyer, prop, units_purchased, BALANCE);
      rc = 0;
      bool bValid = true;
      p_txlistdb->recordPaymentTX(txid, bValid, blockNow, vout, prop, units_purchased, buyer, seller);

      fprintf(mp_fp, "#######################################################\n");
  }

  // reduce the amount of units still desired by the buyer and if 0 must destroy the Accept
  if (p_accept->reduceAcceptAmountRemaining_andIsZero(units_purchased))
  {
  const uint64_t selloffer_reserve = getMPbalance(seller, prop, SELLOFFER_RESERVE);
  const uint64_t accept_reserve = getMPbalance(seller, prop, ACCEPT_RESERVE);

    DEx_acceptDestroy(buyer, seller, prop, true);

    // delete the Offer object if there is nothing in its Reserves -- everything got puchased and paid for
    if ((0 == selloffer_reserve) && (0 == accept_reserve))
    {
      DEx_offerDestroy(seller, prop);
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

      // extract the seller, buyer & property from the Key
      std::vector<std::string> vstr;
      boost::split(vstr, my_it->first, boost::is_any_of("-+"), token_compress_on);
      string seller = vstr[0];
      int property = atoi(vstr[1]);
      string buyer = vstr[2];

      DEx_acceptDestroy(buyer, seller, property);

      my_accepts.erase(my_it++);

      ++how_many_erased;
    }
    else my_it++;

  }

  return how_many_erased;
}

// pretty much directly linked to the ADD TX21 command off the wire
int mastercore::MetaDEx_ADD(const string &sender_addr, unsigned int prop, uint64_t amount, int block, unsigned int property_desired, uint64_t amount_desired, const uint256 &txid, unsigned int idx)
{
int rc = METADEX_ERROR -1;

  // MetaDEx implementation phase 1 check
  if ((prop != OMNI_PROPERTY_MSC) && (property_desired != OMNI_PROPERTY_MSC) &&
   (prop != OMNI_PROPERTY_TMSC) && (property_desired != OMNI_PROPERTY_TMSC))
  {
    return METADEX_ERROR -800;
  }

    // store the data into the temp MetaDEx object here
    CMPMetaDEx new_mdex(sender_addr, block, prop, amount, property_desired, amount_desired, txid, idx, CMPTransaction::ADD);
    XDOUBLE neworder_buyersprice = new_mdex.getEffectivePrice();

    if (msc_debug_metadex) fprintf(mp_fp, "%s(); buyer obj: %s\n", __FUNCTION__, new_mdex.ToString().c_str());

    // given the property & the price find the proper place for insertion

    // TODO: reconsider for boost::multiprecision
    // FIXME
    if (0 >= neworder_buyersprice)
    {
      // do not work with 0 prices
      return METADEX_ERROR -66;
    }

    if (msc_debug_metadex3) MetaDEx_debug_print(mp_fp);

    // TRADE, check matches, remainder of the order will be put into the order book
    x_Trade(&new_mdex);

    if (msc_debug_metadex3) MetaDEx_debug_print(mp_fp);

    // if anything is left in the new order, INSERT
    if (0 < new_mdex.getAmount())
    {
      x_Add(&new_mdex); // straight match to ADD
    }

    if (msc_debug_metadex3) MetaDEx_debug_print(mp_fp);

    // plain insert
    if (0 < new_mdex.getAmount())
    { // not added nor subtracted, insert as new or post-traded amounts
    md_PricesMap temp_prices, *p_prices = get_Prices(prop);
    md_Set temp_indexes, *p_indexes = NULL;
    std::pair<md_Set::iterator,bool> ret;

      if (p_prices)
      {
        p_indexes = get_Indexes(p_prices, neworder_buyersprice);
      }

      if (!p_indexes) p_indexes = &temp_indexes;

      ret = p_indexes->insert(new_mdex);

      if (false == ret.second)
      {
        printf("%s() ERROR: ALREADY EXISTS, line %d, file: %s\n", __FUNCTION__, __LINE__, __FILE__);
        fprintf(mp_fp, "%s() ERROR: ALREADY EXISTS, line %d, file: %s\n", __FUNCTION__, __LINE__, __FILE__);
      }
      else
      {
        if (update_tally_map(sender_addr, prop, - new_mdex.getAmount(), BALANCE)) // subtract from what's available
        {
          update_tally_map(sender_addr, prop, new_mdex.getAmount(), SELLOFFER_RESERVE); // put in reserve
        }

        // price check
        PriceCheck("Insert", neworder_buyersprice, new_mdex.getEffectivePrice(), mp_fp);

        if (msc_debug_metadex) fprintf(mp_fp, "==== INSERTED: %s= %s\n", neworder_buyersprice.str(DISPLAY_PRECISION_LEN, std::ios_base::fixed).c_str(), new_mdex.ToString().c_str());
      }

      if (!p_prices) p_prices = &temp_prices;

      (*p_prices)[neworder_buyersprice] = *p_indexes;

      metadex[prop] = *p_prices;
    } // Must Insert

  rc = 0;

  if (msc_debug_metadex3) MetaDEx_debug_print(mp_fp);

  return rc;
}

int mastercore::MetaDEx_CANCEL_AT_PRICE(const string &sender_addr, unsigned int prop, uint64_t amount, unsigned int property_desired, uint64_t amount_desired)
{
int rc = METADEX_ERROR -20;
CMPMetaDEx mdex(sender_addr, 0, prop, amount, property_desired, amount_desired, 0, 0, CMPTransaction::CANCEL_AT_PRICE);

  mp_log("%s(), line %d, file: %s\n", __FUNCTION__, __LINE__, __FILE__);
  if (msc_debug_metadex2) MetaDEx_debug_print(mp_fp);

  x_Add(&mdex, true); // re-using the function to CANCEL at exact price

  if (msc_debug_metadex2) MetaDEx_debug_print(mp_fp);

  return rc;
}

int mastercore::MetaDEx_CANCEL_ALL_FOR_PAIR(const string &sender_addr, unsigned int prop, unsigned int property_desired)
{
int rc = METADEX_ERROR -20;
md_PricesMap *prices = get_Prices(prop);
const CMPMetaDEx *p_older = NULL;

  mp_log("%s(), line %d, file: %s\n", __FUNCTION__, __LINE__, __FILE__);
  if (msc_debug_metadex2) MetaDEx_debug_print(mp_fp);

  md_Set *indexes;

  // within the desired property map (given one property) iterate over the items
  for (md_PricesMap::iterator my_it = prices->begin(); my_it != prices->end(); ++my_it)
  {
    indexes = &(my_it->second);

    // at good price level and property iterate over offers looking at all parameters to find the match
    md_Set::iterator iitt;
    for (iitt = indexes->begin(); iitt != indexes->end(); ++iitt)
    { // for iitt
      p_older = &(*iitt);


      // ...
    }
  }

  if (msc_debug_metadex2) MetaDEx_debug_print(mp_fp);

  return rc;
}

int mastercore::MetaDEx_CANCEL_EVERYTHING(const string &sender_addr)
{
int rc = METADEX_ERROR -20;

  mp_log("%s(), line %d, file: %s\n", __FUNCTION__, __LINE__, __FILE__);
  if (msc_debug_metadex2) MetaDEx_debug_print(mp_fp);

  // ...

  if (msc_debug_metadex2) MetaDEx_debug_print(mp_fp);

  return rc;
}

bool MetaDEx_compare::operator()(const CMPMetaDEx &lhs, const CMPMetaDEx &rhs) const
{
  if (lhs.getBlock() == rhs.getBlock()) return lhs.getIdx() < rhs.getIdx();
  else return lhs.getBlock() < rhs.getBlock();
}
