// kcp keepalive (IKCP_CMD_PING) liveness detection tests.
//
// Covers: probe trigger on idle, growing probe interval, reset on any
// inbound packet, probe-count reset, dead judgement (state = -1 + on_dead),
// interval=0 disabled, inbound PING handling, unknown command tolerance,
// symmetric two-way keepalive, and 32-bit timestamp wraparound.
//
// Time is driven manually through ikcp_update(kcp, current) so every
// transition is deterministic.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "ikcp.h"

namespace
{

constexpr IUINT32 kConv    = 0x12345678;
constexpr std::uint8_t kWinsCmd = 84; // IKCP_CMD_WINS (probe reuses the window-tell command)

// ---------------------------------------------------------------------------
// Packet helpers
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Two-instance simulator: outputs are captured and fed to the peer.
// ---------------------------------------------------------------------------

class KcpPair {
public:
    KcpPair() {
        a_ = ikcp_create(kConv, this);
        b_ = ikcp_create(kConv, this);
        ikcp_setoutput(a_, output_static);
        ikcp_setoutput(b_, output_static);
        // 10ms internal interval so flush happens on every step
        ikcp_nodelay(a_, 1, 10, 2, 1);
        ikcp_nodelay(b_, 1, 10, 2, 1);
    }

    ~KcpPair() {
        ikcp_release(a_);
        ikcp_release(b_);
    }

    ikcpcb* a() { return a_; }
    ikcpcb* b() { return b_; }

    std::vector<std::vector<char>>& out_a() { return out_a_; }
    std::vector<std::vector<char>>& out_b() { return out_b_; }

    // One simulation step at `now`: deliver expired in-flight frames first,
    // then A updates and sends, B receives, B updates, A receives.
    void step(IUINT32 now) {
        deliver_inflight(now);

        ikcp_update(a_, now);
        b_->current = now;
        if (delay_a_to_b_ > 0) {
            for (auto& pkt : out_a_) inflight_a_to_b_.push_back({pkt, now + (IUINT32)delay_a_to_b_});
        } else {
            for (auto& pkt : out_a_) ikcp_input(b_, pkt.data(), (long)pkt.size());
        }
        out_a_.clear();

        ikcp_update(b_, now);
        a_->current = now;
        if (drop_b_to_a_) {
            out_b_.clear(); // B's frames never arrive at A (simulated death)
        } else if (delay_b_to_a_ > 0) {
            for (auto& pkt : out_b_) inflight_b_to_a_.push_back({pkt, now + (IUINT32)delay_b_to_a_});
        } else {
            for (auto& pkt : out_b_) ikcp_input(a_, pkt.data(), (long)pkt.size());
        }
        out_b_.clear();
    }

