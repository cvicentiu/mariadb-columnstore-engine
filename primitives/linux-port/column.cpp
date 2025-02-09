/* Copyright (C) 2014 InfiniDB, Inc.
   Copyright (C) 2016-2021 MariaDB Corporation

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
   MA 02110-1301, USA. */

#include <iostream>
#include <sstream>
//#define NDEBUG
#include <cassert>
#include <cmath>
#ifndef _MSC_VER
#include <pthread.h>
#else
#endif
using namespace std;

#include <boost/scoped_array.hpp>
using namespace boost;

#include "primitiveprocessor.h"
#include "messagelog.h"
#include "messageobj.h"
#include "we_type.h"
#include "stats.h"
#include "primproc.h"
#include "dataconvert.h"
#include "mcs_decimal.h"

using namespace logging;
using namespace dbbc;
using namespace primitives;
using namespace primitiveprocessor;
using namespace execplan;

namespace
{
using RID_T = uint16_t;  // Row index type, as used in rid arrays

// Column filtering is dispatched 4-way based on the column type,
// which defines implementation of comparison operations for the column values
enum ENUM_KIND {KIND_DEFAULT,   // compared as signed integers
                KIND_UNSIGNED,  // compared as unsigned integers
                KIND_FLOAT,     // compared as floating-point numbers
                KIND_TEXT};     // whitespace-trimmed and then compared as signed integers

inline uint64_t order_swap(uint64_t x)
{
    uint64_t ret = (x >> 56) |
                   ((x << 40) & 0x00FF000000000000ULL) |
                   ((x << 24) & 0x0000FF0000000000ULL) |
                   ((x << 8)  & 0x000000FF00000000ULL) |
                   ((x >> 8)  & 0x00000000FF000000ULL) |
                   ((x >> 24) & 0x0000000000FF0000ULL) |
                   ((x >> 40) & 0x000000000000FF00ULL) |
                   (x << 56);
    return ret;
}

template <class T>
inline int  compareBlock(  const void* a, const void* b )
{
    return ( (*(T*)a) - (*(T*)b) );
}

//this function is out-of-band, we don't need to inline it
void logIt(int mid, int arg1, const string& arg2 = string())
{
    MessageLog logger(LoggingID(28));
    logging::Message::Args args;
    Message msg(mid);

    args.add(arg1);

    if (arg2.length() > 0)
        args.add(arg2);

    msg.format(args);
    logger.logErrorMessage(msg);
}

template<class T>
inline bool colCompare_(const T& val1, const T& val2, uint8_t COP)
{
    switch (COP)
    {
        case COMPARE_NIL:
            return false;

        case COMPARE_LT:
            return val1 < val2;

        case COMPARE_EQ:
            return val1 == val2;

        case COMPARE_LE:
            return val1 <= val2;

        case COMPARE_GT:
            return val1 > val2;

        case COMPARE_NE:
            return val1 != val2;

        case COMPARE_GE:
            return val1 >= val2;

        default:
            logIt(34, COP, "colCompare");
            return false;						// throw an exception here?
    }
}

inline bool colCompareStr(const ColRequestHeaderDataType &type,
                          uint8_t COP,
                          const utils::ConstString &val1,
                          const utils::ConstString &val2)
{
    int error = 0;
    bool rc = primitives::StringComparator(type).op(&error, COP, val1, val2);
    if (error)
    {
        logIt(34, COP, "colCompareStr");
        return false;  // throw an exception here?
    }
    return rc;
}


template<class T>
inline bool colCompare_(const T& val1, const T& val2, uint8_t COP, uint8_t rf)
{
    switch (COP)
    {
        case COMPARE_NIL:
            return false;

        case COMPARE_LT:
            return val1 < val2 || (val1 == val2 && (rf & 0x01));

        case COMPARE_LE:
            return val1 < val2 || (val1 == val2 && rf ^ 0x80);

        case COMPARE_EQ:
            return val1 == val2 && rf == 0;

        case COMPARE_NE:
            return val1 != val2 || rf != 0;

        case COMPARE_GE:
            return val1 > val2 || (val1 == val2 && rf ^ 0x01);

        case COMPARE_GT:
            return val1 > val2 || (val1 == val2 && (rf & 0x80));

        default:
            logIt(34, COP, "colCompare_");
            return false;						// throw an exception here?
    }
}


//@bug 1828  Like must be a string compare.
inline bool colStrCompare_(uint64_t val1, uint64_t val2, uint8_t COP, uint8_t rf)
{
    switch (COP)
    {
        case COMPARE_NIL:
            return false;

        case COMPARE_LT:
            return val1 < val2 || (val1 == val2 && rf != 0);

        case COMPARE_LE:
            return val1 <= val2;

        case COMPARE_EQ:
            return val1 == val2 && rf == 0;

        case COMPARE_NE:
            return val1 != val2 || rf != 0;

        case COMPARE_GE:
            return val1 > val2 || (val1 == val2 && rf == 0);

        case COMPARE_GT:
            return val1 > val2;

        case COMPARE_LIKE:
        case COMPARE_NLIKE:
        default:
            logIt(34, COP, "colStrCompare_");
            return false;						// throw an exception here?
    }
}

// Set the minimum and maximum in the return header if we will be doing a block scan and
// we are dealing with a type that is comparable as a 64 bit integer.  Subsequent calls can then
// skip this block if the value being searched is outside of the Min/Max range.
inline bool isMinMaxValid(const NewColRequestHeader* in)
{
    if (in->NVALS != 0)
    {
        return false;
    }
    else
    {
        switch (in->colType.DataType)
        {
            case CalpontSystemCatalog::CHAR:
                return (in->colType.DataSize < 9);

            case CalpontSystemCatalog::VARCHAR:
            case CalpontSystemCatalog::BLOB:
            case CalpontSystemCatalog::TEXT:
                return (in->colType.DataSize < 8);

            case CalpontSystemCatalog::TINYINT:
            case CalpontSystemCatalog::SMALLINT:
            case CalpontSystemCatalog::MEDINT:
            case CalpontSystemCatalog::INT:
            case CalpontSystemCatalog::DATE:
            case CalpontSystemCatalog::BIGINT:
            case CalpontSystemCatalog::DATETIME:
            case CalpontSystemCatalog::TIME:
            case CalpontSystemCatalog::TIMESTAMP:
            case CalpontSystemCatalog::UTINYINT:
            case CalpontSystemCatalog::USMALLINT:
            case CalpontSystemCatalog::UMEDINT:
            case CalpontSystemCatalog::UINT:
            case CalpontSystemCatalog::UBIGINT:
                return true;

            case CalpontSystemCatalog::DECIMAL:
            case CalpontSystemCatalog::UDECIMAL:
                return (in->colType.DataSize <= datatypes::MAXDECIMALWIDTH);

            default:
                return false;
        }
    }
}

template<ENUM_KIND KIND, int COL_WIDTH, bool IS_NULL, typename T1, typename T2,
         typename std::enable_if<COL_WIDTH == sizeof(int32_t) && KIND == KIND_FLOAT && !IS_NULL, T1>::type* = nullptr>
inline bool colCompareDispatcherT(
    T1 columnValue,
    T2 filterValue,
    uint8_t cop,
    uint8_t rf,
    const ColRequestHeaderDataType& typeHolder,
    bool isVal2Null)
{
    float dVal1 = *((float*) &columnValue);
    float dVal2 = *((float*) &filterValue);
    return colCompare_(dVal1, dVal2, cop);
}

template<ENUM_KIND KIND, int COL_WIDTH, bool IS_NULL, typename T1, typename T2,
         typename std::enable_if<COL_WIDTH == sizeof(int64_t) && KIND == KIND_FLOAT && !IS_NULL, T1>::type* = nullptr>
inline bool colCompareDispatcherT(
    T1 columnValue,
    T2 filterValue,
    uint8_t cop,
    uint8_t rf,
    const ColRequestHeaderDataType& typeHolder,
    bool isVal2Null)
{
    double dVal1 = *((double*) &columnValue);
    double dVal2 = *((double*) &filterValue);
    return colCompare_(dVal1, dVal2, cop);
}

template<ENUM_KIND KIND, int COL_WIDTH, bool IS_NULL, typename T1, typename T2,
         typename std::enable_if<KIND == KIND_TEXT && !IS_NULL, T1>::type* = nullptr>
inline bool colCompareDispatcherT(
    T1 columnValue,
    T2 filterValue,
    uint8_t cop,
    uint8_t rf,
    const ColRequestHeaderDataType& typeHolder,
    bool isVal2Null)
{
    if (cop & COMPARE_LIKE) // LIKE and NOT LIKE
    {
        utils::ConstString subject{reinterpret_cast<const char*>(&columnValue), COL_WIDTH};
        utils::ConstString pattern{reinterpret_cast<const char*>(&filterValue), COL_WIDTH};
        return typeHolder.like(cop & COMPARE_NOT, subject.rtrimZero(),
                                                  pattern.rtrimZero());
    }

    if (!rf)
    {
        // A temporary hack for xxx_nopad_bin collations
        // TODO: MCOL-4534 Improve comparison performance in 8bit nopad_bin collations
        if ((typeHolder.getCharset().state & (MY_CS_BINSORT|MY_CS_NOPAD)) ==
            (MY_CS_BINSORT|MY_CS_NOPAD))
          return colCompare_(order_swap(columnValue), order_swap(filterValue), cop);
        utils::ConstString s1{reinterpret_cast<const char*>(&columnValue), COL_WIDTH};
        utils::ConstString s2{reinterpret_cast<const char*>(&filterValue), COL_WIDTH};
        return colCompareStr(typeHolder, cop, s1.rtrimZero(), s2.rtrimZero());
    }
    else
        return colStrCompare_(order_swap(columnValue), order_swap(filterValue), cop, rf);

}

// This template where IS_NULL = true is used only comparing filter predicate
// values with column NULL so I left branching here.
template<ENUM_KIND KIND, int COL_WIDTH, bool IS_NULL, typename T1, typename T2,
         typename std::enable_if<IS_NULL, T1>::type* = nullptr>
inline bool colCompareDispatcherT(
    T1 columnValue,
    T2 filterValue,
    uint8_t cop,
    uint8_t rf,
    const ColRequestHeaderDataType& typeHolder,
    bool isVal2Null)
{
    if (IS_NULL == isVal2Null || (isVal2Null && cop == COMPARE_NE))
    {
        if (KIND_UNSIGNED == KIND)
        {
            // Ugly hack to convert all to the biggest type b/w T1 and T2.
            // I presume that sizeof(T2) AKA a filter predicate type is GEQ sizeof(T1) AKA col type.
            using UT2 = typename datatypes::make_unsigned<T2>::type;
            UT2 ucolumnValue = columnValue;
            UT2 ufilterValue = filterValue;
            return colCompare_(ucolumnValue, ufilterValue, cop, rf);
        }
        else
        {
            // Ugly hack to convert all to the biggest type b/w T1 and T2.
            // I presume that sizeof(T2) AKA a filter predicate type is GEQ sizeof(T1) AKA col type.
            T2 tempVal1 = columnValue;
            return colCompare_(tempVal1, filterValue, cop, rf);
        }
    }
    else
        return false;
}

template<ENUM_KIND KIND, int COL_WIDTH, bool IS_NULL, typename T1, typename T2,
         typename std::enable_if<KIND == KIND_UNSIGNED && !IS_NULL, T1>::type* = nullptr>
inline bool colCompareDispatcherT(
    T1 columnValue,
    T2 filterValue,
    uint8_t cop,
    uint8_t rf,
    const ColRequestHeaderDataType& typeHolder,
    bool isVal2Null)
{
    if (IS_NULL == isVal2Null || (isVal2Null && cop == COMPARE_NE))
    {
        // Ugly hack to convert all to the biggest type b/w T1 and T2.
        // I presume that sizeof(T2)(a filter predicate type) is GEQ T1(col type).
        using UT2 = typename datatypes::make_unsigned<T2>::type;
        UT2 ucolumnValue = columnValue;
        UT2 ufilterValue = filterValue;
        return colCompare_(ucolumnValue, ufilterValue, cop, rf);
    }
    else
        return false;
}

template<ENUM_KIND KIND, int COL_WIDTH, bool IS_NULL, typename T1, typename T2,
         typename std::enable_if<KIND == KIND_DEFAULT && !IS_NULL, T1>::type* = nullptr>
inline bool colCompareDispatcherT(
    T1 columnValue,
    T2 filterValue,
    uint8_t cop,
    uint8_t rf,
    const ColRequestHeaderDataType& typeHolder,
    bool isVal2Null)
{
    if (IS_NULL == isVal2Null || (isVal2Null && cop == COMPARE_NE))
    {
        // Ugly hack to convert all to the biggest type b/w T1 and T2.
        // I presume that sizeof(T2)(a filter predicate type) is GEQ T1(col type).
        T2 tempVal1 = columnValue;
        return colCompare_(tempVal1, filterValue, cop, rf);
    }
    else
        return false;
}

// Compare two column values using given comparison operation,
// taking into account all rules about NULL values, string trimming and so on
template<ENUM_KIND KIND, int COL_WIDTH, bool IS_NULL = false, typename T1, typename T2>
inline bool colCompare(
    T1 columnValue,
    T2 filterValue,
    uint8_t cop,
    uint8_t rf,
    const ColRequestHeaderDataType& typeHolder,
    bool isVal2Null = false)
{
// 	cout << "comparing " << hex << columnValue << " to " << filterValue << endl;
    if (COMPARE_NIL == cop) return false;

    return colCompareDispatcherT<KIND, COL_WIDTH, IS_NULL, T1, T2>(columnValue, filterValue,
        cop, rf, typeHolder, isVal2Null);
}

/*****************************************************************************
 *** NULL/EMPTY VALUES FOR EVERY COLUMN TYPE/WIDTH ***************************
 *****************************************************************************/
// Bit pattern representing EMPTY value for given column type/width
// TBD Use typeHandler
template<typename T,
         typename std::enable_if<sizeof(T) == sizeof(int128_t), T>::type* = nullptr>
T getEmptyValue(uint8_t type)
{
    return datatypes::Decimal128Empty;
}

template<typename T,
         typename std::enable_if<sizeof(T) == sizeof(int64_t), T>::type* = nullptr>
T getEmptyValue(uint8_t type)
{
    switch (type)
    {
        case CalpontSystemCatalog::DOUBLE:
        case CalpontSystemCatalog::UDOUBLE:
            return joblist::DOUBLEEMPTYROW;

        case CalpontSystemCatalog::CHAR:
        case CalpontSystemCatalog::VARCHAR:
        case CalpontSystemCatalog::DATE:
        case CalpontSystemCatalog::DATETIME:
        case CalpontSystemCatalog::TIMESTAMP:
        case CalpontSystemCatalog::TIME:
        case CalpontSystemCatalog::VARBINARY:
        case CalpontSystemCatalog::BLOB:
        case CalpontSystemCatalog::TEXT:
            return joblist::CHAR8EMPTYROW;

        case CalpontSystemCatalog::UBIGINT:
            return joblist::UBIGINTEMPTYROW;

        default:
            return joblist::BIGINTEMPTYROW;
    }
}

template<typename T,
         typename std::enable_if<sizeof(T) == sizeof(int32_t), T>::type* = nullptr>
T getEmptyValue(uint8_t type)
{
    switch (type)
    {
        case CalpontSystemCatalog::FLOAT:
        case CalpontSystemCatalog::UFLOAT:
            return joblist::FLOATEMPTYROW;

        case CalpontSystemCatalog::CHAR:
        case CalpontSystemCatalog::VARCHAR:
        case CalpontSystemCatalog::BLOB:
        case CalpontSystemCatalog::TEXT:
        case CalpontSystemCatalog::DATE:
        case CalpontSystemCatalog::DATETIME:
        case CalpontSystemCatalog::TIMESTAMP:
        case CalpontSystemCatalog::TIME:
            return joblist::CHAR4EMPTYROW;

        case CalpontSystemCatalog::UINT:
        case CalpontSystemCatalog::UMEDINT:
            return joblist::UINTEMPTYROW;

        default:
            return joblist::INTEMPTYROW;
    }
}

template<typename T,
         typename std::enable_if<sizeof(T) == sizeof(int16_t), T>::type* = nullptr>
T getEmptyValue(uint8_t type)
{
    switch (type)
    {
        case CalpontSystemCatalog::CHAR:
        case CalpontSystemCatalog::VARCHAR:
        case CalpontSystemCatalog::BLOB:
        case CalpontSystemCatalog::TEXT:
        case CalpontSystemCatalog::DATE:
        case CalpontSystemCatalog::DATETIME:
        case CalpontSystemCatalog::TIMESTAMP:
        case CalpontSystemCatalog::TIME:
            return joblist::CHAR2EMPTYROW;

        case CalpontSystemCatalog::USMALLINT:
            return joblist::USMALLINTEMPTYROW;

        default:
            return joblist::SMALLINTEMPTYROW;
    }
}

template<typename T,
         typename std::enable_if<sizeof(T) == sizeof(int8_t), T>::type* = nullptr>
T getEmptyValue(uint8_t type)
{
    switch (type)
    {
        case CalpontSystemCatalog::CHAR:
        case CalpontSystemCatalog::VARCHAR:
        case CalpontSystemCatalog::BLOB:
        case CalpontSystemCatalog::TEXT:
        case CalpontSystemCatalog::DATE:
        case CalpontSystemCatalog::DATETIME:
        case CalpontSystemCatalog::TIMESTAMP:
        case CalpontSystemCatalog::TIME:
            return joblist::CHAR1EMPTYROW;

        case CalpontSystemCatalog::UTINYINT:
            return joblist::UTINYINTEMPTYROW;

        default:
            return joblist::TINYINTEMPTYROW;
    }
}

// Bit pattern representing NULL value for given column type/width
// TBD Use TypeHandler
template<typename T,
         typename std::enable_if<sizeof(T) == sizeof(int128_t), T>::type* = nullptr>
T getNullValue(uint8_t type)
{
    return datatypes::Decimal128Null;
}

template<typename T,
         typename std::enable_if<sizeof(T) == sizeof(int64_t), T>::type* = nullptr>
T getNullValue(uint8_t type)
{
    switch (type)
    {
        case CalpontSystemCatalog::DOUBLE:
        case CalpontSystemCatalog::UDOUBLE:
            return joblist::DOUBLENULL;

        case CalpontSystemCatalog::CHAR:
        case CalpontSystemCatalog::VARCHAR:
        case CalpontSystemCatalog::DATE:
        case CalpontSystemCatalog::DATETIME:
        case CalpontSystemCatalog::TIMESTAMP:
        case CalpontSystemCatalog::TIME:
        case CalpontSystemCatalog::VARBINARY:
        case CalpontSystemCatalog::BLOB:
        case CalpontSystemCatalog::TEXT:
            return joblist::CHAR8NULL;

        case CalpontSystemCatalog::UBIGINT:
            return joblist::UBIGINTNULL;

        default:
            return joblist::BIGINTNULL;
    }
}

template<typename T,
         typename std::enable_if<sizeof(T) == sizeof(int32_t), T>::type* = nullptr>
T getNullValue(uint8_t type)
{
    switch (type)
    {
        case CalpontSystemCatalog::FLOAT:
        case CalpontSystemCatalog::UFLOAT:
            return joblist::FLOATNULL;

        case CalpontSystemCatalog::CHAR:
        case CalpontSystemCatalog::VARCHAR:
        case CalpontSystemCatalog::BLOB:
        case CalpontSystemCatalog::TEXT:
            return joblist::CHAR4NULL;

        case CalpontSystemCatalog::DATE:
        case CalpontSystemCatalog::DATETIME:
        case CalpontSystemCatalog::TIMESTAMP:
        case CalpontSystemCatalog::TIME:
            return joblist::DATENULL;

        case CalpontSystemCatalog::UINT:
        case CalpontSystemCatalog::UMEDINT:
            return joblist::UINTNULL;

        default:
            return joblist::INTNULL;
    }
}

template<typename T,
         typename std::enable_if<sizeof(T) == sizeof(int16_t), T>::type* = nullptr>
T getNullValue(uint8_t type)
{
    switch (type)
    {
        case CalpontSystemCatalog::CHAR:
        case CalpontSystemCatalog::VARCHAR:
        case CalpontSystemCatalog::BLOB:
        case CalpontSystemCatalog::TEXT:
        case CalpontSystemCatalog::DATE:
        case CalpontSystemCatalog::DATETIME:
        case CalpontSystemCatalog::TIMESTAMP:
        case CalpontSystemCatalog::TIME:
            return joblist::CHAR2NULL;

        case CalpontSystemCatalog::USMALLINT:
            return joblist::USMALLINTNULL;

        default:
            return joblist::SMALLINTNULL;
    }
}

template<typename T,
         typename std::enable_if<sizeof(T) == sizeof(int8_t), T>::type* = nullptr>
T getNullValue(uint8_t type)
{
    switch (type)
    {
        case CalpontSystemCatalog::CHAR:
        case CalpontSystemCatalog::VARCHAR:
        case CalpontSystemCatalog::BLOB:
        case CalpontSystemCatalog::TEXT:
        case CalpontSystemCatalog::DATE:
        case CalpontSystemCatalog::DATETIME:
        case CalpontSystemCatalog::TIMESTAMP:
        case CalpontSystemCatalog::TIME:
            return joblist::CHAR1NULL;

        case CalpontSystemCatalog::UTINYINT:
            return joblist::UTINYINTNULL;

        default:
            return joblist::TINYINTNULL;
    }
}

// Check whether val is NULL (or alternative NULL bit pattern for 64-bit string types)
template<ENUM_KIND KIND, typename T>
inline bool isNullValue(const T val, const T NULL_VALUE)
{
    return val == NULL_VALUE;
}

template<>
inline bool isNullValue<KIND_TEXT, int64_t>(const int64_t val, const int64_t NULL_VALUE)
{
    //@bug 339 might be a token here
    //TODO: what's up with the alternative NULL here?
    constexpr const int64_t ALT_NULL_VALUE = 0xFFFFFFFFFFFFFFFELL;

    return (val == NULL_VALUE ||
            val == ALT_NULL_VALUE);
}

//
// FILTER A COLUMN VALUE
//

template<bool IS_NULL, typename T, typename FT,
         typename std::enable_if<IS_NULL == true, T>::type* = nullptr>
inline bool noneValuesInArray(const T curValue,
                              const FT* filterValues,
                              const uint32_t filterCount)
{
    // ignore NULLs in the array and in the column data
    return false;
}

template<bool IS_NULL, typename T, typename FT,
         typename std::enable_if<IS_NULL == false, T>::type* = nullptr>
inline bool noneValuesInArray(const T curValue,
                              const FT* filterValues,
                              const uint32_t filterCount)
{
    for (uint32_t argIndex = 0; argIndex < filterCount; argIndex++)
    {
        if (curValue == static_cast<T>(filterValues[argIndex]))
            return false;
    }

    return true;
}

template<bool IS_NULL, typename T, typename ST,
         typename std::enable_if<IS_NULL == true, T>::type* = nullptr>
inline bool noneValuesInSet(const T curValue, const ST* filterSet)
{
    // bug 1920: ignore NULLs in the set and in the column data
    return false;
}

template<bool IS_NULL, typename T, typename ST,
         typename std::enable_if<IS_NULL == false, T>::type* = nullptr>
inline bool noneValuesInSet(const T curValue, const ST* filterSet)
{
    bool found = (filterSet->find(curValue) != filterSet->end());
    return !found;
}

// The routine is used to test the value from a block against filters
// according with columnFilterMode(see the corresponding enum for details).
// Returns true if the curValue matches the filter.
template<ENUM_KIND KIND, int COL_WIDTH, bool IS_NULL = false,
        typename T, typename FT, typename ST>
inline bool matchingColValue(const T curValue,
                             const ColumnFilterMode columnFilterMode,
                             const ST* filterSet, // Set of values for simple filters (any of values / none of them)
                             const uint32_t filterCount, // Number of filter elements, each described by one entry in the following arrays:
                             const uint8_t* filterCOPs, //   comparison operation
                             const FT* filterValues, //   value to compare to
                             const uint8_t* filterRFs, // reverse byte order flags
                             const ColRequestHeaderDataType& typeHolder,
                             const T NULL_VALUE)                   // Bit pattern representing NULL value for this column type/width
{
    /* In order to make filtering as fast as possible, we replaced the single generic algorithm
       with several algorithms, better tailored for more specific cases:
       empty filter, single comparison, and/or/xor comparison results, one/none of small/large set of values
    */
    switch (columnFilterMode)
    {
        // Empty filter is always true
        case ALWAYS_TRUE:
            return true;


        // Filter consisting of exactly one comparison operation
        case SINGLE_COMPARISON:
        {
            auto filterValue = filterValues[0];
            // This can be future optimized checking if a filterValue is NULL or not
            bool cmp = colCompare<KIND, COL_WIDTH, IS_NULL>(curValue, filterValue, filterCOPs[0],
                                                            filterRFs[0], typeHolder, isNullValue<KIND,T>(filterValue,
                                                            NULL_VALUE));
            return cmp;
        }


        // Filter is true if ANY comparison is true (BOP_OR)
        case ANY_COMPARISON_TRUE:
        {
            for (uint32_t argIndex = 0; argIndex < filterCount; argIndex++)
            {
                auto filterValue = filterValues[argIndex];
                // This can be future optimized checking if a filterValues are NULLs or not before the higher level loop.
                bool cmp = colCompare<KIND, COL_WIDTH, IS_NULL>(curValue, filterValue, filterCOPs[argIndex],
                                                                filterRFs[argIndex], typeHolder, isNullValue<KIND,T>(filterValue,
                                                                NULL_VALUE));

                // Short-circuit the filter evaluation - true || ... == true
                if (cmp == true)
                    return true;
            }

            // We can get here only if all filters returned false
            return false;
        }


        // Filter is true only if ALL comparisons are true (BOP_AND)
        case ALL_COMPARISONS_TRUE:
        {
            for (uint32_t argIndex = 0; argIndex < filterCount; argIndex++)
            {
                auto filterValue = filterValues[argIndex];
                // This can be future optimized checking if a filterValues are NULLs or not before the higher level loop.
                bool cmp = colCompare<KIND, COL_WIDTH, IS_NULL>(curValue, filterValue, filterCOPs[argIndex],
                                                                filterRFs[argIndex], typeHolder,
                                                                isNullValue<KIND,T>(filterValue, NULL_VALUE));

                // Short-circuit the filter evaluation - false && ... = false
                if (cmp == false)
                    return false;
            }

            // We can get here only if all filters returned true
            return true;
        }


        // XORing results of comparisons (BOP_XOR)
        case XOR_COMPARISONS:
        {
            bool result = false;

            for (uint32_t argIndex = 0; argIndex < filterCount; argIndex++)
            {
                auto filterValue = filterValues[argIndex];
                // This can be future optimized checking if a filterValues are NULLs or not before the higher level loop.
                bool cmp = colCompare<KIND, COL_WIDTH, IS_NULL>(curValue, filterValue, filterCOPs[argIndex],
                                                                filterRFs[argIndex], typeHolder,
                                                                isNullValue<KIND,T>(filterValue, NULL_VALUE));
                result ^= cmp;
            }

            return result;
        }


        // ONE of the values in the small set represented by an array (BOP_OR + all COMPARE_EQ)
        case ONE_OF_VALUES_IN_ARRAY:
        {
            for (uint32_t argIndex = 0; argIndex < filterCount; argIndex++)
            {
                if (curValue == static_cast<T>(filterValues[argIndex]))
                    return true;
            }

            return false;
        }


        // NONE of the values in the small set represented by an array (BOP_AND + all COMPARE_NE)
        case NONE_OF_VALUES_IN_ARRAY:
            return noneValuesInArray<IS_NULL, T, FT>(curValue, filterValues, filterCount);


        // ONE of the values in the set is equal to the value checked (BOP_OR + all COMPARE_EQ)
        case ONE_OF_VALUES_IN_SET:
        {
            bool found = (filterSet->find(curValue) != filterSet->end());
            return found;
        }


        // NONE of the values in the set is equal to the value checked (BOP_AND + all COMPARE_NE)
        case NONE_OF_VALUES_IN_SET:
            return noneValuesInSet<IS_NULL, T, ST>(curValue, filterSet);


        default:
            idbassert(0);
            return true;
    }
}

/*****************************************************************************
 *** READ COLUMN VALUES ******************************************************
 *****************************************************************************/

// Read one ColValue from the input block.
// Return true on success, false on End of Block.
// Values are read from srcArray either in natural order or in the order defined by ridArray.
// Empty values are skipped, unless ridArray==0 && !(OutputType & OT_RID).
template<typename T, int COL_WIDTH>
inline bool nextColValue(
    T& result,            // Place for the value returned
    bool* isEmpty,              // ... and flag whether it's EMPTY
    uint32_t* index,                 // Successive index either in srcArray (going from 0 to srcSize-1) or ridArray (0..ridSize-1)
    uint16_t* rid,              // Index in srcArray of the value returned
    const T* srcArray,          // Input array
    const uint32_t srcSize,     // ... and its size
    const uint16_t* ridArray,   // Optional array of indexes into srcArray, that defines the read order
    const uint16_t ridSize,          // ... and its size
    const uint8_t OutputType,   // Used to decide whether to skip EMPTY values
    T EMPTY_VALUE)
{
    auto i = *index;    // local copy of *index to speed up loops
    T value;            // value to be written into *result, local for the same reason

    if (ridArray)
    {
        // Read next non-empty value in the order defined by ridArray
        for( ; ; i++)
        {
            if (UNLIKELY(i >= ridSize))
                return false;

            value = srcArray[ridArray[i]];

            if (value != EMPTY_VALUE)
                break;
        }

        *rid = ridArray[i];
        *isEmpty = false;
    }
    else if (OutputType & OT_RID)   //TODO: check correctness of this condition for SKIP_EMPTY_VALUES
    {
        // Read next non-empty value in the natural order
        for( ; ; i++)
        {
            if (UNLIKELY(i >= srcSize))
                return false;

            value = srcArray[i];

            if (value != EMPTY_VALUE)
                break;
        }

        *rid = i;
        *isEmpty = false;
    }
    else
    {
        // Read next value in the natural order
        if (UNLIKELY(i >= srcSize))
            return false;

        *rid = i;
        value = srcArray[i];
        *isEmpty = (value == EMPTY_VALUE);
    }

    *index = i+1;
    result = value;
    return true;
}

///
/// WRITE COLUMN VALUES
///

// Append value to the output buffer with debug-time check for buffer overflow
template<typename T>
inline void checkedWriteValue(
    void* out,
    unsigned outSize,
    unsigned* outPos,
    const T* src,
    int errSubtype)
{
#ifdef PRIM_DEBUG
    if (sizeof(T) > outSize - *outPos)
    {
        logIt(35, errSubtype);
        throw logic_error("PrimitiveProcessor::checkedWriteValue(): output buffer is too small");
    }
#endif
    uint8_t* out8 = reinterpret_cast<uint8_t*>(out);
    memcpy(out8 + *outPos, src, sizeof(T));
    *outPos += sizeof(T);
}

// Write the value index in srcArray and/or the value itself, depending on bits in OutputType,
// into the output buffer and update the output pointer.
template<typename T>
inline void writeColValue(
    uint8_t OutputType,
    NewColResultHeader* out,
    unsigned outSize,
    unsigned* written,
    uint16_t rid,
    const T* srcArray)
{
    if (OutputType & OT_RID)
    {
        checkedWriteValue(out, outSize, written, &rid, 1);
        out->RidFlags |= (1 << (rid >> 9)); // set the (row/512)'th bit
    }

    if (OutputType & (OT_TOKEN | OT_DATAVALUE))
    {
        checkedWriteValue(out, outSize, written, &srcArray[rid], 2);
    }

    out->NVALS++;   //TODO: Can be computed at the end from *written value
}

/* WIP
template <bool WRITE_RID, bool WRITE_DATA, bool IS_NULL_VALUE_MATCHES, typename FILTER_ARRAY_T, typename RID_T, typename T>
void writeArray(
    size_t dataSize,
    const T* dataArray,
    const RID_T* dataRid,
    const FILTER_ARRAY_T *filterArray,
    uint8_t* outbuf,
    unsigned* written,
    uint16_t* NVALS,
    uint8_t* RidFlagsPtr,
    T NULL_VALUE)
{
    uint8_t* out = outbuf;
    uint8_t RidFlags = *RidFlagsPtr;

    for (size_t i = 0; i < dataSize; ++i)
    {
        //TODO: optimize handling of NULL values and flags by avoiding non-predictable jumps
        if (dataArray[i]==NULL_VALUE? IS_NULL_VALUE_MATCHES : filterArray[i])
        {
            if (WRITE_RID)
            {
                copyValue(out, &dataRid[i], sizeof(RID_T));
                out += sizeof(RID_T);

                RidFlags |= (1 << (dataRid[i] >> 10)); // set the (row/1024)'th bit
            }

            if (WRITE_DATA)
            {
                copyValue(out, &dataArray[i], sizeof(T));
                out += sizeof(T);
            }
        }
    }

    // Update number of written values, number of written bytes and out->RidFlags
    int size1 = (WRITE_RID? sizeof(RID_T) : 0) + (WRITE_DATA? sizeof(T) : 0);
    *NVALS += (out - outbuf) / size1;
    *written += out - outbuf;
    *RidFlagsPtr = RidFlags;
}
*/

/*****************************************************************************
 *** RUN DATA THROUGH A COLUMN FILTER ****************************************
 *****************************************************************************/

/* "Vertical" processing of the column filter:
   1. load all data into temporary vector
   2. process one filter element over entire vector before going to a next one
   3. write records, that succesfully passed through the filter, to outbuf
*/
/*
template<typename T, ENUM_KIND KIND, typename VALTYPE>
void processArray(
    // Source data
    const T* srcArray,
    size_t srcSize,
    uint16_t* ridArray,
    size_t ridSize,                 // Number of values in ridArray
    // Filter description
    int BOP,
    prestored_set_t* filterSet,     // Set of values for simple filters (any of values / none of them)
    uint32_t filterCount,           // Number of filter elements, each described by one entry in the following arrays:
    uint8_t* filterCOPs,            //   comparison operation
    int64_t* filterValues,          //   value to compare to
    // Output buffer/stats
    uint8_t* outbuf,                // Pointer to the place for output data
    unsigned* written,              // Number of written bytes, that we need to update
    uint16_t* NVALS,                // Number of written values, that we need to update
    uint8_t* RidFlagsPtr,           // Pointer to out->RidFlags
    // Processing parameters
    bool WRITE_RID,
    bool WRITE_DATA,
    bool SKIP_EMPTY_VALUES,
    T EMPTY_VALUE,
    bool IS_NULL_VALUE_MATCHES,
    T NULL_VALUE,
    // Min/Max search
    bool ValidMinMax,
    VALTYPE* MinPtr,
    VALTYPE* MaxPtr)
{
    // Alloc temporary arrays
    size_t inputSize = (ridArray? ridSize : srcSize);

    // Temporary array with data to filter
    std::vector<T> dataVec(inputSize);
    auto dataArray = dataVec.data();

    // Temporary array with RIDs of corresponding dataArray elements
    std::vector<RID_T> dataRidVec(WRITE_RID? inputSize : 0);
    auto dataRid = dataRidVec.data();


    // Copy input data into temporary array, opt. storing RIDs, opt. skipping EMPTYs
    size_t dataSize;  // number of values copied into dataArray
    if (ridArray != NULL)
    {
        SKIP_EMPTY_VALUES = true;  // let findMinMaxArray() know that empty values will be skipped

        dataSize = WRITE_RID? readArray<true, true,true>(srcArray, srcSize, dataArray, dataRid, ridArray, ridSize, EMPTY_VALUE)
                            : readArray<false,true,true>(srcArray, srcSize, dataArray, dataRid, ridArray, ridSize, EMPTY_VALUE);
    }
    else if (SKIP_EMPTY_VALUES)
    {
        dataSize = WRITE_RID? readArray<true, false,true>(srcArray, srcSize, dataArray, dataRid, ridArray, ridSize, EMPTY_VALUE)
                            : readArray<false,false,true>(srcArray, srcSize, dataArray, dataRid, ridArray, ridSize, EMPTY_VALUE);
    }
    else
    {
        dataSize = WRITE_RID? readArray<true, false,false>(srcArray, srcSize, dataArray, dataRid, ridArray, ridSize, EMPTY_VALUE)
                            : readArray<false,false,false>(srcArray, srcSize, dataArray, dataRid, ridArray, ridSize, EMPTY_VALUE);
    }

    // If required, find Min/Max values of the data
    if (ValidMinMax)
    {
        SKIP_EMPTY_VALUES? findMinMaxArray<true> (dataSize, dataArray, MinPtr, MaxPtr, EMPTY_VALUE, NULL_VALUE)
                         : findMinMaxArray<false>(dataSize, dataArray, MinPtr, MaxPtr, EMPTY_VALUE, NULL_VALUE);
    }


    // Choose initial filterArray[i] value depending on the operation
    bool initValue = false;
    if      (filterCount == 0) {initValue = true;}
    else if (BOP_NONE == BOP)  {initValue = false;  BOP = BOP_OR;}
    else if (BOP_OR   == BOP)  {initValue = false;}
    else if (BOP_XOR  == BOP)  {initValue = false;}
    else if (BOP_AND  == BOP)  {initValue = true;}

    // Temporary array accumulating results of filtering for each record
    std::vector<uint8_t> filterVec(dataSize, initValue);
    auto filterArray = filterVec.data();

    // Real type of column data, may be floating-point (used only for comparisons in the filtering)
    using FLOAT_T = typename std::conditional<sizeof(T) == 8, double, float>::type;
    using DATA_T  = typename std::conditional<KIND_FLOAT == KIND, FLOAT_T, T>::type;
    auto realDataArray = reinterpret_cast<DATA_T*>(dataArray);


    // Evaluate column filter on elements of dataArray and store results into filterArray
    if (filterSet != NULL  &&  BOP == BOP_OR)
    {
        applySetFilter<BOP_OR>(dataSize, dataArray, filterSet, filterArray);
    }
    else if (filterSet != NULL  &&  BOP == BOP_AND)
    {
        applySetFilter<BOP_AND>(dataSize, dataArray, filterSet, filterArray);
    }
    else

        for (int i = 0; i < filterCount; ++i)
        {
            DATA_T cmp_value;   // value for comparison, may be floating-point
            copyValue(&cmp_value, &filterValues[i], sizeof(cmp_value));

            switch(BOP)
            {
                case BOP_AND:  applyFilterElement<BOP_AND>(filterCOPs[i], dataSize, realDataArray, cmp_value, filterArray);  break;
                case BOP_OR:   applyFilterElement<BOP_OR> (filterCOPs[i], dataSize, realDataArray, cmp_value, filterArray);  break;
                case BOP_XOR:  applyFilterElement<BOP_XOR>(filterCOPs[i], dataSize, realDataArray, cmp_value, filterArray);  break;
                default:       idbassert(0);
            }
        }
    }


    // Copy filtered data and/or their RIDs into output buffer
    if (WRITE_RID && WRITE_DATA)
    {
        IS_NULL_VALUE_MATCHES? writeArray<true,true,true> (dataSize, dataArray, dataRid, filterArray, outbuf, written, NVALS, RidFlagsPtr, NULL_VALUE)
                             : writeArray<true,true,false>(dataSize, dataArray, dataRid, filterArray, outbuf, written, NVALS, RidFlagsPtr, NULL_VALUE);
    }
    else if (WRITE_RID)
    {
        IS_NULL_VALUE_MATCHES? writeArray<true,false,true> (dataSize, dataArray, dataRid, filterArray, outbuf, written, NVALS, RidFlagsPtr, NULL_VALUE)
                             : writeArray<true,false,false>(dataSize, dataArray, dataRid, filterArray, outbuf, written, NVALS, RidFlagsPtr, NULL_VALUE);
    }
    else
    {
        IS_NULL_VALUE_MATCHES? writeArray<false,true,true> (dataSize, dataArray, dataRid, filterArray, outbuf, written, NVALS, RidFlagsPtr, NULL_VALUE)
                             : writeArray<false,true,false>(dataSize, dataArray, dataRid, filterArray, outbuf, written, NVALS, RidFlagsPtr, NULL_VALUE);
    }
}
*/

// These two are templates update min/max values in the loop iterating the values in filterColumnData.
template<ENUM_KIND KIND, typename T,
         typename std::enable_if<KIND == KIND_TEXT, T>::type* = nullptr>
inline void updateMinMax(T& Min, T& Max, T& curValue, NewColRequestHeader* in)
{
    constexpr int COL_WIDTH = sizeof(T);
    if (colCompare<KIND_TEXT, COL_WIDTH>(Min, curValue, COMPARE_GT, false, in->colType))
        Min = curValue;

    if (colCompare<KIND_TEXT, COL_WIDTH>(Max, curValue, COMPARE_LT, false, in->colType))
        Max = curValue;
}

template<ENUM_KIND KIND, typename T,
         typename std::enable_if<KIND != KIND_TEXT, T>::type* = nullptr>
inline void updateMinMax(T& Min, T& Max, T& curValue, NewColRequestHeader* in)
{
    if (Min > curValue)
        Min = curValue;

    if (Max < curValue)
        Max = curValue;
}

// TBD Check if MCS really needs to copy values from in into out msgs or
// it is possible to copy from in msg into BPP::values directly.
// This template contains the main scanning/filtering loop.
// Copy data matching parsedColumnFilter from input to output.
// Input is srcArray[srcSize], optionally accessed in the order defined by ridArray[ridSize].
// Output is BLOB out[outSize], written starting at offset *written, which is updated afterward.
template<typename T, ENUM_KIND KIND>
void filterColumnData(
    NewColRequestHeader* in,
    NewColResultHeader* out,
    unsigned outSize,
    unsigned* written,
    uint16_t* ridArray,
    const uint16_t ridSize,                // Number of values in ridArray
    int* srcArray16,
    const uint32_t srcSize,
    boost::shared_ptr<ParsedColumnFilter> parsedColumnFilter)
{
    using FT = typename IntegralTypeToFilterType<T>::type;
    using ST = typename IntegralTypeToFilterSetType<T>::type;
    constexpr int COL_WIDTH = sizeof(T);
    const T* srcArray = reinterpret_cast<const T*>(srcArray16);

    // Cache some structure fields in local vars
    auto dataType = (CalpontSystemCatalog::ColDataType) in->colType.DataType;  // Column datatype
    uint32_t filterCount = in->NOPS;        // Number of elements in the filter
    uint8_t  outputType  = in->OutputType;

    // If no pre-parsed column filter is set, parse the filter in the message
    if (parsedColumnFilter.get() == nullptr  &&  filterCount > 0)
        parsedColumnFilter = _parseColumnFilter<T>(in->getFilterStringPtr(),
                                                   dataType, filterCount, in->BOP);

    // Cache parsedColumnFilter fields in local vars
    auto columnFilterMode = filterCount==0 ? ALWAYS_TRUE : parsedColumnFilter->columnFilterMode;
    FT* filterValues  = filterCount==0 ? nullptr : parsedColumnFilter->getFilterVals<FT>();
    auto filterCOPs  = filterCount==0 ? nullptr : parsedColumnFilter->prestored_cops.get();
    auto filterRFs   = filterCount==0 ? nullptr : parsedColumnFilter->prestored_rfs.get();
    ST* filterSet    = filterCount==0 ? nullptr : parsedColumnFilter->getFilterSet<ST>();

    // ###########################
    // Bit patterns in srcArray[i] representing EMPTY and NULL values
    T EMPTY_VALUE = getEmptyValue<T>(dataType);
    T NULL_VALUE  = getNullValue<T>(dataType);

    // Precompute filter results for NULL values
    bool isNullValueMatches = matchingColValue<KIND, COL_WIDTH, true>(NULL_VALUE, columnFilterMode,
        filterSet, filterCount, filterCOPs, filterValues, filterRFs, in->colType, NULL_VALUE);

    // Boolean indicating whether to capture the min and max values
    bool ValidMinMax = isMinMaxValid(in);
    // Local vars to capture the min and max values
    T Min = datatypes::numeric_limits<T>::max();
    T Max = (KIND == KIND_UNSIGNED) ? 0 : datatypes::numeric_limits<T>::min();

/* WIP add vertical processing
    // If possible, use faster "vertical" filtering approach
    if (KIND != KIND_TEXT)
    {
        bool canUseFastFiltering = true;
        for (int i = 0; i < filterCount; ++i)
            if (filterRFs[i] != 0)
            canUseFastFiltering = false;

        if (canUseFastFiltering)
        {
            processArray<T, KIND, T>(srcArray, srcSize, ridArray, ridSize,
                         in->BOP, filterSet, filterCount, filterCOPs, filterValues,
                         reinterpret_cast<uint8_t*>(out) + *written,
                         written, & out->NVALS, & out->RidFlags,
                         (outputType & OT_RID) != 0,
                         (outputType & (OT_TOKEN | OT_DATAVALUE)) != 0,
                         (outputType & OT_RID) != 0,  //TODO: check correctness of this condition for SKIP_EMPTY_VALUES
                         EMPTY_VALUE,
                         isNullValueMatches, NULL_VALUE,
                         ValidMinMax, &Min, &Max);
            return;
        }
    }
*/

    // Loop-local variables
    T curValue = 0;
    uint16_t rid = 0;
    bool isEmpty = false;

    // Loop over the column values, storing those matching the filter, and updating the min..max range
    for (uint32_t i = 0;
         nextColValue<T, COL_WIDTH>(curValue, &isEmpty,
                                    &i, &rid,
                                    srcArray, srcSize, ridArray, ridSize,
                                    outputType, EMPTY_VALUE); )
    {
        if (isEmpty)
            continue;
        else if (isNullValue<KIND,T>(curValue, NULL_VALUE))
        {
            // If NULL values match the filter, write curValue to the output buffer
            if (isNullValueMatches)
                writeColValue<T>(outputType, out, outSize, written, rid, srcArray);
        }
        else
        {
            // If curValue matches the filter, write it to the output buffer
            if (matchingColValue<KIND, COL_WIDTH, false>(curValue, columnFilterMode, filterSet, filterCount,
                                filterCOPs, filterValues, filterRFs, in->colType, NULL_VALUE))
            {
                writeColValue<T>(outputType, out, outSize, written, rid, srcArray);
            }

            // Update Min and Max if necessary.  EMPTY/NULL values are processed in other branches.
            if (ValidMinMax)
                updateMinMax<KIND>(Min, Max, curValue, in);
        }
    }


    // Write captured Min/Max values to *out
    out->ValidMinMax = ValidMinMax;
    if (ValidMinMax)
    {
        out->Min = Min;
        out->Max = Max;
    }
} // end of filterColumnData

} //namespace anon

