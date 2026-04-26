#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <iostream>
#include <fstream>
#include <chrono>
#include <algorithm>
#include <sstream>
#include <cmath>
#include <cctype>

#define PORT 55001
#define MAX_EVTS 64
#define DB_FILE "users.json"

using Clock = std::chrono::steady_clock;

static const int   COST[5] = { 5,   7,  10,  12,  15 };
static const float SPEED[5] = { 65.f,55.f,45.f,60.f,35.f };
static const float SPAWN_CD[5] = { 2.f, 3.f, 4.5f, 6.f, 8.f };

static const float LANE_T_DMG = 4.f;
static const float LANE_T_RANGE = 80.f;
static const float LANE_T_RATE = 1.0f;

static const float MAIN_T_DMG[3] = { 5.f,  9.f, 14.f };
static const float MAIN_T_RANGE[3] = { 90.f,125.f,165.f };
static const float MAIN_T_RATE[3] = { 0.8f, 1.3f, 1.9f };
static const float UPGRADE_COST[3] = { 20.f, 35.f, 55.f };

// User database
// name - password (plain text for a LAN game)
static std::unordered_map<std::string, std::string> userDB;
// names that currently have an active connection
static std::unordered_set<std::string> activeNames;

// JSON persistence
// File: users.json  (stored next to the server binary)
// Format (one entry per line for easy diff/inspection):
// Characters allowed by validName / validPass are all JSON-safe
// (alphanumeric, underscore, no quotes, no backslashes, no control chars),
// so we can serialize without any escaping logic.

static void saveDB()
{
    std::ofstream f(DB_FILE, std::ios::trunc);
    if (!f)
    {
        std::cerr << "[DB] Cannot write " << DB_FILE << "\n";
        return;
    }
    f << "{\n";
    bool first = true;
    for (auto& [name, pass] : userDB)
    {
        if (!first) f << ",\n";
        first = false;
        f << "  \"" << name << "\": \"" << pass << '"';
    }
    f << "\n}\n";
    std::cout << "[DB] Saved " << userDB.size() << " user(s) to " << DB_FILE << "\n";
}

static void loadDB()
{
    std::ifstream f(DB_FILE);
    if (!f)
    {
        std::cout << "[DB] " << DB_FILE << " not found — starting with empty database.\n";
        return;
    }

    // Minimal hand-rolled JSON parser sufficient for our flat string→string map.
    // Handles the exact format produced by saveDB().
    // Skips '{', '}', whitespace; reads "key": "value" pairs.
    userDB.clear();
    std::string line;
    while (std::getline(f, line))
    {
        // Find first '"' — start of key
        auto k0 = line.find('"');
        if (k0 == std::string::npos) continue;
        auto k1 = line.find('"', k0 + 1);
        if (k1 == std::string::npos) continue;
        std::string key = line.substr(k0 + 1, k1 - k0 - 1);
        if (key.empty()) continue;

        // Find ':' then the next pair of '"' — value
        auto sep = line.find(':', k1 + 1);
        if (sep == std::string::npos) continue;
        auto v0 = line.find('"', sep + 1);
        if (v0 == std::string::npos) continue;
        auto v1 = line.find('"', v0 + 1);
        if (v1 == std::string::npos) continue;
        std::string val = line.substr(v0 + 1, v1 - v0 - 1);

        userDB[key] = val;
    }
    std::cout << "[DB] Loaded " << userDB.size() << " user(s) from " << DB_FILE << "\n";
}

// Validation
// 3-16 alphanumeric/underscore chars, no spaces
static bool validName(const std::string& s)
{
    if (s.size() < 3 || s.size() > 16) return false;
    for (char c : s) if (!isalnum((unsigned char)c) && c != '_') return false;
    return true;
}
// 4-32 chars with no spaces
static bool validPass(const std::string& s)
{
    if (s.size() < 4 || s.size() > 32) return false;
    for (char c : s) if (c == ' ') return false;
    return true;
}

struct Unit
{
    float x, hp;
    int   dmg, type, lane;
    float speed;
    bool  siege;
};

struct Base
{
    int   owner = 0;
    float capture = 0.f;
    float turretCD = 0.f;
};

struct MainTurret
{
    int   level = 0;
    float cd = 0.f;
};

struct Player
{
    int    id, fd;
    float  coins = 15.f;
    int    baseHP = 100;
    int    matchID = -1;
    float  spawnCD[5] = {};
    std::string recvBuf;

    std::string name;
    bool        authenticated = false;
};

struct Match
{
    int     id;
    Player* p1, * p2;
    std::vector<Unit> u1[3], u2[3];
    Base       bases[3];
    MainTurret mainTurret[2];
    bool       running = true;
};