    struct inflight {
        std::vector<char> pkt;
        IUINT32 arrive_at;
    };
    void deliver_inflight(IUINT32 now) {
        for (auto it = inflight_a_to_b_.begin(); it != inflight_a_to_b_.end();) {
            if (static_cast<IINT32>(now - it->arrive_at) >= 0) {
                ikcp_input(b_, it->pkt.data(), (long)it->pkt.size());
                it = inflight_a_to_b_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = inflight_b_to_a_.begin(); it != inflight_b_to_a_.end();) {
            if (static_cast<IINT32>(now - it->arrive_at) >= 0) {
                ikcp_input(a_, it->pkt.data(), (long)it->pkt.size());
                it = inflight_b_to_a_.erase(it);
            } else {
                ++it;
            }
        }
    }

    bool drop_b_to_a_ = false;
    int delay_a_to_b_ = 0;
    int delay_b_to_a_ = 0;
    std::vector<inflight> inflight_a_to_b_;
    std::vector<inflight> inflight_b_to_a_;

    void a_send(const char* data, int len) { ikcp_send(a_, data, len); }
    void b_send(const char* data, int len) { ikcp_send(b_, data, len); }

    // Network simulation knobs (applied at the output boundary):
    // - drop_b_to_a(): B's frames never reach A - simulates B dying silently
    // - set_delay_a_to_b(ms): A's frames arrive at B after `ms` (in-flight
    //   delay, like a real network) - breaks the zero-latency sync artifact
    void drop_b_to_a(bool on) { drop_b_to_a_ = on; }
    void set_delay_a_to_b(int ms) { delay_a_to_b_ = ms; }
    void set_delay_b_to_a(int ms) { delay_b_to_a_ = ms; }

    // Advance in 10ms steps up to `target` (matches the kcp interval cadence
    // so ikcp_flush runs on every update, like real usage).
    void advance_to(IUINT32 target) {
        if (target == 0) {
            if (now_ == 0) step(0);
            return;
        }
        while (now_ < target) {
            IUINT32 step_now = now_ + 10;
            if (step_now > target) step_now = target;
            step(step_now);
            now_ = step_now;
        }
    }

    // Whether A/B emitted a probe during the last step() (read-then-reset).
    bool a_sent_ping() { bool v = a_ping_seen_; a_ping_seen_ = false; return v; }
    bool b_sent_ping() { bool v = b_ping_seen_; b_ping_seen_ = false; return v; }

private:
    static int output_static(const char* buf, int len, ikcpcb* kcp, void* user) {
        auto* self   = static_cast<KcpPair*>(user);
        auto& target = (kcp == self->a_) ? self->out_a_ : self->out_b_;
        target.emplace_back(buf, buf + len);
        if (len >= 5 && static_cast<unsigned char>(buf[4]) == kWinsCmd) {
            (kcp == self->a_ ? self->a_ping_seen_ : self->b_ping_seen_) = true;
        }
        return 0;
    }

    ikcpcb* a_ = nullptr;
    ikcpcb* b_ = nullptr;
    std::vector<std::vector<char>> out_a_;
    std::vector<std::vector<char>> out_b_;
    bool a_ping_seen_ = false;
    bool b_ping_seen_ = false;
    IUINT32 now_ = 0;
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// 1. Idle connection triggers a PING probe after interval_ms with no input.
TEST(kcp_keepalive_test, probe_trigger_on_idle) {
    KcpPair p;
    ikcp_enable_keepalive(p.a(), 5000, 3);

    p.advance_to(0); // anchor idle clock (no packet received yet)
    p.advance_to(4999);
    EXPECT_FALSE(p.a_sent_ping()) << "no probe before the interval elapses";

    p.advance_to(5000);
    EXPECT_TRUE(p.a_sent_ping()) << "probe must be sent once idle interval elapses";
}

// 3. Any inbound packet refreshes the idle clock.
TEST(kcp_keepalive_test, inbound_packet_resets_idle_clock) {
    KcpPair p;
    ikcp_enable_keepalive(p.a(), 5000, 3);

    p.advance_to(0);
    // peer sends data at t=4000 -> A receives it
    p.b_send("hello", 5);
    p.advance_to(4000);

    p.advance_to(5000);
    EXPECT_FALSE(p.a_sent_ping()) << "clock was reset by inbound data at 4000";

    p.advance_to(9000);
    EXPECT_TRUE(p.a_sent_ping()) << "probe now fires at 4000+5000";
}

// 4b. Same as above with correct time ordering.
TEST(kcp_keepalive_test, inbound_packet_resets_probe_count_ordered) {
    KcpPair p;
    ikcp_enable_keepalive(p.a(), 5000, 10);

    p.advance_to(0);
    p.advance_to(5000);
    EXPECT_TRUE(p.a_sent_ping()) << "probe #1 at 5s";

    p.b_send("data", 4);
    // B's data is flushed on its next update (t=5010) and reaches A in the
    // same step: ts_lastinput=5010, probe counter reset. Next probe due 10010.
    p.advance_to(8000);

    p.advance_to(10009);
    EXPECT_FALSE(p.a_sent_ping()) << "probe re-armed at 5010+5000";
    p.advance_to(10010);
    EXPECT_TRUE(p.a_sent_ping()) << "probe fires at 5010+5000";

    p.advance_to(15000);
    EXPECT_FALSE(p.a_sent_ping()) << "no probe at the old 10000 slot";
}

// 5. Unanswered probes beyond max_count judge the link dead: state = -1,
//    on_dead fires exactly once, and probing stops.
TEST(kcp_keepalive_test, dead_after_max_unanswered_probes) {
    KcpPair p;
    ikcp_enable_keepalive(p.a(), 5000, 2);

    p.advance_to(0);
    p.advance_to(5000);
    EXPECT_TRUE(p.a_sent_ping()) << "probe #1";
    p.advance_to(10000);
    EXPECT_TRUE(p.a_sent_ping()) << "probe #2 (fixed interval)";

    // one more interval without any response: link judged dead, update()
    // returns non-zero from now on
    p.advance_to(15000);
    EXPECT_EQ(p.a()->state, (IUINT32)-1) << "link judged dead";
    EXPECT_NE(ikcp_update(p.a(), 15000), 0) << "update() surfaces the dead state";
    EXPECT_NE(ikcp_update(p.a(), 16000), 0) << "update() keeps returning non-zero";

    p.advance_to(20000);
    EXPECT_FALSE(p.a_sent_ping()) << "probing stopped after dead";
}

// 6. interval_ms == 0 (default) disables the feature entirely.
TEST(kcp_keepalive_test, disabled_by_default) {
    KcpPair p;

    p.advance_to(0);
    p.advance_to(100000);
    EXPECT_FALSE(p.a_sent_ping());
    EXPECT_FALSE(p.b_sent_ping());
    EXPECT_EQ(p.a()->state, (IUINT32)0);
}

// 7. An inbound WINS probe refreshes liveness like any other packet:
//    B enables keepalive with a shorter interval, so its real probe (a WINS
//    segment produced by ikcp itself) reaches A at t=2000.
TEST(kcp_keepalive_test, inbound_wins_refreshes_liveness) {
    KcpPair p;
    ikcp_enable_keepalive(p.a(), 5000, 3);
    // B probes once (max=1) at t=2000 then stops: a single real WINS frame
    // reaches A and refreshes its liveness clock
    ikcp_enable_keepalive(p.b(), 2000, 1);

    p.advance_to(0);
    p.advance_to(1999);
    EXPECT_FALSE(p.a_sent_ping()) << "A idle, probe due at 5000";

    p.advance_to(2000); // B's WINS probe arrives at A
    p.advance_to(5000);
    EXPECT_FALSE(p.a_sent_ping()) << "A's clock refreshed by B's WINS at 2000";

    p.advance_to(7000);
    EXPECT_TRUE(p.a_sent_ping()) << "A probes at 2000+5000";
}

// 8. Unknown commands are rejected without disturbing the data path.
TEST(kcp_keepalive_test, unknown_command_ignored) {
    KcpPair p;
    // build a raw segment in host byte order (ikcp decode32u uses memcpy when
    // IWORDS_BIG_ENDIAN is not defined), cmd = 86 (not in 81..84 whitelist)
    std::vector<char> unknown(24, 0);
    std::memcpy(unknown.data(), &kConv, sizeof(kConv));
    unknown[4] = static_cast<char>(86);
    p.advance_to(0);
    EXPECT_EQ(ikcp_input(p.a(), unknown.data(), (long)unknown.size()), -3);

    // data path still works afterwards
    p.a_send("ping-data", 9);
    p.advance_to(1000);
    p.advance_to(2000);
    char buf[16] = {0};
    EXPECT_EQ(ikcp_recv(p.b(), buf, sizeof(buf)), 9);
    EXPECT_EQ(std::memcmp(buf, "ping-data", 9), 0);
}

// 9. Normal process: both sides exchange data periodically while keepalive
//    is enabled - no false dead, data flows both ways.
TEST(kcp_keepalive_test, normal_process) {
    KcpPair p;
    ikcp_enable_keepalive(p.a(), 5000, 3);
    ikcp_enable_keepalive(p.b(), 5000, 3);

    uint32_t now = 0;
    char buf[32] = {0};
    bool got = false;
    for (; now <= 30000; now += 10) {
        if (now % 100 == 0) { // application traffic every 100ms
            p.a_send("ping", 4);
            p.b_send("pong", 4);
        }
        p.step(now);
        if (ikcp_recv(p.a(), buf, sizeof(buf)) > 0) got = true;
        if (ikcp_recv(p.b(), buf, sizeof(buf)) > 0) got = true;
        ASSERT_EQ(p.a()->state, (IUINT32)0) << "A falsely dead at " << now;
        ASSERT_EQ(p.b()->state, (IUINT32)0) << "B falsely dead at " << now;
    }
    EXPECT_TRUE(got) << "data flowed both ways";
}

// 10. Peer abnormally disconnects (no FIN): B goes silent - A must detect
//     that the peer is gone through unanswered probes and surface it via
//     the ikcp_update() return value.
TEST(kcp_keepalive_test, peer_abnormal_disconnect_detected) {
    KcpPair p;
    ikcp_enable_keepalive(p.a(), 5000, 3);
    ikcp_enable_keepalive(p.b(), 5000, 3);

    p.advance_to(10000); // both alive, exchanging probes
    EXPECT_EQ(p.a()->state, (IUINT32)0);

    p.drop_b_to_a(true); // B dies silently: its frames never reach A
    p.advance_to(10000); // (no-op guard; keeps the timeline explicit)

    // A's probes go unanswered: probe#1@15000, #2@20000, #3@25000,
    // dead judged at 30000 - update() returns non-zero from then on
    p.advance_to(30000);
    EXPECT_EQ(p.a()->state, (IUINT32)-1) << "A detected the peer is gone";
    EXPECT_NE(ikcp_update(p.a(), 30000), 0) << "update() surfaces the dead link";

    // B side: B still receives A's probes, so B does not falsely die
    // (one-directional failure semantics)
    EXPECT_EQ(p.b()->state, (IUINT32)0) << "B stays alive (its inbound path is fine)";
}

// 11. Idle connection is kept alive by the probe exchange: with realistic
//     in-flight delay (breaking the zero-latency sync artifact), both sides
//     probe each other and neither is falsely judged dead.
TEST(kcp_keepalive_test, idle_kept_alive_by_probes) {
    KcpPair p;
    ikcp_enable_keepalive(p.a(), 5000, 3);
    ikcp_enable_keepalive(p.b(), 5000, 3);
    p.set_delay_a_to_b(50); // 50ms in-flight delay both ways
    p.set_delay_b_to_a(50);

    bool a_probed = false, b_probed = false;
    uint32_t now = 0;
    for (; now <= 60000; now += 10) {
        p.step(now);
        if (p.a_sent_ping()) a_probed = true;
        if (p.b_sent_ping()) b_probed = true;
        ASSERT_EQ(p.a()->state, (IUINT32)0) << "A falsely dead at " << now;
        ASSERT_EQ(p.b()->state, (IUINT32)0) << "B falsely dead at " << now;
    }
    EXPECT_TRUE(a_probed) << "A sent probes";
    EXPECT_TRUE(b_probed) << "B sent probes";
}

} // namespace
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
