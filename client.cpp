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

enum class Screen { MENU, CONNECTING, WAITING, PLAYING, WIN, LOSE };

struct Unit { float x; int hp, maxHp, type; };

struct GameData 
{
    int myCoins = 0, enemyCoins = 0;
    int myBaseHP = 100, enemyBaseHP = 100;
    int myTurretLvl = 0, enemyTurretLvl = 0;
    int baseOwner  [3] = {};
    int baseCapture[3] = {};
    int spawnCDPct [5] = {};          // 0=ready, 100=just-spawned
    std::vector<Unit> myUnits[3], enemyUnits[3];
    int myID      = 0;
    int basesOwned = 0;               // derived: count of bases I own
};

static const char*  UNAME[5]         = {"Grunt","Brute","Tank","Mage","Siege"};
static const char*  UKEY [5]         = {"Q","W","E","R","T"};
static const int    UCOST[5]         = {5, 7, 10, 12, 15};
static const float  SPAWN_CD_CLI[5]  = {2.f, 3.f, 4.5f, 6.f, 8.f};
static const int    UPGRADE_COST_CLI[3] = {20, 35, 55};

static const sf::Color MY_COL[5] = 
{
    {70,150,255},{70,210,110},{230,175,50},{170,70,215},{230,70,70}
};
static const sf::Color EN_COL[5] = 
{
    {255,100,70},{255,65,140},{255,195,65},{195,95,255},{140,40,40}
};
// Turret level colours: green → gold → orange-red
static const sf::Color LVL_COL[3] = 
{
    {100,210,110},{230,185,50},{230,100,55}
};

static void drawBar(sf::RenderWindow& w,
                    float x, float y, float bw, float bh, float fill,
                    sf::Color bg, sf::Color fg)
{
    sf::RectangleShape back({bw, bh});
    back.setPosition(x,y); back.setFillColor(bg); w.draw(back);
    if (fill > 0.f) {
        sf::RectangleShape front({bw * std::clamp(fill,0.f,1.f), bh});
        front.setPosition(x,y); front.setFillColor(fg); w.draw(front);
    }
}

static void centerText(sf::Text& t, float x, float y) 
{
    auto b = t.getLocalBounds();
    t.setOrigin(b.left + b.width*.5f, b.top + b.height*.5f);
    t.setPosition(x, y);
}

