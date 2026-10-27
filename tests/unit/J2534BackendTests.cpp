// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "plugins/driver-j2534/J2534CanBackend.h"
#include "plugins/driver-j2534/J2534Types.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>

using namespace torquebus;
using namespace std::chrono_literals;

namespace {

// In-memory mock J2534 device state
struct MockDeviceState {
    unsigned long nextDeviceId{100};
    unsigned long nextChannelId{200};
    unsigned long nextFilterId{300};

    bool openCalled{false};
    bool closeCalled{false};
    bool connectCalled{false};
    bool disconnectCalled{false};
    bool startFilterCalled{false};
    bool stopFilterCalled{false};

    unsigned long lastBaudrate{0};
    unsigned long lastProtocol{0};

    std::vector<j2534::PASSTHRU_MSG> writtenMsgs;
    std::vector<j2534::PASSTHRU_MSG> msgsToRead;

    void reset()
    {
        *this = MockDeviceState{};
    }
};

MockDeviceState g_mockState;
std::mutex g_mockMutex;

long CALLBACK MockPassThruOpen(void* /*pName*/, unsigned long* pDeviceID)
{
    if (!pDeviceID) {
        return j2534::ERR_NULLPARAMETER;
    }
    std::lock_guard lock{g_mockMutex};
    g_mockState.openCalled = true;
    *pDeviceID = ++g_mockState.nextDeviceId;
    return j2534::STATUS_NOERROR;
}

long CALLBACK MockPassThruClose(unsigned long /*DeviceID*/)
{
    std::lock_guard lock{g_mockMutex};
    g_mockState.closeCalled = true;
    return j2534::STATUS_NOERROR;
}

long CALLBACK MockPassThruConnect(unsigned long /*DeviceID*/,
                                  unsigned long ProtocolID,
                                  unsigned long /*Flags*/,
                                  unsigned long Baudrate,
                                  unsigned long* pChannelID)
{
    if (!pChannelID) {
        return j2534::ERR_NULLPARAMETER;
    }
    std::lock_guard lock{g_mockMutex};
    g_mockState.connectCalled = true;
    g_mockState.lastProtocol = ProtocolID;
    g_mockState.lastBaudrate = Baudrate;
    *pChannelID = ++g_mockState.nextChannelId;
    return j2534::STATUS_NOERROR;
}

long CALLBACK MockPassThruDisconnect(unsigned long /*ChannelID*/)
{
    std::lock_guard lock{g_mockMutex};
    g_mockState.disconnectCalled = true;
    return j2534::STATUS_NOERROR;
}

long CALLBACK MockPassThruReadMsgs(unsigned long /*ChannelID*/,
                                   j2534::PASSTHRU_MSG* pMsg,
                                   unsigned long* pNumMsgs,
                                   unsigned long /*Timeout*/)
{
    if (!pMsg || !pNumMsgs) {
        return j2534::ERR_NULLPARAMETER;
    }
    std::lock_guard lock{g_mockMutex};
    if (g_mockState.msgsToRead.empty()) {
        *pNumMsgs = 0;
        return j2534::ERR_BUFFER_EMPTY;
    }

    const unsigned long toCopy =
        (std::min)(*pNumMsgs, static_cast<unsigned long>(g_mockState.msgsToRead.size()));
    for (unsigned long i = 0; i < toCopy; ++i) {
        pMsg[i] = g_mockState.msgsToRead[i];
    }
    g_mockState.msgsToRead.erase(g_mockState.msgsToRead.begin(),
                                 g_mockState.msgsToRead.begin() + toCopy);
    *pNumMsgs = toCopy;
    return j2534::STATUS_NOERROR;
}

long CALLBACK MockPassThruWriteMsgs(unsigned long /*ChannelID*/,
                                    j2534::PASSTHRU_MSG* pMsg,
                                    unsigned long* pNumMsgs,
                                    unsigned long /*Timeout*/)
{
    if (!pMsg || !pNumMsgs) {
        return j2534::ERR_NULLPARAMETER;
    }
    std::lock_guard lock{g_mockMutex};
    for (unsigned long i = 0; i < *pNumMsgs; ++i) {
        g_mockState.writtenMsgs.push_back(pMsg[i]);
    }
    return j2534::STATUS_NOERROR;
}

long CALLBACK MockPassThruStartMsgFilter(unsigned long /*ChannelID*/,
                                         unsigned long /*FilterType*/,
                                         j2534::PASSTHRU_MSG* /*pMaskMsg*/,
                                         j2534::PASSTHRU_MSG* /*pPatternMsg*/,
                                         j2534::PASSTHRU_MSG* /*pFlowControlMsg*/,
                                         unsigned long* pFilterID)
{
    if (!pFilterID) {
        return j2534::ERR_NULLPARAMETER;
    }
    std::lock_guard lock{g_mockMutex};
    g_mockState.startFilterCalled = true;
    *pFilterID = ++g_mockState.nextFilterId;
    return j2534::STATUS_NOERROR;
}

long CALLBACK MockPassThruStopMsgFilter(unsigned long /*ChannelID*/, unsigned long /*FilterID*/)
{
    std::lock_guard lock{g_mockMutex};
    g_mockState.stopFilterCalled = true;
    return j2534::STATUS_NOERROR;
}

j2534::J2534FunctionTable makeMockTable()
{
    j2534::J2534FunctionTable table{};
    table.PassThruOpen = &MockPassThruOpen;
    table.PassThruClose = &MockPassThruClose;
    table.PassThruConnect = &MockPassThruConnect;
    table.PassThruDisconnect = &MockPassThruDisconnect;
    table.PassThruReadMsgs = &MockPassThruReadMsgs;
    table.PassThruWriteMsgs = &MockPassThruWriteMsgs;
    table.PassThruStartMsgFilter = &MockPassThruStartMsgFilter;
    table.PassThruStopMsgFilter = &MockPassThruStopMsgFilter;
    return table;
}

} // namespace

