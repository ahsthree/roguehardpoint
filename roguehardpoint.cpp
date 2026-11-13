// roguehardpoint.cpp - Timed hardpoint "battle for control" for BZFS Rogue wars
//
// Build (adjust the include path to your bzflag source tree):
//   g++ -shared -fPIC -I/path/to/bzflag/include -o roguehardpoint.so roguehardpoint.cpp
//
// Run:
//   bzfs -loadplugin ./roguehardpoint.so -world mymap.bzw
//        -set _rogueHardPointInterval 300 -set _rogueHardPointTime 60
//        -set _rogueHardPointWarning 10
//
// Uses the SAME map object as hardpoint.cpp (one per hardpoint):
//   hardpoint
//     name red
//     pos 0 0 0
//     size 30 30 30      # half-extents in x/y, height in z (same as a box)
//     rot 45             # optional, degrees counterclockwise (also: rotation)
//     spawn 70 0         # optional, repeatable: spawn spot x y [z], world coordinates
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
//   * Once live, the _rogueHardPointTime-second HOLD COUNTDOWN does not start until
//     a tank enters. It runs only while exactly ONE living rogue tank is inside;
//     that tank earns personal score at a rate that scales with the number of
//     rogue players online (1 point/second at 10+ players, 1 point per 10 seconds
//     at 2 players). Two or more tanks = contested: countdown paused, nobody
//     scores. Empty = paused too. The last tank standing runs it to completion.
//     If nobody holds it for _rogueHardPointIdleTime seconds, it closes.
//   * While a hardpoint is announced or live, respawning players can be placed
//     near/inside it for a chaotic fight (great for a hard-to-reach upper deck).
//     Every spot is checked for safety:
//       1. `spawn x y [z]` lines in the hardpoint object win: one is picked at
//          random and the player faces the hardpoint. You (or your map tool) pick
//          these spots, so they are safe. z defaults to the hardpoint's z.
//       2. Otherwise the plugin asks the server for spawn spots (bz_getStandardSpawn,
//          which already checks buildings, ground, enemies and dangerous flags) and
//          keeps the first one within _rogueHardPointSpawnRadius of the zone and
//          within _rogueHardPointSpawnZTol of its height.
//       3. If none qualifies, the server's normal spawn stands.
//     Each player gets _rogueHardPointMaxSpawns tries per hardpoint (default 3).
//     They are told how many they have used, and once they are out they respawn
//     normally, so the zone can't turn into endless chaos.
//   * Nothing starts until _rogueHardPointMinPlayers rogue players are on the
//     server, so one player can't farm points alone.
//
// BZDB variables:
//   _rogueHardPointInterval   seconds between activations, start to start   default 300
//   _rogueHardPointTime       seconds of holding needed to finish; starts when a
//                             tank enters, paused while contested or empty   default 60
//   _rogueHardPointWarning    countdown seconds before it goes live (0=off)  default 10
//   _rogueHardPointFirstDelay seconds after enough players join until the
//                             first activation                               default 30
//   _rogueHardPointMinPlayers rogue players needed to run                    default 2
//   _rogueHardPointFullPlayers rogue players at/above which the rate is 1 point
//                             per second                                     default 10
//   _rogueHardPointSlowSeconds seconds per point at the minimum player count   default 10
//   _rogueHardPointStarSpokes shots in the marker star                       default 3
//   _rogueHardPointIdleTime   close the hardpoint if nobody has held it alone for
//                             this many seconds (0 = never)                  default 120
//   _rogueHardPointMaxSpawns  hardpoint spawn tries per player per hardpoint
//                             (0 = unlimited)                                default 3
//   _rogueHardPointSpawnChance percent of respawns placed near the hardpoint while
//                             it is announced/live (0 = off)                 default 100
//   _rogueHardPointSpawnRadius max distance from the zone for server-picked
//                             spots (0 = never use near-hardpoint spawns)    default 100
//   _rogueHardPointSpawnTries server spots tried before giving up            default 60
//   _rogueHardPointSpawnZTol  server-picked spots must be within this height of
//                             the hardpoint's pos z (set pos z at deck top)  default 4
//   _rogueHardPointStarInterval seconds between marker volleys               default 1.0

