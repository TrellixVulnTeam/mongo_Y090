/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <map>

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/stats/scalar_histogram.h"
#include "mongo/db/query/stats/stats_gen.h"

namespace mongo::stats {
using TypeCounts = std::map<sbe::value::TypeTags, double>;

class ArrayHistogram {
public:
    /**
     * Factory method for constructing an ArrayHistogram using StatsPath IDL as input. Caller is
     * responsible for freeing.
     */
    static ArrayHistogram* makeArrayHistogram(Statistics stats);

    // Constructs an empty scalar histogram.
    ArrayHistogram();

    // Constructor for scalar field histograms.
    ArrayHistogram(ScalarHistogram scalar,
                   TypeCounts typeCounts,
                   double trueCount = 0.0,
                   double falseCount = 0.0);

    // Constructor for array field histograms. We have to initialize all array fields in this case.
    ArrayHistogram(ScalarHistogram scalar,
                   TypeCounts typeCounts,
                   ScalarHistogram arrayUnique,
                   ScalarHistogram arrayMin,
                   ScalarHistogram arrayMax,
                   TypeCounts arrayTypeCounts,
                   double emptyArrayCount = 0.0,
                   double trueCount = 0.0,
                   double falseCount = 0.0);

    // ArrayHistogram is neither copy-constructible nor copy-assignable.
    ArrayHistogram(const ArrayHistogram&) = delete;
    ArrayHistogram& operator=(const ArrayHistogram&) = delete;

    // However, it is move-constructible and move-assignable.
    ArrayHistogram(ArrayHistogram&&) = default;
    ArrayHistogram& operator=(ArrayHistogram&&) = default;
    ~ArrayHistogram() = default;

    std::string toString() const;

    // Serialize to BSON for storage in stats collection.
    BSONObj serialize() const;

    const ScalarHistogram& getScalar() const;
    const ScalarHistogram& getArrayUnique() const;
    const ScalarHistogram& getArrayMin() const;
    const ScalarHistogram& getArrayMax() const;
    const TypeCounts& getTypeCounts() const;
    const TypeCounts& getArrayTypeCounts() const;

    // Returns whether or not this histogram includes array data points.
    bool isArray() const;

    // Get the total number of arrays in the histogram's path including empty arrays.
    double getArrayCount() const;

    // Get the total number of empty arrays ( [] ) in the histogram's path.
    double getEmptyArrayCount() const {
        return _emptyArrayCount;
    }

    // Get the count of true booleans.
    double getTrueCount() const {
        return _trueCount;
    }

    // Get the count of false booleans.
    double getFalseCount() const {
        return _falseCount;
    }

private:
    /* Fields for all paths. */

    // Contains values which appeared originally as scalars on the path.
    ScalarHistogram _scalar;
    // The number of values of each type.
    TypeCounts _typeCounts;
    // The number of empty arrays - they are not accounted for in the histograms.
    double _emptyArrayCount;
    // The counts of true & false booleans.
    double _trueCount;
    double _falseCount;

    /* Fields for array paths (only initialized if arrays are present). */

    // Contains unique scalar values originating from arrays.
    boost::optional<ScalarHistogram> _arrayUnique;
    // Contains minimum values originating from arrays **per class**.
    boost::optional<ScalarHistogram> _arrayMin;
    // Contains maximum values originating from arrays **per class**.
    boost::optional<ScalarHistogram> _arrayMax;
    // The number of values of each type inside all arrays.
    boost::optional<TypeCounts> _arrayTypeCounts;
};

/**
 * Returns an owned BSON Object representing data matching mongo::Statistics IDL.
 */
BSONObj makeStatistics(double documents, const ArrayHistogram& arrayHistogram);

/**
 * Returns an owned BSON Object representing data matching mongo::StatsPath IDL.
 */
BSONObj makeStatsPath(StringData path, double documents, const ArrayHistogram& arrayHistogram);
}  // namespace mongo::stats