TEST(J2534BackendTests, Standard11BitFrameConvertsToPassThruMsgAndBack)
{
    CanFrame frame;
    frame.identifier = 0x123;
    frame.format = CanFrameFormat::Standard;
    frame.length = 4;
    frame.dlc = 4;
    frame.data[0] = 0xAA;
    frame.data[1] = 0xBB;
    frame.data[2] = 0xCC;
    frame.data[3] = 0xDD;

    j2534::PASSTHRU_MSG msg{};
    ASSERT_TRUE(J2534CanBackend::canFrameToMsg(frame, msg));

    EXPECT_EQ(msg.ProtocolID, j2534::PROTOCOL_CAN);
    EXPECT_EQ(msg.DataSize, 8U); // 4 bytes ID + 4 bytes payload
    EXPECT_EQ(msg.TxFlags & j2534::CAN_29BIT_ID, 0U);
    EXPECT_EQ(msg.Data[0], 0x00);
    EXPECT_EQ(msg.Data[1], 0x00);
    EXPECT_EQ(msg.Data[2], 0x01);
    EXPECT_EQ(msg.Data[3], 0x23);
    EXPECT_EQ(msg.Data[4], 0xAA);
    EXPECT_EQ(msg.Data[5], 0xBB);
    EXPECT_EQ(msg.Data[6], 0xCC);
    EXPECT_EQ(msg.Data[7], 0xDD);

    CanFrame decoded;
    msg.Timestamp = 12345;
    ASSERT_TRUE(J2534CanBackend::msgToCanFrame(msg, decoded));

    EXPECT_EQ(decoded.identifier, 0x123U);
    EXPECT_FALSE(decoded.isExtended());
    EXPECT_EQ(decoded.length, 4U);
    EXPECT_EQ(decoded.data[0], 0xAA);
    EXPECT_EQ(decoded.data[1], 0xBB);
    EXPECT_EQ(decoded.data[2], 0xCC);
    EXPECT_EQ(decoded.data[3], 0xDD);
    EXPECT_EQ(decoded.timestampNs, 12345ULL * 1000ULL);
}

TEST(J2534BackendTests, Extended29BitFrameConvertsCorrectly)
{
    CanFrame frame;
    frame.identifier = 0x18FEE500;
    frame.format = CanFrameFormat::Extended;
    frame.length = 8;
    frame.dlc = 8;
    frame.data.fill(0xFF);

    j2534::PASSTHRU_MSG msg{};
    ASSERT_TRUE(J2534CanBackend::canFrameToMsg(frame, msg));

    EXPECT_EQ(msg.ProtocolID, j2534::PROTOCOL_CAN);
    EXPECT_EQ(msg.DataSize, 12U); // 4 bytes ID + 8 bytes payload
    EXPECT_NE(msg.TxFlags & j2534::CAN_29BIT_ID, 0U);
    EXPECT_EQ(msg.Data[0], 0x18);
    EXPECT_EQ(msg.Data[1], 0xFE);
    EXPECT_EQ(msg.Data[2], 0xE5);
    EXPECT_EQ(msg.Data[3], 0x00);

    CanFrame decoded;
    msg.RxStatus = j2534::CAN_29BIT_ID;
    ASSERT_TRUE(J2534CanBackend::msgToCanFrame(msg, decoded));

    EXPECT_EQ(decoded.identifier, 0x18FEE500U);
    EXPECT_TRUE(decoded.isExtended());
    EXPECT_EQ(decoded.length, 8U);
    EXPECT_EQ(decoded.data[7], 0xFF);
}

TEST(J2534BackendTests, ShortMsgReturnsFalse)
{
    j2534::PASSTHRU_MSG msg{};
    msg.DataSize = 3; // less than 4 bytes for CAN ID
    CanFrame frame;
    EXPECT_FALSE(J2534CanBackend::msgToCanFrame(msg, frame));
}

