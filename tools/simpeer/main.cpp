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
#include <map>
#include <sstream>
#include <vector>

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

// --at X Y / --yaw DEG place the bot; --no-hands reports both controllers as untracked
static float g_at_x = 0, g_at_y = 0, g_yaw = 90;
static bool g_no_hands = false;

static PoseFrame scripted_pose(const std::string& pattern, double t) {
  PoseFrame f{};
  f.flags = g_no_hands ? 0 : 3;
  float ang = pattern == "orbit" ? (float)(t * 0.5) : 0.0f;
  float r = pattern == "orbit" ? 150.0f : 0.0f;
  Vec3 head = {g_at_x + std::cos(ang) * r, g_at_y + std::sin(ang) * r, 165.0f + 3.0f * (float)std::sin(t * 1.3)};
  float face = pattern == "orbit" ? ang + 1.5708f : g_yaw * 3.14159265f / 180.0f;
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
  if (pattern == "reach") {
    // big arm movements: left hand sweeps from the hip to overhead, right hand reaches far out to the side and front
    float fx = std::cos(face), fy = std::sin(face), rx = -fy, ry = fx;
    float a = (float)std::sin(t * 1.1), b = (float)std::sin(t * 0.8 + 1.0);
    f.left.p = {head.x + fx * (25 + 20 * a) - rx * 25, head.y + fy * (25 + 20 * a) - ry * 25, head.z - 70 + 95 * (a * 0.5f + 0.5f)};
    f.right.p = {head.x + fx * (30 + 30 * b) + rx * (20 + 35 * (1 - b)), head.y + fy * (30 + 30 * b) + ry * (20 + 35 * (1 - b)), head.z - 30 + 20 * b};
  }
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
  bool npc_died = false;
  std::string npc_pose_arg;  // --npc-pose "21 numbers": a real ragdoll pose (7 bones x/y/z) recorded from a host game
  std::map<std::string, int> state_counts;
  std::map<std::string, std::string> state_last;
  double last_state_report = 0;
  std::string hold_class;  // --hold-class <blueprint class path>: sent first, like the game does
  std::string hold_mesh;  // --hold <skeletal mesh path>: "hold" it in the right hand like a gun
  bool hold_sent = false;
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
    else if (a == "--npc-pose") npc_pose_arg = next();
    else if (a == "--hold") hold_mesh = next();
    else if (a == "--hold-class") hold_class = next();
    else if (a == "--at") { g_at_x = (float)atof(next().c_str()); g_at_y = (float)atof(next().c_str()); }
    else if (a == "--yaw") g_yaw = (float)atof(next().c_str());
    else if (a == "--no-hands") g_no_hands = true;
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

  if (mode == "relay-regions") {
    relay::RelayApi api(s.unity_project_id, s.unity_environment);
    relay::AuthToken tok;
    relay::ApiError err;
    std::vector<std::pair<std::string, std::string>> regs;
    if (!api.sign_in(tok, err) || !api.list_regions(tok, regs, err)) {
      printf("error: %s\n", err.message.c_str());
      return 1;
    }
    for (auto& [id, desc] : regs) BTT_INFO("region %s  %s", id.c_str(), desc.c_str());
    return 0;
  }
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
    {
      PoseFrame pf = scripted_pose(pattern, now - start);
      if (!hold_mesh.empty()) {
        pf.heldR = pf.right;  // the gun sits in the right hand, pointing where the hand points
        pf.flags |= 8;
        if (!hold_sent && ses.state() == SessionState::Connected) {
          // same lines the game sends: part count, then one line per mesh part (relative transform)
          NetEvent e;
          e.type = EventType::Game;
          if (!hold_class.empty()) { e.s1 = "heldclass R " + hold_class; ses.send_event(e); }
          e.s1 = "held R 1 1.000";
          ses.send_event(e);
          e.s1 = "heldpart R 1 " + hold_mesh + (hold_mesh.find("/SM_") != std::string::npos ? " S" : " K") + " 0 0 0 0 0 0 1 1.000";
          ses.send_event(e);
          hold_sent = true;
        }
      }
      ses.set_local_pose(pf);
    }
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
      // scripted NPC on the unreliable state channel (protocol 4): walk 0-15 s, crouch 15-25 s,
      // knocked down 25-35 s (lying pose), walk 35-45 s, dies at 45 s (reliable) and lies as a corpse
      double x = 300.0, y = 150.0 * std::sin(t * 0.3), vy = 45.0 * std::cos(t * 0.3);
      int stance = (t > 15 && t < 25) ? 1 : 0;
      bool down = t > 25 && t < 35, dead = t > 45;
      if (down || dead) { y = 150.0 * std::sin(25 * 0.3); vy = 0; }
      char pose[640];
      snprintf(pose, sizeof(pose), "%.0f %.0f 15 %.0f %.0f 15 %.0f %.0f 14 %.0f %.0f 10 %.0f %.0f 10 %.0f %.0f 8 %.0f %.0f 8 %.0f %.0f 12 %.0f %.0f 12 %.0f %.0f 10 %.0f %.0f 10",
               x, y, x + 25, y, x + 55, y, x + 30, y - 30, x + 30, y + 30, x - 80, y - 15, x - 80, y + 15,
               x + 20, y - 25, x + 20, y + 25, x - 40, y - 12, x - 40, y + 12);
      if (!npc_pose_arg.empty()) {
        // the recorded pose, moved so its pelvis is where the scripted NPC went down
        // any number of bones (x y z each), as many as the game's EX.POSE_BONES sends
        std::vector<float> v;
        {
          std::istringstream in(npc_pose_arg);
          float f;
          while (in >> f) v.push_back(f);
        }
        if (v.size() >= 3 && v.size() % 3 == 0) {
          double dx = x - v[0], dy = y - v[1];
          std::string out;
          for (size_t i = 0; i < v.size() / 3; ++i) {
            char one[64];
            snprintf(one, sizeof(one), "%s%.0f %.0f %.0f", i ? " " : "", v[i * 3] + dx, v[i * 3 + 1] + dy, v[i * 3 + 2]);
            out += one;
          }
          snprintf(pose, sizeof(pose), "%s", out.c_str());
        }
      }
      char b[900];
      if (dead) {
        if (!npc_died) {
          NetEvent d;
          d.type = EventType::Game;
          d.s1 = "npc 1 0 0 0 0 0 0 1";
          ses.send_event(d);
          npc_died = true;
          BTT_INFO("fake NPC died");
        }
        snprintf(b, sizeof(b), "npcbody 1 %s", pose);
      } else {
        snprintf(b, sizeof(b), "npc 1 %.1f %.1f 92.0 180.0 0 %.0f 0 %d %d%s%s", x, y, vy, stance, down ? 1 : 0, down ? " " : "", down ? pose : "");
      }
      ses.send_state("n1", b);
    }
    if (!game_line.empty() && ses.state() == SessionState::Connected && now - last_game > 10.0 && now - start > 15.0) {
      last_game = now;
      NetEvent e;
      e.type = EventType::Game;
      e.s1 = game_line;
      ses.send_event(e);
      BTT_INFO("sent game event: %s", game_line.c_str());
    }
    if (now - last_state_report > 3.0 && !state_counts.empty()) {
      last_state_report = now;
      for (auto& kv : state_counts) BTT_INFO("peer state: %d x %s, latest: %s", kv.second, kv.first.c_str(), state_last[kv.first].c_str());
      state_counts.clear();
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
        std::string kind = e.s1.substr(0, e.s1.find(' '));
        if (kind == "npc" || kind == "npcbody" || kind == "npcdrag" || kind == "prop") {
          ++state_counts[kind];  // high-rate state: summarised below instead of one line each
          state_last[kind] = e.s1;
        } else {
          BTT_INFO("peer game event: %s", e.s1.c_str());
        }
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
