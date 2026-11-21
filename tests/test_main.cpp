// BloodTrail Together unit + integration tests (no game, no Unity account needed).
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <cmath>
#include <functional>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <thread>

#include "core/config.h"
#include "core/log.h"
#include "engine/net_engine.h"
#include "core/util.h"
#include "net/bytes.h"
#include "net/impairment.h"
#include "net/loopback_transport.h"
#include "net/session.h"
#include "net/udp_transport.h"
#include "relay/hmac.h"
#include "relay/json.h"
#include "relay/relay_api.h"
#include "relay/relay_messages.h"
#include "relay/relay_transport.h"
#include "sync/interp.h"
#include "sync/pose_codec.h"

using namespace btt;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (cond) ++g_pass;                                                      \
    else { ++g_fail; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)

static void section(const char* n) { printf("[%s]\n", n); }

// ---------------------------------------------------------------- primitives
static void test_base64_uuid() {
  section("base64/uuid");
  const char* s = "BloodTrail Together!";
  auto enc = base64_encode((const uint8_t*)s, strlen(s));
  std::vector<uint8_t> dec;
  CHECK(base64_decode(enc, dec));
  CHECK(std::string(dec.begin(), dec.end()) == s);
  uint8_t u[16];
  CHECK(uuid_parse("01234567-89ab-cdef-0011-2233445566ff", u));
  CHECK(u[0] == 0x01 && u[15] == 0xff && u[8] == 0x00);
  CHECK(semver_compare("v0.2.0", "0.1.9") == 1 && semver_compare("0.1.0", "v0.1.0") == 0);
}

static void test_hmac() {
  section("hmac-sha256");
  // RFC 4231 test case 2
  uint8_t out[32];
  const char* key = "Jefe";
  const char* msg = "what do ya want for nothing?";
  CHECK(hmac_sha256((const uint8_t*)key, 4, (const uint8_t*)msg, strlen(msg), out));
  CHECK(hex(out, 32) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

static void test_relay_messages() {
  section("relay wire format");
  relay::HmacKey key;
  for (int i = 0; i < 64; ++i) key[i] = (uint8_t)i;
  relay::ConnectionData conn;
  for (int i = 0; i < 255; ++i) conn[i] = (uint8_t)((i * 3) % 256);
  auto bind = relay::make_bind(0x0102, conn, key);
  CHECK(bind.size() == 4 + 1 + 2 + 1 + 255 + 32);
  CHECK(bind[0] == 0xDA && bind[1] == 0x72 && bind[2] == 0 && bind[3] == 0);
  CHECK(bind[5] == 0x02 && bind[6] == 0x01);  // nonce little-endian (Unity Transport)
  CHECK(bind[7] == 255);
  // Reference value computed independently with Python's hmac module.
  CHECK(hex(bind.data() + 263, 32) == "594f99e8e19f14a5c383c9da3fcd76693d4dc1b0645f3b2a92a73c1dcac3d334");

  relay::AllocationId a{}, b{};
  a[0] = 1; b[0] = 2;
  uint8_t payload[300];
  for (int i = 0; i < 300; ++i) payload[i] = (uint8_t)i;
  auto rel = relay::make_relay(a, b, payload, 300);
  CHECK(rel.size() == 38 + 300);
  CHECK(rel[36] == 0x01 && rel[37] == 0x2C);  // 300 big-endian
  relay::Parsed p;
  CHECK(relay::parse(rel.data(), rel.size(), p));
  CHECK(p.type == relay::MsgType::Relay && p.a == a && p.b == b && p.payload_len == 300 && p.payload[299] == 43);
  CHECK(relay::make_relay(a, b, payload, 1401).empty());
  auto ping = relay::make_ping(a, 1);
  CHECK(ping.size() == 22 && ping[20] == 1 && ping[21] == 0);
  uint8_t err[] = {0xDA, 0x72, 0, 12, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 4};
  CHECK(relay::parse(err, sizeof(err), p) && p.type == relay::MsgType::Error && p.error == 4);
}

static void test_json_allocation() {
  section("json + allocation parsing");
  // Shape of Unity's /v1/join response (fields per com.unity.services.relay models).
  std::string body = R"({"meta":{"requestId":"x"},"data":{"allocation":{
    "allocationId":"8a1f6b3e-0000-4000-8000-000000000001",
    "allocationIdBytes":"ih9rPgAAQACAAAAAAAAAAQ==",
    "serverEndpoints":[{"connectionType":"dtls","host":"1.2.3.4","port":7777,"network":"udp","reliable":false,"secure":true},
                       {"connectionType":"udp","host":"1.2.3.4","port":7778,"network":"udp","reliable":false,"secure":false}],
    "relayServer":{"ipV4":"1.2.3.4","port":7778},
    "key":")" + base64_encode(std::vector<uint8_t>(64, 7).data(), 64) + R"(",
    "connectionData":")" + base64_encode(std::vector<uint8_t>(16, 9).data(), 16) + R"(",
    "hostConnectionData":")" + base64_encode(std::vector<uint8_t>(16, 5).data(), 16) + R"(",
    "region":"europe-west4"}}})";
  Json j;
  std::string err;
  CHECK(Json::parse(body, j, &err));
  relay::AllocationInfo info;
  std::string why;
  CHECK(relay::RelayApi::parse_allocation(j["data"]["allocation"], true, info, why));
  CHECK(info.udp.port == 7778 && info.udp.host == "1.2.3.4");
  CHECK(info.key[63] == 7 && info.connection_data[15] == 9 && info.connection_data[16] == 0);
  CHECK(info.host_connection_data[0] == 5 && info.has_host_data);
  CHECK(info.allocation_id[0] == 0x8a && info.allocation_id[15] == 0x01);
  CHECK(info.region == "europe-west4");
  Json bad;
  CHECK(!Json::parse("{\"a\":", bad));
}

