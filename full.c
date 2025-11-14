#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#define MAX_TRAINS 32
#define MAX_TRACKS 64
#define MAX_NAME_LEN 32
#define MAX_CHECKPOINTS 16

// ANSI Color Codes for enhanced terminal output
static const char *C_RESET = "\x1b[0m";
static const char *C_BOLD = "\x1b[1m";
static const char *C_RED = "\x1b[31m";
static const char *C_GREEN = "\x1b[32m";
static const char *C_YELLOW = "\x1b[33m";
static const char *C_BLUE = "\x1b[34m";
static const char *C_MAGENTA = "\x1b[35m";
static const char *C_CYAN = "\x1b[36m";

// Structure representing the current state of the railway system
typedef struct {
    int ntrains;
    int ntracks;
    char tname[MAX_TRAINS][MAX_NAME_LEN];
    char rname[MAX_TRACKS][MAX_NAME_LEN];
    int available[MAX_TRACKS];         // Available resource units
    int maximum[MAX_TRAINS][MAX_TRACKS];    // Max units each train may request
    int allocation[MAX_TRAINS][MAX_TRACKS]; // Currently allocated units
    int need[MAX_TRAINS][MAX_TRACKS];       // Max - Allocation
} RailwayState;

// Structure for saving/restoring the system state (Checkpoints)
typedef struct {
    RailwayState state;
    int valid;
    char note[128];
} CP;

// Structure for the Wait-For Graph (WFG)
typedef struct {
    int n;
    int adj[MAX_TRAINS][MAX_TRAINS]; // Adjacency matrix: adj[i][j] = 1 if Train i waits for Train j
} WFG;

static RailwayState rail;
static CP checkpoints[MAX_CHECKPOINTS];

// --- Utility Functions ---

// Fatal error handler
static void die(const char *s) {
    fprintf(stderr, "Fatal: %s\n", s);
    exit(EXIT_FAILURE);
}

// Safer string copy
static void safe_strcpy(char *dst, const char *src, size_t n) {
    strncpy(dst, src, n-1);
    dst[n-1] = '\0';
}

// Recalculates the Need matrix: Need = Maximum - Allocation
static void compute_need(RailwayState *s) {
    for (int i = 0; i < s->ntrains; ++i)
        for (int j = 0; j < s->ntracks; ++j)
            s->need[i][j] = s->maximum[i][j] - s->allocation[i][j];
}

// Initializes the state with empty/zero values
static void init_empty(RailwayState *s, int ntrains, int ntracks) {
    if (ntrains < 1 || ntrains > MAX_TRAINS || ntracks < 1 || ntracks > MAX_TRACKS) die("invalid sizes");
    s->ntrains = ntrains;
    s->ntracks = ntracks;
    for (int i = 0; i < ntrains; ++i) snprintf(s->tname[i], MAX_NAME_LEN, "Train%d", i);
    for (int j = 0; j < ntracks; ++j) snprintf(s->rname[j], MAX_NAME_LEN, "Track%d", j);
    for (int j = 0; j < ntracks; ++j) s->available[j] = 0;
    for (int i = 0; i < ntrains; ++i)
        for (int j = 0; j < ntracks; ++j) s->maximum[i][j] = s->allocation[i][j] = s->need[i][j] = 0;
}

// Saves the current state as a checkpoint
static int save_checkpoint(const RailwayState *s, const char *note) {
    for (int i = 0; i < MAX_CHECKPOINTS; ++i) {
        if (!checkpoints[i].valid) {
            checkpoints[i].state = *s;
            checkpoints[i].valid = 1;
            if (note && note[0]) safe_strcpy(checkpoints[i].note, note, sizeof(checkpoints[i].note));
            else safe_strcpy(checkpoints[i].note, "checkpoint", sizeof(checkpoints[i].note));
            return i;
        }
    }
    return -1;
}

// Restores a previously saved checkpoint
static int restore_checkpoint(RailwayState *s, int idx) {
    if (idx < 0 || idx >= MAX_CHECKPOINTS) return -1;
    if (!checkpoints[idx].valid) return -1;
    *s = checkpoints[idx].state;
    checkpoints[idx].valid = 0;
    return 0;
}

// Checks if a request is less than or equal to the available resources
static int request_le_available(int m, const int request[], const int available[]) {
    for (int j = 0; j < m; ++j) if (request[j] > available[j]) return 0;
    return 1;
}

// --- Banker's Algorithm Implementation (Deadlock Avoidance) ---

// Checks if the current state is safe (finds a safe sequence)
static int safety_check(const RailwayState *s, int safe_seq[]) {
    int n = s->ntrains;
    int m = s->ntracks;
    int work[MAX_TRACKS];
    int finish[MAX_TRAINS];

    for (int j = 0; j < m; ++j) work[j] = s->available[j];
    for (int i = 0; i < n; ++i) finish[i] = 0;

    int count = 0;
    while (count < n) {
        int found = 0;
        for (int i = 0; i < n; ++i) {
            if (!finish[i]) {
                int ok = 1;
                // Check if Need[i] <= Work
                for (int j = 0; j < m; ++j) if (s->need[i][j] > work[j]) { ok = 0; break; }
                
                if (ok) {
                    // Simulate completion: Work = Work + Allocation[i]
                    for (int j = 0; j < m; ++j) work[j] += s->allocation[i][j];
                    finish[i] = 1;
                    if (safe_seq) safe_seq[count] = i; // Record in safe sequence
                    ++count;
                    found = 1;
                }
            }
        }
        if (!found) break; // No train can proceed
    }
    return (count == n); // True if all trains finished
}

// Attempts to grant a track request using the Banker's Algorithm
static int bankers_request(RailwayState *s, int tid, const int request[]) {
    if (tid < 0 || tid >= s->ntrains) return 0;
    int m = s->ntracks;

    // 1. Check if Request <= Need[tid]
    for (int j = 0; j < m; ++j) if (request[j] > s->need[tid][j]) return 0;

    // 2. Check if Request <= Available
    if (!request_le_available(m, request, s->available)) return 0;

    // 3. Tentatively allocate resources (modify state)
    for (int j = 0; j < m; ++j) {
        s->available[j] -= request[j];
        s->allocation[tid][j] += request[j];
        s->need[tid][j] -= request[j];
    }

    // 4. Check if the new state is safe
    int seq[MAX_TRAINS];
    int ok = safety_check(s, seq);

    if (!ok) {
        // State is unsafe: Rollback the allocation
        for (int j = 0; j < m; ++j) {
            s->available[j] += request[j];
            s->allocation[tid][j] -= request[j];
            s->need[tid][j] += request[j];
        }
        return 0;
    }
    
    // Request is safe and granted
    return 1;
}

// --- Wait-For Graph (WFG) Implementation (Deadlock Detection) ---

// Builds the Wait-For Graph (T_i -> T_j if T_i needs resource r held by T_j and r is not available)
static void build_wfg(const RailwayState *s, WFG *g) {
    int n = s->ntrains;
    int m = s->ntracks;
    g->n = n;
    
    // Initialize graph
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            g->adj[i][j] = 0;

    for (int i = 0; i < n; ++i) { // Train i (the potential waiter)
        int needs_any = 0;
        for (int r = 0; r < m; ++r) if (s->need[i][r] > 0) needs_any = 1;
        if (!needs_any) continue; // Train i is not waiting

        for (int r = 0; r < m; ++r) { // Resource r
            if (s->need[i][r] <= 0) continue; // T_i doesn't need r

            // Deadlock is only possible if the needed resource has zero available units
            if (s->available[r] > 0) continue; 

            for (int j = 0; j < n; ++j) { // Train j (the resource holder)
                // If T_j holds resource r and is not T_i itself, then T_i waits for T_j
                if (s->allocation[j][r] > 0 && j != i) g->adj[i][j] = 1;
            }
        }
    }
}

// Utility function for DFS to detect a cycle (deadlock)
static int dfs_cycle_util(const WFG *g, int u, int visited[], int stack[], int cycle_buf[], int *cycle_len) {
    visited[u] = 1;
    stack[u] = 1;

    for (int v = 0; v < g->n; ++v) {
        if (!g->adj[u][v]) continue;

        if (!visited[v]) {
            if (dfs_cycle_util(g, v, visited, stack, cycle_buf, cycle_len)) {
                // Cycle found in subtree, add current node to cycle path
                if (*cycle_len < MAX_TRAINS) cycle_buf[(*cycle_len)++] = u;
                return 1;
            }
        } else if (stack[v]) {
            // Cycle detected (back edge to node currently in recursion stack)
            if (*cycle_len < MAX_TRAINS) {
                cycle_buf[(*cycle_len)++] = v; // Start of the cycle
                cycle_buf[(*cycle_len)++] = u; // Next node
            }
            return 1;
        }
    }
    stack[u] = 0; // Remove from recursion stack
    return 0;
}

// Main function to detect a cycle in the WFG
static int detect_cycle_wfg(const WFG *g, int cycle_buf[], int *cycle_len) {
    int n = g->n;
    int visited[MAX_TRAINS] = {0};
    int stack[MAX_TRAINS] = {0}; // Recursion stack
    *cycle_len = 0;

    for (int i = 0; i < n; ++i) if (!visited[i]) {
        if (dfs_cycle_util(g, i, visited, stack, cycle_buf, cycle_len)) return 1;
    }
    return 0;
}

// Exports the Resource Allocation Graph (RAG) and WFG to a Graphviz DOT file
static void export_dot(const RailwayState *s, const WFG *g, const char *filename) {
    FILE *f = fopen(filename, "w");
    if (!f) {
        fprintf(stderr, "Cannot open %s: %s\n", filename, strerror(errno));
        return;
    }
    fprintf(f, "digraph RailwayRAG {\n");
    fprintf(f, " \trankdir=LR;\n");

    // 1. Define nodes: Trains (circles) and Resources (boxes)
    for (int i = 0; i < s->ntrains; ++i) fprintf(f, " \tT%d [shape=circle,label=\"%s\"];\n", i, s->tname[i]);
    for (int j = 0; j < s->ntracks; ++j) fprintf(f, " \tR%d [shape=box,label=\"%s\\n(av:%d)\"];\n", j, s->rname[j], s->available[j]);
    
    // 2. Add Resource Allocation Graph (RAG) edges
    for (int i = 0; i < s->ntrains; ++i)
        for (int j = 0; j < s->ntracks; ++j) {
            // Allocation Edge: Resource -> Train (Solid line)
            if (s->allocation[i][j] > 0) fprintf(f, " \tR%d -> T%d [label=\"%d\"];\n", j, i, s->allocation[i][j]);
            // Request Edge: Train -> Resource (Dashed line)
            if (s->need[i][j] > 0) fprintf(f, " \tT%d -> R%d [label=\"need:%d\", style=dashed];\n", i, j, s->need[i][j]);
        }
    
    // 3. Add Wait-For Graph (WFG) edges (Red lines)
    for (int i = 0; i < g->n; ++i)
        for (int j = 0; j < g->n; ++j)
            if (g->adj[i][j]) fprintf(f, " \tT%d -> T%d [color=red];\n", i, j);
            
    fprintf(f, "}\n");
    fclose(f);
}

// --- Deadlock Recovery Functions ---

// Simulates termination of a train, releasing its tracks
static int terminate_train(RailwayState *s, int tid) {
    if (tid < 0 || tid >= s->ntrains) return 0;
    for (int j = 0; j < s->ntracks; ++j) {
        s->available[j] += s->allocation[tid][j];
        s->allocation[tid][j] = 0;
        s->maximum[tid][j] = 0;
        s->need[tid][j] = 0;
    }
    safe_strcpy(s->tname[tid], "(REMOVED)", MAX_NAME_LEN);
    return 1;
}

// Simulates preemption (taking tracks) from a train
static int preempt_from_train(RailwayState *s, int tid, const int preempt[]) {
    if (tid < 0 || tid >= s->ntrains) return 0;
    for (int j = 0; j < s->ntracks; ++j) {
        int take = preempt[j];
        if (take < 0) take = 0;
        // Ensure we don't take more than allocated
        if (take > s->allocation[tid][j]) take = s->allocation[tid][j]; 
        
        s->allocation[tid][j] -= take;
        s->available[j] += take;
    }
    compute_need(s);
    return 1;
}

// --- Display Functions ---

static void print_horizontal(int w) {
    for (int i = 0; i < w; ++i) putchar('-');
    putchar('\n');
}

static void print_state(const RailwayState *s) {
    printf("%s%sRAILWAY DEADLOCK SIMULATOR - RAIL MODE%s\n\n", C_BOLD, C_CYAN, C_RESET);
    printf("%sTrains:%s %d    %sTrack Sections:%s %d\n\n", C_GREEN, C_RESET, s->ntrains, C_GREEN, C_RESET, s->ntracks);

    // Dynamic width calculation for table (rough estimate)
    int table_width = 20 + 3 * s->ntracks * 3 + 12; // Base + 3 columns * (spaces + 2 digits)

    print_horizontal(table_width);
    printf("%-4s %-12s |", "ID", "Train");
    printf(" Alloc");
    for (int j = 0; j < s->ntracks - 1; ++j) printf("   ");
    printf(" | Max");
    for (int j = 0; j < s->ntracks - 1; ++j) printf("   ");
    printf(" | Need\n");
    print_horizontal(table_width);

    // Print column headers for resources/tracks
    printf("%-4s %-12s |", "", "");
    for (int j = 0; j < s->ntracks; ++j) printf(" R%d", j);
    printf(" |");
    for (int j = 0; j < s->ntracks; ++j) printf(" R%d", j);
    printf(" |");
    for (int j = 0; j < s->ntracks; ++j) printf(" R%d", j);
    printf("\n");
    print_horizontal(table_width);


    for (int i = 0; i < s->ntrains; ++i) {
        printf("%3d  %-12s |", i, s->tname[i]);
        for (int j = 0; j < s->ntracks; ++j) printf(" %2d", s->allocation[i][j]);
        printf(" |");
        for (int j = 0; j < s->ntracks; ++j) printf(" %2d", s->maximum[i][j]);
        printf(" |");
        for (int j = 0; j < s->ntracks; ++j) printf(" %2d", s->need[i][j]);
        printf("\n");
    }
    print_horizontal(table_width);
    printf("%sAvailable tracks:%s", C_MAGENTA, C_RESET);
    for (int j = 0; j < s->ntracks; ++j) printf(" R%d=%d", j, s->available[j]);
    printf("\n\n");
}

static void print_wfg(const WFG *g) {
    printf("%sWait-For Graph (train -> train):%s\n", C_YELLOW, C_RESET);
    for (int i = 0; i < g->n; ++i) {
        printf("T%d (%s) waits for:", i, rail.tname[i]);
        int any = 0;
        for (int j = 0; j < g->n; ++j) if (g->adj[i][j]) { printf(" T%d (%s)", j, rail.tname[j]); any = 1; }
        if (!any) printf(" none");
        printf("\n");
    }
    printf("\n");
}

// --- Scenario Initialization Functions ---

// Initializes a random, multi-unit scenario (best for testing non-binary values)
static void fill_random_railway(RailwayState *s, int ntrains, int ntracks, int max_units_per_track) {
    init_empty(s, ntrains, ntracks);
    srand((unsigned)time(NULL));

    // 1. Set total available (Work pool is initialized later)
    for (int j = 0; j < ntracks; ++j) s->available[j] = 1 + (rand() % max_units_per_track);

    // 2. Allocate resources randomly
    for (int j = 0; j < ntracks; ++j) {
        // A generous maximum capacity to ensure resources can be distributed
        int cap = s->available[j] + ntrains * max_units_per_track; 
        if (cap < 1) cap = 1;
        
        // Randomly decide how many resources in total will be allocated
        int remaining = rand() % (cap + 1); 
        
        for (int i = 0; i < ntrains; ++i) {
            int take = remaining ? (rand() % (remaining + 1)) : 0;
            s->allocation[i][j] = take;
            remaining -= take;
        }
    }
    
    // 3. Recalculate Available (Total capacity = Allocated + Available)
    for (int j = 0; j < ntracks; ++j) {
        int total_alloc = 0;
        for (int i = 0; i < ntrains; ++i) total_alloc += s->allocation[i][j];
        // The previously set s->available[j] is now treated as initial available units.
        int total_units = total_alloc + s->available[j]; 
        if (total_units < 1) total_units = 1;
        s->available[j] = total_units - total_alloc;
    }

    // 4. Set Maximum (Allocation + random Need)
    for (int i = 0; i < ntrains; ++i)
        for (int j = 0; j < ntracks; ++j)
            s->maximum[i][j] = s->allocation[i][j] + (rand() % (max_units_per_track + 1));
            
    compute_need(s);
}

// Initializes a specific non-deadlocked sample scenario (mostly binary)
static void sample_railway(RailwayState *s) {
    init_empty(s, 5, 5);
    safe_strcpy(s->tname[0], "A", MAX_NAME_LEN);
    safe_strcpy(s->tname[1], "B", MAX_NAME_LEN);
    safe_strcpy(s->tname[2], "C", MAX_NAME_LEN);
    safe_strcpy(s->tname[3], "D", MAX_NAME_LEN);
    safe_strcpy(s->tname[4], "E", MAX_NAME_LEN);
    safe_strcpy(s->rname[0], "T0", MAX_NAME_LEN);
    safe_strcpy(s->rname[1], "T1", MAX_NAME_LEN);
    safe_strcpy(s->rname[2], "T2", MAX_NAME_LEN);
    safe_strcpy(s->rname[3], "T3", MAX_NAME_LEN);
    safe_strcpy(s->rname[4], "T4", MAX_NAME_LEN);

    // Available Resources (Tracks)
    s->available[0] = 1;
    s->available[1] = 1;
    s->available[2] = 0;
    s->available[3] = 1;
    s->available[4] = 0;

    // Maximum Demand Matrix M
    int M[5][5] = {
        {1,1,1,0,0},
        {0,1,0,1,0},
        {0,0,1,0,1},
        {0,1,0,1,0},
        {1,0,0,0,1}
    };
    // Allocation Matrix A
    int A[5][5] = {
        {0,0,0,0,0},
        {0,1,0,0,0},
        {0,0,1,0,0},
        {0,0,0,0,0},
        {1,0,0,0,0}
    };
    
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 5; ++j) {
            s->maximum[i][j] = M[i][j];
            s->allocation[i][j] = A[i][j];
        }
    compute_need(s);
}

// Interactive manual input for a custom scenario
static void manual_railway(RailwayState *s) {
    int ntr, ntrks;
    printf("Enter number of trains (1-%d): ", MAX_TRAINS);
    if (scanf("%d", &ntr) != 1) { while(getchar()!='\n'); return; }
    printf("Enter number of track sections (1-%d): ", MAX_TRACKS);
    if (scanf("%d", &ntrks) != 1) { while(getchar()!='\n'); return; }
    if (ntr < 1 || ntr > MAX_TRAINS || ntrks < 1 || ntrks > MAX_TRACKS) { printf("Invalid sizes\n"); return; }
    init_empty(s, ntr, ntrks);
    
    for (int j = 0; j < ntrks; ++j) {
        printf("Total available units for Track %d: ", j);
        if (scanf("%d", &s->available[j]) != 1) { while(getchar()!='\n'); return; }
        snprintf(s->rname[j], MAX_NAME_LEN, "Trk%02d", j);
    }
    
    for (int i = 0; i < ntr; ++i) {
        char tmp[64];
        printf("Train name for T%d: ", i);
        getchar(); // consume leftover newline
        if (fgets(tmp, sizeof(tmp), stdin)) {
            tmp[strcspn(tmp, "\n")] = 0;
            if (tmp[0]) safe_strcpy(s->tname[i], tmp, MAX_NAME_LEN);
            else snprintf(s->tname[i], MAX_NAME_LEN, "Train%d", i);
        }

        for (int j = 0; j < ntrks; ++j) {
            printf("Allocation of Track %d for %s: ", j, s->tname[i]);
            if (scanf("%d", &s->allocation[i][j]) != 1) { while(getchar()!='\n'); return; }
            printf("Maximum demand of Track %d for %s: ", j, s->tname[i]);
            if (scanf("%d", &s->maximum[i][j]) != 1) { while(getchar()!='\n'); return; }
            if (s->allocation[i][j] > s->maximum[i][j]) s->maximum[i][j] = s->allocation[i][j];
        }
    }
    compute_need(s);
}

// --- Menu Handlers ---

static void handle_bankers(RailwayState *s) {
    int tid;
    printf("Enter train id requesting track(s) (0-%d): ", s->ntrains-1);
    if (scanf("%d", &tid) != 1) { while(getchar()!='\n'); return; }

    int req[MAX_TRACKS] = {0};
    for (int j = 0; j < s->ntracks; ++j) {
        printf("Request units of Track %d: ", j);
        if (scanf("%d", &req[j]) != 1) { while(getchar()!='\n'); return; }
    }

    save_checkpoint(s, "pre-bankers");
    int ok = bankers_request(s, tid, req);
    if (ok) printf("%sRequest granted safely.%s\n", C_GREEN, C_RESET);
    else printf("%sRequest denied (unsafe or invalid).%s\n", C_RED, C_RESET);
}

static void handle_detect(RailwayState *s) {
    WFG g;
    build_wfg(s, &g);
    print_wfg(&g);

    int cycle[MAX_TRAINS];
    int clen = 0;
    int found = detect_cycle_wfg(&g, cycle, &clen);
    
    if (found) {
        printf("%sDeadlock detected! Cycle:%s ", C_RED, C_RESET);
        // Print cycle in reverse order as DFS records it from tail to head
        for (int i = clen-1; i >= 0; --i) {
            printf("%s", s->tname[cycle[i]]);
            if (i > 0) printf(" -> ");
        }
        printf("\n");
    } else {
        printf("%sNo deadlock detected by WFG (or system is in a safe/avoidable state).%s\n", C_GREEN, C_RESET);
    }
    
    // Also run safety check for completeness, even if WFG didn't find a cycle.
    int seq[MAX_TRAINS];
    if (safety_check(s, seq)) {
        printf("%sSystem is in a SAFE state (Banker's Check).%s\n", C_GREEN, C_RESET);
    } else {
        printf("%sSystem is in an UNSAFE state (Banker's Check).%s\n", C_RED, C_RESET);
    }
}

static void handle_terminate(RailwayState *s) {
    int tid;
    printf("Enter train id to terminate: ");
    if (scanf("%d", &tid) != 1) { while(getchar()!='\n'); return; }

    save_checkpoint(s, "pre-terminate");
    if (terminate_train(s, tid)) printf("%sTrain %d terminated and tracks released.%s\n", C_YELLOW, tid, C_RESET);
    else printf("%sTermination failed (invalid id).%s\n", C_RED, C_RESET);
}

static void handle_preempt(RailwayState *s) {
    int tid;
    printf("Enter victim train id for preemption: ");
    if (scanf("%d", &tid) != 1) { while(getchar()!='\n'); return; }

    if (tid < 0 || tid >= s->ntrains) { printf("%sInvalid train ID.%s\n", C_RED, C_RESET); return; }

    int pre[MAX_TRACKS];
    for (int j = 0; j < s->ntracks; ++j) {
        printf("Units to preempt from Track %d (0..%d): ", j, s->allocation[tid][j]);
        if (scanf("%d", &pre[j]) != 1) { while(getchar()!='\n'); return; }
    }
    
    save_checkpoint(s, "pre-preempt");
    if (preempt_from_train(s, tid, pre)) printf("%sPreemption done from train %d.%s\n", C_YELLOW, tid, C_RESET);
    else printf("%sPreemption failed.%s\n", C_RED, C_RESET);
}

static void handle_save_cp(RailwayState *s) {
    char note[128];
    printf("Note for checkpoint: ");
    getchar(); // consume leftover newline
    if (!fgets(note, sizeof(note), stdin)) note[0] = 0;
    note[strcspn(note, "\n")] = 0;

    int idx = save_checkpoint(s, note);
    if (idx >= 0) printf("%sSaved checkpoint %d (%s).%s\n", C_GREEN, idx, checkpoints[idx].note, C_RESET);
    else printf("%sNo free checkpoint slots.%s\n", C_RED, C_RESET);
}

static void handle_restore_cp(RailwayState *s) {
    int idx;
    printf("Available Checkpoints:\n");
    for (int i = 0; i < MAX_CHECKPOINTS; ++i) {
        if (checkpoints[i].valid) {
            printf("  %d: %s\n", i, checkpoints[i].note);
        }
    }
    printf("Enter checkpoint index to restore (0-%d): ", MAX_CHECKPOINTS-1);
    if (scanf("%d", &idx) != 1) { while(getchar()!='\n'); return; }

    if (restore_checkpoint(s, idx) == 0) printf("%sRestored checkpoint %d.%s\n", C_GREEN, idx, C_RESET);
    else printf("%sRestore failed (invalid or unused index).%s\n", C_RED, C_RESET);
}

static void handle_export(RailwayState *s) {
    WFG g;
    build_wfg(s, &g);
    char fname[128];
    printf("Enter filename for DOT export (e.g., railway.dot): ");
    if (scanf("%s", fname) != 1) { while(getchar()!='\n'); return; }
    export_dot(s, &g, fname);
    printf("%sDOT exported to %s. Use 'dot -Tpng %s -o out.png' (Graphviz) to render.%s\n", C_CYAN, fname, fname, C_RESET);
}

static void show_menu(void) {
    printf("\n%sRAILWAY MODE - MENU%s\n", C_BOLD, C_RESET);
    printf("----------------------------------\n");
    printf("1) Load sample railway scenario\n");
    printf("2) Generate random railway scenario\n");
    printf("3) Manual input\n");
    printf("4) Show current state\n");
    printf("5) Try Banker's request for a train (Deadlock Avoidance)\n");
    printf("6) Detect deadlock (Wait-For Graph & Safety Check)\n");
    printf("7) Recover: terminate train\n");
    printf("8) Recover: preempt tracks from train\n");
    printf("9) Save checkpoint\n");
    printf("10) Restore checkpoint\n");
    printf("11) Export DOT for Graphviz\n");
    printf("q) Quit\n");
    printf("Enter choice: ");
}

static void init_checkpoints(void) {
    for (int i = 0; i < MAX_CHECKPOINTS; ++i) checkpoints[i].valid = 0;
}

int main(void) {
    init_checkpoints();
    sample_railway(&rail);
    compute_need(&rail);
    printf("\nWelcome to the Railway Deadlock Simulator (Rail Mode)\n\n");

    int quit = 0;
    while (!quit) {
        show_menu();
        char choice[8];
        // Clear input buffer before reading choice
        int c;
        while ((c = getchar()) != '\n' && c != EOF); 
        
        if (scanf("%s", choice) != 1) break;

        if (choice[0] == '1') { 
            sample_railway(&rail); 
            compute_need(&rail); 
            printf("%sSample scenario loaded.%s\n\n", C_CYAN, C_RESET); 
        }
        else if (choice[0] == '2') {
            int nt, nk, maxu;
            printf("Enter ntrains ntracks max_units_per_track (e.g., 6 6 2): ");
            if (scanf("%d %d %d", &nt, &nk, &maxu) == 3) { 
                fill_random_railway(&rail, nt, nk, maxu); 
                printf("%sRandom scenario created.%s\n\n", C_CYAN, C_RESET); 
            }
        }
        else if (choice[0] == '3') { 
            manual_railway(&rail); 
            printf("%sManual scenario set.%s\n\n", C_CYAN, C_RESET); 
        }
        else if (choice[0] == '4') { 
            print_state(&rail); 
        }
        else if (choice[0] == '5') { 
            handle_bankers(&rail); 
        }
        else if (choice[0] == '6') { 
            handle_detect(&rail); 
        }
        else if (choice[0] == '7') { 
            handle_terminate(&rail); 
        }
        else if (choice[0] == '8') { 
            handle_preempt(&rail); 
        }
        else if (choice[0] == '9') { 
            handle_save_cp(&rail); 
        }
        else if (strcmp(choice, "10") == 0) { 
            handle_restore_cp(&rail); 
        }
        else if (strcmp(choice, "11") == 0) { 
            handle_export(&rail); 
        }
        else if (choice[0] == 'q' || choice[0] == 'Q') { 
            quit = 1; 
            break; 
        }
        else {
            printf("%sUnknown choice.%s\n", C_RED, C_RESET);
        }
        
        printf("\nPress Enter to continue...");
        // Consume any remaining input and wait for next newline/Enter
        while ((c = getchar()) != '\n' && c != EOF); 
        // Second getchar() is to wait for the user's explicit Enter press
        getchar();
    }
    printf("\nGoodbye.\n");
    return 0;
}