std::vector<Player*>          lobby;
std::unordered_map<int, Player*> byFD;
std::unordered_map<int, Match>   matches;
int nextPID = 1, nextMID = 1;


static void setNB(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}
static void sendMsg(int fd, const std::string& s)
{
    send(fd, s.c_str(), s.size(), 0);
}

static std::string buildState(Match& m, Player* p, Player* e,
    std::vector<Unit> mine[3], std::vector<Unit> enmy[3],
    int myTurrIdx)
{
    std::ostringstream msg;
    msg << "STATE "
        << (int)p->coins << ' ' << (int)e->coins << ' '
        << p->baseHP << ' ' << e->baseHP << ' '
        << m.mainTurret[myTurrIdx].level << ' '
        << m.mainTurret[1 - myTurrIdx].level;

    for (int l = 0; l < 3; l++)
        msg << " B " << m.bases[l].owner << ' ' << (int)m.bases[l].capture;

    msg << " CD";
    for (int i = 0; i < 5; i++)
    {
        int pct = (SPAWN_CD[i] > 0.f)
            ? (int)(p->spawnCD[i] / SPAWN_CD[i] * 100.f) : 0;
        msg << ' ' << std::clamp(pct, 0, 100);
    }

    for (int l = 0; l < 3; l++)
    {
        msg << " M " << l << ' ' << mine[l].size();
        for (auto& u : mine[l])
            msg << ' ' << (int)u.x << ' ' << (int)u.hp << ' ' << u.type;
    }
    for (int l = 0; l < 3; l++)
    {
        msg << " E " << l << ' ' << enmy[l].size();
        for (auto& u : enmy[l])
            msg << ' ' << (int)u.x << ' ' << (int)u.hp << ' ' << u.type;
    }
    msg << '\n';
    return msg.str();
}

static void sendState(Match& m)
{
    sendMsg(m.p1->fd, buildState(m, m.p1, m.p2, m.u1, m.u2, 0));
    sendMsg(m.p2->fd, buildState(m, m.p2, m.p1, m.u2, m.u1, 1));
}

static void updateMatch(Match& m, float dt)
{
    if (!m.running) return;

    float inc1 = 2.f, inc2 = 2.f;
    for (int l = 0; l < 3; l++)
    {
        if (m.bases[l].owner == 1) inc1 += 0.5f;
        else if (m.bases[l].owner == 2) inc2 += 0.5f;
    }
    m.p1->coins = std::min(m.p1->coins + inc1 * dt, 99.f);
    m.p2->coins = std::min(m.p2->coins + inc2 * dt, 99.f);

    for (int i = 0; i < 5; i++)
    {
        m.p1->spawnCD[i] = std::max(0.f, m.p1->spawnCD[i] - dt);
        m.p2->spawnCD[i] = std::max(0.f, m.p2->spawnCD[i] - dt);
    }

    for (int l = 0; l < 3; l++)
    {
        for (auto& u : m.u1[l]) u.x += u.speed * dt;
        for (auto& u : m.u2[l]) u.x -= u.speed * dt;

        for (auto& a : m.u1[l])
            for (auto& b : m.u2[l])
                if (std::abs(a.x - b.x) < 30.f)
                {
                    a.hp -= b.dmg * dt * 5.f;
                    b.hp -= a.dmg * dt * 5.f;
                }

        Base& base = m.bases[l];
        if (base.owner != 0)
        {
            base.turretCD = std::max(0.f, base.turretCD - dt);
            if (base.turretCD <= 0.f)
            {
                auto& targets = (base.owner == 1) ? m.u2[l] : m.u1[l];
                for (auto& u : targets)
                {
                    if (std::abs(u.x - 400.f) <= LANE_T_RANGE)
                    {
                        u.hp -= LANE_T_DMG;
                        base.turretCD = 1.f / LANE_T_RATE;
                        break;
                    }
                }
            }
        }

        auto clean = [](std::vector<Unit>& v)
            {
                v.erase(std::remove_if(v.begin(), v.end(),
                    [](auto& u) { return u.hp <= 0; }), v.end());
            };
        clean(m.u1[l]);
        clean(m.u2[l]);

        m.u1[l].erase(std::remove_if(m.u1[l].begin(), m.u1[l].end(), [&](Unit& u)
            {
                if (u.x >= 760.f) { m.p2->baseHP -= u.siege ? u.dmg * 3 : u.dmg; return true; }
                return false;
            }), m.u1[l].end());

        m.u2[l].erase(std::remove_if(m.u2[l].begin(), m.u2[l].end(), [&](Unit& u)
            {
                if (u.x <= 40.f) { m.p1->baseHP -= u.siege ? u.dmg * 3 : u.dmg; return true; }
                return false;
            }), m.u2[l].end());

        int c1 = (int)m.u1[l].size(), c2 = (int)m.u2[l].size();
        base.capture += (c1 - c2) * dt * 15.f;
        base.capture = std::clamp(base.capture, -100.f, 100.f);
        int prevOwner = base.owner;
        base.owner = (base.capture > 30.f) ? 1
            : (base.capture < -30.f) ? 2 : 0;
        if (base.owner != prevOwner) base.turretCD = 0.f;
    }

    auto fireMain = [&](int pi, std::vector<Unit> enemies[3])
        {
            MainTurret& mt = m.mainTurret[pi];
            if (mt.level == 0) return;
            mt.cd = std::max(0.f, mt.cd - dt);
            if (mt.cd > 0.f) return;
            int   lv = mt.level - 1;
            float tX = (pi == 0) ? 26.f : 774.f;
            for (int l = 0; l < 3; l++)
                for (auto& u : enemies[l])
                    if (std::abs(u.x - tX) <= MAIN_T_RANGE[lv])
                    {
                        u.hp -= MAIN_T_DMG[lv];
                        mt.cd = 1.f / MAIN_T_RATE[lv];
                        return;
                    }
        };
    fireMain(0, m.u2);
    fireMain(1, m.u1);

    if (m.p1->baseHP <= 0)
    {
        sendMsg(m.p1->fd, "LOSE\n"); sendMsg(m.p2->fd, "WIN\n");
        m.running = false; return;
    }
    if (m.p2->baseHP <= 0)
    {
        sendMsg(m.p1->fd, "WIN\n");  sendMsg(m.p2->fd, "LOSE\n");
        m.running = false; return;
    }

    sendState(m);
}