// ---------------------------------------------------------------- pose codec / interp
static void test_pose_codec() {
  section("pose codec");
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> u(-1, 1);
  double worst_deg = 0;
  for (int i = 0; i < 20000; ++i) {
    Quat q = qnormalize({u(rng), u(rng), u(rng), u(rng)});
    Writer w;
    write_quat32(w, q);
    Reader r(w.buf.data(), w.size());
    Quat d = read_quat32(r);
    double dot = std::fabs(q.x * d.x + q.y * d.y + q.z * d.z + q.w * d.w);
    double ang = 2 * std::acos(std::min(1.0, dot)) * 180 / 3.14159265;
    worst_deg = std::max(worst_deg, ang);
  }
  printf("  worst quaternion error %.3f deg\n", worst_deg);
  CHECK(worst_deg < 0.25);
  PoseFrame f;
  f.head.p = {123456.7f, -98765.4f, 170.3f};
  f.flags = 3;
  Writer w;
  write_pose(w, f);
  CHECK(w.size() == 65);
  Reader r(w.buf.data(), w.size());
  PoseFrame g = read_pose(r);
  CHECK(std::fabs(g.head.p.x - 123456.7f) < 0.06f && std::fabs(g.head.p.z - 170.3f) < 0.06f && g.flags == 3);
}

static void test_interpolation() {
  section("interpolation");
  PoseInterpolator ip;
  ip.configure(0.1, 0.15);
  std::mt19937 rng(3);
  std::uniform_real_distribution<double> jit(0.0, 0.03);
  // Head moves along X at 100 cm/s; samples every 33 ms with 0-30 ms jitter, 50 ms base latency.
  double worst = 0;
  for (int i = 0; i < 300; ++i) {
    double t = i / 30.0;
    PoseFrame f;
    f.head.p = {(float)(t * 100.0), 0, 170};
    ip.push((uint32_t)(t * 1000.0) + 5000, t + 0.05 + jit(rng), f);
    if (i > 30) {
      double now = t + 0.06;
      PoseFrame o;
      CHECK(ip.sample(now, o));
      // Expected position ~ (now - offset - delay) where offset ~= 0.05
      double expect = (now - 0.05 - 0.1) * 100.0;
      worst = std::max(worst, std::fabs(o.head.p.x - expect));
    }
  }
  printf("  worst interpolation error %.2f cm (jitter up to 30 ms)\n", worst);
  CHECK(worst < 4.0);
  PoseFrame o;
  CHECK(!ip.sample(100.0, o));  // stale after 3 s
}

// ---------------------------------------------------------------- session over impaired loopback
static bool run_until(double seconds, Session& a, Session& b, double& now, double dt,
                      const std::function<bool()>& done) {
  double end = now + seconds;
  while (now < end) {
    a.update(now);
    b.update(now);
    if (done()) return true;
    now += dt;
  }
  return false;
}

