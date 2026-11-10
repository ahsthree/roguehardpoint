// roguehardpoint.cpp - Timed hardpoint "battle for control" for BZFS Rogue wars
//
// Build (adjust the include path to your bzflag source tree):
//   g++ -shared -fPIC -I/path/to/bzflag/include -o roguehardpoint.so roguehardpoint.cpp
//
// Run:
//   bzfs -loadplugin ./roguehardpoint.so -world mymap.bzw \
//        -set _rogueHardPointInterval 300 -set _rogueHardPointTime 60 \
//        -set _rogueHardPointWarning 10
//
// Uses the SAME map object as hardpoint.cpp (one per hardpoint):
//   hardpoint
//     name red
//     pos 0 0 0
//     size 30 30 30      # half-extents in x/y, height in z (same as a box)
//     rot 45             # optional, degrees counterclockwise (also: rotation)
//   end
//
// NOTE: both plugins register the HARDPOINT map object, so load only ONE of
// hardpoint.so / roguehardpoint.so on a given server.
//
// How it plays:
//   * Every _rogueHardPointInterval seconds (start to start) a RANDOM hardpoint
//     is picked (never the same one twice in a row). The server announces it by
//     name and the sky-shot star starts firing 50 units above it.
//   * A visible countdown (10, 9, 8 ... 1, one number per line) runs for
//     _rogueHardPointWarning seconds, then the hardpoint goes live.
//   * It stays live for _rogueHardPointTime seconds. Each second, exactly ONE
//     living rogue tank inside = +1 personal score. Two or more = contested,
//     nobody scores. Nobody inside = nobody scores. The clock never pauses.
//   * Nothing starts until _rogueHardPointMinPlayers rogue players are on the
//     server, so one player can't farm points alone.
//
// BZDB variables:
//   _rogueHardPointInterval   seconds between activations, start to start   default 300
//   _rogueHardPointTime       seconds a hardpoint stays live                 default 60
//   _rogueHardPointWarning    countdown seconds before it goes live (0=off)  default 10
//   _rogueHardPointFirstDelay seconds after enough players join until the
//                             first activation                               default 30
//   _rogueHardPointMinPlayers rogue players needed to run                    default 2
//   _rogueHardPointStarSpokes shots in the marker star                       default 3
//   _rogueHardPointStarInterval seconds between marker volleys               default 1.0

#include "bzfsAPI.h"

#include <vector>
#include <string>
#include <sstream>
#include <map>
#include <random>
#include <cmath>

struct Hardpoint {
    std::string name;
    float pos[3] = {0, 0, 0};
    float size[3] = {10, 10, 10};   // half-extents x/y, height z (box convention)
    float cosR = 1.0f, sinR = 0.0f; // rotation about z, set from `rot`
};

static std::vector<Hardpoint> g_points;

// ---------------------------------------------------------------- map object
// Identical parsing to hardpoint.cpp so the same maps work with both plugins.

class HardpointMapHandler : public bz_CustomMapObjectHandler {
public:
    bool MapObject(bz_ApiString object, bz_CustomMapObjectInfo *data) override {
        if (!data) return false;

        Hardpoint hp;
        for (unsigned int i = 0; i < data->data.size(); i++) {
            std::istringstream ss(data->data.get(i).c_str());
            std::string key;
            ss >> key;
            if (key == "name") {
                std::getline(ss >> std::ws, hp.name);   // allow multi-word names
                while (!hp.name.empty() &&
                       (hp.name.back() == '\r' || hp.name.back() == ' '))
                    hp.name.pop_back();
            }
            else if (key == "pos")  ss >> hp.pos[0] >> hp.pos[1] >> hp.pos[2];
            else if (key == "size") ss >> hp.size[0] >> hp.size[1] >> hp.size[2];
            else if (key == "rot" || key == "rotation") {
                float deg = 0;
                ss >> deg;
                float r = deg * 3.14159265f / 180.0f;
                hp.cosR = cosf(r);
                hp.sinR = sinf(r);
            }
        }
        if (hp.name.empty())
            hp.name = std::to_string(g_points.size() + 1);
        g_points.push_back(hp);
        return true;
    }
};

// -------------------------------------------------------------------- plugin

class RogueHardpointPlugin : public bz_Plugin {
public:
    const char *Name() override { return "Rogue Hardpoint"; }
    void Init(const char *config) override;
    void Cleanup() override;
    void Event(bz_EventData *eventData) override;

private:
    enum State { WAITING, WARNING, ACTIVE };

    HardpointMapHandler mapHandler;
    std::mt19937 rng;

    State  state = WAITING;
    bool   armed = false;       // true once enough players have been seen
    int    active = -1;         // index into g_points
    int    lastPick = -1;       // previous hardpoint, to avoid immediate repeats

    double cycleStart = 0;      // when the current activation was announced
    double nextStart = 0;       // when the next activation is announced
    double warnEnd = 0;         // when the countdown ends
    double activeEnd = 0;       // when the live period ends
    double lastStep = 0;
    double nextPulse = 0;
    double pointAccum = 0;      // progress toward the next point
    int    lastCount = -1;      // last countdown number shown
    int    lastHolder = -1;     // -1 empty, -2 contested, otherwise player id
    float  spin = 0;