static void spawnUnit(Player* p, int type, int lane)
{
    if (type < 0 || type > 4 || lane < 0 || lane > 2) return;
    if (p->matchID == -1) return;
    auto it = matches.find(p->matchID);
    if (it == matches.end()) return;
    Match& m = it->second;
    if (!m.running) return;

    if (type >= 3)
    {
        int pnum = (m.p1 == p) ? 1 : 2;
        bool hasBase = false;
        for (int l = 0; l < 3; l++)
            if (m.bases[l].owner == pnum) { hasBase = true; break; }
        if (!hasBase) return;
    }

    if (p->coins < COST[type]) return;
    if (p->spawnCD[type] > 0.f) return;

    p->coins -= COST[type];
    p->spawnCD[type] = SPAWN_CD[type];

    Unit u;
    u.hp = 20.f + type * 10.f;
    u.dmg = 2 + type * 2;
    u.speed = SPEED[type];
    u.type = type;
    u.lane = lane;
    u.siege = (type == 4);

    if (m.p1 == p) { u.x = 60.f;  m.u1[lane].push_back(u); }
    else { u.x = 740.f; m.u2[lane].push_back(u); }
}

static void upgradeMainTurret(Player* p)
{
    if (p->matchID == -1) return;
    auto it = matches.find(p->matchID);
    if (it == matches.end() || !it->second.running) return;
    Match& m = it->second;
    int pi = (m.p1 == p) ? 0 : 1;
    MainTurret& mt = m.mainTurret[pi];
    if (mt.level >= 3) return;
    float cost = UPGRADE_COST[mt.level];
    if (p->coins < cost) return;
    p->coins -= cost;
    mt.level++;
}

static void tryMatch()
{
    while (lobby.size() >= 2)
    {
        Player* a = lobby[0]; Player* b = lobby[1];
        lobby.erase(lobby.begin(), lobby.begin() + 2);

        Match match;
        match.id = nextMID++;
        match.p1 = a; match.p2 = b;
        a->matchID = match.id; b->matchID = match.id;
        matches[match.id] = match;

        sendMsg(a->fd, "START 1 " + a->name + " " + b->name + "\n");
        sendMsg(b->fd, "START 2 " + b->name + " " + a->name + "\n");

        std::cout << "Match " << match.id
            << " started: " << a->name << " vs " << b->name << "\n";
    }
}

static void removePlayer(int epfd, int fd)
{
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);

    auto it = byFD.find(fd);
    if (it == byFD.end()) return;
    Player* p = it->second;

    if (p->authenticated)
        activeNames.erase(p->name);

    lobby.erase(std::remove(lobby.begin(), lobby.end(), p), lobby.end());

    if (p->matchID != -1)
    {
        auto mit = matches.find(p->matchID);
        if (mit != matches.end() && mit->second.running)
        {
            Player* opp = (mit->second.p1 == p) ? mit->second.p2 : mit->second.p1;
            sendMsg(opp->fd, "WIN\n");
            mit->second.running = false;
        }
    }

    byFD.erase(fd);
    std::cout << (p->authenticated ? p->name : "(unauthed)") << " disconnected (fd=" << fd << ")\n";
    delete p;
}

