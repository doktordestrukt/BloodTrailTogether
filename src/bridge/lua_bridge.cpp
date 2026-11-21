// Text protocol spoken with Scripts/main.lua. One request per game frame; one reply.
//
// Request lines (Lua -> DLL):
//   H <lua-mod-version>                         handshake (first frame after connecting)
//   P hx hy hz hqx hqy hqz hqw  lx .. lqw  rx .. rqw  flags    local VR pose, world space (cm)
//   M <map package path>                        current map, e.g. /Game/Maps/Sandbox/Map_Sandbox
//   S <asset path> x y z volume pitch           a player-originated sound to share
//   K <action>                                  hotkey: toggle_window | sim | disconnect
//   W <key> <value>                             host-authoritative world-state change
//   D <text>                                    log line from Lua
// Reply lines (DLL -> Lua):
//   CFG <shareRadiusCm> <devMode 0/1>           (only in reply to H)
//   ST <state> <role> <quality 0-3> <rttMs> <lossPct> <simulated 0/1> <code|-> <peerName|->
//   RP - | RP hx hy hz hqx hqy hqz hqw lx .. lqw rx .. rqw flags     remote pose (interpolated)
//   TR <map package path>                       travel there (client follows host)
//   SN <asset path> x y z volume pitch          play this sound at that location
//   NT <text>                                   show a short notification in VR
//   WS <key> <value>                            apply world-state change
#include <cstdio>
#include <sstream>
#include <vector>

#include "bridge/app.h"
#include "core/log.h"
#include "core/util.h"

namespace btt {

namespace {

bool parse_floats(std::istringstream& in, float* out, int n) {
  for (int i = 0; i < n; ++i)
    if (!(in >> out[i])) return false;
  return true;
}

void put_xform(std::string& s, const Xform& x) {
  char b[160];
  snprintf(b, sizeof(b), " %.2f %.2f %.2f %.5f %.5f %.5f %.5f", x.p.x, x.p.y, x.p.z, x.q.x, x.q.y, x.q.z, x.q.w);
  s += b;
}

}  // namespace

std::string lua_handle_request(const std::string& req) {
  App& a = app();
  NetEngine* eng = a.engine.get();
  std::string out;
  out.reserve(512);
  if (!eng) return "ST Starting - 0 0 0 0 - -\nRP -\n";

  a.stat_frames++;
  std::istringstream lines(req);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.size() < 2) continue;
    char kind = line[0];
    std::string rest = line.substr(2);
    std::istringstream in(rest);
    switch (kind) {
      case 'H': {
        a.lua_version = rest;
        BTT_INFO("bridge: Lua mod %s handshake", rest.c_str());
        char b[96];
        snprintf(b, sizeof(b), "CFG %.1f %d\n", a.settings.share_sound_radius_cm, a.settings.dev_mode ? 1 : 0);
        out += b;
        break;
      }
      case 'P': {
        float v[22];
        if (!parse_floats(in, v, 21)) break;
        int flags = 0;
        in >> flags;
        PoseFrame f;
        Xform* xs[3] = {&f.head, &f.left, &f.right};
        for (int i = 0; i < 3; ++i) {
          float* p = v + i * 7;
          xs[i]->p = {p[0], p[1], p[2]};
          xs[i]->q = qnormalize({p[3], p[4], p[5], p[6]});
        }
        f.flags = (uint8_t)flags;
        eng->set_local_pose(f);
        a.stat_poses++;
        if (flags & 3) a.stat_hands++;
        break;
      }
      case 'M':
        if (!rest.empty()) eng->set_local_map(rest);
        break;
      case 'S': {
        std::string path;
        float v[5];
        if (in >> path && parse_floats(in, v, 5)) {
          a.stat_sounds_out++;
          eng->report_sound(path, {v[0], v[1], v[2]}, v[3], v[4]);
        }
        break;
      }
      case 'K':
        if (rest == "toggle_window") control_window_toggle();
        else if (rest == "sim") eng->start_simulated_peer(SimMode::Mirror);
        else if (rest == "disconnect") eng->disconnect();
        break;
      case 'W': {
        std::string key, value;
        in >> key;
        std::getline(in, value);
        if (!value.empty() && value[0] == ' ') value.erase(0, 1);
        eng->send_world(key, value);
        break;
      }
      case 'D':
        BTT_INFO("lua: %s", rest.c_str());
        break;
      default:
        break;
    }
  }

  EngineSnapshot s = eng->snapshot();
  char st[512];
  snprintf(st, sizeof(st), "ST %s %s %d %.0f %.1f %d %s %s\n", s.state.c_str(), s.role.empty() ? "-" : s.role.c_str(),
           s.quality, s.rtt_ms, s.loss_pct, s.simulated ? 1 : 0, s.join_code.empty() ? "-" : s.join_code.c_str(),
           token_safe(s.peer_name).c_str());
  out += st;

  PoseFrame rp;
  if (eng->remote_pose(rp)) {
    a.stat_remote++;
    out += "RP";
    put_xform(out, rp.head);
    put_xform(out, rp.left);
    put_xform(out, rp.right);
    out += " " + std::to_string(rp.flags) + "\n";
  } else {
    out += "RP -\n";
  }

  std::vector<GameCommand> cmds;
  eng->pop_commands(cmds);
  for (auto& c : cmds) {
    char b[1400];
    switch (c.kind) {
      case GameCommand::Travel:
        a.stat_travel++;
        out += "TR " + c.a + "\n";
        break;
      case GameCommand::Sound:
        a.stat_sounds_in++;
        snprintf(b, sizeof(b), "SN %s %.1f %.1f %.1f %.3f %.3f\n", token_safe(c.a).c_str(), c.v.x, c.v.y, c.v.z, c.f0, c.f1);
        out += b;
        break;
      case GameCommand::Notify:
        out += "NT " + c.a + "\n";
        break;
      case GameCommand::Ui:
        if (c.a.rfind("world ", 0) == 0) out += "WS " + c.a.substr(6) + "\n";
        break;
    }
  }
  return out;
}

}  // namespace btt