static void test_session(const char* label, ImpairmentParams imp) {
  printf("[session over loopback: %s]\n", label);
  auto [ta, tb] = LoopbackTransport::make_pair();
  auto ia = std::make_unique<ImpairedTransport>(std::move(ta));
  auto ib = std::make_unique<ImpairedTransport>(std::move(tb));
  ia->set_params(imp);
  ib->set_params(imp);
  Session host(std::move(ia), Role::Host, "Alice", "test");
  Session cli(std::move(ib), Role::Client, "Bob", "test");
  double now = 0;
  bool ok = run_until(10, host, cli, now, 0.002, [&] {
    return host.state() == SessionState::Connected && cli.state() == SessionState::Connected;
  });
  CHECK(ok);
  CHECK(host.peer_name() == "Bob" && cli.peer_name() == "Alice");

  const int N = 300;
  for (int i = 0; i < N; ++i) {
    NetEvent e;
    e.type = EventType::Notice;
    e.s1 = "msg" + std::to_string(i);
    host.send_event(e);
  }
  std::vector<NetEvent> got;
  run_until(30, host, cli, now, 0.002, [&] {
    cli.pop_events(got);
    return (int)got.size() >= N;
  });
  bool order = (int)got.size() == N;
  for (int i = 0; order && i < N; ++i) order = got[i].s1 == "msg" + std::to_string(i);
  CHECK(order);
  printf("  delivered %zu/%d reliable events in order\n", got.size(), N);

  // Pose stream: host moves at 100 cm/s; client samples should track it.
  double worst = 0;
  int samples = 0;
  double start = now;
  run_until(3, host, cli, now, 0.002, [&] {
    PoseFrame f;
    f.head.p = {(float)((now - start) * 100.0), 0, 0};
    host.set_local_pose(f);
    PoseFrame o;
    if (now - start > 1.0 && cli.sample_remote(now, o)) {
      double lag = imp.latency_ms / 1000.0 + 0.1;  // latency + interp delay
      worst = std::max(worst, std::fabs(o.head.p.x - (now - start - lag) * 100.0));
      ++samples;
    }
    return false;
  });
  auto st = cli.stats(now);
  printf("  pose samples %d, worst error %.1f cm, rtt %.0f ms, loss %.1f%%\n", samples, worst, st.rtt_ms, st.loss_pct);
  CHECK(samples > 100);
  CHECK(worst < 6.0 + imp.jitter_ms * 0.15);

  // Peer restart detection: client goes silent, host must flag interruption.
  run_until(3, host, cli, now, 0.002, [&] { return false; });
  CHECK(host.state() == SessionState::Connected);
}

static void test_session_timeout() {
  section("session interruption + disconnect");
  auto [ta, tb] = LoopbackTransport::make_pair();
  Session host(std::move(ta), Role::Host, "A", "t");
  Session cli(std::move(tb), Role::Client, "B", "t");
  double now = 0;
  run_until(2, host, cli, now, 0.005, [&] { return host.state() == SessionState::Connected; });
  CHECK(host.state() == SessionState::Connected);
  // Stop updating the client -> host sees silence.
  for (int i = 0; i < 700; ++i) { host.update(now); now += 0.005; }
  CHECK(host.state() == SessionState::Interrupted);
  // Client comes back -> restored.
  run_until(1, host, cli, now, 0.005, [&] { return host.state() == SessionState::Connected; });
  CHECK(host.state() == SessionState::Connected);
  // Client leaves politely.
  cli.close();
  for (int i = 0; i < 50; ++i) { host.update(now); now += 0.005; }
  CHECK(host.state() != SessionState::Connected && host.state() != SessionState::Disconnected && host.peer_name().empty());
}

// ---------------------------------------------------------------- mock Unity Relay server
// Implements the server side of the documented protocol on 127.0.0.1 so RelayTransport's
// data plane (BIND/HMAC, PING, CONNECT_REQUEST/ACCEPTED, RELAY, DISCONNECT) can be tested
// end-to-end. The HTTPS control plane is not involved here.
class MockRelay {
 public:
  struct Alloc {
    relay::AllocationId id{};
    relay::HmacKey key{};
    relay::ConnectionData conn{};
    sockaddr_in addr{};
    bool bound = false;
    int bad_hmacs = 0;
  };
  std::map<std::string, Alloc> allocs;  // by hex id
  SOCKET s = INVALID_SOCKET;
  uint16_t port = 0;
  std::atomic<bool> run{true};
  std::thread th;
  int relayed = 0;

