#ifndef EXTENSIONS_CORE_MODIFICATIONS_SLICE_H
#define EXTENSIONS_CORE_MODIFICATIONS_SLICE_H

#include <stddef.h>
#include <vector>

/**
 * Returns a part of a vector specified by start and end position.
 *
 * @param[in] vch     The vector
 * @param[in] nBegin  First position
 * @param[in] nEnd    Last position
 * @return The subvector
 */
template <typename T>
inline std::vector<T> Subrange(const std::vector<T>& vch, size_t nStart, size_t nEnd)
{
    const size_t nSize = vch.size();

    // Ensure it's not beyond the last element
    if (nEnd > nSize) {
        nEnd = nSize;
    }

    return std::vector<T>(vch.begin() + nStart, vch.begin() + nEnd);
}

/**
 * Slices a vector into parts of the given length.
 *
 * @param[in]  vch      The vector
 * @param[out] vvchRet  The subvectors
 * @param[in]  nLength  The length of each part
 * @return The number of subvectors
 */
template <typename T>
inline size_t Slice(const std::vector<T>& vch, std::vector<std::vector<T> >& vvchRet, size_t nLength)
{
    const size_t nTotal = vch.size();
    const size_t nItems = (nTotal / nLength) + (nTotal % nLength != 0);

    vvchRet.reserve(nItems);

    for (size_t nPos = 0; nPos < nTotal; nPos += nLength) {
        vvchRet.push_back(Subrange(vch, nPos, nPos + nLength));
    }

    return nItems;
}

#endif // EXTENSIONS_CORE_MODIFICATIONS_SLICE_H