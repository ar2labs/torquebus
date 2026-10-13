// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/database/CanMessage.h"

#include <algorithm>
#include <utility>

namespace torquebus {

const CanSignal* CanMessage::multiplexerSwitch() const noexcept
{
    const auto match = std::find_if(signalList.begin(),
                                    signalList.end(),
                                    [](const CanSignal& signal) { return signal.isMultiplexer; });

    return match == signalList.end() ? nullptr : &*match;
}

void CanMessage::signalsIn(const std::uint8_t* payload,
                           std::size_t payloadLength,
                           std::vector<const CanSignal*>& out) const
{
    // clear() keeps the capacity, which is the point: a caller on the frame
    // path hands the same vector back every time and never allocates again
    // after the first message wide enough to fill it.
    out.clear();

    const CanSignal* switchSignal = multiplexerSwitch();

    // Read the switch once. Reading it per multiplexed signal would be the
    // obvious way to write this and would cost a full bit extraction for every
    // signal in the message.
    std::optional<std::int64_t> selector;
    if (switchSignal != nullptr && payload != nullptr) {
        selector = switchSignal->rawValue(payload, payloadLength);
    }

    for (const CanSignal& signal : signalList) {
        if (!signal.multiplexerValue.has_value()) {
            out.push_back(&signal);
            continue;
        }

        // A multiplexed signal in a message with no switch is a broken
        // database. Leaving it out is the honest answer: there is no way to
        // know whether this frame carries it.
        if (!selector.has_value()) {
            continue;
        }

        if (static_cast<std::int64_t>(*signal.multiplexerValue) == *selector) {
            out.push_back(&signal);
        }
    }
}

std::vector<const CanSignal*> CanMessage::signalsIn(const std::uint8_t* payload,
                                                    std::size_t payloadLength) const
{
    std::vector<const CanSignal*> present;
    present.reserve(signalList.size());

    signalsIn(payload, payloadLength, present);

    return present;
}

CanFrame CanMessage::makeFrame() const noexcept
{
    CanFrame frame;
    frame.identifier = identifier;
    frame.format = format;
    frame.direction = CanDirection::Tx;

    // More than eight bytes is a CAN FD message, and the frame has to say so. It used
    // to say a payload of twelve bytes in a data length code of eight - a classic
    // frame that does not exist, which a driver refuses and a receiver cannot read.
    // FD payloads come in steps (12, 16, 20, 24, 32, 48, 64), so a length between two
    // is carried by the next one up, the spare bytes zero.
    //
    // Clamped first: a database built in code rather than parsed has not been
    // through the parser's check, and a length past the array is a frame that reads
    // past it.
    const auto payload = static_cast<std::uint8_t>(std::min<std::size_t>(length, kMaxCanPayload));

    frame.fd = payload > kMaxClassicCanPayload;
    frame.dlc = dlcFromPayloadLength(payload, frame.fd);
    frame.length = payloadLengthFromDlc(frame.dlc, frame.fd);
    return frame;
}

const CanSignal* CanMessage::findSignal(std::string_view signalName) const noexcept
{
    const auto match =
        std::find_if(signalList.begin(), signalList.end(), [signalName](const CanSignal& signal) {
            return signal.name == signalName;
        });

    return match == signalList.end() ? nullptr : &*match;
}

CanSignal* CanMessage::findSignal(std::string_view signalName) noexcept
{
    return const_cast<CanSignal*>(std::as_const(*this).findSignal(signalName));
}

void CanDatabase::addMessage(CanMessage message)
{
    const std::uint64_t key = keyFor(message.identifier, message.format);

    if (const auto existing = m_byIdentifier.find(key); existing != m_byIdentifier.end()) {
        m_messages[existing->second] = std::move(message);
        return;
    }

    m_byIdentifier.emplace(key, m_messages.size());
    m_messages.push_back(std::move(message));
}

const CanMessage* CanDatabase::find(std::uint32_t identifier, CanFrameFormat format) const noexcept
{
    const auto match = m_byIdentifier.find(keyFor(identifier, format));
    return match == m_byIdentifier.end() ? nullptr : &m_messages[match->second];
}

const CanMessage* CanDatabase::findByName(std::string_view name) const noexcept
{
    const auto match =
        std::find_if(m_messages.begin(), m_messages.end(), [name](const CanMessage& message) {
            return message.name == name;
        });

    return match == m_messages.end() ? nullptr : &*match;
}

std::size_t CanDatabase::signalCount() const noexcept
{
    std::size_t total = 0;
    for (const CanMessage& message : m_messages) {
        total += message.signalList.size();
    }
    return total;
}

void CanDatabase::clear()
{
    m_messages.clear();
    m_byIdentifier.clear();
    nodes.clear();
    version.clear();
    sourcePath.clear();
}

} // namespace torquebus
