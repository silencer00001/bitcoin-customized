#ifndef MASTERCORE_MDEX_H
#define MASTERCORE_MDEX_H

#include "mastercore_log.h"

#include "uint256.h"

#include <boost/lexical_cast.hpp>
#include <boost/multiprecision/cpp_dec_float.hpp>
#include <boost/multiprecision/cpp_int.hpp>
#include <boost/rational.hpp>

#include <openssl/sha.h>

#include <stdint.h>

#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <string>

using boost::multiprecision::int128_t;

typedef boost::multiprecision::cpp_dec_float_100 dec_float;
typedef boost::rational<int128_t> rational_t;

#define DISPLAY_PRECISION_LEN  50

inline bool rangeInt64(const int128_t& value)
{
    return (std::numeric_limits<int64_t>::min() <= value && value <= std::numeric_limits<int64_t>::max());
}

inline bool rangeInt64(const rational_t& value)
{
    return (rangeInt64(value.numerator()) && rangeInt64(value.denominator()));
}

inline std::string xToString(const dec_float& value)
{
    return value.str(DISPLAY_PRECISION_LEN, std::ios_base::fixed);
}

inline std::string xToString(const int128_t& value)
{
    return strprintf("%s", boost::lexical_cast<std::string>(value));
}

inline std::string xToString(const rational_t& value)
{
    if (rangeInt64(value)) {
        int64_t num = value.numerator().convert_to<int64_t>();
        int64_t denom = value.denominator().convert_to<int64_t>();
        dec_float x = dec_float(num) / dec_float(denom);
        return xToString(x);
    } else {
        return strprintf("%s / %s", xToString(value.numerator()), xToString(value.denominator()));
    }
}

inline int128_t xToInt128(const rational_t& value, bool fRoundUp = true)
{
    // for integer rounding up: ceil(num / denom) => 1 + (num - 1) / denom
    int128_t result(0);

    if (!fRoundUp) {
        result = value.numerator() / value.denominator();
    } else {
        result = int128_t(1) + (value.numerator() - int128_t(1)) / value.denominator();
    }

    return result;
}

inline int64_t xToInt64(const rational_t& value, bool fRoundUp = true)
{
    int128_t result = xToInt128(value, fRoundUp);

    assert(rangeInt64(result));

    return result.convert_to<int64_t>();
}

/** A trade on the distributed exchange.
 */
class CMPMetaDEx
{
private:
    int block;
    uint256 txid;
    unsigned int idx; // index within block
    uint32_t property;
    int64_t amount_forsale;
    uint32_t desired_property;
    int64_t amount_desired;
    int64_t amount_remaining;
    uint8_t subaction;
    std::string addr;

public:
    uint256 getHash() const { return txid; }
    void setHash(const uint256& hash) { txid = hash; }

    unsigned int getProperty() const { return property; }
    unsigned int getDesProperty() const { return desired_property; }

    int64_t getAmountForSale() const { return amount_forsale; }
    int64_t getAmountDesiredOriginal() const { return amount_desired; }
    int64_t getAmountDesired() const;
    int64_t getAmountRemaining() const { return amount_remaining; }

    void setAmountRemaining(int64_t ar, const std::string& label = "");

    uint8_t getAction() const { return subaction; }

    const std::string& getAddr() const { return addr; }

    int getBlock() const { return block; }
    unsigned int getIdx() const { return idx; }

    uint64_t getBlockTime() const;

    // needed only by the RPC functions
    // needed only by the RPC functions
    CMPMetaDEx()
      : block(0), txid(0), idx(0), property(0), amount_forsale(0), desired_property(0), amount_desired(0),
        amount_remaining(0), subaction(0) {}

    CMPMetaDEx(const std::string& addr, int b, uint32_t c, int64_t nValue, uint32_t cd, int64_t ad,
               const uint256& tx, uint32_t i, uint8_t suba)
      : block(b), txid(tx), idx(i), property(c), amount_forsale(nValue), desired_property(cd), amount_desired(ad),
        amount_remaining(nValue), subaction(suba), addr(addr) {}

    CMPMetaDEx(const std::string& addr, int b, uint32_t c, int64_t nValue, uint32_t cd, int64_t ad,
               const uint256& tx, uint32_t i, uint8_t suba, int64_t ar)
      : block(b), txid(tx), idx(i), property(c), amount_forsale(nValue), desired_property(cd), amount_desired(ad),
        amount_remaining(ar), subaction(suba), addr(addr) {}

    void Set(const std::string&, int, uint32_t, int64_t, uint32_t, int64_t, const uint256&, uint32_t, uint8_t);

    std::string ToString() const;

    rational_t unitPrice() const;
    rational_t inversePrice() const;

    void saveOffer(std::ofstream& file, SHA256_CTX* shaCtx) const;
};

namespace mastercore
{
struct MetaDEx_compare
{
    bool operator()(const CMPMetaDEx& lhs, const CMPMetaDEx& rhs) const;
};

// ---------------
//! Set of objects sorted by block+idx
typedef std::set<CMPMetaDEx, MetaDEx_compare> md_Set; 
//! Map of prices; there is a set of sorted objects for each price
typedef std::map<rational_t, md_Set> md_PricesMap;
//! Map of properties; there is a map of prices for each property
typedef std::map<uint32_t, md_PricesMap> md_PropertiesMap;

extern md_PropertiesMap metadex;

// TODO: explore a property-pair, instead of a single property as map's key........
md_PricesMap* get_Prices(uint32_t prop);
md_Set* get_Indexes(md_PricesMap* p, rational_t price);
// ---------------

int MetaDEx_ADD(const std::string& sender_addr, uint32_t, int64_t, int block, uint32_t property_desired, int64_t amount_desired, const uint256& txid, unsigned int idx);
int MetaDEx_CANCEL_AT_PRICE(const uint256&, uint32_t, const std::string&, uint32_t, int64_t, uint32_t, int64_t);
int MetaDEx_CANCEL_ALL_FOR_PAIR(const uint256&, uint32_t, const std::string&, uint32_t, uint32_t);
int MetaDEx_CANCEL_EVERYTHING(const uint256& txid, uint32_t block, const std::string& sender_addr, unsigned char ecosystem);
bool MetaDEx_INSERT(const CMPMetaDEx& objMetaDEx);
void MetaDEx_debug_print(bool bShowPriceLevel = false, bool bDisplay = false);
}

#endif // MASTERCORE_MDEX_H