int main()
{
    sf::RenderWindow window(sf::VideoMode(800,600), "BaseBrawl v3", sf::Style::Close);
    window.setFramerateLimit(60);

    sf::Font font; bool hasFont = false;
    for (auto& fp : std::vector<std::string>{
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
        "C:/Windows/Fonts/arial.ttf"
    }) { if (font.loadFromFile(fp)) { hasFont = true; break; } }

    // Text factory
    auto mkText = [&](const std::string& s, unsigned sz,
                      sf::Color c = sf::Color::White) {
        sf::Text t;
        if (hasFont) t.setFont(font);
        t.setString(s); t.setCharacterSize(sz); t.setFillColor(c);
        return t;
    };

    Screen     screen     = Screen::MENU;
    GameData   g;
    int        sock       = -1;
    std::string recvBuf;

    int   selLane    = 1;
    float upgDebounce = 0.f;
    float dotTimer   = 0.f;
    int   dotCount   = 0;

    // Layout
    const float LANE_Y[3]  = {128.f, 255.f, 382.f};
    const float PANEL_Y    = 430.f;
    const float PANEL_H    = 600.f - PANEL_Y;   // 170
    const float BASE_W     = 54.f;               // side-panel width
    const float BTN_W      = 148.f;
    const float BTN_H      =  82.f;
    const float BTN_GAP    =   4.f;
    const float BTN_START  =   8.f;

    sf::Clock clk;

    // Connect helper
    auto doConnect = [&]() -> bool 
        {
        sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return false;
        sockaddr_in serv{};
        serv.sin_family = AF_INET;
        serv.sin_port   = htons(PORT);
        inet_pton(AF_INET, SERVER_IP, &serv.sin_addr);
        if (connect(sock, (sockaddr*)&serv, sizeof(serv)) < 0)
            { close(sock); sock = -1; return false; }
        fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK);
        return true;
    };

    // State-machine message handler
    auto handleLine = [&](const std::string& line) 
        {
        if (line.empty()) return;
        std::istringstream ss(line);
        std::string tok; ss >> tok;

        if (tok == "WAITING") 
        {
            screen = Screen::WAITING;
        } else if (tok == "START") 
{
            int pid; ss >> pid;
            g = GameData{}; g.myID = pid;
            screen = Screen::PLAYING;
        } else if (tok == "WIN")  { screen = Screen::WIN; }
          else if (tok == "LOSE") { screen = Screen::LOSE; }
        else if (tok == "STATE") 
        {
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
                        g.myUnits[lane].push_back({(float)x, hp, 20+type*10, type});
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
                        g.enemyUnits[lane].push_back({(float)x, hp, 20+type*10, type});
                }
            }
            g.basesOwned = 0;
            for (int l = 0; l < 3; l++)
                if (g.baseOwner[l] == g.myID) g.basesOwned++;
        }
    };

    while (window.isOpen()) 
    {
        float dt = clk.restart().asSeconds();
        upgDebounce = std::max(0.f, upgDebounce - dt);
        dotTimer   += dt;
        if (dotTimer > 0.45f) { dotTimer = 0; dotCount = (dotCount+1)%4; }

        // Events
        sf::Event ev;
        while (window.pollEvent(ev)) 
        {
            if (ev.type == sf::Event::Closed) window.close();
            if (ev.type == sf::Event::KeyPressed) 
            {
                if (screen == Screen::MENU && ev.key.code == sf::Keyboard::Return)
                    screen = Screen::CONNECTING;
                if ((screen == Screen::WIN || screen == Screen::LOSE)
                    && ev.key.code == sf::Keyboard::R) 
                {
                    if (sock >= 0) { close(sock); sock = -1; }
                    g = GameData{}; recvBuf.clear();
                    screen = Screen::CONNECTING;
                }
            }
        }

        if (screen == Screen::CONNECTING)
            screen = doConnect() ? Screen::WAITING : Screen::MENU;

        // Input: playing
        if (screen == Screen::PLAYING && window.hasFocus()) 
        {
            if (sf::Keyboard::isKeyPressed(sf::Keyboard::Num1)) selLane = 0;
            if (sf::Keyboard::isKeyPressed(sf::Keyboard::Num2)) selLane = 1;
            if (sf::Keyboard::isKeyPressed(sf::Keyboard::Num3)) selLane = 2;

            // Unit spawns – server enforces cooldown, client just sends request
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
                    g.spawnCDPct[b.t] = 99; // optimistic (corrected by next STATE)
                }
            }

            // Upgrade turret
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
            char buf[4096]; int r = recv(sock, buf, sizeof(buf)-1, 0);
            if (r > 0) 
            {
                buf[r] = 0; recvBuf += buf;
                size_t pos;
                while ((pos = recvBuf.find('\n')) != std::string::npos) 
                {
                    handleLine(recvBuf.substr(0, pos));
                    recvBuf.erase(0, pos+1);
                }
            }
        }

        window.clear(sf::Color(13,14,24));

        // MENU 
        if (screen == Screen::MENU) 
        {
            for (int l = 0; l < 3; l++) 
            {
                sf::RectangleShape s({800.f,60.f});
                s.setPosition(0,LANE_Y[l]-30.f);
                s.setFillColor(sf::Color(20,24,42,190)); window.draw(s);
            }
            auto title = mkText("BaseBrawl", 72, sf::Color(255,210,50));
            centerText(title, 400, 155); window.draw(title);

            auto sub = mkText("3-Lane Real-Time Strategy", 20, sf::Color(148,152,196));
            centerText(sub, 400, 248); window.draw(sub);

            auto prompt = mkText("Press ENTER to find a match" + std::string(dotCount,'.'),
                                 22, sf::Color(75,202,255));
            centerText(prompt, 400, 322); window.draw(prompt);

            std::vector<std::string> hints = 
            {
                "1/2/3  Select lane     Q/W/E/R/T  Spawn unit     U  Upgrade turret",
                "Mage & Siege unlock once you own a Capture Point",
                "Capture Points give bonus coins — own all 3 to dominate"
            };
            for (int i = 0; i < (int)hints.size(); i++) 
            {
                auto h = mkText(hints[i], 12, sf::Color(88,92,125));
                centerText(h, 400, 400.f + i*20.f); window.draw(h);
            }

        // WAITING
        } 
        else if (screen == Screen::WAITING || screen == Screen::CONNECTING) 
        {
            auto t = mkText("Finding opponent" + std::string(dotCount,'.'),
                            30, sf::Color(75,202,255));
            centerText(t, 400, 270); window.draw(t);
            auto t2 = mkText("Close window to cancel", 14, sf::Color(88,92,125));
            centerText(t2, 400, 330); window.draw(t2);

        // PLAYING / WIN / LOSE
        } 
        else 
        {

            //Lane backgrounds
            for (int l = 0; l < 3; l++) 
            {
                sf::RectangleShape lane({800.f - BASE_W*2.f, 58.f});
                lane.setPosition(BASE_W, LANE_Y[l]-29.f);
                lane.setFillColor(sf::Color(20,24,42)); window.draw(lane);
                for (float oy : {-29.f, 29.f}) 
                {
                    sf::RectangleShape border({800.f - BASE_W*2.f, 1.f});
                    border.setPosition(BASE_W, LANE_Y[l]+oy);
                    border.setFillColor(sf::Color(36,42,70)); window.draw(border);
                }
            }

            // Side base panels
            bool iAmP1 = (g.myID == 1);

            sf::RectangleShape p1Panel({BASE_W, PANEL_Y-48.f});
            p1Panel.setPosition(0, 48.f);
            p1Panel.setFillColor(sf::Color(14,30,68)); window.draw(p1Panel);

            sf::RectangleShape p2Panel({BASE_W, PANEL_Y-48.f});
            p2Panel.setPosition(800.f-BASE_W, 48.f);
            p2Panel.setFillColor(sf::Color(68,14,14)); window.draw(p2Panel);

            // Vertical HP bars on inner edge of each panel
            {
                float barH     = PANEL_Y - 90.f;
                float barTop   = 68.f;
                float p1HpFrac = iAmP1 ? g.myBaseHP/100.f : g.enemyBaseHP/100.f;
                float p2HpFrac = iAmP1 ? g.enemyBaseHP/100.f : g.myBaseHP/100.f;

                // P1 bar (right edge of left panel)
                drawBar(window, BASE_W-8.f, barTop + barH*(1.f-p1HpFrac),
                        6.f, barH*p1HpFrac, 1.f,
                        sf::Color(0,0,0,0), sf::Color(50,140,255));
                // P2 bar (left edge of right panel)
                drawBar(window, 800.f-BASE_W+2.f, barTop + barH*(1.f-p2HpFrac),
                        6.f, barH*p2HpFrac, 1.f,
                        sf::Color(0,0,0,0), sf::Color(200,50,50));
            }

            // Main base turret icons on panels
            // Helper - draw turret icon (diamond + barrel) at (cx,cy)
            auto drawTurretIcon = [&](float cx, float cy, int level,
                                      sf::Color col, bool facingRight) {
                if (level == 0) 
                {
                    // Ghosted "no turret" placeholder
                    sf::CircleShape ghost(9.f);
                    ghost.setOrigin(9.f,9.f); ghost.setPosition(cx,cy);
                    ghost.setFillColor(sf::Color(0,0,0,0));
                    ghost.setOutlineColor(sf::Color(55,60,85));
                    ghost.setOutlineThickness(2.f); window.draw(ghost);
                    return;
                }
                // Body
                sf::RectangleShape body({14.f,14.f});
                body.setOrigin(7.f,7.f); body.setRotation(45.f);
                body.setPosition(cx,cy); body.setFillColor(col);
                window.draw(body);
                // Barrel
                sf::RectangleShape barrel({20.f, 5.f});
                barrel.setOrigin(0.f, 2.5f);
                barrel.setPosition(facingRight ? cx+7.f : cx-27.f, cy);
                barrel.setFillColor(col); window.draw(barrel);
                // Level dot row
                for (int d = 0; d < level; d++) 
                {
                    sf::CircleShape dot(3.f);
                    dot.setOrigin(3.f,3.f);
                    dot.setPosition(cx - (level-1)*4.f + d*8.f, cy+16.f);
                    dot.setFillColor(col); window.draw(dot);
                }
            };

            float midPanelY  = (PANEL_Y - 48.f) * 0.5f + 48.f;
            // P1 turret: left panel, barrel faces RIGHT
            int p1TurrLvl = iAmP1 ? g.myTurretLvl   : g.enemyTurretLvl;
            int p2TurrLvl = iAmP1 ? g.enemyTurretLvl : g.myTurretLvl;
            sf::Color p1TC = (p1TurrLvl > 0) ? LVL_COL[p1TurrLvl-1] : sf::Color(60,65,90);
            sf::Color p2TC = (p2TurrLvl > 0) ? LVL_COL[p2TurrLvl-1] : sf::Color(60,65,90);

            drawTurretIcon(BASE_W*0.5f,            midPanelY, p1TurrLvl, p1TC, true);
            drawTurretIcon(800.f-BASE_W*0.5f, midPanelY, p2TurrLvl, p2TC, false);

            // HP numbers over panels
            {
                auto p1HpT = mkText(std::to_string(iAmP1 ? g.myBaseHP : g.enemyBaseHP),
                                    13, sf::Color(180,185,220));
                centerText(p1HpT, BASE_W*0.5f, 56.f); window.draw(p1HpT);

                auto p2HpT = mkText(std::to_string(iAmP1 ? g.enemyBaseHP : g.myBaseHP),
                                    13, sf::Color(180,185,220));
                centerText(p2HpT, 800.f-BASE_W*0.5f, 56.f); window.draw(p2HpT);
            }

            // Lane capture points + turrets
            for (int l = 0; l < 3; l++) 
            {
                bool p1Owns = (g.baseOwner[l] == 1);
                bool p2Owns = (g.baseOwner[l] == 2);
                bool neutral = !p1Owns && !p2Owns;

                sf::Color ringCol = neutral  ? sf::Color(55,60,90)
                                  : p1Owns   ? sf::Color(45,95,185)
                                             : sf::Color(175,40,40);

                // Outer ring
                sf::CircleShape ring(17.f);
                ring.setOrigin(17.f,17.f); ring.setPosition(400.f, LANE_Y[l]);
                ring.setFillColor(sf::Color(13,14,24));
                ring.setOutlineColor(ringCol); ring.setOutlineThickness(3.f);
                window.draw(ring);

                // Capture progress arc approximated as bar
                float fill = (g.baseCapture[l] + 100.f) / 200.f;
                drawBar(window, 368.f, LANE_Y[l]+21.f, 64.f, 5.f, fill,
                        sf::Color(45,25,25), sf::Color(70,155,255));

                // Turret icon at capture point (when owned)
                if (!neutral) 
                {
                    // Diamond body
                    sf::RectangleShape tbody({12.f,12.f});
                    tbody.setOrigin(6.f,6.f); tbody.setRotation(45.f);
                    tbody.setPosition(400.f, LANE_Y[l]);
                    tbody.setFillColor(ringCol); window.draw(tbody);

                    // Barrel points toward enemy territory
                    sf::RectangleShape barrel({18.f, 4.f});
                    barrel.setOrigin(0.f, 2.f);
                    barrel.setPosition(p1Owns ? 406.f : 376.f, LANE_Y[l]);
                    barrel.setFillColor(ringCol); window.draw(barrel);

                    // "owner" small dot
                    sf::CircleShape dot(3.f); dot.setOrigin(3.f,3.f);
                    dot.setPosition(400.f, LANE_Y[l]-20.f);
                    dot.setFillColor(ringCol); window.draw(dot);
                }
            }

            // Lane selected indicator
            if (screen == Screen::PLAYING)
            {
                sf::RectangleShape sel({4.f, 58.f});
                sel.setPosition(BASE_W, LANE_Y[selLane]-29.f);
                sel.setFillColor(sf::Color(255,215,50));
                window.draw(sel);
            }

            // Units
            for (int l = 0; l < 3; l++)
            {
                auto drawUnit = [&](const Unit& u, bool mine) 
                    {
                    sf::Color col = mine ? MY_COL[u.type] : EN_COL[u.type];
                    sf::RectangleShape r({16.f,16.f});
                    r.setOrigin(8.f,8.f); r.setPosition(u.x, LANE_Y[l]);
                    r.setFillColor(col); window.draw(r);
                    drawBar(window, u.x-10.f, LANE_Y[l]-22.f, 20.f, 4.f,
                            (float)u.hp/u.maxHp,
                            sf::Color(45,12,12), sf::Color(55,205,65));
                };
                for (auto& u : g.myUnits[l])    drawUnit(u,true);
                for (auto& u : g.enemyUnits[l]) drawUnit(u,false);
            }

            // Bottom UI Panel
            sf::RectangleShape panel({800.f, PANEL_H});
            panel.setPosition(0.f, PANEL_Y);
            panel.setFillColor(sf::Color(9,9,18)); window.draw(panel);

            sf::RectangleShape panelLine({800.f, 2.f});
            panelLine.setPosition(0.f, PANEL_Y);
            panelLine.setFillColor(sf::Color(36,50,95)); window.draw(panelLine);

            // Row 1: HP bars + coins + bases + upgrade
            {
                // My base HP bar
                drawBar(window, 6.f, PANEL_Y+5.f, 130.f, 14.f, g.myBaseHP/100.f,
                        sf::Color(22,12,12), sf::Color(48,135,255));
                auto hpMe = mkText("MY BASE " + std::to_string(g.myBaseHP), 10);
                hpMe.setPosition(8.f, PANEL_Y+6.f); window.draw(hpMe);

                // Enemy base HP bar
                drawBar(window, 664.f, PANEL_Y+5.f, 130.f, 14.f, g.enemyBaseHP/100.f,
                        sf::Color(22,12,12), sf::Color(200,48,48));
                auto hpEn = mkText("ENEMY " + std::to_string(g.enemyBaseHP), 10);
                hpEn.setPosition(666.f, PANEL_Y+6.f); window.draw(hpEn);

                // Coins
                auto coinsT = mkText("Coins: " + std::to_string(g.myCoins),
                                     16, sf::Color(255,215,50));
                coinsT.setPosition(6.f, PANEL_Y+24.f); window.draw(coinsT);

                // Bases owned indicator
                sf::Color basCol = (g.basesOwned > 0) ? sf::Color(70,200,70)
                                                       : sf::Color(160,70,70);
                auto basT = mkText("Bases:" + std::to_string(g.basesOwned), 14, basCol);
                basT.setPosition(130.f, PANEL_Y+26.f); window.draw(basT);

                // Turret upgrade section
                if (g.myTurretLvl < 3) 
                {
                    int   ucost  = UPGRADE_COST_CLI[g.myTurretLvl];
                    bool  canUpg = (g.myCoins >= ucost);
                    std::string lvlStr = (g.myTurretLvl == 0)
                                       ? "No Turret" : "Turret Lv." + std::to_string(g.myTurretLvl);

                    sf::RectangleShape upgBtn({195.f, 22.f});
                    upgBtn.setPosition(215.f, PANEL_Y+23.f);
                    upgBtn.setFillColor(canUpg ? sf::Color(22,42,22) : sf::Color(18,16,24));
                    upgBtn.setOutlineColor(canUpg ? sf::Color(65,185,65) : sf::Color(48,48,65));
                    upgBtn.setOutlineThickness(1.5f); window.draw(upgBtn);

                    auto upgT = mkText("[U] Upgrade $" + std::to_string(ucost)
                                       + "  (" + lvlStr + "→Lv."
                                       + std::to_string(g.myTurretLvl+1) + ")",
                                       10, canUpg ? sf::Color(70,210,70) : sf::Color(90,90,110));
                    upgT.setPosition(218.f, PANEL_Y+27.f); window.draw(upgT);
                } 
                else 
                {
                    auto maxT = mkText("Turret: LV.MAX", 13, LVL_COL[2]);
                    maxT.setPosition(215.f, PANEL_Y+26.f); window.draw(maxT);
                }
            }

            // Row 2: lane hint
            {
                const char* lnames[3] = {"TOP","MID","BOT"};
                auto laneT = mkText(
                    "[1]TOP  [2]MID  [3]BOT    Active lane: " + std::string(lnames[selLane]),
                    12, sf::Color(130,135,175));
                laneT.setPosition(6.f, PANEL_Y+50.f); window.draw(laneT);
            }

            // Unit spawn buttons
            for (int i = 0; i < 5; i++) 
            {
                float bx = BTN_START + i*(BTN_W + BTN_GAP);
                float by = PANEL_Y + 68.f;

                bool locked = (i >= 3 && g.basesOwned == 0);
                bool onCD   = (g.spawnCDPct[i] > 0);
                bool canAff = (g.myCoins >= UCOST[i]);
                bool ready  = !locked && !onCD && canAff;

                // Button background
                sf::RectangleShape btn({BTN_W, BTN_H});
                btn.setPosition(bx, by);
                btn.setFillColor(ready ? sf::Color(28,42,68) : sf::Color(18,18,28));
                btn.setOutlineColor(ready ? MY_COL[i] : sf::Color(42,45,62));
                btn.setOutlineThickness(2.f);
                window.draw(btn);

                // Name
                auto nmT = mkText(UNAME[i], 13, ready ? sf::Color::White : sf::Color(72,75,90));
                nmT.setPosition(bx+5.f, by+4.f); window.draw(nmT);

                // Cost
                auto coT = mkText("$" + std::to_string(UCOST[i]), 11, sf::Color(255,215,50));
                coT.setPosition(bx+5.f, by+21.f); window.draw(coT);

                // Stats (HP / damage / base CD)
                {
                    int uHp = 20+i*10, uDmg = 2+i*2;
                    std::ostringstream oss; oss << std::fixed << std::setprecision(1);
                    oss << "HP" << uHp << " D" << uDmg << " " << SPAWN_CD_CLI[i] << "s CD";
                    auto stT = mkText(oss.str(), 10, sf::Color(88,92,122));
                    stT.setPosition(bx+5.f, by+38.f); window.draw(stT);
                }

                // Key hint (bottom right)
                auto kyT = mkText(std::string("[") + UKEY[i] + "]", 17,
                                  sf::Color(148,152,205));
                {
                    auto kb = kyT.getLocalBounds();
                    kyT.setPosition(bx + BTN_W - kb.width - kb.left - 6.f,
                                    by + BTN_H - 24.f);
                }
                window.draw(kyT);

                // Cooldown overlay
                if (onCD && !locked) 
                {
                    float cdFrac   = g.spawnCDPct[i] / 100.f;
                    float overlayH = BTN_H * cdFrac;

                    sf::RectangleShape cdOver({BTN_W, overlayH});
                    cdOver.setPosition(bx, by);
                    cdOver.setFillColor(sf::Color(0,0,0,182));
                    window.draw(cdOver);

                    // Remaining-time text, centred on dark area
                    float remSec = SPAWN_CD_CLI[i] * cdFrac;
                    std::ostringstream oss; oss << std::fixed << std::setprecision(1);
                    oss << remSec << "s";
                    auto cdT = mkText(oss.str(), 22, sf::Color(210,215,235));
                    centerText(cdT, bx + BTN_W*.5f,
                               by + overlayH*.5f);
                    window.draw(cdT);

                    // Thin progress bar at bottom of overlay
                    drawBar(window, bx, by+overlayH-3.f, BTN_W*cdFrac, 3.f, 1.f,
                            sf::Color(0,0,0,0), MY_COL[i]);
                }

                // Locked overlay (Mage/Siege, no bases)
                if (locked) 
                {
                    sf::RectangleShape lockOver({BTN_W, BTN_H});
                    lockOver.setPosition(bx, by);
                    lockOver.setFillColor(sf::Color(50,0,0,200));
                    window.draw(lockOver);

                    auto lockT = mkText("LOCKED", 14, sf::Color(215,65,65));
                    centerText(lockT, bx+BTN_W*.5f, by+BTN_H*.5f-10.f);
                    window.draw(lockT);

                    auto hintT = mkText("Own a Capture Point", 9, sf::Color(145,55,55));
                    centerText(hintT, bx+BTN_W*.5f, by+BTN_H*.5f+8.f);
                    window.draw(hintT);
                }

                //  Cant afford dim
                if (!locked && !onCD && !canAff) 
                {
                    sf::RectangleShape dimOver({BTN_W, BTN_H});
                    dimOver.setPosition(bx, by);
                    dimOver.setFillColor(sf::Color(0,0,0,115));
                    window.draw(dimOver);
                }
            }

            // WIN / LOSE overlay
            if (screen == Screen::WIN || screen == Screen::LOSE) 
            {
                sf::RectangleShape overlay({800.f,600.f});
                overlay.setFillColor(sf::Color(0,0,0,168));
                window.draw(overlay);
                bool won = (screen == Screen::WIN);
                auto big = mkText(won ? "VICTORY!" : "DEFEAT", 72,
                                  won ? sf::Color(255,215,50) : sf::Color(215,50,50));
                centerText(big, 400, 230); window.draw(big);
                auto sub = mkText("Press R to play again", 22, sf::Color(168,172,215));
                centerText(sub, 400, 345); window.draw(sub);
            }
        } // end PLAYING block

        window.display();
    } // end main loop

    if (sock >= 0) close(sock);
    return 0;
}