TEST(J2534BackendTests, LifecycleWithMockJ2534Device)
{
    {
        std::lock_guard lock{g_mockMutex};
        g_mockState.reset();
    }

    J2534CanBackend backend;
    backend.setFunctionTableForTesting(makeMockTable(), "Tactrix OpenPort Mock");

    EXPECT_TRUE(backend.isAvailable());

    const auto devices = backend.enumerate();
    ASSERT_FALSE(devices.empty());
    EXPECT_EQ(devices[0].backend, "j2534");
    EXPECT_EQ(devices[0].name, "Tactrix OpenPort Mock");

    CanChannelConfig config;
    config.timing.bitrate = 500'000;
    config.deviceHandle = "j2534:0";

    const Result openResult = backend.open(config);
    ASSERT_TRUE(openResult.succeeded()) << openResult.message();
    EXPECT_TRUE(backend.isOpen());

    {
        std::lock_guard lock{g_mockMutex};
        EXPECT_TRUE(g_mockState.openCalled);
        EXPECT_TRUE(g_mockState.connectCalled);
        EXPECT_TRUE(g_mockState.startFilterCalled);
        EXPECT_EQ(g_mockState.lastBaudrate, 500'000U);
        EXPECT_EQ(g_mockState.lastProtocol, j2534::PROTOCOL_CAN);
    }

    std::vector<CanFrame> receivedFrames;
    std::mutex rxMutex;
    backend.setFrameHandler([&](std::span<const CanFrame> batch) {
        std::lock_guard lock{rxMutex};
        receivedFrames.insert(receivedFrames.end(), batch.begin(), batch.end());
    });

    const Result startResult = backend.start();
    ASSERT_TRUE(startResult.succeeded());

    // Transmit a frame
    CanFrame txFrame;
    txFrame.identifier = 0x7E0;
    txFrame.length = 8;
    txFrame.dlc = 8;
    txFrame.data[0] = 0x02;
    txFrame.data[1] = 0x10;
    txFrame.data[2] = 0x01;

    const Result txResult = backend.transmit(txFrame);
    EXPECT_TRUE(txResult.succeeded());

    {
        std::lock_guard lock{g_mockMutex};
        ASSERT_EQ(g_mockState.writtenMsgs.size(), 1U);
        EXPECT_EQ(g_mockState.writtenMsgs[0].Data[3], 0xE0);
    }

    // Inject incoming frame to mock device
    j2534::PASSTHRU_MSG rxMsg{};
    rxMsg.ProtocolID = j2534::PROTOCOL_CAN;
    rxMsg.DataSize = 6;
    rxMsg.Data[0] = 0x00;
    rxMsg.Data[1] = 0x00;
    rxMsg.Data[2] = 0x07;
    rxMsg.Data[3] = 0xE8;
    rxMsg.Data[4] = 0x06;
    rxMsg.Data[5] = 0x50;

    {
        std::lock_guard lock{g_mockMutex};
        g_mockState.msgsToRead.push_back(rxMsg);
    }

    // Wait a brief moment for rx thread to process
    for (int i = 0; i < 50; ++i) {
        {
            std::lock_guard lock{rxMutex};
            // 1 echo from Tx + 1 from Rx = 2
            if (receivedFrames.size() >= 2) {
                break;
            }
        }
        std::this_thread::sleep_for(10ms);
    }

    {
        std::lock_guard lock{rxMutex};
        ASSERT_GE(receivedFrames.size(), 2U);
        bool foundRx = false;
        for (const auto& f : receivedFrames) {
            if (f.identifier == 0x7E8 && f.direction == CanDirection::Rx) {
                foundRx = true;
                EXPECT_EQ(f.length, 2U);
                EXPECT_EQ(f.data[0], 0x06);
                EXPECT_EQ(f.data[1], 0x50);
            }
        }
        EXPECT_TRUE(foundRx);
    }

    backend.stop();
    backend.close();
    EXPECT_FALSE(backend.isOpen());

    {
        std::lock_guard lock{g_mockMutex};
        EXPECT_TRUE(g_mockState.stopFilterCalled);
        EXPECT_TRUE(g_mockState.disconnectCalled);
        EXPECT_TRUE(g_mockState.closeCalled);
    }
}

TEST(J2534BackendTests, OpenFailsCleanlyWhenConnectReturnsError)
{
    j2534::J2534FunctionTable table = makeMockTable();
    table.PassThruConnect = [](unsigned long, unsigned long, unsigned long, unsigned long, unsigned long*) -> long {
        return j2534::ERR_DEVICE_NOT_CONNECTED;
    };

    J2534CanBackend backend;
    backend.setFunctionTableForTesting(table);

    CanChannelConfig config;
    config.timing.bitrate = 250'000;
    const Result res = backend.open(config);
    EXPECT_TRUE(res.failed());
    EXPECT_FALSE(backend.isOpen());
    EXPECT_TRUE(res.message().find("Device not connected") != std::string::npos);
}
