// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/j1939/J1939AddressTable.h"

#include <algorithm>

namespace torquebus {

bool J1939AddressTable::onFrame(const CanFrame& frame, std::uint64_t nowNs)
{
    const std::optional<J1939Id> id = j1939Decompose(frame);
    if (!id.has_value()) {
        return false;
    }

    if (id->pgn() == kPgnAddressClaimed) {
        const std::optional<J1939Name> name = j1939NameFromClaim(frame);
        if (!name.has_value()) {
            // Short or malformed. The NAME layer already refuses to invent the
            // missing bytes, and there is nothing here to record: a claim
            // without a NAME names nobody.
            return true;
        }

        if (j1939IsCannotClaimAddress(frame)) {
            onCannotClaim(*name, nowNs);
            return true;
        }

        if (id->sourceAddress > kJ1939MaxNodeAddress) {
            // A claim from the global address is malformed. Recording it would
            // create a seat at 255, which is every seat.
            return true;
        }

        onClaim(id->sourceAddress, *name, nowNs);
        return true;
    }

    if (id->sourceAddress > kJ1939MaxNodeAddress) {
        // Ordinary traffic from 254 or 255. The null address transmitting is
        // real and already visible through the defeated list; the global one is
        // not a transmitter at all.
        return true;
    }

    J1939NetworkNode& node = nodeFor(id->sourceAddress, nowNs);
    const bool firstTraffic = !node.trafficSeen;

    node.trafficSeen = true;
    node.lastSeenNs = nowNs;
    ++node.framesSeen;

    if (firstTraffic && !node.claimSeen) {
        J1939NetworkEvent event;
        event.kind = J1939NetworkEvent::Kind::UnclaimedTraffic;
        event.address = node.address;
        event.timestampNs = nowNs;

        m_events.push_back(std::move(event));
    }

    return true;
}

J1939NetworkNode& J1939AddressTable::nodeFor(std::uint8_t address, std::uint64_t nowNs)
{
    const auto position = std::lower_bound(m_nodes.begin(), m_nodes.end(), address,
                                           [](const J1939NetworkNode& node, std::uint8_t wanted) {
                                               return node.address < wanted;
                                           });

    if (position != m_nodes.end() && position->address == address) {
        return *position;
    }

    J1939NetworkNode node;
    node.address = address;
    node.firstSeenNs = nowNs;
    node.lastSeenNs = nowNs;

    // Kept sorted by address, which is the order a panel wants and costs
    // nothing to maintain: there are at most 254 of these and they appear once.
    return *m_nodes.insert(position, std::move(node));
}

void J1939AddressTable::onClaim(std::uint8_t address,
                                const J1939Name& name,
                                std::uint64_t nowNs)
{
    J1939NetworkNode& node = nodeFor(address, nowNs);

    node.lastSeenNs = nowNs;
    ++node.framesSeen;

    if (!node.claimSeen) {
        node.claimSeen = true;
        node.name = name;

        J1939NetworkEvent event;
        event.kind = J1939NetworkEvent::Kind::AddressClaimed;
        event.address = address;
        event.name = name;
        event.timestampNs = nowNs;

        m_events.push_back(std::move(event));
        return;
    }

    if (node.name.has_value() && node.name->value() == name.value()) {
        // The same ECU announcing itself again, which they do on request and
        // after a contest. Two physically different ECUs shipped with identical
        // NAMEs would look exactly like this, and there is nothing on the wire
        // that could tell them apart - which is the reason a NAME is supposed
        // to carry a serial number.
        return;
    }

    const J1939Name previous = *node.name;

    J1939NetworkEvent contest;
    contest.kind = J1939NetworkEvent::Kind::AddressContested;
    contest.address = address;
    contest.name = name;
    contest.previousName = previous;
    contest.timestampNs = nowNs;

    m_events.push_back(std::move(contest));

    if (!name.winsAgainst(previous)) {
        // The arriving NAME is the higher one, so it loses and the seat does
        // not change. The loser is required to stop using the address; whether
        // it does is exactly what somebody is watching this table to find out.
        return;
    }

    node.name = name;

    J1939NetworkEvent taken;
    taken.kind = J1939NetworkEvent::Kind::AddressTaken;
    taken.address = address;
    taken.name = name;
    taken.previousName = previous;
    taken.timestampNs = nowNs;

    m_events.push_back(std::move(taken));
}

void J1939AddressTable::onCannotClaim(const J1939Name& name, std::uint64_t nowNs)
{
    const auto found = std::find_if(m_defeated.begin(), m_defeated.end(),
                                    [&name](const J1939Defeated& entry) {
                                        return entry.name.value() == name.value();
                                    });

    if (found != m_defeated.end()) {
        found->lastSeenNs = nowNs;
        ++found->announcements;
        return;
    }

    J1939Defeated entry;
    entry.name = name;
    entry.firstSeenNs = nowNs;
    entry.lastSeenNs = nowNs;
    entry.announcements = 1U;

    m_defeated.push_back(std::move(entry));

    J1939NetworkEvent event;
    event.kind = J1939NetworkEvent::Kind::CannotClaim;
    event.address = kJ1939NullAddress;
    event.name = name;
    event.timestampNs = nowNs;

    m_events.push_back(std::move(event));
}

const J1939NetworkNode* J1939AddressTable::find(std::uint8_t address) const noexcept
{
    const auto position = std::lower_bound(m_nodes.begin(), m_nodes.end(), address,
                                           [](const J1939NetworkNode& node, std::uint8_t wanted) {
                                               return node.address < wanted;
                                           });

    if (position == m_nodes.end() || position->address != address) {
        return nullptr;
    }

    return &*position;
}

void J1939AddressTable::reset()
{
    m_nodes.clear();
    m_defeated.clear();
    m_events.clear();
}

} // namespace torquebus
