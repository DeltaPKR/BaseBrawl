#include <SFML/Graphics.hpp>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <vector>
#include <string>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <iomanip>

#define SERVER_IP "127.0.0.1"
#define PORT 55001

// State machine
// AUTH = filling in login/register form (no socket yet)
// CONNECTING = socket open, waiting for AUTH_OK / AUTH_FAIL
enum class Screen { MENU, AUTH, CONNECTING, WAITING, PLAYING, WIN, LOSE };

// ── Game data ────────────────────────────────────────────────────────────────
struct Unit { float x; int hp, maxHp, type; };

struct Projectile
{
    float x, y, vx, vy;
    float life;
    sf::Color col;
    float radius;
};

// Hit-flash event: a brief glowing disc that appears where a unit took damage
struct FlashEvent
{
    float x, y;
    float timer;
    static constexpr float MAX = 0.28f;
};

// Per-frame unit snapshot for flash detection (compare HP across STATE packets)
struct UnitSnap { float x; int hp; int type; };

struct GameData
{
    int myCoins = 0, enemyCoins = 0;
    int myBaseHP = 100, enemyBaseHP = 100;
    int myTurretLvl = 0, enemyTurretLvl = 0;
    int baseOwner[3] = {};
    int baseCapture[3] = {};
    int spawnCDPct[5] = {};
    std::vector<Unit> myUnits[3], enemyUnits[3];
    int myID = 0;
    int basesOwned = 0;
};

// Shared constants (must match server)
static const char* UNAME[5] = { "Grunt","Brute","Tank","Mage","Siege" };
static const char* UKEY[5] = { "Q","W","E","R","T" };
static const int    UCOST[5] = { 5, 7, 10, 12, 15 };
static const float  SPAWN_CD_CLI[5] = { 2.f, 3.f, 4.5f, 6.f, 8.f };
static const int    UPGRADE_COST_CLI[3] = { 20, 35, 55 };

static const sf::Color MY_COL[5] =
{
    {70,150,255},{70,210,110},{230,175,50},{170,70,215},{230,70,70}
};
static const sf::Color EN_COL[5] =
{
    {255,100,70},{255,65,140},{255,195,65},{195,95,255},{140,40,40}
};
static const sf::Color LVL_COL[3] =
{
    {100,210,110},{230,185,50},{230,100,55}
};

// Draw helpers
static void drawBar(sf::RenderWindow& w,
    float x, float y, float bw, float bh, float fill,
    sf::Color bg, sf::Color fg)
{
    sf::RectangleShape back({ bw, bh });
    back.setPosition(x, y); back.setFillColor(bg); w.draw(back);
    if (fill > 0.f)
    {
        sf::RectangleShape front({ bw * std::clamp(fill,0.f,1.f), bh });
        front.setPosition(x, y); front.setFillColor(fg); w.draw(front);
    }
}

static void centerText(sf::Text& t, float x, float y)
{
    auto b = t.getLocalBounds();
    t.setOrigin(b.left + b.width * .5f, b.top + b.height * .5f);
    t.setPosition(x, y);
}