namespace primitives
{

// The routine used to dispatch CHAR|VARCHAR|TEXT|BLOB scan.
inline bool isDictTokenScan(NewColRequestHeader* in)
{
    switch (in->colType.DataType)
    {
        case CalpontSystemCatalog::CHAR:
            return (in->colType.DataSize > 8);

        case CalpontSystemCatalog::VARCHAR:
        case CalpontSystemCatalog::BLOB:
        case CalpontSystemCatalog::TEXT:
            return (in->colType.DataSize > 7);
        default:
            return false;
    }
}

// A set of dispatchers for different column widths/integral types.
template<typename T,
// Remove this ugly preprocessor macrosses when RHEL7 reaches EOL.
// This ugly preprocessor if is here b/c of templated class method parameter default value syntax diff b/w gcc versions.
#ifdef __GNUC__
 #if ___GNUC__ >= 5
         typename std::enable_if<sizeof(T) == sizeof(int32_t), T>::type* = nullptr> // gcc >= 5
 #else
         typename std::enable_if<sizeof(T) == sizeof(int32_t), T>::type*> // gcc 4.8.5
 #endif
#else
         typename std::enable_if<sizeof(T) == sizeof(int32_t), T>::type* = nullptr>
#endif
void PrimitiveProcessor::scanAndFilterTypeDispatcher(NewColRequestHeader* in,
    NewColResultHeader* out,
    unsigned outSize,
    unsigned* written)
{
    constexpr int W = sizeof(T);
    auto dataType = (execplan::CalpontSystemCatalog::ColDataType) in->colType.DataType;
    if (dataType == execplan::CalpontSystemCatalog::FLOAT)
    {
// WIP make this inline function
        const uint16_t ridSize = in->NVALS;
        uint16_t* ridArray = in->getRIDArrayPtr(W);
        const uint32_t itemsPerBlock = logicalBlockMode ? BLOCK_SIZE
                                                        : BLOCK_SIZE / W;
        filterColumnData<T, KIND_FLOAT>(in, out, outSize, written, ridArray, ridSize, block, itemsPerBlock, parsedColumnFilter);
        return;
    }
    _scanAndFilterTypeDispatcher<T>(in, out, outSize, written);
}

template<typename T,
#ifdef __GNUC__
 #if ___GNUC__ >= 5
         typename std::enable_if<sizeof(T) == sizeof(int64_t), T>::type* = nullptr> // gcc >= 5
 #else
         typename std::enable_if<sizeof(T) == sizeof(int64_t), T>::type*> // gcc 4.8.5
 #endif
#else
         typename std::enable_if<sizeof(T) == sizeof(int64_t), T>::type* = nullptr>
#endif
void PrimitiveProcessor::scanAndFilterTypeDispatcher(NewColRequestHeader* in,
    NewColResultHeader* out,
    unsigned outSize,
    unsigned* written)
{
    constexpr int W = sizeof(T);
    auto dataType = (execplan::CalpontSystemCatalog::ColDataType) in->colType.DataType;
    if (dataType == execplan::CalpontSystemCatalog::DOUBLE)
    {
        const uint16_t ridSize = in->NVALS;
        uint16_t* ridArray = in->getRIDArrayPtr(W);
        const uint32_t itemsPerBlock = logicalBlockMode ? BLOCK_SIZE
                                                        : BLOCK_SIZE / W;
        filterColumnData<T, KIND_FLOAT>(in, out, outSize, written, ridArray, ridSize, block, itemsPerBlock, parsedColumnFilter);
        return;
    }
    _scanAndFilterTypeDispatcher<T>(in, out, outSize, written);
}

template<typename T,
         typename std::enable_if<sizeof(T) == sizeof(int8_t) ||
                                 sizeof(T) == sizeof(int16_t) ||
#ifdef __GNUC__
 #if ___GNUC__ >= 5
                                 sizeof(T) == sizeof(int128_t), T>::type* = nullptr> // gcc >= 5
 #else
                                 sizeof(T) == sizeof(int128_t), T>::type*> // gcc 4.8.5
 #endif
#else
                                 sizeof(T) == sizeof(int128_t), T>::type* = nullptr>
#endif
void PrimitiveProcessor::scanAndFilterTypeDispatcher(NewColRequestHeader* in,
    NewColResultHeader* out,
    unsigned outSize,
    unsigned* written)
{
    _scanAndFilterTypeDispatcher<T>(in, out, outSize, written);
}

template<typename T,
#ifdef __GNUC__
 #if ___GNUC__ >= 5
         typename std::enable_if<sizeof(T) == sizeof(int128_t), T>::type* = nullptr> // gcc >= 5
 #else
         typename std::enable_if<sizeof(T) == sizeof(int128_t), T>::type*> // gcc 4.8.5
 #endif
#else
         typename std::enable_if<sizeof(T) == sizeof(int128_t), T>::type* = nullptr>
#endif
void PrimitiveProcessor::_scanAndFilterTypeDispatcher(NewColRequestHeader* in,
    NewColResultHeader* out,
    unsigned outSize,
    unsigned* written)
{
    constexpr int W = sizeof(T);
    const uint16_t ridSize = in->NVALS;
    uint16_t* ridArray = in->getRIDArrayPtr(W);
    const uint32_t itemsPerBlock = logicalBlockMode ? BLOCK_SIZE
                                                    : BLOCK_SIZE / W;

    filterColumnData<T, KIND_DEFAULT>(in, out, outSize, written, ridArray, ridSize, block, itemsPerBlock, parsedColumnFilter);
}

template<typename T,
#ifdef __GNUC__
 #if ___GNUC__ >= 5
         typename std::enable_if<sizeof(T) <= sizeof(int64_t), T>::type* = nullptr> // gcc >= 5
 #else
         typename std::enable_if<sizeof(T) <= sizeof(int64_t), T>::type*> // gcc 4.8.5
 #endif
#else
         typename std::enable_if<sizeof(T) <= sizeof(int64_t), T>::type* = nullptr>
#endif
void PrimitiveProcessor::_scanAndFilterTypeDispatcher(NewColRequestHeader* in,
    NewColResultHeader* out,
    unsigned outSize,
    unsigned* written)
{
    constexpr int W = sizeof(T);
    const uint16_t ridSize = in->NVALS;
    uint16_t* ridArray = in->getRIDArrayPtr(W);
    const uint32_t itemsPerBlock = logicalBlockMode ? BLOCK_SIZE
                                                    : BLOCK_SIZE / W;

    auto dataType = (execplan::CalpontSystemCatalog::ColDataType) in->colType.DataType;
    if ((dataType == execplan::CalpontSystemCatalog::CHAR ||
        dataType == execplan::CalpontSystemCatalog::VARCHAR ||
        dataType == execplan::CalpontSystemCatalog::TEXT) &&
        !isDictTokenScan(in))
    {
        filterColumnData<T, KIND_TEXT>(in, out, outSize, written, ridArray, ridSize, block, itemsPerBlock, parsedColumnFilter);
        return;
    }

    if (datatypes::isUnsigned(dataType))
    {
        using UT = typename std::conditional<std::is_unsigned<T>::value || datatypes::is_uint128_t<T>::value, T, typename datatypes::make_unsigned<T>::type>::type;
        filterColumnData<UT, KIND_UNSIGNED>(in, out, outSize, written, ridArray, ridSize, block, itemsPerBlock, parsedColumnFilter);
        return;
    }
    filterColumnData<T, KIND_DEFAULT>(in, out, outSize, written, ridArray, ridSize, block, itemsPerBlock, parsedColumnFilter);
}

// The entrypoint for block scanning and filtering.
// The block is in in msg, out msg is used to store values|RIDs matched.
template<typename T>
void PrimitiveProcessor::columnScanAndFilter(NewColRequestHeader* in, NewColResultHeader* out,
                                             unsigned outSize, unsigned* written)
{
#ifdef PRIM_DEBUG
    auto markEvent = [&] (char eventChar)
    {
        if (fStatsPtr)
            fStatsPtr->markEvent(in->LBID, pthread_self(), in->hdr.SessionID, eventChar);
    };
#endif
    constexpr int W = sizeof(T);

    void *outp = static_cast<void*>(out);
    memcpy(outp, in, sizeof(ISMPacketHeader) + sizeof(PrimitiveHeader));
    out->NVALS = 0;
    out->LBID = in->LBID;
    out->ism.Command = COL_RESULTS;
    out->OutputType = in->OutputType;
    out->RidFlags = 0;
    *written = sizeof(NewColResultHeader);
    //...Initialize I/O counts;
    out->CacheIO    = 0;
    out->PhysicalIO = 0;

#if 0
    // short-circuit the actual block scan for testing
    if (out->LBID >= 802816)
    {
        out->ValidMinMax = false;
        out->Min = 0;
        out->Max = 0;
        return;
    }
#endif

#ifdef PRIM_DEBUG
    markEvent('B');
#endif

    // Sort ridArray (the row index array) if there are RIDs with this in msg
    in->sortRIDArrayIfNeeded(W);
    scanAndFilterTypeDispatcher<T>(in, out, outSize, written);
#ifdef PRIM_DEBUG
    markEvent('C');
#endif
}

template
void primitives::PrimitiveProcessor::columnScanAndFilter<int8_t>(NewColRequestHeader*, NewColResultHeader*, unsigned, unsigned*);
template
void primitives::PrimitiveProcessor::columnScanAndFilter<int16_t>(NewColRequestHeader*, NewColResultHeader*, unsigned int, unsigned int*);
template
void primitives::PrimitiveProcessor::columnScanAndFilter<int32_t>(NewColRequestHeader*, NewColResultHeader*, unsigned int, unsigned int*);
template
void primitives::PrimitiveProcessor::columnScanAndFilter<int64_t>(NewColRequestHeader*, NewColResultHeader*, unsigned int, unsigned int*);
template
void primitives::PrimitiveProcessor::columnScanAndFilter<int128_t>(NewColRequestHeader*, NewColResultHeader*, unsigned int, unsigned int*);

} // namespace primitives
// vim:ts=4 sw=4:
