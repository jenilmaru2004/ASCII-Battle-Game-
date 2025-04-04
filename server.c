/*
 * TCP-based ASCII Battle Game Server
 * This server accepts up to 4 clients and manages a 5x5 grid with obstacles and players.
 * Each client is handled in a separate thread and can send commands: MOVE, ATTACK, QUIT.
 * The server broadcasts the game state (grid + player info) to all clients after each valid action.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>

// Constants for game configuration
#define GRID_SIZE 5
#define MAX_PLAYERS 4
#define MAX_HP 100
#define DAMAGE 20

// Structure to hold player info
// -------- Data Structures and Global Variables --------
typedef struct {
    char symbol;       // Unique symbol representing the player (e.g., 'A', 'B', 'C', 'D')
    int row, col;      // Current position on the grid
    int hp;            // Hit points
    int socket_fd;     // Socket file descriptor for the player's connection
    int active;        // Whether this player slot is active (1) or free (0)
} Player;

// Global game state
Player players[MAX_PLAYERS];
int player_count = 0;
int obstacles[GRID_SIZE][GRID_SIZE]; // 0 for empty, 1 for obstacle

// Mutex for synchronizing access to game state
pthread_mutex_t state_lock;

// Helper function to send the current game state to all connected clients.
// Assumes state_lock is already held by the caller.
// -------- Helper Functions --------
void broadcast_state_locked() {
    char state_msg[512];
    int offset = 0;
    // Build grid representation
    offset += snprintf(state_msg + offset, sizeof(state_msg) - offset, "Grid:\n");
    for (int r = 0; r < GRID_SIZE; ++r) {
        for (int c = 0; c < GRID_SIZE; ++c) {
            char cell = '.'; // default empty cell
            if (obstacles[r][c] == 1) {
                cell = 'X'; // obstacle
            }
            // Check if any active player is at this cell
            for (int p = 0; p < MAX_PLAYERS; ++p) {
                if (players[p].active && players[p].row == r && players[p].col == c) {
                    cell = players[p].symbol;
                    break;
                }
            }
            // Add cell to message (with a space for readability)
            offset += snprintf(state_msg + offset, sizeof(state_msg) - offset, "%c ", cell);
        }
        offset += snprintf(state_msg + offset, sizeof(state_msg) - offset, "\n");
    }
    // Build players info section
    offset += snprintf(state_msg + offset, sizeof(state_msg) - offset, "Players:\n");
    for (int p = 0; p < MAX_PLAYERS; ++p) {
        if (players[p].active) {
            offset += snprintf(state_msg + offset, sizeof(state_msg) - offset,
                                "%c: HP=%d at (%d,%d)\n",
                                players[p].symbol, players[p].hp,
                                players[p].row, players[p].col);
        }
    }

    // Send the state message to all active players.
    // Handle if any client disconnects during send.
    for (int p = 0; p < MAX_PLAYERS; ++p) {
        if (!players[p].active) continue;
        int sock = players[p].socket_fd;
        if (sock < 0) continue;
        // Attempt to send the state message
        ssize_t bytes = send(sock, state_msg, strlen(state_msg), 0);
        if (bytes < 0) {
            // Send failed: likely client disconnected
            fprintf(stderr, "Broadcast: client %c send failed, removing player\n", players[p].symbol);
            // Remove this player from game
            close(sock);
            players[p].socket_fd = -1;
            players[p].active = 0;
            player_count--;
            // Note: We do not attempt to re-send current state to this client (they're gone).
            // We will handle broadcasting the updated state (with this player removed) 
            // in the next command cycle or below if needed.
        }
    }
}

// Thread function to handle communication with a client
// -------- Thread Routine for Client Handling --------
void *client_handler(void *arg) {
    int player_index = (intptr_t) arg;
    int sockfd = players[player_index].socket_fd;
    char buffer[256];
    // Notify this client of their symbol
    char welcome_msg[64];
    snprintf(welcome_msg, sizeof(welcome_msg), "Welcome to the game! You are player %c.\n", players[player_index].symbol);
    send(sockfd, welcome_msg, strlen(welcome_msg), 0);

    // Main loop to receive and handle commands from this client
    while (1) {
        memset(buffer, 0, sizeof(buffer));
        ssize_t n = recv(sockfd, buffer, sizeof(buffer) - 1, 0);
        if (n <= 0) {
            // If recv returns 0 or negative, the client disconnected or error occurred
            pthread_mutex_lock(&state_lock);
            if (players[player_index].active) {
                // The client disconnected unexpectedly (did not send QUIT)
                // Remove player from the game
                close(players[player_index].socket_fd);
                players[player_index].socket_fd = -1;
                players[player_index].active = 0;
                player_count--;
                // Notify other players that this player has left
                broadcast_state_locked();
            }
            pthread_mutex_unlock(&state_lock);
            break;
        }
        // Null-terminate the input and strip newline characters
        buffer[n] = '\0';
        // Remove any trailing newline or carriage return
        char *newline = strpbrk(buffer, "\r\n");
        if (newline) *newline = '\0';

        // Parse and handle the command
        if (strncasecmp(buffer, "MOVE", 4) == 0) {
            // Format: MOVE <DIRECTION>
            char direction[16];
            if (sscanf(buffer + 4, "%15s", direction) != 1) {
                // No direction provided
                const char *msg = "Usage: MOVE <UP|DOWN|LEFT|RIGHT>\n";
                send(sockfd, msg, strlen(msg), 0);
                continue;
            }
            // Normalize direction to uppercase
            for (char *d = direction; *d; ++d) *d = toupper(*d);
            int newR = players[player_index].row;
            int newC = players[player_index].col;
            if (strcmp(direction, "UP") == 0) {
                newR -= 1;
            } else if (strcmp(direction, "DOWN") == 0) {
                newR += 1;
            } else if (strcmp(direction, "LEFT") == 0) {
                newC -= 1;
            } else if (strcmp(direction, "RIGHT") == 0) {
                newC += 1;
            } else {
                const char *msg = "Invalid direction. Use UP, DOWN, LEFT, or RIGHT.\n";
                send(sockfd, msg, strlen(msg), 0);
                continue;
            }
            // Attempt move within a locked state update
            pthread_mutex_lock(&state_lock);
            // Check bounds and obstacles/players
            if (newR < 0 || newR >= GRID_SIZE || newC < 0 || newC >= GRID_SIZE) {
                // Out of bounds
                const char *msg = "Move blocked: out of bounds.\n";
                send(sockfd, msg, strlen(msg), 0);
                // No state change, no broadcast
            } else if (obstacles[newR][newC] == 1) {
                // Target cell has an obstacle
                const char *msg = "Move blocked: obstacle in the way.\n";
                send(sockfd, msg, strlen(msg), 0);
                // No state change
            } else {
                // Check if another player occupies the target cell
                int occupied = 0;
                for (int q = 0; q < MAX_PLAYERS; ++q) {
                    if (players[q].active && q != player_index &&
                        players[q].row == newR && players[q].col == newC) {
                        occupied = 1;
                        break;
                    }
                }
                if (occupied) {
                    const char *msg = "Move blocked: another player is in that cell.\n";
                    send(sockfd, msg, strlen(msg), 0);
                } else {
                    // Move is valid, update player's position
                    players[player_index].row = newR;
                    players[player_index].col = newC;
                    // Broadcast new state to all players
                    broadcast_state_locked();
                }
            }
            pthread_mutex_unlock(&state_lock);
        } else if (strcasecmp(buffer, "ATTACK") == 0) {
            pthread_mutex_lock(&state_lock);
            // Determine if any adjacent players exist and apply damage
            int attackerR = players[player_index].row;
            int attackerC = players[player_index].col;
            int hit = 0;
            for (int q = 0; q < MAX_PLAYERS; ++q) {
                if (players[q].active && q != player_index) {
                    int dr = players[q].row - attackerR;
                    int dc = players[q].col - attackerC;
                    // Check adjacency (Manhattan distance 1)
                    if ((abs(dr) == 1 && dc == 0) || (abs(dc) == 1 && dr == 0)) {
                        // Adjacent player found
                        players[q].hp -= DAMAGE;
                        if (players[q].hp <= 0) {
                            // Player is dead, remove them from game
                            players[q].hp = 0;
                            close(players[q].socket_fd);
                            players[q].socket_fd = -1;
                            players[q].active = 0;
                            player_count--;
                        }
                        hit = 1;
                    }
                }
            }
            if (!hit) {
                // No adjacent players, send a message to attacker only (no state change)
                const char *msg = "No targets adjacent to attack.\n";
                send(sockfd, msg, strlen(msg), 0);
            } else {
                // At least one player was hit or removed, broadcast updated state
                broadcast_state_locked();
            }
            pthread_mutex_unlock(&state_lock);
        } else if (strcasecmp(buffer, "QUIT") == 0) {
            // Client wants to quit the game
            pthread_mutex_lock(&state_lock);
            // Remove this player from the game
            close(players[player_index].socket_fd);
            players[player_index].socket_fd = -1;
            players[player_index].active = 0;
            player_count--;
            // Broadcast updated state to others
            broadcast_state_locked();
            pthread_mutex_unlock(&state_lock);
            break; // break out of the loop to terminate thread
        } else {
            // Unknown command
            const char *msg = "Unknown command. Available commands: MOVE, ATTACK, QUIT.\n";
            send(sockfd, msg, strlen(msg), 0);
        }
    } // end of command handling loop

    // Cleanup: If loop ended, ensure this player's resources are cleaned up (if not already)
    pthread_mutex_lock(&state_lock);
    if (players[player_index].active) {
        // Make sure to remove player if still marked active (for safety)
        players[player_index].active = 0;
        player_count--;
    }
    pthread_mutex_unlock(&state_lock);
    close(sockfd);
    fprintf(stderr, "Player %c disconnected, thread terminating.\n", players[player_index].symbol);
    pthread_exit(NULL);
    return NULL;
}

// -------- Main Server Setup and Loop --------
int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    int port = atoi(argv[1]);
    if (port <= 0) {
        fprintf(stderr, "Invalid port number.\n");
        exit(EXIT_FAILURE);
    }

    int server_fd, client_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    pthread_t thread_id;

    // Initialize game state and mutex
    srand(time(NULL));
    pthread_mutex_init(&state_lock, NULL);
    // Initialize players and obstacles
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        players[i].active = 0;
        players[i].socket_fd = -1;
        players[i].symbol = 'A' + i; // pre-assign symbols based on index
        players[i].hp = 0;
        players[i].row = players[i].col = 0;
    }
    // Place random obstacles on the grid
    memset(obstacles, 0, sizeof(obstacles));
    int obstacle_count = 3 + rand() % 3; // 3 to 5 obstacles
    for (int k = 0; k < obstacle_count; ++k) {
        int r = rand() % GRID_SIZE;
        int c = rand() % GRID_SIZE;
        if (obstacles[r][c] == 1) {
            k--; // already an obstacle here, try again
        } else {
            obstacles[r][c] = 1;
        }
    }

    // Ignore SIGPIPE to prevent crashes on send to disconnected clients
    signal(SIGPIPE, SIG_IGN);

    // Create TCP socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }
    // Allow reuse of address to avoid "Address already in use" on quick restart
    int optval = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    // Bind socket to the specified port
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // Listen for incoming connections (backlog up to 4)
    if (listen(server_fd, 4) < 0) {
        perror("Listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    printf("Server started on port %d. Waiting for players...\n", port);

    // Accept loop
    while (1) {
        client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            perror("Accept failed");
            continue;
        }
        // Limit concurrent clients to MAX_PLAYERS
        pthread_mutex_lock(&state_lock);
        if (player_count >= MAX_PLAYERS) {
            pthread_mutex_unlock(&state_lock);
            // Refuse new connection
            const char *msg = "Server full. Try again later.\n";
            send(client_fd, msg, strlen(msg), 0);
            close(client_fd);
            continue;
        }
        // Find an available player slot
        int idx = -1;
        for (int i = 0; i < MAX_PLAYERS; ++i) {
            if (!players[i].active) {
                idx = i;
                break;
            }
        }
        if (idx == -1) {
            // This should not happen if player_count was accurate, but handle gracefully
            pthread_mutex_unlock(&state_lock);
            const char *msg = "Server error: no slot available.\n";
            send(client_fd, msg, strlen(msg), 0);
            close(client_fd);
            continue;
        }
        // Initialize the new player slot
        players[idx].active = 1;
        players[idx].socket_fd = client_fd;
        players[idx].hp = MAX_HP;
        // Find a random free position (not an obstacle and not occupied by another player)
        do {
            players[idx].row = rand() % GRID_SIZE;
            players[idx].col = rand() % GRID_SIZE;
        } while (obstacles[players[idx].row][players[idx].col] == 1 || 
                 ({ int occupied=0; for(int j=0;j<MAX_PLAYERS;j++){ if(players[j].active && j!=idx && players[j].row==players[idx].row && players[j].col==players[idx].col) { occupied=1; break; } } occupied; }));
        player_count++;
        printf("New player %c joined at position (%d,%d).\n", players[idx].symbol, players[idx].row, players[idx].col);
        // Broadcast updated game state to all clients (including the new one)
        broadcast_state_locked();
        pthread_mutex_unlock(&state_lock);

        // Create a detached thread for the new client
        if (pthread_create(&thread_id, NULL, client_handler, (void*)(intptr_t)idx) != 0) {
            perror("Could not create thread for new client");
            // If thread creation fails, cleanup the allocated slot
            pthread_mutex_lock(&state_lock);
            close(players[idx].socket_fd);
            players[idx].socket_fd = -1;
            players[idx].active = 0;
            player_count--;
            pthread_mutex_unlock(&state_lock);
            continue;
        }
        pthread_detach(thread_id);
    }

    // Cleanup (unreachable in infinite loop unless we break out)
    close(server_fd);
    pthread_mutex_destroy(&state_lock);
    return 0;
}