static void handleAuth(int fd, Player* p, const std::string& cmd,
    const std::string& name, const std::string& pass)
{
    if (!validName(name))
    {
        sendMsg(fd, "AUTH_FAIL Username must be 3-16 alphanumeric/underscore chars\n");
        return;
    }
    if (!validPass(pass))
    {
        sendMsg(fd, "AUTH_FAIL Password must be 4-32 chars with no spaces\n");
        return;
    }

    if (cmd == "REGISTER")
    {
        if (userDB.count(name))
        {
            sendMsg(fd, "AUTH_FAIL Username already taken\n");
            return;
        }
        userDB[name] = pass;
        saveDB();   // persist immediately after every new registration
        std::cout << "Registered new user: " << name << "\n";
    }
    else // LOGIN
    {
        auto it = userDB.find(name);
        if (it == userDB.end())
        {
            sendMsg(fd, "AUTH_FAIL Username not found\n");
            return;
        }
        if (it->second != pass)
        {
            sendMsg(fd, "AUTH_FAIL Wrong password\n");
            return;
        }
    }

    if (activeNames.count(name))
    {
        sendMsg(fd, "AUTH_FAIL That account is already logged in\n");
        return;
    }

    p->name = name;
    p->authenticated = true;
    activeNames.insert(name);

    sendMsg(fd, "AUTH_OK " + name + "\n");
    std::cout << name << " authenticated (fd=" << fd << ")\n";

    lobby.push_back(p);
    tryMatch();
}

static void processLine(int fd, const std::string& line)
{
    if (line.empty()) return;
    auto pit = byFD.find(fd);
    if (pit == byFD.end()) return;
    Player* p = pit->second;

    std::istringstream ss(line);
    std::string t; ss >> t;

    if (!p->authenticated)
    {
        if (t == "REGISTER" || t == "LOGIN")
        {
            std::string name, pass;
            if (ss >> name >> pass)
                handleAuth(fd, p, t, name, pass);
            else
                sendMsg(fd, "AUTH_FAIL Missing username or password\n");
        }
        return;
    }

    if (t == "SPAWN")
    {
        int type, lane;
        if (ss >> type >> lane) spawnUnit(p, type, lane);
    }
    else if (t == "UPGRADE")
    {
        upgradeMainTurret(p);
    }
}

int main()
{
    loadDB();   // restore registered users from disk before accepting connections

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(lfd, (sockaddr*)&addr, sizeof(addr));
    listen(lfd, SOMAXCONN);
    setNB(lfd);

    int ep = epoll_create1(0);
    epoll_event ev{ EPOLLIN, {.fd = lfd} };
    epoll_ctl(ep, EPOLL_CTL_ADD, lfd, &ev);

    epoll_event events[MAX_EVTS];
    auto last = Clock::now();
    std::cout << "BaseBrawl server on :" << PORT << "\n";

    while (true)
    {
        int n = epoll_wait(ep, events, MAX_EVTS, 10);

        for (int i = 0; i < n; i++)
        {
            int fd = events[i].data.fd;

            if (fd == lfd)
            {
                int c = accept(lfd, nullptr, nullptr);
                if (c < 0) continue;
                setNB(c);
                epoll_event ce{ EPOLLIN, {.fd = c} };
                epoll_ctl(ep, EPOLL_CTL_ADD, c, &ce);

                Player* p = new Player();
                p->id = nextPID++;
                p->fd = c;
                byFD[c] = p;

                std::cout << "Connection fd=" << c << " (awaiting auth)\n";
            }
            else
            {
                char buf[4096];
                int  r = recv(fd, buf, sizeof(buf) - 1, 0);
                if (r <= 0) { removePlayer(ep, fd); continue; }

                buf[r] = 0;
                auto pit = byFD.find(fd);
                if (pit == byFD.end()) continue;
                Player* p = pit->second;

                p->recvBuf += buf;
                size_t pos;
                while ((pos = p->recvBuf.find('\n')) != std::string::npos)
                {
                    processLine(fd, p->recvBuf.substr(0, pos));
                    p->recvBuf.erase(0, pos + 1);
                }
            }
        }

        auto  now = Clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        if (dt >= 0.016f)
        {
            for (auto& [_, m] : matches) updateMatch(m, dt);
            for (auto it = matches.begin(); it != matches.end(); )
                it = it->second.running ? std::next(it) : matches.erase(it);
            last = now;
        }
    }
}