    std::map<int, int> earned;  // points earned by player this activation

    bool enoughPlayers();
    bool inside(const Hardpoint &hp, const float *p);
    int  tanksInZone(int &holderId);
    void pickHardpoint();
    void startWarning(double now);
    void goLive(double now);
    void endActive(double now);
    void cancel(const char *why);
    void award(int playerId);
    void pulseStar();
};

BZ_PLUGIN(RogueHardpointPlugin)

static void ensureVar(const char *name, double value) {
    if (!bz_BZDBItemExists(name))
        bz_setBZDBDouble(name, value);
}

void RogueHardpointPlugin::Init(const char *) {
    bz_registerCustomMapObject("HARDPOINT", &mapHandler);

    ensureVar("_rogueHardPointInterval",     300.0);
    ensureVar("_rogueHardPointTime",          60.0);
    ensureVar("_rogueHardPointWarning",       10.0);
    ensureVar("_rogueHardPointFirstDelay",    30.0);
    ensureVar("_rogueHardPointMinPlayers",     2.0);
    ensureVar("_rogueHardPointStarSpokes",     3.0);
    ensureVar("_rogueHardPointStarInterval",   1.0);

    std::random_device rd;
    rng.seed(rd());

    Register(bz_eTickEvent);
}

void RogueHardpointPlugin::Cleanup() {
    Flush();
    bz_removeCustomMapObject("HARDPOINT");
    g_points.clear();
}

// Enough rogue players on the server to run the mode.
bool RogueHardpointPlugin::enoughPlayers() {
    int minPlayers = (int)bz_getBZDBDouble("_rogueHardPointMinPlayers");
    if (minPlayers < 1) minPlayers = 1;
    return bz_getTeamCount(eRogueTeam) >= minPlayers;
}

bool RogueHardpointPlugin::inside(const Hardpoint &hp, const float *p) {
    // move the point into the hardpoint's own (unrotated) frame
    float dx = p[0] - hp.pos[0];
    float dy = p[1] - hp.pos[1];
    float lx =  dx * hp.cosR + dy * hp.sinR;
    float ly = -dx * hp.sinR + dy * hp.cosR;
    return fabsf(lx) <= hp.size[0] &&
           fabsf(ly) <= hp.size[1] &&
           p[2] >= hp.pos[2] && p[2] <= hp.pos[2] + hp.size[2];
}

// Counts living rogue tanks inside the active hardpoint. If exactly one is
// inside, holderId is set to that player's id.
int RogueHardpointPlugin::tanksInZone(int &holderId) {
    holderId = -1;
    int count = 0;
    const Hardpoint &hp = g_points[active];

    bz_APIIntList *players = bz_getPlayerIndexList();
    if (!players) return 0;

    for (unsigned int i = 0; i < players->size(); i++) {
        bz_BasePlayerRecord *pr = bz_getPlayerByIndex(players->get(i));
        if (!pr) continue;
        if (pr->spawned && pr->team == eRogueTeam &&
            inside(hp, pr->lastKnownState.pos)) {
            count++;
            holderId = pr->playerID;
        }
        bz_freePlayerRecord(pr);
    }
    bz_deleteIntList(players);

    if (count != 1) holderId = -1;
    return count;
}

// Random hardpoint, never the same one twice in a row (when there are 2+).
void RogueHardpointPlugin::pickHardpoint() {
    const int n = (int)g_points.size();
    int pick = (int)(rng() % n);
    if (n > 1 && pick == lastPick)
        pick = (pick + 1 + (int)(rng() % (n - 1))) % n;
    active = pick;
    lastPick = pick;
}

void RogueHardpointPlugin::startWarning(double now) {
    cycleStart = now;
    pickHardpoint();

    const double warn = bz_getBZDBDouble("_rogueHardPointWarning");
    nextPulse = now;    // star starts right away so players can see where it is

    if (warn > 0) {
        state = WARNING;
        warnEnd = now + warn;
        lastCount = -1;
        bz_sendTextMessagef(BZ_SERVER, BZ_ALLUSERS,
            "Go to the %s hardpoint! It goes live in...",
            g_points[active].name.c_str());
    } else {
        bz_sendTextMessagef(BZ_SERVER, BZ_ALLUSERS,
            "Go to the %s hardpoint!", g_points[active].name.c_str());
        goLive(now);
    }
}

void RogueHardpointPlugin::goLive(double now) {
    state = ACTIVE;
    const double t = bz_getBZDBDouble("_rogueHardPointTime");
    activeEnd = now + t;
    lastStep = now;
    pointAccum = 0;
    lastHolder = -1;
    earned.clear();

    bz_sendTextMessagef(BZ_SERVER, BZ_ALLUSERS,
        "The %s hardpoint is LIVE! Hold it alone for 1 point per second (%.0f seconds).",
        g_points[active].name.c_str(), t);
}

