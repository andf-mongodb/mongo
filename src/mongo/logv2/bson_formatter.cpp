/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/logv2/bson_formatter.h"

#include <boost/container/small_vector.hpp>
#include <boost/log/attributes/value_extraction.hpp>
#include <boost/log/utility/formatting_ostream.hpp>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/logv2/attribute_storage.h"
#include "mongo/logv2/attributes.h"
#include "mongo/logv2/constants.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/logv2/log_tag.h"
#include "mongo/logv2/named_arg_formatter.h"
#include "mongo/util/time_support.h"

#include <fmt/format.h>

namespace mongo {
namespace logv2 {

namespace {
struct NameExtractor {
    template <typename T>
    void operator()(StringData name, const T& value) {
        name_args.push_back(fmt::internal::make_arg<fmt::format_context>(name));
    }

    boost::container::small_vector<fmt::basic_format_arg<fmt::format_context>,
                                   constants::kNumStaticAttrs>
        name_args;
};

struct BSONValueExtractor {
    BSONValueExtractor(BSONObjBuilder& builder)
        : _builder(builder.subobjStart(constants::kAttributesFieldName)) {}

    ~BSONValueExtractor() {
        _builder.done();
    }

    void operator()(StringData name, CustomAttributeValue const& val) {
        if (val.BSONAppend) {
            val.BSONAppend(_builder, name);
        } else if (val.toBSON) {
            _builder.append(name, val.toBSON());
        } else {
            _builder.append(name, val.toString());
        }
    }

    // BSONObj is coming as a pointer, the generic one handles references
    void operator()(StringData name, const BSONObj* val) {
        _builder.append(name, *val);
    }

    // BSON is lacking unsigned types, so store unsigned int32 as signed int64
    void operator()(StringData name, unsigned int val) {
        _builder.append(name, static_cast<long long>(val));
    }

    // BSON is lacking unsigned types, so store unsigned int64 as signed int64, users need to deal
    // with this.
    void operator()(StringData name, unsigned long long val) {
        _builder.append(name, static_cast<long long>(val));
    }

    template <typename T>
    void operator()(StringData name, const T& value) {
        _builder.append(name, value);
    }

private:
    BSONObjBuilder _builder;
};
}  // namespace

void BSONFormatter::operator()(boost::log::record_view const& rec,
                               boost::log::formatting_ostream& strm) const {
    using boost::log::extract;

    // Build a JSON object for the user attributes.
    const auto& attrs = extract<TypeErasedAttributeStorage>(attributes::attributes(), rec).get();

    BSONObjBuilder builder;
    builder.append(constants::kTimestampFieldName,
                   extract<Date_t>(attributes::timeStamp(), rec).get());
    builder.append(constants::kSeverityFieldName,
                   extract<LogSeverity>(attributes::severity(), rec).get().toStringDataCompact());
    builder.append(constants::kComponentFieldName,
                   extract<LogComponent>(attributes::component(), rec).get().getNameForLog());
    builder.append(constants::kContextFieldName,
                   extract<StringData>(attributes::threadName(), rec).get());
    auto stable_id = extract<StringData>(attributes::stableId(), rec).get();
    if (!stable_id.empty()) {
        builder.append(constants::kStableIdFieldName, stable_id);
    }

    NameExtractor nameExtractor;
    attrs.apply(nameExtractor);

    // Insert the attribute names back into the message string using a special formatter
    fmt::memory_buffer buffer;
    fmt::vformat_to<detail::NamedArgFormatter, char>(
        buffer,
        extract<StringData>(attributes::message(), rec).get().toString(),
        fmt::basic_format_args<fmt::format_context>(nameExtractor.name_args.data(),
                                                    nameExtractor.name_args.size()));
    builder.append(constants::kMessageFieldName, fmt::to_string(buffer));

    if (!attrs.empty()) {
        BSONValueExtractor extractor(builder);
        attrs.apply(extractor);
    }
    LogTag tags = extract<LogTag>(attributes::tags(), rec).get();
    if (tags != LogTag::kNone) {
        builder.append(constants::kTagsFieldName, tags.toBSON());
    }

    BSONObj obj = builder.obj();
    strm.write(obj.objdata(), obj.objsize());
}

}  // namespace logv2
}  // namespace mongo
