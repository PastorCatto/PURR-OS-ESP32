#pragma once
// mesh::PacketManager (static packet pool + outbound/inbound queues) and
// mesh::MeshTables (dedup ring) implementations. MeshCore defines no
// concrete implementation of either — entirely the integrator's design,
// confirmed by reading Dispatcher.h/Mesh.h directly (both are pure
// abstract interfaces with no default backing store).

#include <Mesh.h>  // MeshTables is declared here, not Dispatcher.h
#include <Packet.h>

#define MC_PACKET_POOL_SIZE     16
#define MC_OUTBOUND_QUEUE_SIZE  16
#define MC_INBOUND_QUEUE_SIZE   8
#define MC_DEDUP_RING_SIZE      32

class PurrPacketManager : public mesh::PacketManager {
public:
    mesh::Packet* allocNew() override;
    void free(mesh::Packet* packet) override;

    void queueOutbound(mesh::Packet* packet, uint8_t priority, uint32_t scheduled_for) override;
    mesh::Packet* getNextOutbound(uint32_t now) override;
    int getOutboundCount(uint32_t now) const override;
    int getOutboundTotal() const override;
    int getFreeCount() const override;
    mesh::Packet* getOutboundByIdx(int i) override;
    mesh::Packet* removeOutboundByIdx(int i) override;
    void queueInbound(mesh::Packet* packet, uint32_t scheduled_for) override;
    mesh::Packet* getNextInbound(uint32_t now) override;

private:
    mesh::Packet pool_[MC_PACKET_POOL_SIZE];
    bool pool_used_[MC_PACKET_POOL_SIZE] = {false};

    struct QueuedPacket {
        mesh::Packet* packet = nullptr;
        uint8_t priority = 0;
        uint32_t scheduled_for = 0;
    };
    QueuedPacket outbound_[MC_OUTBOUND_QUEUE_SIZE];
    QueuedPacket inbound_[MC_INBOUND_QUEUE_SIZE];
};

class PurrMeshTables : public mesh::MeshTables {
public:
    bool hasSeen(const mesh::Packet* packet) override;
    void clear(const mesh::Packet* packet) override;

private:
    struct DedupEntry {
        bool used = false;
        uint8_t hash[MAX_HASH_SIZE] = {0};
    };
    DedupEntry ring_[MC_DEDUP_RING_SIZE];
    int next_slot_ = 0;
};
