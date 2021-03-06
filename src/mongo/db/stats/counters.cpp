/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/stats/counters.h"

#include "mongo/client/authenticate.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/log.h"

namespace mongo {

void OpCounters::gotOp(int op, bool isCommand) {
    switch (op) {
        case dbInsert: /*gotInsert();*/
            break;     // need to handle multi-insert
        case dbQuery:
            if (isCommand)
                gotCommand();
            else
                gotQuery();
            break;

        case dbUpdate:
            gotUpdate();
            break;
        case dbDelete:
            gotDelete();
            break;
        case dbGetMore:
            gotGetMore();
            break;
        case dbKillCursors:
        case opReply:
            break;
        default:
            log() << "OpCounters::gotOp unknown op: " << op << std::endl;
    }
}

void OpCounters::_checkWrap(CacheAligned<AtomicWord<long long>> OpCounters::*counter, int n) {
    static constexpr auto maxCount = 1LL << 60;
    auto oldValue = (this->*counter).fetchAndAddRelaxed(n);
    if (oldValue > maxCount) {
        _insert.store(0);
        _query.store(0);
        _update.store(0);
        _delete.store(0);
        _getmore.store(0);
        _command.store(0);
    }
}

BSONObj OpCounters::getObj() const {
    BSONObjBuilder b;
    b.append("insert", _insert.loadRelaxed());
    b.append("query", _query.loadRelaxed());
    b.append("update", _update.loadRelaxed());
    b.append("delete", _delete.loadRelaxed());
    b.append("getmore", _getmore.loadRelaxed());
    b.append("command", _command.loadRelaxed());
    return b.obj();
}

void NetworkCounter::hitPhysicalIn(long long bytes) {
    static const int64_t MAX = 1ULL << 60;

    // don't care about the race as its just a counter
    const bool overflow = _physicalBytesIn.loadRelaxed() > MAX;

    if (overflow) {
        _physicalBytesIn.store(bytes);
    } else {
        _physicalBytesIn.fetchAndAdd(bytes);
    }
}

void NetworkCounter::hitPhysicalOut(long long bytes) {
    static const int64_t MAX = 1ULL << 60;

    // don't care about the race as its just a counter
    const bool overflow = _physicalBytesOut.loadRelaxed() > MAX;

    if (overflow) {
        _physicalBytesOut.store(bytes);
    } else {
        _physicalBytesOut.fetchAndAdd(bytes);
    }
}

void NetworkCounter::hitLogicalIn(long long bytes) {
    static const int64_t MAX = 1ULL << 60;

    // don't care about the race as its just a counter
    const bool overflow = _together.logicalBytesIn.loadRelaxed() > MAX;

    if (overflow) {
        _together.logicalBytesIn.store(bytes);
        // The requests field only gets incremented here (and not in hitPhysical) because the
        // hitLogical and hitPhysical are each called for each operation. Incrementing it in both
        // functions would double-count the number of operations.
        _together.requests.store(1);
    } else {
        _together.logicalBytesIn.fetchAndAdd(bytes);
        _together.requests.fetchAndAdd(1);
    }
}

void NetworkCounter::hitLogicalOut(long long bytes) {
    static const int64_t MAX = 1ULL << 60;

    // don't care about the race as its just a counter
    const bool overflow = _logicalBytesOut.loadRelaxed() > MAX;

    if (overflow) {
        _logicalBytesOut.store(bytes);
    } else {
        _logicalBytesOut.fetchAndAdd(bytes);
    }
}

void NetworkCounter::acceptedTFOIngress() {
    _tfo.accepted.fetchAndAddRelaxed(1);
}

void NetworkCounter::append(BSONObjBuilder& b) {
    b.append("bytesIn", static_cast<long long>(_together.logicalBytesIn.loadRelaxed()));
    b.append("bytesOut", static_cast<long long>(_logicalBytesOut.loadRelaxed()));
    b.append("physicalBytesIn", static_cast<long long>(_physicalBytesIn.loadRelaxed()));
    b.append("physicalBytesOut", static_cast<long long>(_physicalBytesOut.loadRelaxed()));
    b.append("numRequests", static_cast<long long>(_together.requests.loadRelaxed()));

    BSONObjBuilder tfo;
#ifdef __linux__
    tfo.append("kernelSetting", _tfo.kernelSetting);
#endif
    tfo.append("serverSupported", _tfo.kernelSupportServer);
    tfo.append("clientSupported", _tfo.kernelSupportClient);
    tfo.append("accepted", _tfo.accepted.loadRelaxed());
    b.append("tcpFastOpen", tfo.obj());
}

void AuthCounter::initializeMechanismMap(const std::vector<std::string>& mechanisms) {
    invariant(_mechanisms.empty());

    for (const auto& mech : mechanisms) {
        _mechanisms.emplace(
            std::piecewise_construct, std::forward_as_tuple(mech), std::forward_as_tuple());
    }
}

void AuthCounter::incSpeculativeAuthenticateReceived(const std::string& mechanism) try {
    _mechanisms.at(mechanism).speculativeAuthenticate.received.fetchAndAddRelaxed(1);
} catch (const std::out_of_range&) {
    uasserted(51767,
              str::stream() << "Received " << auth::kSpeculativeAuthenticate << " for mechanism "
                            << mechanism << " which is unknown or not enabled");
}

void AuthCounter::incSpeculativeAuthenticateSuccessful(const std::string& mechanism) try {
    _mechanisms.at(mechanism).speculativeAuthenticate.successful.fetchAndAddRelaxed(1);
} catch (const std::out_of_range&) {
    // Should never actually occur since it'd mean we succeeded at a mechanism
    // we're not configured for.
    uasserted(51768,
              str::stream() << "Unexpectedly succeeded at " << auth::kSpeculativeAuthenticate
                            << " for " << mechanism << " which is not enabled");
}

/**
 * authentication: {
 *   "mechanisms": {
 *     "SCRAM-SHA-256": {
 *       "speculativeAuthenticate": { received: ###, successful: ### },
 *     },
 *     "MONGODB-X509": {
 *       "speculativeAuthenticate": { received: ###, successful: ### },
 *     },
 *   },
 * }
 */
void AuthCounter::append(BSONObjBuilder* b) {
    BSONObjBuilder mechsBuilder(b->subobjStart("mechanisms"));

    for (const auto& it : _mechanisms) {
        const auto received = it.second.speculativeAuthenticate.received.load();
        const auto successful = it.second.speculativeAuthenticate.successful.load();

        BSONObjBuilder mechBuilder(mechsBuilder.subobjStart(it.first));
        BSONObjBuilder specAuthBuilder(mechBuilder.subobjStart(auth::kSpeculativeAuthenticate));
        specAuthBuilder.append("received", received);
        specAuthBuilder.append("successful", successful);
        specAuthBuilder.done();
        mechBuilder.done();
    }

    mechsBuilder.done();
}

OpCounters globalOpCounters;
OpCounters replOpCounters;
NetworkCounter networkCounter;
AuthCounter authCounter;
}  // namespace mongo