  relay::AllocationInfo make(uint8_t tag) {
    Alloc a;
    for (int i = 0; i < 16; ++i) a.id[i] = (uint8_t)(tag * 16 + i);
    for (int i = 0; i < 64; ++i) a.key[i] = (uint8_t)(tag ^ i);
    for (int i = 0; i < 40; ++i) a.conn[i] = (uint8_t)(tag + i * 7);
    allocs[hex(a.id.data(), 16)] = a;
    relay::AllocationInfo info;
    info.allocation_id = a.id;
    info.key = a.key;
    info.connection_data = a.conn;
    info.udp = {"127.0.0.1", port, "udp", false};
    return info;
  }
  void start() {
    net_startup();
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(s, (sockaddr*)&a, sizeof(a));
    int l = sizeof(a);
    getsockname(s, (sockaddr*)&a, &l);
    port = ntohs(a.sin_port);
    DWORD tmo = 20;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
    th = std::thread([this] { loop(); });
  }
  void stop() {
    run = false;
    th.join();
    closesocket(s);
    net_cleanup();
  }
  void sendto_(const std::vector<uint8_t>& v, const sockaddr_in& to) {
    sendto(s, (const char*)v.data(), (int)v.size(), 0, (const sockaddr*)&to, sizeof(to));
  }
  Alloc* by_id(const relay::AllocationId& id) {
    auto it = allocs.find(hex(id.data(), 16));
    return it == allocs.end() ? nullptr : &it->second;
  }
  void loop() {
    uint8_t buf[2048];
    while (run) {
      sockaddr_in from{};
      int fl = sizeof(from);
      int n = recvfrom(s, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
      if (n <= 0) continue;
      if (n < 4 || buf[0] != 0xDA || buf[1] != 0x72) continue;
      uint8_t type = buf[3];
      if (type == 0 && n == 295) {  // BIND
        uint16_t nonce = (uint16_t)(buf[5] | (buf[6] << 8));
        relay::ConnectionData conn;
        memcpy(conn.data(), buf + 8, 255);
        for (auto& [k, a] : allocs) {
          if (a.conn != conn) continue;
          uint8_t mac[32];
          relay::compute_bind_hmac(nonce, conn, a.key, mac);
          if (memcmp(mac, buf + 263, 32) != 0) { ++a.bad_hmacs; break; }
          a.addr = from;
          a.bound = true;
          sendto_({0xDA, 0x72, 0, 1}, from);
        }
      } else if (type == 2) {  // PING -> echo
        sendto(s, (const char*)buf, n, 0, (sockaddr*)&from, fl);
      } else if (type == 3) {  // CONNECT_REQUEST
        relay::AllocationId self;
        memcpy(self.data(), buf + 4, 16);
        relay::ConnectionData to;
        memcpy(to.data(), buf + 21, 255);
        for (auto& [k, a] : allocs) {
          if (a.conn == to && a.bound) {
            std::vector<uint8_t> acc = {0xDA, 0x72, 0, 6};
            acc.insert(acc.end(), a.id.begin(), a.id.end());
            acc.insert(acc.end(), self.begin(), self.end());
            sendto_(acc, from);
          }
        }
      } else if (type == 10 || type == 9) {  // RELAY / DISCONNECT -> forward to "to"
        relay::AllocationId to;
        memcpy(to.data(), buf + 20, 16);
        if (auto* a = by_id(to); a && a->bound) {
          sendto(s, (const char*)buf, n, 0, (sockaddr*)&a->addr, sizeof(a->addr));
          if (type == 10) ++relayed;
        }
      }
    }
  }
};

static void test_relay_transport_mock() {
  section("RelayTransport against mock relay server");
  MockRelay mock;
  mock.start();
  auto host_info = mock.make(1);
  auto join_info = mock.make(2);
  join_info.host_connection_data = host_info.connection_data;
  join_info.has_host_data = true;

  Session host(RelayTransport::from_allocation(host_info, true, "TEST01"), Role::Host, "HostVR", "t");
  Session cli(RelayTransport::from_allocation(join_info, false, "TEST01"), Role::Client, "JoinVR", "t");
  double t0 = now_sec();
  bool ok = false;
  while (now_sec() - t0 < 8) {
    double now = now_sec();
    host.update(now);
    cli.update(now);
    if (host.state() == SessionState::Connected && cli.state() == SessionState::Connected) { ok = true; break; }
    Sleep(2);
  }
  CHECK(ok);
  CHECK(cli.peer_name() == "HostVR");
  NetEvent e;
  e.type = EventType::Sound;
  e.s1 = "/Game/Audio/test.test";
  e.v = {1, 2, 3};
  cli.send_event(e);
  std::vector<NetEvent> got;
  t0 = now_sec();
  while (now_sec() - t0 < 3 && got.empty()) {
    double now = now_sec();
    host.update(now);
    cli.update(now);
    host.pop_events(got);
    Sleep(2);
  }
  CHECK(got.size() == 1 && got[0].s1 == "/Game/Audio/test.test" && got[0].v.z == 3);
  printf("  relayed packets: %d, bad HMACs seen by server: %d/%d\n", mock.relayed,
         mock.allocs.begin()->second.bad_hmacs, std::next(mock.allocs.begin())->second.bad_hmacs);
  CHECK(mock.allocs.begin()->second.bad_hmacs == 0);
  cli.close();
  t0 = now_sec();
  while (now_sec() - t0 < 1) { host.update(now_sec()); Sleep(2); }
  CHECK(host.state() != SessionState::Connected && host.state() != SessionState::Disconnected);
  // A second player joins the same, still-open host session (same join code).
  auto join2 = mock.make(3);
  join2.host_connection_data = host_info.connection_data;
  join2.has_host_data = true;
  Session cli2(RelayTransport::from_allocation(join2, false, "TEST01"), Role::Client, "Rejoiner", "t");
  t0 = now_sec();
  bool rejoined = false;
  while (now_sec() - t0 < 8) {
    double now = now_sec();
    host.update(now);
    cli2.update(now);
    if (host.state() == SessionState::Connected && cli2.state() == SessionState::Connected) { rejoined = true; break; }
    Sleep(2);
  }
  CHECK(rejoined);
  CHECK(host.peer_name() == "Rejoiner");
  cli2.close();
  host.close();
  mock.stop();
}


// ---------------------------------------------------------------- engine + simulated player (real threads/clock)
static void test_engine_simulated_player() {
  section("engine with simulated player (6 s real time)");
  Settings s;
  s.player_name = "Tester";
  NetEngine eng(s);
  eng.start();
  eng.start_simulated_peer(SimMode::Mirror);
  double t0 = now_sec();
  int per_sec[7] = {};
  int sounds = 0;
  bool sent = false;
  while (now_sec() - t0 < 6.0) {
    double t = now_sec() - t0;
    PoseFrame f;
    f.flags = 3;
    f.head.p = {(float)(t * 50), 0, 170};
    f.left.p = {(float)(t * 50 + 30), -25, 130};
    f.right.p = {(float)(t * 50 + 30), 25, 130};
    eng.set_local_pose(f);
    eng.set_local_map("/Game/Maps/Sandbox/Map_Sandbox");
    if (!sent && t > 2.0) {
      eng.report_sound("/Game/Test.Test", {1, 2, 3}, 1, 1);
      sent = true;
    }
    PoseFrame r;
    if (eng.remote_pose(r)) per_sec[(int)t]++;
    std::vector<GameCommand> cmds;
    eng.pop_commands(cmds);
    for (auto& c : cmds)
      if (c.kind == GameCommand::Sound) ++sounds;
    Sleep(11);
  }
  auto snap = eng.snapshot();
  printf("  remote pose frames per second: %d %d %d %d %d %d, sounds echoed %d, state %s\n", per_sec[0], per_sec[1],
         per_sec[2], per_sec[3], per_sec[4], per_sec[5], sounds, snap.state.c_str());
  for (int i = 1; i < 6; ++i) CHECK(per_sec[i] > 50);
  CHECK(sounds == 1);
  CHECK(snap.state == "Connected" && snap.same_map);
  eng.stop();
}

int main() {
  log_init(L"", getenv("BTT_TEST_LOG") != nullptr, LogLevel::Debug);
  net_startup();
  test_base64_uuid();
  test_hmac();
  test_relay_messages();
  test_json_allocation();
  test_pose_codec();
  test_interpolation();
  test_session("clean", {});
  test_session("100ms +-20ms, 10% loss, 5% dup", {100, 20, 10.0, 5.0});
  test_session("250ms +-50ms, 25% loss", {250, 50, 25.0, 0.0});
  test_session_timeout();
  test_relay_transport_mock();
  test_engine_simulated_player();
  net_cleanup();
  printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