// Main
int main()
{
    sf::RenderWindow window(sf::VideoMode(800, 600), "BaseBrawl", sf::Style::Close);
    window.setFramerateLimit(60);

    sf::Font font; bool hasFont = false;
    for (auto& fp : std::vector<std::string>
        {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
        "C:/Windows/Fonts/arial.ttf"
        }) {
        if (font.loadFromFile(fp)) { hasFont = true; break; }
    }

    auto mkText = [&](const std::string& s, unsigned sz,
        sf::Color c = sf::Color::White)
        {
            sf::Text t;
            if (hasFont) t.setFont(font);
            t.setString(s); t.setCharacterSize(sz); t.setFillColor(c);
            return t;
        };

    // Core state
    Screen     screen = Screen::MENU;
    GameData   g;
    int        sock = -1;
    std::string recvBuf;

    int   selLane = 1;
    float upgDebounce = 0.f;
    float dotTimer = 0.f;
    int   dotCount = 0;

    // Auth state
    std::string authUsername;
    std::string authPassword;
    bool        authIsRegister = false;
    std::string authError;
    int         authActiveField = 0;   // 0=username, 1=password
    float       cursorTimer = 0.f;
    bool        cursorVisible = true;

    // Player names (populated from START message)
    std::string myName = "You";
    std::string opponentName = "Opponent";

    // Hit-flash system
    std::vector<FlashEvent>  flashEvents;
    // Previous-frame unit snapshots for HP-drop detection
    std::vector<UnitSnap> snapMy[3], snapEn[3];

    // Screen-shake state
    float shakeTimer = 0.f;  // counts down from >0 to 0

    // Client-side projectile VFX
    float laneTurrCD[3] = {};
    float myMainTurrCD = 0.f;
    float enMainTurrCD = 0.f;
    std::vector<Projectile> projectiles;

    // Perspective colors
    const sf::Color MY_TEAM{ 60, 135, 255 };
    const sf::Color EN_TEAM{ 215, 52,  52 };

    // Layout
    const float HDR_H = 42.f;
    const float LANE_Y[3] = { 128.f, 255.f, 382.f };
    const float PANEL_Y = 430.f;
    const float PANEL_H = 600.f - PANEL_Y;
    const float BASE_W = 54.f;
    const float BTN_W = 148.f;
    const float BTN_H = 82.f;
    const float BTN_GAP = 4.f;
    const float BTN_START = 8.f;

    sf::Clock clk;

    // Connect helper
    auto doConnect = [&]() -> bool
        {
            sock = ::socket(AF_INET, SOCK_STREAM, 0);
            if (sock < 0) return false;
            sockaddr_in serv{};
            serv.sin_family = AF_INET;
            serv.sin_port = htons(PORT);
            inet_pton(AF_INET, SERVER_IP, &serv.sin_addr);
            if (connect(sock, (sockaddr*)&serv, sizeof(serv)) < 0)
            {
                close(sock); sock = -1; return false;
            }
            fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK);
            return true;
        };

    // Auth-submit helper (connect + send creds → CONNECTING)
    auto submitAuth = [&]()
        {
            if (authUsername.empty() || authPassword.empty()) return;
            authError.clear();
            if (!doConnect())
            {
                authError = "Cannot connect to server";
                return;
            }
            std::string cmd = std::string(authIsRegister ? "REGISTER " : "LOGIN ")
                + authUsername + " " + authPassword + "\n";
            send(sock, cmd.c_str(), cmd.size(), 0);
            screen = Screen::CONNECTING;
        };

    // State-machine message handler
    auto handleLine = [&](const std::string& line)
        {
            if (line.empty()) return;
            std::istringstream ss(line);
            std::string tok; ss >> tok;

            if (tok == "AUTH_OK")
            {
                std::string name; ss >> name;
                myName = name;
                screen = Screen::WAITING;
            }
            else if (tok == "AUTH_FAIL")
            {
                std::string reason;
                std::getline(ss, reason);
                if (!reason.empty() && reason[0] == ' ') reason.erase(0, 1);
                authError = reason;
                if (sock >= 0) { close(sock); sock = -1; }
                screen = Screen::AUTH;
            }
            else if (tok == "WAITING")
            {
                screen = Screen::WAITING;
            }
            else if (tok == "START")
            {
                int pid; std::string mname, oname;
                ss >> pid >> mname >> oname;
                myName = mname;
                opponentName = oname;
                g = GameData{}; g.myID = pid;
                flashEvents.clear();
                shakeTimer = 0.f;
                for (int l = 0; l < 3; l++) { snapMy[l].clear(); snapEn[l].clear(); }
                screen = Screen::PLAYING;
            }
            else if (tok == "WIN") { screen = Screen::WIN; }
            else if (tok == "LOSE") { screen = Screen::LOSE; }
            else if (tok == "STATE")
            {
                int oldMyBaseHP = g.myBaseHP;

                for (int l = 0; l < 3; l++)
                {
                    snapMy[l].clear();
                    for (auto& u : g.myUnits[l])
                        snapMy[l].push_back({ u.x, u.hp, u.type });
                    snapEn[l].clear();
                    for (auto& u : g.enemyUnits[l])
                        snapEn[l].push_back({ u.x, u.hp, u.type });
                }

                ss >> g.myCoins >> g.enemyCoins
                    >> g.myBaseHP >> g.enemyBaseHP
                    >> g.myTurretLvl >> g.enemyTurretLvl;

                for (int i = 0; i < 3; i++)
                {
                    std::string B;
                    ss >> B >> g.baseOwner[i] >> g.baseCapture[i];
                }
                std::string CD; ss >> CD;
                for (int i = 0; i < 5; i++) ss >> g.spawnCDPct[i];

                for (int i = 0; i < 3; i++)
                {
                    std::string M; int lane, cnt;
                    ss >> M >> lane >> cnt;
                    if (lane < 0 || lane > 2) continue;
                    g.myUnits[lane].clear();
                    for (int j = 0; j < cnt; j++)
                    {
                        int x, hp, type; ss >> x >> hp >> type;
                        if (type >= 0 && type < 5)
                            g.myUnits[lane].push_back({ (float)x, hp, 20 + type * 10, type });
                    }
                }
                for (int i = 0; i < 3; i++)
                {
                    std::string E; int lane, cnt;
                    ss >> E >> lane >> cnt;
                    if (lane < 0 || lane > 2) continue;
                    g.enemyUnits[lane].clear();
                    for (int j = 0; j < cnt; j++)
                    {
                        int x, hp, type; ss >> x >> hp >> type;
                        if (type >= 0 && type < 5)
                            g.enemyUnits[lane].push_back({ (float)x, hp, 20 + type * 10, type });
                    }
                }

                g.basesOwned = 0;
                for (int l = 0; l < 3; l++)
                    if (g.baseOwner[l] == g.myID) g.basesOwned++;

                auto detectFlash = [&](const std::vector<Unit>& newUs,
                    const std::vector<UnitSnap>& snaps,
                    int lane)
                    {
                        for (auto& nu : newUs)
                        {
                            float bestD = 65.f;
                            const UnitSnap* best = nullptr;
                            for (auto& s : snaps)
                            {
                                if (s.type != nu.type) continue;
                                float d = std::abs(s.x - nu.x);
                                if (d < bestD) { bestD = d; best = &s; }
                            }
                            if (best && nu.hp < best->hp)
                                flashEvents.push_back({ nu.x, LANE_Y[lane], FlashEvent::MAX });
                        }
                    };
                for (int l = 0; l < 3; l++)
                {
                    detectFlash(g.myUnits[l], snapMy[l], l);
                    detectFlash(g.enemyUnits[l], snapEn[l], l);
                }

                if (g.myBaseHP < oldMyBaseHP)
                {
                    float dmg = (float)(oldMyBaseHP - g.myBaseHP);
                    shakeTimer = std::min(shakeTimer + dmg * 0.055f, 0.48f);
                }
            }
        };

    // MAIN LOOP
    while (window.isOpen())
    {
        float dt = clk.restart().asSeconds();
        upgDebounce = std::max(0.f, upgDebounce - dt);
        dotTimer += dt;
        if (dotTimer > 0.45f) { dotTimer = 0; dotCount = (dotCount + 1) % 4; }
        cursorTimer += dt;
        if (cursorTimer > 0.5f) { cursorTimer = 0.f; cursorVisible = !cursorVisible; }
        shakeTimer = std::max(0.f, shakeTimer - dt);

        // Events
        sf::Event ev;
        while (window.pollEvent(ev))
        {
            if (ev.type == sf::Event::Closed) window.close();

            if (ev.type == sf::Event::TextEntered && screen == Screen::AUTH)
            {
                auto uc = ev.text.unicode;
                std::string& field = (authActiveField == 0) ? authUsername : authPassword;
                if (uc == 8 && !field.empty())
                    field.pop_back();
                else if (uc >= 32 && uc < 127 && uc != ' ')
                {
                    int maxLen = (authActiveField == 0) ? 16 : 32;
                    if ((int)field.size() < maxLen) field += (char)uc;
                }
            }

            if (ev.type == sf::Event::KeyPressed)
            {
                if (screen == Screen::MENU && ev.key.code == sf::Keyboard::Return)
                    screen = Screen::AUTH;

                if (screen == Screen::AUTH)
                {
                    if (ev.key.code == sf::Keyboard::Tab)
                        authActiveField = 1 - authActiveField;
                    if (ev.key.code == sf::Keyboard::Return)
                        submitAuth();
                    if (ev.key.code == sf::Keyboard::F1)
                        authIsRegister = false;
                    if (ev.key.code == sf::Keyboard::F2)
                        authIsRegister = true;
                    if (ev.key.code == sf::Keyboard::Escape)
                    {
                        authError.clear(); screen = Screen::MENU;
                    }
                }

                if ((screen == Screen::WIN || screen == Screen::LOSE)
                    && ev.key.code == sf::Keyboard::R)
                {
                    if (sock >= 0) { close(sock); sock = -1; }
                    g = GameData{}; recvBuf.clear();
                    flashEvents.clear(); shakeTimer = 0.f;
                    for (int l = 0; l < 3; l++) { snapMy[l].clear(); snapEn[l].clear(); }

                    if (!authUsername.empty() && !authPassword.empty())
                        submitAuth();
                    else
                    {
                        authError.clear();
                        screen = Screen::AUTH;
                    }
                }
            }
        }

        // Input: playing
        if (screen == Screen::PLAYING && window.hasFocus())
        {
            if (sf::Keyboard::isKeyPressed(sf::Keyboard::Num1)) selLane = 0;
            if (sf::Keyboard::isKeyPressed(sf::Keyboard::Num2)) selLane = 1;
            if (sf::Keyboard::isKeyPressed(sf::Keyboard::Num3)) selLane = 2;

            struct { sf::Keyboard::Key k; int t; } binds[] =
            {
                {sf::Keyboard::Q,0},{sf::Keyboard::W,1},{sf::Keyboard::E,2},
                {sf::Keyboard::R,3},{sf::Keyboard::T,4}
            };
            for (auto& b : binds)
            {
                if (sf::Keyboard::isKeyPressed(b.k)
                    && g.spawnCDPct[b.t] == 0
                    && (b.t < 3 || g.basesOwned > 0)
                    && g.myCoins >= UCOST[b.t]
                    && sock >= 0)
                {
                    std::string cmd = "SPAWN " + std::to_string(b.t)
                        + " " + std::to_string(selLane) + "\n";
                    send(sock, cmd.c_str(), cmd.size(), 0);
                    g.spawnCDPct[b.t] = 99;
                }
            }
            if (sf::Keyboard::isKeyPressed(sf::Keyboard::U)
                && upgDebounce <= 0.f && g.myTurretLvl < 3 && sock >= 0)
            {
                send(sock, "UPGRADE\n", 8, 0);
                upgDebounce = 0.5f;
            }
        }

        // Receive
        if (sock >= 0)
        {
            char buf[4096];
            int r = recv(sock, buf, sizeof(buf) - 1, 0);
            if (r > 0)
            {
                buf[r] = 0; recvBuf += buf;
                size_t pos;
                while ((pos = recvBuf.find('\n')) != std::string::npos)
                {
                    handleLine(recvBuf.substr(0, pos));
                    recvBuf.erase(0, pos + 1);
                }
            }
        }

        bool iAmP1 = (g.myID == 1);

        // Tick flash events
        for (auto& fe : flashEvents) fe.timer -= dt;
        flashEvents.erase(std::remove_if(flashEvents.begin(), flashEvents.end(),
            [](auto& fe) { return fe.timer <= 0.f; }), flashEvents.end());

        // Projectile VFX simulation (client-only, cosmetic)
        if (screen == Screen::PLAYING)
        {
            static const float LANE_T_RANGE_C = 80.f;
            static const float MAIN_T_RANGE_C[3] = { 90.f,125.f,165.f };
            static const float MAIN_T_RATE_C[3] = { 0.8f, 1.3f, 1.9f };
            static const float PROJ_SPEED = 320.f;

            for (auto& p : projectiles)
            {
                p.x += p.vx * dt; p.y += p.vy * dt; p.life -= dt;
            }
            projectiles.erase(
                std::remove_if(projectiles.begin(), projectiles.end(),
                    [](auto& p) { return p.life <= 0; }),
                projectiles.end());

            auto spawnProj = [&](float ox, float oy, float tx, float ty, sf::Color col)
                {
                    float dx = tx - ox, dy = ty - oy;
                    float dist = std::sqrt(dx * dx + dy * dy) + 0.001f;
                    Projectile p;
                    p.x = ox; p.y = oy;
                    p.vx = (dx / dist) * PROJ_SPEED; p.vy = (dy / dist) * PROJ_SPEED;
                    p.life = 0.5f; p.col = col; p.radius = 4.f;
                    projectiles.push_back(p);
                };

            for (int l = 0; l < 3; l++)
            {
                if (g.baseOwner[l] == 0) { laneTurrCD[l] = 0.f; continue; }
                laneTurrCD[l] = std::max(0.f, laneTurrCD[l] - dt);
                if (laneTurrCD[l] > 0.f) continue;
                bool iOwnLane = (g.baseOwner[l] == g.myID);
                auto& targets = iOwnLane ? g.enemyUnits[l] : g.myUnits[l];
                sf::Color col = iOwnLane ? MY_TEAM : EN_TEAM;
                for (auto& u : targets)
                {
                    if (std::abs(u.x - 400.f) <= LANE_T_RANGE_C)
                    {
                        spawnProj(400.f, LANE_Y[l], u.x, LANE_Y[l], col);
                        laneTurrCD[l] = 1.f;
                        break;
                    }
                }
            }

            auto fireMainTurr = [&](float turrX, int level, bool isMine, float& cd)
                {
                    if (level == 0) return;
                    cd = std::max(0.f, cd - dt);
                    if (cd > 0.f) return;
                    int lv = level - 1;
                    auto& lanes = isMine ? g.enemyUnits : g.myUnits;
                    sf::Color col = isMine ? MY_TEAM : EN_TEAM;
                    for (int l = 0; l < 3; l++)
                        for (auto& u : lanes[l])
                            if (std::abs(u.x - turrX) <= MAIN_T_RANGE_C[lv])
                            {
                                spawnProj(turrX, LANE_Y[l], u.x, LANE_Y[l], col);
                                cd = 1.f / MAIN_T_RATE_C[lv];
                                return;
                            }
                };
            float myTurrX = iAmP1 ? 26.f : 774.f;
            float enTurrX = iAmP1 ? 774.f : 26.f;
            fireMainTurr(myTurrX, g.myTurretLvl, true, myMainTurrCD);
            fireMainTurr(enTurrX, g.enemyTurretLvl, false, enMainTurrCD);
        }

        // RENDER
        window.clear(sf::Color(13, 14, 24));

        // MENU
        if (screen == Screen::MENU)
        {
            for (int l = 0; l < 3; l++)
            {
                sf::RectangleShape s({ 800.f,60.f });
                s.setPosition(0, LANE_Y[l] - 30.f);
                s.setFillColor(sf::Color(20, 24, 42, 190)); window.draw(s);
            }
            auto title = mkText("BaseBrawl", 72, sf::Color(255, 210, 50));
            centerText(title, 400, 155); window.draw(title);
            auto sub = mkText("3-Lane Real-Time Strategy", 20, sf::Color(148, 152, 196));
            centerText(sub, 400, 248); window.draw(sub);
            auto prompt = mkText("Press ENTER to continue" + std::string(dotCount, '.'),
                22, sf::Color(75, 202, 255));
            centerText(prompt, 400, 322); window.draw(prompt);
            std::vector<std::string> hints =
            {
                "1/2/3  Select lane     Q/W/E/R/T  Spawn unit     U  Upgrade turret",
                "Mage & Siege unlock once you own a Capture Point",
                "Capture Points give bonus coins — own all 3 to dominate"
            };
            for (int i = 0; i < (int)hints.size(); i++)
            {
                auto h = mkText(hints[i], 12, sf::Color(88, 92, 125));
                centerText(h, 400, 400.f + i * 20.f); window.draw(h);
            }
        }

        // AUTH screen
        else if (screen == Screen::AUTH)
        {
            sf::RectangleShape bg({ 800.f, 600.f });
            bg.setFillColor(sf::Color(9, 10, 20)); window.draw(bg);

            auto title = mkText("BaseBrawl", 52, sf::Color(255, 210, 50));
            centerText(title, 400.f, 80.f); window.draw(title);

            {
                sf::Color loginC = !authIsRegister
                    ? sf::Color(65, 185, 255) : sf::Color(50, 55, 80);
                sf::Color regC = authIsRegister
                    ? sf::Color(75, 225, 120) : sf::Color(50, 55, 80);

                sf::RectangleShape lpill({ 150.f, 32.f });
                lpill.setPosition(220.f, 148.f);
                lpill.setFillColor(!authIsRegister ? sf::Color(18, 36, 65) : sf::Color(14, 14, 22));
                lpill.setOutlineColor(loginC); lpill.setOutlineThickness(2.f);
                window.draw(lpill);
                auto lt = mkText("[F1]  Login", 14, loginC);
                centerText(lt, 295.f, 164.f); window.draw(lt);

                sf::RectangleShape rpill({ 150.f, 32.f });
                rpill.setPosition(430.f, 148.f);
                rpill.setFillColor(authIsRegister ? sf::Color(14, 38, 22) : sf::Color(14, 14, 22));
                rpill.setOutlineColor(regC); rpill.setOutlineThickness(2.f);
                window.draw(rpill);
                auto rt = mkText("[F2]  Register", 14, regC);
                centerText(rt, 505.f, 164.f); window.draw(rt);
            }

            auto drawField = [&](float cy, const std::string& label,
                const std::string& value, bool isPass, bool active)
                {
                    float fx = 250.f, fw = 300.f, fh = 38.f;
                    float fy = cy;

                    auto lbl = mkText(label, 12,
                        active ? sf::Color(130, 165, 215) : sf::Color(80, 85, 110));
                    lbl.setPosition(fx, fy - 18.f); window.draw(lbl);

                    sf::RectangleShape box({ fw, fh });
                    box.setPosition(fx, fy);
                    box.setFillColor(sf::Color(14, 16, 28));
                    box.setOutlineColor(active ? sf::Color(70, 135, 255) : sf::Color(42, 48, 72));
                    box.setOutlineThickness(2.f);
                    window.draw(box);

                    std::string disp = isPass ? std::string(value.size(), '*') : value;
                    if (active && cursorVisible) disp += '|';
                    auto vt = mkText(disp, 16, sf::Color(220, 225, 245));
                    vt.setPosition(fx + 10.f, fy + (fh - 16.f) * 0.45f);
                    window.draw(vt);
                };

            drawField(235.f, "Username", authUsername, false, authActiveField == 0);
            drawField(315.f, "Password", authPassword, true, authActiveField == 1);

            auto tabHint = mkText("Tab = switch field    Esc = back to menu", 11,
                sf::Color(65, 70, 100));
            centerText(tabHint, 400.f, 380.f); window.draw(tabHint);

            bool canSubmit = (!authUsername.empty() && !authPassword.empty());
            auto sub = mkText("Press Enter to " + std::string(authIsRegister ? "register" : "login"),
                17, canSubmit ? sf::Color(80, 200, 100) : sf::Color(55, 60, 80));
            centerText(sub, 400.f, 418.f); window.draw(sub);

            if (!authError.empty())
            {
                sf::RectangleShape errBox({ 700.f, 32.f });
                errBox.setPosition(50.f, 454.f);
                errBox.setFillColor(sf::Color(50, 10, 10));
                errBox.setOutlineColor(sf::Color(180, 40, 40));
                errBox.setOutlineThickness(1.5f);
                window.draw(errBox);
                auto et = mkText(authError, 13, sf::Color(235, 90, 90));
                centerText(et, 400.f, 470.f); window.draw(et);
            }
        }

        // CONNECTING / WAITING
        else if (screen == Screen::CONNECTING || screen == Screen::WAITING)
        {
            std::string msg = (screen == Screen::CONNECTING)
                ? "Authenticating" : "Finding opponent";
            auto t = mkText(msg + std::string(dotCount, '.'),
                30, sf::Color(75, 202, 255));
            centerText(t, 400, 270); window.draw(t);
            auto t2 = mkText("Close window to cancel", 14, sf::Color(88, 92, 125));
            centerText(t2, 400, 330); window.draw(t2);
        }

        // PLAYING / WIN / LOSE
        else
        {
            // Compute shake offset
            sf::Vector2f shakeOff(0.f, 0.f);
            if (shakeTimer > 0.f)
            {
                float mag = (shakeTimer / 0.48f) * 9.f;
                shakeOff.x = std::sin(shakeTimer * 88.f) * mag;
                shakeOff.y = std::cos(shakeTimer * 67.f) * mag;
            }

            sf::View shakeView = window.getDefaultView();
            shakeView.move(shakeOff);
            window.setView(shakeView);

            // Lane backgrounds
            for (int l = 0; l < 3; l++)
            {
                sf::RectangleShape lane({ 800.f - BASE_W * 2.f, 58.f });
                lane.setPosition(BASE_W, LANE_Y[l] - 29.f);
                lane.setFillColor(sf::Color(20, 24, 42)); window.draw(lane);
                for (float oy : {-29.f, 29.f})
                {
                    sf::RectangleShape border({ 800.f - BASE_W * 2.f, 1.f });
                    border.setPosition(BASE_W, LANE_Y[l] + oy);
                    border.setFillColor(sf::Color(36, 42, 70)); window.draw(border);
                }
            }

            // Side base panels
            // Left panel belongs to P1, right panel to P2.
            // Color them from each player's perspective: own = blue, enemy = red.
            sf::Color myPanelCol(12, 28, 65), enPanelCol(60, 12, 12);
            sf::Color leftPanelCol = iAmP1 ? myPanelCol : enPanelCol;
            sf::Color rightPanelCol = iAmP1 ? enPanelCol : myPanelCol;

            sf::RectangleShape p1Panel({ BASE_W, PANEL_Y - HDR_H });
            p1Panel.setPosition(0, HDR_H);
            p1Panel.setFillColor(leftPanelCol); window.draw(p1Panel);

            sf::RectangleShape p2Panel({ BASE_W, PANEL_Y - HDR_H });
            p2Panel.setPosition(800.f - BASE_W, HDR_H);
            p2Panel.setFillColor(rightPanelCol); window.draw(p2Panel);

            // Vertical HP bars on inner edge of panels
            {
                float barH = PANEL_Y - HDR_H - 42.f;
                float barTop = HDR_H + 20.f;
                // Left bar always shows P1's HP; right bar always shows P2's HP
                float p1HpFrac = iAmP1 ? g.myBaseHP / 100.f : g.enemyBaseHP / 100.f;
                float p2HpFrac = iAmP1 ? g.enemyBaseHP / 100.f : g.myBaseHP / 100.f;
                sf::Color leftBarCol = iAmP1 ? sf::Color(50, 135, 255) : sf::Color(205, 48, 48);
                sf::Color rightBarCol = iAmP1 ? sf::Color(205, 48, 48) : sf::Color(50, 135, 255);
                drawBar(window, BASE_W - 8.f, barTop + barH * (1.f - p1HpFrac),
                    6.f, barH * p1HpFrac, 1.f, sf::Color(0, 0, 0, 0), leftBarCol);
                drawBar(window, 800.f - BASE_W + 2.f, barTop + barH * (1.f - p2HpFrac),
                    6.f, barH * p2HpFrac, 1.f, sf::Color(0, 0, 0, 0), rightBarCol);
            }

            // Turret icons
            auto drawTurretIcon = [&](float cx, float cy, int level,
                sf::Color col, bool facingRight)
                {
                    if (level == 0)
                    {
                        sf::CircleShape ghost(9.f);
                        ghost.setOrigin(9.f, 9.f); ghost.setPosition(cx, cy);
                        ghost.setFillColor(sf::Color(0, 0, 0, 0));
                        ghost.setOutlineColor(sf::Color(55, 60, 85));
                        ghost.setOutlineThickness(2.f); window.draw(ghost);
                        return;
                    }
                    sf::RectangleShape body({ 14.f,14.f });
                    body.setOrigin(7.f, 7.f); body.setRotation(45.f);
                    body.setPosition(cx, cy); body.setFillColor(col); window.draw(body);
                    sf::RectangleShape barrel({ 20.f,5.f });
                    barrel.setOrigin(0.f, 2.5f);
                    barrel.setPosition(facingRight ? cx + 7.f : cx - 27.f, cy);
                    barrel.setFillColor(col); window.draw(barrel);
                    for (int d = 0; d < level; d++)
                    {
                        sf::CircleShape dot(3.f); dot.setOrigin(3.f, 3.f);
                        dot.setPosition(cx - (level - 1) * 4.f + d * 8.f, cy + 16.f);
                        dot.setFillColor(col); window.draw(dot);
                    }
                };

            float midPanelY = (PANEL_Y - HDR_H) * 0.5f + HDR_H;
            int p1TurrLvl = iAmP1 ? g.myTurretLvl : g.enemyTurretLvl;
            int p2TurrLvl = iAmP1 ? g.enemyTurretLvl : g.myTurretLvl;
            sf::Color leftTurrCol = (p1TurrLvl > 0) ? (iAmP1 ? MY_TEAM : EN_TEAM) : sf::Color(58, 63, 90);
            sf::Color rightTurrCol = (p2TurrLvl > 0) ? (iAmP1 ? EN_TEAM : MY_TEAM) : sf::Color(58, 63, 90);
            drawTurretIcon(BASE_W * 0.5f, midPanelY, p1TurrLvl, leftTurrCol, true);
            drawTurretIcon(800.f - BASE_W * 0.5f, midPanelY, p2TurrLvl, rightTurrCol, false);

            // Capture points
            for (int l = 0; l < 3; l++)
            {
                bool iOwn = (g.baseOwner[l] == g.myID);
                bool theyOwn = (g.baseOwner[l] != 0 && !iOwn);
                sf::Color ringCol = iOwn ? MY_TEAM
                    : theyOwn ? EN_TEAM : sf::Color(55, 60, 90);

                sf::CircleShape ring(17.f);
                ring.setOrigin(17.f, 17.f); ring.setPosition(400.f, LANE_Y[l]);
                ring.setFillColor(sf::Color(13, 14, 24));
                ring.setOutlineColor(ringCol); ring.setOutlineThickness(3.f);
                window.draw(ring);

                float fill = (g.baseCapture[l] + 100.f) / 200.f;
                float myFill = iAmP1 ? fill : (1.f - fill);
                sf::Color barFillCol = iOwn ? MY_TEAM
                    : (theyOwn ? EN_TEAM : sf::Color(80, 85, 115));
                drawBar(window, 368.f, LANE_Y[l] + 21.f, 64.f, 5.f, myFill,
                    sf::Color(35, 20, 20), barFillCol);

                bool p1Owns = (g.baseOwner[l] == 1);
                if (iOwn || theyOwn)
                {
                    sf::RectangleShape tbody({ 12.f,12.f });
                    tbody.setOrigin(6.f, 6.f); tbody.setRotation(45.f);
                    tbody.setPosition(400.f, LANE_Y[l]);
                    tbody.setFillColor(ringCol); window.draw(tbody);
                    sf::RectangleShape barrel({ 18.f,4.f });
                    barrel.setOrigin(0.f, 2.f);
                    barrel.setPosition(p1Owns ? 406.f : 376.f, LANE_Y[l]);
                    barrel.setFillColor(ringCol); window.draw(barrel);
                    sf::CircleShape dot(3.f); dot.setOrigin(3.f, 3.f);
                    dot.setPosition(400.f, LANE_Y[l] - 20.f);
                    dot.setFillColor(ringCol); window.draw(dot);
                }
            }

            // Lane selected indicator
            if (screen == Screen::PLAYING)
            {
                sf::RectangleShape sel({ 4.f,58.f });
                sel.setPosition(BASE_W, LANE_Y[selLane] - 29.f);
                sel.setFillColor(sf::Color(255, 215, 50));
                window.draw(sel);
            }

            // Projectiles
            for (auto& p : projectiles)
            {
                float alpha = std::clamp(p.life / 0.5f, 0.f, 1.f);
                sf::Color glowCol = p.col; glowCol.a = (sf::Uint8)(80 * alpha);
                sf::CircleShape glow(p.radius * 2.2f);
                glow.setOrigin(p.radius * 2.2f, p.radius * 2.2f);
                glow.setPosition(p.x, p.y); glow.setFillColor(glowCol);
                window.draw(glow);
                sf::CircleShape core(p.radius);
                core.setOrigin(p.radius, p.radius);
                core.setPosition(p.x, p.y);
                sf::Color coreCol = p.col; coreCol.a = (sf::Uint8)(230 * alpha);
                core.setFillColor(coreCol); window.draw(core);
            }

            // Units
            for (int l = 0; l < 3; l++)
            {
                auto drawUnit = [&](const Unit& u, bool mine)
                    {
                        sf::Color col = mine ? MY_COL[u.type] : EN_COL[u.type];
                        sf::RectangleShape r({ 16.f,16.f });
                        r.setOrigin(8.f, 8.f); r.setPosition(u.x, LANE_Y[l]);
                        r.setFillColor(col); window.draw(r);
                        drawBar(window, u.x - 10.f, LANE_Y[l] - 22.f, 20.f, 4.f,
                            (float)u.hp / u.maxHp, sf::Color(45, 12, 12), sf::Color(55, 205, 65));
                    };
                for (auto& u : g.myUnits[l])    drawUnit(u, true);
                for (auto& u : g.enemyUnits[l]) drawUnit(u, false);
            }

            // Hit-flash events
            for (auto& fe : flashEvents)
            {
                float frac = fe.timer / FlashEvent::MAX;
                float radius = 10.f + (1.f - frac) * 10.f;
                sf::Uint8 alpha = (sf::Uint8)(frac * 210.f);

                sf::CircleShape ring(radius);
                ring.setOrigin(radius, radius);
                ring.setPosition(fe.x, fe.y);
                ring.setFillColor(sf::Color(0, 0, 0, 0));
                ring.setOutlineColor(sf::Color(255, 230, 80, alpha));
                ring.setOutlineThickness(3.f);
                window.draw(ring);

                float coreR = 6.f * frac;
                sf::CircleShape core(coreR);
                core.setOrigin(coreR, coreR);
                core.setPosition(fe.x, fe.y);
                core.setFillColor(sf::Color(255, 255, 200, (sf::Uint8)(frac * 180.f)));
                window.draw(core);
            }

            // Switch to fixed (non-shaken) view for overlays
            window.setView(window.getDefaultView());

            // ── Header bar ────────────────────────────────────────────────────
            // Rule: the name on the LEFT always belongs to the base on the LEFT
            // (P1), and the name on the RIGHT always belongs to the base on the
            // RIGHT (P2). So for P2, own name goes right, opponent goes left.
            {
                sf::RectangleShape hdr({ 800.f, HDR_H });
                hdr.setFillColor(sf::Color(8, 9, 18)); window.draw(hdr);
                sf::RectangleShape hdrLine({ 800.f, 1.f });
                hdrLine.setPosition(0.f, HDR_H - 1.f);
                hdrLine.setFillColor(sf::Color(28, 36, 75)); window.draw(hdrLine);

                // Determine which name/coins to put on each side
                const std::string& leftName = iAmP1 ? myName : opponentName;
                const std::string& rightName = iAmP1 ? opponentName : myName;
                int leftCoins = iAmP1 ? g.myCoins : g.enemyCoins;
                int rightCoins = iAmP1 ? g.enemyCoins : g.myCoins;
                sf::Color leftCol = iAmP1 ? MY_TEAM : EN_TEAM;
                sf::Color rightCol = iAmP1 ? EN_TEAM : MY_TEAM;

                // Left name + coins
                auto leftNT = mkText(leftName, 16, leftCol);
                leftNT.setPosition(8.f, 5.f); window.draw(leftNT);
                auto leftCT = mkText("$" + std::to_string(leftCoins), 11, sf::Color(200, 175, 55));
                leftCT.setPosition(8.f, 24.f); window.draw(leftCT);

                // VS centre
                auto vsT = mkText("VS", 14, sf::Color(55, 60, 95));
                centerText(vsT, 400.f, HDR_H * 0.5f); window.draw(vsT);

                // Right name + coins (right-aligned)
                auto rightNT = mkText(rightName, 16, rightCol);
                auto rightNB = rightNT.getLocalBounds();
                rightNT.setPosition(800.f - rightNB.width - rightNB.left - 8.f, 5.f);
                window.draw(rightNT);
                auto rightCT = mkText("$" + std::to_string(rightCoins), 11, sf::Color(200, 175, 55));
                auto rightCB = rightCT.getLocalBounds();
                rightCT.setPosition(800.f - rightCB.width - rightCB.left - 8.f, 24.f);
                window.draw(rightCT);
            }

            // Bottom UI Panel
            sf::RectangleShape panel({ 800.f, PANEL_H });
            panel.setPosition(0.f, PANEL_Y);
            panel.setFillColor(sf::Color(9, 9, 18)); window.draw(panel);
            sf::RectangleShape panelLine({ 800.f,2.f });
            panelLine.setPosition(0.f, PANEL_Y);
            panelLine.setFillColor(sf::Color(36, 50, 95)); window.draw(panelLine);

            // Row 1: HP bars + coins + bases + upgrade
            {
                drawBar(window, 6.f, PANEL_Y + 5.f, 130.f, 14.f, g.myBaseHP / 100.f,
                    sf::Color(22, 12, 12), sf::Color(48, 135, 255));
                auto hpMe = mkText("MY BASE " + std::to_string(g.myBaseHP), 10);
                hpMe.setPosition(8.f, PANEL_Y + 6.f); window.draw(hpMe);

                drawBar(window, 664.f, PANEL_Y + 5.f, 130.f, 14.f, g.enemyBaseHP / 100.f,
                    sf::Color(22, 12, 12), sf::Color(200, 48, 48));
                auto hpEn = mkText("ENEMY " + std::to_string(g.enemyBaseHP), 10);
                hpEn.setPosition(666.f, PANEL_Y + 6.f); window.draw(hpEn);

                auto coinsT = mkText("Coins: " + std::to_string(g.myCoins),
                    16, sf::Color(255, 215, 50));
                coinsT.setPosition(6.f, PANEL_Y + 24.f); window.draw(coinsT);

                sf::Color basCol = (g.basesOwned > 0) ? sf::Color(70, 200, 70)
                    : sf::Color(160, 70, 70);
                auto basT = mkText("Bases: " + std::to_string(g.basesOwned), 14, basCol);
                basT.setPosition(130.f, PANEL_Y + 26.f); window.draw(basT);

                if (g.myTurretLvl < 3)
                {
                    int  ucost = UPGRADE_COST_CLI[g.myTurretLvl];
                    bool canUpg = (g.myCoins >= ucost);
                    std::string lvlStr = (g.myTurretLvl == 0)
                        ? "No Turret" : "Turret Lv." + std::to_string(g.myTurretLvl);
                    sf::RectangleShape upgBtn({ 195.f,22.f });
                    upgBtn.setPosition(215.f, PANEL_Y + 23.f);
                    upgBtn.setFillColor(canUpg ? sf::Color(22, 42, 22) : sf::Color(18, 16, 24));
                    upgBtn.setOutlineColor(canUpg ? sf::Color(65, 185, 65) : sf::Color(48, 48, 65));
                    upgBtn.setOutlineThickness(1.5f); window.draw(upgBtn);
                    auto upgT = mkText("[U] Upgrade $" + std::to_string(ucost)
                        + "  (" + lvlStr + "→Lv." + std::to_string(g.myTurretLvl + 1) + ")",
                        10, canUpg ? sf::Color(70, 210, 70) : sf::Color(90, 90, 110));
                    upgT.setPosition(218.f, PANEL_Y + 27.f); window.draw(upgT);
                }
                else
                {
                    auto maxT = mkText("Turret: LV.MAX", 13, LVL_COL[2]);
                    maxT.setPosition(215.f, PANEL_Y + 26.f); window.draw(maxT);
                }
            }

            // Row 2: lane hint
            {
                const char* lnames[3] = { "TOP","MID","BOT" };
                auto laneT = mkText(
                    "[1]TOP  [2]MID  [3]BOT    Active lane: " + std::string(lnames[selLane]),
                    12, sf::Color(130, 135, 175));
                laneT.setPosition(6.f, PANEL_Y + 50.f); window.draw(laneT);
            }

            // Unit spawn buttons
            for (int i = 0; i < 5; i++)
            {
                float bx = BTN_START + i * (BTN_W + BTN_GAP);
                float by = PANEL_Y + 68.f;

                bool locked = (i >= 3 && g.basesOwned == 0);
                bool onCD = (g.spawnCDPct[i] > 0);
                bool canAff = (g.myCoins >= UCOST[i]);
                bool ready = !locked && !onCD && canAff;

                sf::RectangleShape btn({ BTN_W,BTN_H });
                btn.setPosition(bx, by);
                btn.setFillColor(ready ? sf::Color(28, 42, 68) : sf::Color(18, 18, 28));
                btn.setOutlineColor(ready ? MY_COL[i] : sf::Color(42, 45, 62));
                btn.setOutlineThickness(2.f); window.draw(btn);

                auto nmT = mkText(UNAME[i], 13, ready ? sf::Color::White : sf::Color(72, 75, 90));
                nmT.setPosition(bx + 5.f, by + 4.f); window.draw(nmT);
                auto coT = mkText("$" + std::to_string(UCOST[i]), 11, sf::Color(255, 215, 50));
                coT.setPosition(bx + 5.f, by + 21.f); window.draw(coT);
                {
                    int uHp = 20 + i * 10, uDmg = 2 + i * 2;
                    std::ostringstream oss; oss << std::fixed << std::setprecision(1);
                    oss << "HP" << uHp << " D" << uDmg << " " << SPAWN_CD_CLI[i] << "s CD";
                    auto stT = mkText(oss.str(), 10, sf::Color(88, 92, 122));
                    stT.setPosition(bx + 5.f, by + 38.f); window.draw(stT);
                }
                auto kyT = mkText(std::string("[") + UKEY[i] + "]", 17, sf::Color(148, 152, 205));
                { auto kb = kyT.getLocalBounds(); kyT.setPosition(bx + BTN_W - kb.width - kb.left - 6.f, by + BTN_H - 24.f); }
                window.draw(kyT);

                if (onCD && !locked)
                {
                    float cdFrac = g.spawnCDPct[i] / 100.f;
                    float overlayH = BTN_H * cdFrac;
                    sf::RectangleShape cdOver({ BTN_W,overlayH });
                    cdOver.setPosition(bx, by);
                    cdOver.setFillColor(sf::Color(0, 0, 0, 182)); window.draw(cdOver);
                    float remSec = SPAWN_CD_CLI[i] * cdFrac;
                    std::ostringstream oss; oss << std::fixed << std::setprecision(1);
                    oss << remSec << "s";
                    auto cdT = mkText(oss.str(), 22, sf::Color(210, 215, 235));
                    centerText(cdT, bx + BTN_W * .5f, by + overlayH * .5f); window.draw(cdT);
                    drawBar(window, bx, by + overlayH - 3.f, BTN_W * cdFrac, 3.f, 1.f,
                        sf::Color(0, 0, 0, 0), MY_COL[i]);
                }
                if (locked)
                {
                    sf::RectangleShape lockOver({ BTN_W,BTN_H });
                    lockOver.setPosition(bx, by);
                    lockOver.setFillColor(sf::Color(50, 0, 0, 200)); window.draw(lockOver);
                    auto lockT = mkText("LOCKED", 14, sf::Color(215, 65, 65));
                    centerText(lockT, bx + BTN_W * .5f, by + BTN_H * .5f - 10.f); window.draw(lockT);
                    auto hintT = mkText("Own a Capture Point", 9, sf::Color(145, 55, 55));
                    centerText(hintT, bx + BTN_W * .5f, by + BTN_H * .5f + 8.f); window.draw(hintT);
                }
                if (!locked && !onCD && !canAff)
                {
                    sf::RectangleShape dimOver({ BTN_W,BTN_H });
                    dimOver.setPosition(bx, by);
                    dimOver.setFillColor(sf::Color(0, 0, 0, 115)); window.draw(dimOver);
                }
            }

            // WIN / LOSE overlay
            if (screen == Screen::WIN || screen == Screen::LOSE)
            {
                sf::RectangleShape overlay({ 800.f,600.f });
                overlay.setFillColor(sf::Color(0, 0, 0, 168)); window.draw(overlay);
                bool won = (screen == Screen::WIN);
                auto big = mkText(won ? "VICTORY!" : "DEFEAT", 72,
                    won ? sf::Color(255, 215, 50) : sf::Color(215, 50, 50));
                centerText(big, 400, 210); window.draw(big);
                auto sub2 = mkText("Press R to play again", 22, sf::Color(168, 172, 215));
                centerText(sub2, 400, 320); window.draw(sub2);
                std::string resultLine = won
                    ? (myName + " defeated " + opponentName + "!")
                    : (opponentName + " defeated " + myName + "!");
                auto sub3 = mkText(resultLine, 16, sf::Color(140, 145, 180));
                centerText(sub3, 400, 360); window.draw(sub3);
            }

        } // end PLAYING block

        window.display();
    } // end main loop

    if (sock >= 0) close(sock);
    return 0;
}