#include "bzfsAPI.h"

#include <vector>
#include <string>
#include <sstream>
#include <map>
#include <random>
#include <cmath>

// A map-placed spawn spot (spawn x y [z]); z defaults to the hardpoint's z.
struct SpawnLine { float p[3]; bool hasZ; };

struct Hardpoint {
    std::string name;
    float pos[3] = {0, 0, 0};
    float size[3] = {10, 10, 10};   // half-extents x/y, height z (box convention)
    std::vector<SpawnLine> spawns;  // optional map-placed spawn spots
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
            else if (key == "spawn") {
                SpawnLine sl = {{0, 0, 0}, false};
                if (ss >> sl.p[0] >> sl.p[1]) {
                    sl.hasZ = static_cast<bool>(ss >> sl.p[2]);
                    hp.spawns.push_back(sl);
                }
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
    double holdLeft = 0;        // seconds of holding left (runs only while one tank holds it)
    double idle = 0;            // seconds the hardpoint has gone without a lone holder
    bool   holdStarted = false; // a tank has entered, so the hold countdown has begun
    double lastStep = 0;
    double nextPulse = 0;
    double pointAccum = 0;      // progress toward the next point
    int    accumOwner = -1;     // player the progress belongs to
    int    lastCount = -1;      // last countdown number shown
    int    lastHolder = -1;     // -1 empty, -2 contested, otherwise player id
    float  spin = 0;

    std::map<int, int> earned;  // points earned by player this activation
    std::map<int, int> spawnsUsed;  // hardpoint spawn tries used by player this activation

    float distToZone(const Hardpoint &hp, const float *p);
    bool pickSpawn(size_t h, int playerId, float *pos, float *rot);
    void handleSpawn(bz_GetPlayerSpawnPosEventData_V1 *d);
    bool enoughPlayers();
    bool inside(const Hardpoint &hp, const float *p);
    int  tanksInZone(int &holderId);
    double scoreRate();
    void pickHardpoint();
    void startWarning(double now);
    void goLive(double now);
    void endActive(double now, bool idleClose);
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
    ensureVar("_rogueHardPointFullPlayers",   10.0);
    ensureVar("_rogueHardPointSlowSeconds",   10.0);
    ensureVar("_rogueHardPointSpawnChance",  100.0);
    ensureVar("_rogueHardPointSpawnRadius",  100.0);
    ensureVar("_rogueHardPointSpawnTries",    60.0);
    ensureVar("_rogueHardPointSpawnZTol",      4.0);
    ensureVar("_rogueHardPointIdleTime",     120.0);
    ensureVar("_rogueHardPointMaxSpawns",      3.0);
    ensureVar("_rogueHardPointStarSpokes",     3.0);
    ensureVar("_rogueHardPointStarInterval",   1.0);

    std::random_device rd;
    rng.seed(rd());

    Register(bz_eTickEvent);
    Register(bz_eGetPlayerSpawnPosEvent);
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

// Points per second for the holder, based on rogue players online.
// Min players (default 2)  -> 1 point per _rogueHardPointSlowSeconds (default 10)
// Full players (default 10) or more -> 1 point per second
// In between the rate rises in a straight line, so the maximum points over a
// hardpoint also rise linearly (default: 6 at 2 players up to 60 at 10+).
double RogueHardpointPlugin::scoreRate() {
    int minP  = (int)bz_getBZDBDouble("_rogueHardPointMinPlayers");
    int fullP = (int)bz_getBZDBDouble("_rogueHardPointFullPlayers");
    double slow = bz_getBZDBDouble("_rogueHardPointSlowSeconds");
    if (minP < 1) minP = 1;
    if (slow < 1.0) slow = 1.0;

    const int players = bz_getTeamCount(eRogueTeam);
    const double lo = 1.0 / slow;                 // rate at the minimum

    if (fullP <= minP || players >= fullP) return 1.0;
    if (players <= minP) return lo;

    const double t = (double)(players - minP) / (double)(fullP - minP);
    return lo + t * (1.0 - lo);
}

// Horizontal distance from p to the hardpoint's footprint (0 if inside it).
float RogueHardpointPlugin::distToZone(const Hardpoint &hp, const float *p) {
    const float dx = p[0] - hp.pos[0];
    const float dy = p[1] - hp.pos[1];
    const float lx =  dx * hp.cosR + dy * hp.sinR;
    const float ly = -dx * hp.sinR + dy * hp.cosR;
    float ox = fabsf(lx) - hp.size[0];
    float oy = fabsf(ly) - hp.size[1];
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;
    return sqrtf(ox * ox + oy * oy);
}

// Pick a safe spawn for playerId near hardpoint h. Returns false if there is
// none, in which case the server's normal spawn is used.
//
// 1. `spawn` lines in the map win: random pick, facing the hardpoint. They were
//    placed by you / your map tool, so they are safe.
// 2. Otherwise ask the server (bz_getStandardSpawn) for spots. The server only
//    returns spots it has already checked (inside buildings, ground, enemies,
//    dangerous flags), and we keep the first one close enough to the zone and
//    at the zone's height. Costs up to _rogueHardPointSpawnTries server calls.
bool RogueHardpointPlugin::pickSpawn(size_t h, int playerId, float *pos, float *rot) {
    const Hardpoint &hp = g_points[h];

    if (!hp.spawns.empty()) {
        const SpawnLine &sl = hp.spawns[rng() % hp.spawns.size()];
        pos[0] = sl.p[0];
        pos[1] = sl.p[1];
        pos[2] = sl.hasZ ? sl.p[2] : hp.pos[2];
        *rot = atan2f(hp.pos[1] - pos[1], hp.pos[0] - pos[0]);   // face the hardpoint
        return true;
    }

    const double radius = bz_getBZDBDouble("_rogueHardPointSpawnRadius");
    if (radius <= 0) return false;

    int tries = (int)bz_getBZDBDouble("_rogueHardPointSpawnTries");
    if (tries < 1) tries = 1;
    const float ztol = (float)bz_getBZDBDouble("_rogueHardPointSpawnZTol");

    for (int i = 0; i < tries; i++) {
        float p[3], r = 0;
        if (!bz_getStandardSpawn(playerId, p, &r)) break;
        if (distToZone(hp, p) > radius) continue;
        if (fabsf(p[2] - hp.pos[2]) > ztol) continue;
        pos[0] = p[0];
        pos[1] = p[1];
        pos[2] = p[2];
        *rot = r;                                  // keep the server's chosen facing
        return true;
    }
    return false;
}

// Server asks where a player should spawn: while a hardpoint is announced or
// live, put (a percentage of) respawns near it.
void RogueHardpointPlugin::handleSpawn(bz_GetPlayerSpawnPosEventData_V1 *d) {
    if (state != WARNING && state != ACTIVE) return;
    if (d->team != eRogueTeam) return;

    const int chance = (int)bz_getBZDBDouble("_rogueHardPointSpawnChance");
    if (chance <= 0) return;
    if (chance < 100 && (int)(rng() % 100) >= chance) return;

    // Each player gets a limited number of hardpoint spawns per hardpoint.
    const int maxSpawns = (int)bz_getBZDBDouble("_rogueHardPointMaxSpawns");
    int &used = spawnsUsed[d->playerID];
    if (maxSpawns > 0 && used >= maxSpawns) {
        if (used == maxSpawns) {        // tell them once, then stay quiet
            used++;
            bz_sendTextMessage(BZ_SERVER, d->playerID,
                "No hardpoint tries left - you respawn normally.");
        }
        return;
    }

    float pos[3], rot = 0;
    if (!pickSpawn((size_t)active, d->playerID, pos, &rot)) return;

    used++;                             // only a real hardpoint spawn costs a try
    if (maxSpawns > 0) {
        if (used >= maxSpawns)
            bz_sendTextMessagef(BZ_SERVER, d->playerID,
                "Hardpoint try %d of %d - LAST TRY! After you die you respawn normally.",
                used, maxSpawns);
        else
            bz_sendTextMessagef(BZ_SERVER, d->playerID,
                "Hardpoint try %d of %d (%d left)", used, maxSpawns, maxSpawns - used);
    }

    d->pos[0] = pos[0];
    d->pos[1] = pos[1];
    d->pos[2] = pos[2];
    d->rot = rot;
    d->handled = true;
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
    spawnsUsed.clear();
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
    holdLeft = t;
    idle = 0;
    holdStarted = false;
    lastStep = now;
    pointAccum = 0;
    accumOwner = -1;
    lastHolder = -1;
    earned.clear();

    bz_sendTextMessagef(BZ_SERVER, BZ_ALLUSERS,
        "The %s hardpoint is LIVE! The %.0f second countdown starts when a tank enters. Hold it alone to score: 1 point every %.1fs.",
        g_points[active].name.c_str(), t, 1.0 / scoreRate());
}

void RogueHardpointPlugin::endActive(double now, bool idleClose) {
    state = WAITING;

    if (idleClose)
        bz_sendTextMessage(BZ_SERVER, BZ_ALLUSERS,
            "Nobody held the hardpoint long enough - closing it.");

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

// +1 personal score for the holder (called each time a whole point is earned).
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
    if (!eventData || g_points.empty()) return;

    if (eventData->eventType == bz_eGetPlayerSpawnPosEvent) {
        handleSpawn((bz_GetPlayerSpawnPosEventData_V1 *)eventData);
        return;
    }
    if (eventData->eventType != bz_eTickEvent) return;

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

            if (count >= 2) {                  // contested: countdown paused, nobody scores
                if (lastHolder != -2) {
                    if (holdStarted)
                        bz_sendTextMessagef(BZ_SERVER, BZ_ALLUSERS,
                            "Hardpoint contested! Countdown paused with %.0fs left.",
                            ceil(holdLeft));
                    else
                        bz_sendTextMessage(BZ_SERVER, BZ_ALLUSERS, "Hardpoint contested!");
                    lastHolder = -2;
                }
                idle += dt;               // progress toward the next point is kept
            } else if (count == 1) {           // one tank alone: countdown runs, it scores
                if (holder != lastHolder) {
                    const char *cs = bz_getPlayerCallsign(holder);
                    if (!holdStarted) {
                        holdStarted = true;
                        bz_sendTextMessagef(BZ_SERVER, BZ_ALLUSERS,
                            "%s entered the hardpoint - countdown started: %.0f seconds of holding to go!",
                            cs ? cs : "Unknown", holdLeft);
                    } else {
                        bz_sendTextMessagef(BZ_SERVER, BZ_ALLUSERS,
                            "%s is holding the hardpoint (%.0fs left)",
                            cs ? cs : "Unknown", ceil(holdLeft));
                    }
                    lastHolder = holder;
                }
                if (holder != accumOwner) {    // no partial credit across players
                    accumOwner = holder;
                    pointAccum = 0;
                }
                idle = 0;
                const double used = (dt < holdLeft) ? dt : holdLeft;
                holdLeft -= used;
                pointAccum += used * scoreRate();
                while (pointAccum >= 1.0 - 1e-6) {
                    pointAccum -= 1.0;
                    award(holder);
                }
            } else {                           // empty: countdown paused
                if (lastHolder != -1 && holdStarted)
                    bz_sendTextMessagef(BZ_SERVER, BZ_ALLUSERS,
                        "Hardpoint empty. Countdown paused with %.0fs left.", ceil(holdLeft));
                lastHolder = -1;
                idle += dt;
            }
        }

        if (holdStarted && holdLeft <= 1e-9) {
            endActive(now, false);             // the countdown ran to completion
        } else {
            const double idleLimit = bz_getBZDBDouble("_rogueHardPointIdleTime");
            if (idleLimit > 0 && idle >= idleLimit)
                endActive(now, true);
        }
        break;
    }

    }
}
