## 🕹️ Project Overview: Networked ASCII Battle Game (C Implementation)

This project is a **multiplayer, text-based battle game** built using **TCP socket programming in C**, completed as part of **COMPSCI 3N03 (Computer Networks and Security)** at McMaster University.

The goal of the assignment was to apply core networking concepts — such as TCP connections, concurrency, and real-time state synchronization — in a fun and interactive way by designing a **grid-based battle game** with simple commands.

---

## 🔗 How it Works:

- A **server** maintains a shared 5x5 ASCII grid and listens for up to **4 concurrent client connections**.
- Each **client** connects via TCP and is assigned a player symbol (A, B, C, D).
- Players interact with the game using **text-based commands** like:
  - `MOVE <UP|DOWN|LEFT|RIGHT>` — to navigate the grid
  - `ATTACK` — to attack adjacent players (dealing damage)
  - `QUIT` — to disconnect from the game

Game state (including player positions, HP, and obstacles) is broadcast to all clients after each action, keeping everyone's view in sync.

---

## 🧠 Technologies & Concepts Used:

- **C Programming**
- **TCP Sockets**
- **Multithreading with `pthread`** to handle multiple clients
- **Mutex locks** for safe concurrent access to the shared game state
- ASCII-based rendering of the grid and players

---

## 📝 Assignment Requirements Met:

- ✅ Custom game protocol design
- ✅ Server-client architecture using TCP
- ✅ Real-time multiplayer synchronization
- ✅ ASCII UI for gameplay visualization
- ✅ Concurrency using threads
- ✅ Clean disconnection handling

---

## 🎮 Gameplay Summary:

- The grid includes **randomly generated obstacles** and player avatars.
- Movement is restricted by bounds and obstacles.
- Attacks reduce an opponent’s HP by 20.
- A player is removed from the game when their HP reaches zero.
- The server continuously updates all connected players with the current game state.