void RogueHardpointPlugin::endActive(double now) {
    state = WAITING;

    int bestId = -1, best = 0;
    for (const auto &e : earned) {
        if (e.second > best) { best = e.second; bestId = e.first; }
    }

    if (bestId >= 0) {
        const char *cs = bz_getPlayerCallsign(bestId);
        bz_sendTextMessagef(BZ_SERVER, BZ_ALLUSERS,
            "Hardpoint over! %s earned the most: %d point%s.",
            cs ? cs : "Unknown", best, best == 1 ? "" : "s");
    } else {
        bz_sendTextMessage(BZ_SERVER, BZ_ALLUSERS, "Hardpoint over - nobody scored.");
    }

    earned.clear();
    lastHolder = -1;

    // Start-to-start interval, but never sooner than a second from now.
    const double interval = bz_getBZDBDouble("_rogueHardPointInterval");
    nextStart = cycleStart + interval;
    if (nextStart < now + 1.0) nextStart = now + 1.0;
}

void RogueHardpointPlugin::cancel(const char *why) {
    state = WAITING;
    armed = false;
    earned.clear();
    lastHolder = -1;
    bz_sendTextMessage(BZ_SERVER, BZ_ALLUSERS, why);
}

// +1 personal score for the holder.
void RogueHardpointPlugin::award(int playerId) {
    bz_setPlayerWins(playerId, bz_getPlayerWins(playerId) + 1);
    earned[playerId]++;
}

// Marker star: N shots (default 3, i.e. 120 degrees apart) flying level toward
// the center from 100 units out, 50 units above the hardpoint. Fired in the
// rogue team's color; swap eRogueTeam below for another team if you want the
// star to look different from player shots.
void RogueHardpointPlugin::pulseStar() {
    const Hardpoint &hp = g_points[active];

    int spokes = (int)bz_getBZDBDouble("_rogueHardPointStarSpokes");
    if (spokes < 1) spokes = 1;

    const float dist   = 100.0f;
    const float height = 50.0f;
    const float pi     = 3.14159265f;
    const float life   = dist / (float)bz_getBZDBDouble("_shotSpeed");

    for (int i = 0; i < spokes; i++) {
        float a = spin + i * (2.0f * pi / spokes);
        float p[3] = { hp.pos[0] + dist * cosf(a),
                       hp.pos[1] + dist * sinf(a),
                       hp.pos[2] + height };
        bz_fireWorldWep("PZ", life, BZ_SERVER, p, 0.0f, a + pi, 0, 0.0f, eRogueTeam);
    }
    spin += 0.1f;
}

void RogueHardpointPlugin::Event(bz_EventData *eventData) {
    if (!eventData || eventData->eventType != bz_eTickEvent || g_points.empty())
        return;

    const double now = bz_getCurrentTime();

    // ---- need enough rogue players; cancel anything running if they leave
    if (!enoughPlayers()) {
        if (state != WAITING)
            cancel("Rogue hardpoint cancelled - not enough players.");
        armed = false;
        return;
    }
    if (!armed) {
        armed = true;
        nextStart = now + bz_getBZDBDouble("_rogueHardPointFirstDelay");
    }

    switch (state) {

    case WAITING:
        if (now >= nextStart)
            startWarning(now);
        break;

    case WARNING: {
        if (now >= nextPulse) {
            nextPulse = now + bz_getBZDBDouble("_rogueHardPointStarInterval");
            pulseStar();
        }

        // countdown: one number per line (10, 9, 8 ... 1)
        int n = (int)ceil(warnEnd - now);
        if (n >= 1 && n != lastCount) {
            lastCount = n;
            bz_sendTextMessagef(BZ_SERVER, BZ_ALLUSERS, "%d", n);
        }

        if (now >= warnEnd)
            goLive(now);
        break;
    }

    case ACTIVE: {
        if (now >= nextPulse) {
            nextPulse = now + bz_getBZDBDouble("_rogueHardPointStarInterval");
            pulseStar();
        }

        double dt = now - lastStep;
        if (dt >= 0.1) {                       // game step: 10 per second
            if (dt > 1.0) dt = 1.0;
            lastStep = now;

            int holder = -1;
            int count = tanksInZone(holder);

            if (count >= 2) {                  // contested: nobody scores
                if (lastHolder != -2) {
                    bz_sendTextMessage(BZ_SERVER, BZ_ALLUSERS, "Hardpoint contested!");
                    lastHolder = -2;
                }
                pointAccum = 0;
            } else if (count == 1) {           // one tank alone: 1 point/second
                if (holder != lastHolder) {
                    const char *cs = bz_getPlayerCallsign(holder);
                    bz_sendTextMessagef(BZ_SERVER, BZ_ALLUSERS,
                        "%s is holding the hardpoint", cs ? cs : "Unknown");
                    lastHolder = holder;
                    pointAccum = 0;            // no partial credit across players
                }
                pointAccum += dt;
                while (pointAccum >= 1.0) {
                    pointAccum -= 1.0;
                    award(holder);
                }
            } else {                           // empty
                lastHolder = -1;
                pointAccum = 0;
            }
        }

        if (now >= activeEnd)
            endActive(now);
        break;
    }

    }
}
