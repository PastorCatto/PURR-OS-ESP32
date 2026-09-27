#include "mc_packet_pool.h"
#include <string.h>

// ── PurrPacketManager ───────────────────────────────────────────────────

mesh::Packet* PurrPacketManager::allocNew() {
    for (int i = 0; i < MC_PACKET_POOL_SIZE; i++) {
        if (!pool_used_[i]) {
            pool_used_[i] = true;
            pool_[i] = mesh::Packet();  // reset to a clean state
            return &pool_[i];
        }
    }
    return nullptr;
}

void PurrPacketManager::free(mesh::Packet* packet) {
    if (!packet) return;
    ptrdiff_t idx = packet - pool_;
    if (idx >= 0 && idx < MC_PACKET_POOL_SIZE) {
        pool_used_[idx] = false;
    }
}

void PurrPacketManager::queueOutbound(mesh::Packet* packet, uint8_t priority, uint32_t scheduled_for) {
    for (int i = 0; i < MC_OUTBOUND_QUEUE_SIZE; i++) {
        if (outbound_[i].packet == nullptr) {
            outbound_[i] = {packet, priority, scheduled_for};
            return;
        }
    }
    // queue full — drop the packet rather than leak the pool slot
    free(packet);
}

mesh::Packet* PurrPacketManager::getNextOutbound(uint32_t now) {
    int best = -1;
    for (int i = 0; i < MC_OUTBOUND_QUEUE_SIZE; i++) {
        if (outbound_[i].packet == nullptr) continue;
        if ((int32_t)(now - outbound_[i].scheduled_for) < 0) continue;  // not ready yet
        // lower priority number = more urgent (matches Mesh.cpp's usage:
        // priority 0 for direct/routed traffic, higher numbers de-prioritized)
        if (best < 0 || outbound_[i].priority < outbound_[best].priority) {
            best = i;
        }
    }
    if (best < 0) return nullptr;
    mesh::Packet* pkt = outbound_[best].packet;
    outbound_[best].packet = nullptr;
    return pkt;
}

int PurrPacketManager::getOutboundCount(uint32_t now) const {
    int n = 0;
    for (int i = 0; i < MC_OUTBOUND_QUEUE_SIZE; i++) {
        if (outbound_[i].packet && (int32_t)(now - outbound_[i].scheduled_for) >= 0) n++;
    }
    return n;
}

int PurrPacketManager::getOutboundTotal() const {
    int n = 0;
    for (int i = 0; i < MC_OUTBOUND_QUEUE_SIZE; i++) {
        if (outbound_[i].packet) n++;
    }
    return n;
}

int PurrPacketManager::getFreeCount() const {
    int n = 0;
    for (int i = 0; i < MC_PACKET_POOL_SIZE; i++) {
        if (!pool_used_[i]) n++;
    }
    return n;
}

mesh::Packet* PurrPacketManager::getOutboundByIdx(int i) {
    if (i < 0 || i >= MC_OUTBOUND_QUEUE_SIZE) return nullptr;
    return outbound_[i].packet;
}

mesh::Packet* PurrPacketManager::removeOutboundByIdx(int i) {
    if (i < 0 || i >= MC_OUTBOUND_QUEUE_SIZE) return nullptr;
    mesh::Packet* pkt = outbound_[i].packet;
    outbound_[i].packet = nullptr;
    return pkt;
}

void PurrPacketManager::queueInbound(mesh::Packet* packet, uint32_t scheduled_for) {
    for (int i = 0; i < MC_INBOUND_QUEUE_SIZE; i++) {
        if (inbound_[i].packet == nullptr) {
            inbound_[i] = {packet, 0, scheduled_for};
            return;
        }
    }
    free(packet);  // queue full — drop
}

mesh::Packet* PurrPacketManager::getNextInbound(uint32_t now) {
    for (int i = 0; i < MC_INBOUND_QUEUE_SIZE; i++) {
        if (inbound_[i].packet && (int32_t)(now - inbound_[i].scheduled_for) >= 0) {
            mesh::Packet* pkt = inbound_[i].packet;
            inbound_[i].packet = nullptr;
            return pkt;
        }
    }
    return nullptr;
}

// ── PurrMeshTables ──────────────────────────────────────────────────────

bool PurrMeshTables::hasSeen(const mesh::Packet* packet) {
    uint8_t hash[MAX_HASH_SIZE];
    packet->calculatePacketHash(hash);

    for (int i = 0; i < MC_DEDUP_RING_SIZE; i++) {
        if (ring_[i].used && memcmp(ring_[i].hash, hash, MAX_HASH_SIZE) == 0) {
            return true;
        }
    }
    // not seen — record it (ring buffer, evicts oldest)
    ring_[next_slot_].used = true;
    memcpy(ring_[next_slot_].hash, hash, MAX_HASH_SIZE);
    next_slot_ = (next_slot_ + 1) % MC_DEDUP_RING_SIZE;
    return false;
}

void PurrMeshTables::clear(const mesh::Packet* packet) {
    uint8_t hash[MAX_HASH_SIZE];
    packet->calculatePacketHash(hash);

    for (int i = 0; i < MC_DEDUP_RING_SIZE; i++) {
        if (ring_[i].used && memcmp(ring_[i].hash, hash, MAX_HASH_SIZE) == 0) {
            ring_[i].used = false;
            return;
        }
    }
}
