# BaseBrawl

BaseBrawl is a fast-paced **online 1v1 real-time strategy game** built in **C++17** with a custom **client-server architecture**. Players battle across **three lanes**, deploy units, capture objectives, upgrade defenses, and destroy the enemy base.

The project includes:

- **Server** – handles matchmaking, authentication, networking, combat simulation, and game state updates  
- **Client** – renders the game using :contentReference[oaicite:0]{index=0} and sends player actions to the server

---

## Gameplay Overview

Each player controls a base on opposite sides of the map and fights across **3 lanes**:

- Top Lane  
- Mid Lane  
- Bottom Lane  

### Objective

Destroy the enemy base before yours is destroyed.

### Strategic Systems

- Spawn units into specific lanes
- Capture middle objectives
- Upgrade your main turret
- Control economy through map ownership
- Counter enemy pushes in real time

---

## Core Features

### Real-Time Multiplayer

- Online **1v1 PvP**
- Server-authoritative logic
- Live state synchronization
- Instant action commands

### Authentication System

- Register accounts
- Login system
- Duplicate login prevention
- Persistent in-memory user database

### Unit Roster

Five unique units with different costs, cooldowns, health, damage, and speed:

- Grunt
- Brute
- Tank
- Mage
- Siege

### Capture Points

Each lane contains a neutral base in the center.

Owning bases grants:

- Bonus coin income
- Map control
- Unlocks stronger units (Mage / Siege)

### Turret Upgrade System

Upgrade your main turret up to Level 3:

- Increased range
- Faster attacks
- Higher damage

### Combat Feedback

Client visuals include:

- HP bars
- Cooldown overlays
- Projectile effects
- Hit flashes
- Screen shake
- Win / lose screens

---

## Tech Stack

- **Language:** C++17  
- **Graphics:** :contentReference[oaicite:1]{index=1}  
- **Networking:** POSIX sockets + TCP  
- **Server I/O:** Linux epoll event loop  
- **Build System:** CMake

---

### Build System

- CMake

---

## Project Structure

```text
BaseBrawl/
├── server.cpp        # Game server
├── client.cpp        # SFML game client
├── CMakeLists.txt

## Dependencies

## Required

### Linux / Ubuntu / Debian

```bash
sudo apt update
sudo apt install g++ cmake make libsfml-dev