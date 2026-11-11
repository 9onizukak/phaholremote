#include "../src/protocol.h"
#include "../src/subnets.h"

#include <cassert>
#include <iostream>

using namespace remote;

std::vector<uint8_t> Chunk(uint32_t frameId, uint32_t index, uint32_t sessionLow = 2) {
    constexpr uint32_t jpegBytes = 1203;
    const uint32_t length = index == 0 ? kChunkBytes : jpegBytes - kChunkBytes;
    std::vector<uint8_t> packet(UdpChunkHeader::wireBytes + length, static_cast<uint8_t>(index + 1));
    WriteWords(packet.data(), std::array{kMagic, 1u, sessionLow, frameId, jpegBytes, index, 2u});
    return packet;
}

void TestAssembly() {
    FrameAssembler assembler(1, 2);
    const auto now = Clock::now();
    auto push = [&](const auto& packet, Clock::duration delay = 0ms) {
        return assembler.Push(packet.data(), packet.size(), now + delay);
    };

    assert(!push(Chunk(1, 1, 9))); // Other session.
    assert(!push(Chunk(1, 1)));   // Out of order.
    assert(!push(Chunk(1, 1)));   // Duplicate.
    auto first = push(Chunk(1, 0));
    assert(first && first->id == 1 && first->jpeg.size() == 1203);
    assert(first->jpeg.front() == 1 && first->jpeg[1199] == 1 && first->jpeg[1200] == 2);

    assert(!push(Chunk(2, 0)));
    assert(!push(Chunk(4, 0)));
    assert(assembler.TakeDrops() == 2); // Partial #2 and entirely missing #3.
    assert(!push(Chunk(2, 1)));
    auto fourth = push(Chunk(4, 1));
    assert(fourth && fourth->id == 4 && assembler.TakeDrops() == 0);

    assert(!push(Chunk(5, 0)));
    assert(!push(Chunk(5, 1), 120ms));
    assert(assembler.TakeDrops() == 1); // Late last chunk cannot resurrect #5.
    assembler.DropThrough(7);
    assert(assembler.TakeDrops() == 2);
    assembler.DropThrough(7);
    assert(assembler.TakeDrops() == 0);

    assert(!push(Chunk(8, 0)));
    auto truncated = Chunk(8, 1);
    truncated.pop_back();
    assert(!push(truncated));
    assert(push(Chunk(8, 1)));
}

void TestAdaptation() {
    AdaptiveQuality quality;
    const FeedbackPacket drops{1, 4, 2, 12};
    const FeedbackPacket highRtt{2, 4, 0, 71};
    const FeedbackPacket clean{3, 8, 0, 12};
    const FeedbackPacket idle{3, 0, 0, 12};
    const FeedbackPacket unknown{3, 8, 0, kUnknownRtt};
    assert(quality.Update(drops) == 90);
    assert(quality.Update(drops) == 70);
    assert(quality.Update(highRtt) == 50);
    assert(quality.Update(highRtt) == 30);
    assert(quality.Update(highRtt) == 20);
    assert(quality.Update(highRtt) == 20);

    for (int i = 0; i < 10; ++i) {
        assert(quality.Update(idle) == 20);
        assert(quality.Update(unknown) == 20);
    }
    for (int expected = 20; expected < 90; expected += 5) {
        for (int i = 0; i < 5; ++i) assert(quality.Update(clean) == expected);
        assert(quality.Update(clean) == expected + 5);
    }
    for (int i = 0; i < 6; ++i) assert(quality.Update(clean) == 90);
}

void TestTrustedSubnets() {
    const auto subnets = ParseSubnets("192.168.0.0/16 , 172.16.0.0/16\r\n");
    assert(subnets && subnets->size() == 2);
    assert((*subnets)[0].Contains(0xc0a822c4)); // 192.168.34.196
    assert((*subnets)[0].Contains(0xc0a80064)); // 192.168.0.100
    assert((*subnets)[0].Contains(0xc0a8ffff)); // 192.168.255.255
    assert(!(*subnets)[0].Contains(0xc0a90001)); // 192.169.0.1
    assert((*subnets)[1].Contains(0xac10ffff)); // 172.16.255.255
    assert(!(*subnets)[1].Contains(0xac110001)); // 172.17.0.1
    assert(ParseSubnet("192.168.0.20/32")->Contains(0xc0a80014));
    assert(ParseSubnets(" \r\n")->empty());
    assert(!ParseSubnets("192.168.0.0/24,"));
    assert(!ParseSubnets("192.168.0.0/24, 0.0.0.0/0"));
    assert(!ParseSubnet("192.168.256.0/24"));
    assert(!ParseSubnet("192.168.00.0/24"));
}

int main() {
    TestAssembly();
    TestAdaptation();
    TestTrustedSubnets();
    std::cout << "Protocol tests passed\n";
}
