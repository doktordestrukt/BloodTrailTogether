// btt_simpeer: a headless second "player" for testing BloodTrail Together without another person.
// It speaks the exact same session protocol as the game DLL over a real transport:
//   btt_simpeer relay-host                 create a Unity Relay session, print the join code
//   btt_simpeer relay-join <CODE>          join a Unity Relay session (e.g. one hosted in-game)
//   btt_simpeer udp-host [port]            developer: direct UDP host on 127.0.0.1
//   btt_simpeer udp-join <ip> [port]       developer: direct UDP client
// Options: --name N --config path\config.ini --latency ms --jitter ms --loss pct --pattern orbit|idle|wave
//          --seconds N (auto-exit) --log path
// It sends scripted VR tracking (head + both hands), answers sounds with an echo, follows maps,
// and prints connection statistics once per second.
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <string>

#include "core/config.h"
#include "core/log.h"
#include "core/util.h"
#include "net/impairment.h"
#include "net/session.h"
#include "net/udp_transport.h"
#include "relay/relay_transport.h"

using namespace btt;

static volatile bool g_run = true;
static BOOL WINAPI on_ctrl(DWORD) {
  g_run = false;
  return TRUE;
}

static PoseFrame scripted_pose(const std::string& pattern, double t) {
  PoseFrame f{};
  f.flags = 3;
  float ang = pattern == "orbit" ? (float)(t * 0.5) : 0.0f;
  float r = pattern == "orbit" ? 150.0f : 0.0f;
  Vec3 head = {std::cos(ang) * r, std::sin(ang) * r, 165.0f + 3.0f * (float)std::sin(t * 1.3)};
  float face = ang + 1.5708f;
  Quat q = {0, 0, std::sin(face / 2), std::cos(face / 2)};
  f.head = {head, q};
  float wave = pattern == "idle" ? 0.0f : 25.0f * (float)std::sin(t * 4.0);
  auto hand = [&](float side, float up) {
    float fx = std::cos(face), fy = std::sin(face);
    float rx = -fy, ry = fx;
    return Xform{{head.x + fx * 35 + rx * side * 25, head.y + fy * 35 + ry * side * 25, head.z - 40 + up}, q};
  };
  f.left = hand(-1, 0);
  f.right = hand(1, wave);
  f.body = {{head.x, head.y, head.z - 165.0f}, q};  // floor point under the head (same as the game sends)
  return f;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("usage: btt_simpeer relay-host | relay-join CODE | udp-host [port] | udp-join IP [port]\n"
           "       [--name N] [--config file] [--latency ms] [--jitter ms] [--loss pct]\n"
           "       [--pattern orbit|idle|wave] [--seconds N] [--log file]\n");
    return 2;
  }
  std::string mode = argv[1], arg1, arg2;
  std::string name = "SimPeer", cfg_path, pattern = "orbit", log_path;
  std::string start_map = "/Game/Maps/Beta/Map_Beta_Basement";  // as host, the map the game should follow to
  std::string sound_path;  // if set, emit this sound asset from the bot's head every 5 s
  double last_sound = 0;
  std::string game_line;  // if set, send this game event (e.g. "hit 40 spine_02 ammo_9mm_b") every 10 s
  double last_game = 0;
  // --fake-npc (host): pretend the host's game has one NPC walking back and forth 3 m in front of
  // the origin, which dies 40 s after connecting. Exercises the client's NPC mirroring.
  bool fake_npc = false;
  double last_npc = 0, connected_at = -1;
  ImpairmentParams imp;
  double seconds = 0;
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--name") name = next();
    else if (a == "--config") cfg_path = next();
    else if (a == "--latency") imp.latency_ms = atoi(next().c_str());
    else if (a == "--jitter") imp.jitter_ms = atoi(next().c_str());
    else if (a == "--loss") imp.loss_pct = atof(next().c_str());
    else if (a == "--pattern") pattern = next();
    else if (a == "--seconds") seconds = atof(next().c_str());
    else if (a == "--log") log_path = next();
    else if (a == "--map") start_map = next();
    else if (a == "--sound") sound_path = next();
    else if (a == "--game") game_line = next();
    else if (a == "--fake-npc") fake_npc = true;
    else if (arg1.empty()) arg1 = a;
    else arg2 = a;
  }
  log_init(to_wide(log_path), true, LogLevel::Info);
  SetConsoleCtrlHandler(on_ctrl, TRUE);
  net_startup();

  Config cfg;
  if (!cfg_path.empty() && !cfg.load(to_wide(cfg_path))) BTT_WARN("could not read %s", cfg_path.c_str());
  Settings s = Settings::from(cfg);
  RelayConfig rc{s.unity_project_id, s.unity_environment, s.relay_region};

  TransportPtr t;
  Role role = Role::Host;
  if (mode == "relay-host") t = RelayTransport::host(rc);
  else if (mode == "relay-join") { t = RelayTransport::join(rc, arg1); role = Role::Client; }
  else if (mode == "udp-host") t = UdpTransport::host((uint16_t)(arg1.empty() ? 27777 : atoi(arg1.c_str())));
  else if (mode == "udp-join") {
    t = UdpTransport::join(arg1.empty() ? "127.0.0.1" : arg1, (uint16_t)(arg2.empty() ? 27777 : atoi(arg2.c_str())));
    role = Role::Client;
  } else {
    printf("unknown mode %s\n", mode.c_str());
    return 2;
  }
  auto imp_t = std::make_unique<ImpairedTransport>(std::move(t));
  imp_t->set_params(imp);
  Session ses(std::move(imp_t), role, name, BTT_VERSION);
  ses.set_pose_rate(s.pose_send_hz);

  double start = now_sec(), last_print = 0;
  std::string printed_code, my_map = start_map;
  int sounds_echoed = 0;
  while (g_run) {
    double now = now_sec();
    ses.update(now);
    if (!ses.join_code().empty() && ses.join_code() != printed_code) {
      printed_code = ses.join_code();
      printf("\n  JOIN CODE: %s\n\n", printed_code.c_str());
      BTT_INFO("JOIN CODE: %s", printed_code.c_str());
    }
    ses.set_local_pose(scripted_pose(pattern, now - start));
    if (!sound_path.empty() && ses.state() == SessionState::Connected && now - last_sound > 5.0) {
      last_sound = now;
      NetEvent e;
      e.type = EventType::Sound;
      e.s1 = sound_path;
      e.v = scripted_pose(pattern, now - start).head.p;
      e.f0 = 1.0f;
      e.f1 = 1.0f;
      ses.send_event(e);
      BTT_INFO("sent sound %s", sound_path.c_str());
    }
    if (fake_npc && ses.state() == SessionState::Connected && now - last_npc > 0.1) {
      if (connected_at < 0) connected_at = now;
      last_npc = now;
      double t = now - connected_at;
      double x = 300.0, y = 150.0 * std::sin(t * 0.3), vy = 45.0 * std::cos(t * 0.3);
      int dead = t > 40.0 ? 1 : 0;
      NetEvent e;
      e.type = EventType::Game;
      char b[160];
      snprintf(b, sizeof(b), "npc 1 %.1f %.1f 92.0 180.0 0 %.0f %d", x, y, dead ? 0.0 : vy, dead);
      e.s1 = b;
      ses.send_event(e);
    }
    if (!game_line.empty() && ses.state() == SessionState::Connected && now - last_game > 10.0 && now - start > 15.0) {
      last_game = now;
      NetEvent e;
      e.type = EventType::Game;
      e.s1 = game_line;
      ses.send_event(e);
      BTT_INFO("sent game event: %s", game_line.c_str());
    }
    std::vector<NetEvent> evs;
    ses.pop_events(evs);
    for (auto& e : evs) {
      if (e.type == EventType::Map) {
        BTT_INFO("peer map: %s", e.s1.c_str());
        if (role == Role::Client && e.s1 != my_map) {
          my_map = e.s1;  // follow the host like the game would
          NetEvent m;
          m.type = EventType::Map;
          m.s1 = my_map;
          ses.send_event(m);
        }
      } else if (e.type == EventType::Sound) {
        BTT_INFO("peer sound: %s at (%.0f, %.0f, %.0f) vol %.2f", e.s1.c_str(), e.v.x, e.v.y, e.v.z, e.f0);
        if (e.s2 == "echo") continue;  // never echo an echo (two SimPeers would ping-pong forever)
        NetEvent echo = e;
        echo.t_ms = 0;
        echo.s2 = "echo";
        echo.v = scripted_pose(pattern, now - start).head.p;
        ses.send_event(echo);
        ++sounds_echoed;
      } else if (e.type == EventType::Game) {
        BTT_INFO("peer game event: %s", e.s1.c_str());
      } else if (e.type == EventType::Notice) {
        BTT_INFO("peer notice: %s", e.s1.c_str());
      }
    }
    static SessionState prev = SessionState::Idle;
    if (ses.state() != prev) {
      prev = ses.state();
      BTT_INFO("state -> %s", session_state_name(prev));
      if (prev == SessionState::Connected) {
        NetEvent m;
        m.type = EventType::Map;
        m.s1 = my_map;
        ses.send_event(m);
        NetEvent n;
        n.type = EventType::Notice;
        n.s1 = "Hello from " + name;
        ses.send_event(n);
      }
    }
    if (now - last_print >= 1.0) {
      last_print = now;
      auto st = ses.stats(now);
      PoseFrame rp;
      bool have = ses.sample_remote(now, rp);
      printf("[%6.1fs] %-12s link=%-14s rtt=%5.0fms loss=%4.1f%% jitter=%4.1fms in=%llu out=%llu echoed=%d %s\n",
             now - start, session_state_name(ses.state()), link_state_name(ses.link_state()), st.rtt_ms, st.loss_pct,
             st.jitter_ms, (unsigned long long)st.pkts_in, (unsigned long long)st.pkts_out, sounds_echoed,
             have ? ("remote head=(" + std::to_string((int)rp.head.p.x) + "," + std::to_string((int)rp.head.p.y) + "," +
                     std::to_string((int)rp.head.p.z) + ")").c_str()
                  : "");
    }
    if (ses.state() == SessionState::Disconnected) {
      printf("disconnected: %s\n", ses.error().c_str());
      break;
    }
    if (seconds > 0 && now - start > seconds) break;
    Sleep(2);
  }
  ses.close();
  net_cleanup();
  return ses.error().empty() || ses.error() == "the other player left the session" ? 0 : 1;
}
