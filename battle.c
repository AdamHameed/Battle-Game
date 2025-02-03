/*
 * socket demonstrations:
 * This is the server side of an "internet domain" socket connection, for
 * communicating over the network.
 *
 * In this case we are willing to wait for chatter from the client
 * _or_ for a new connection.
*/

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdbool.h>

#ifndef PORT
    #define PORT 58868
#endif

#define NAME_SIZE 20
#define MSG_SIZE  512

#define INITIAL_LB_CAP 128

// initial state of clients, when the server is waiting for them to input their name
#define ST_LOGIN   1
// waiting state for clients, when they're logged in but unmatched
#define ST_WAIT    2
// client state when matched with an opponent
#define ST_COMBAT  3
// client state when in combat and currently speaking a message
#define ST_SPEAK   4

struct combat {
    int against;  // id of opponent
    int hp;       // hitpoints
    int pmoves;   // number of powermoves left
    bool active;  // is active player
};

struct client {
    // client specific
    int id;                    // unique identifier
    char name[NAME_SIZE];      // name (entered upon connecting)

    int state;
    struct combat combat;      // only valid if state == ST_COMBAT
    int last_fought;           // id of last opponent
    char msg[MSG_SIZE];        // message that client wants to speak

    // network fields
    int fd;
    struct in_addr ipaddr;

    // linked list stuff
    struct client *next;
};

struct lb_entry {
    int id;
    char name[NAME_SIZE];

    int wins;
    int losses;
};

struct lb_entry *LB;
int LB_CAP = INITIAL_LB_CAP;
int LB_SIZE = 0;
int IOTA = 0;

// communication helpers

// send a message to a single client
static void to_client(struct client *top, struct client *p, char *s, int size);
// welcome a client (send them a welcome message and broadcast their arrival)
static void welcome(struct client *top, struct client *p);
// broadcast a message to all clients except for client with id id
static void broadcast_except(struct client *top, char *s, int size, int id);

// match making helpers

// iterate over all clients and make all possible matches
static void make_matches(struct client *top);
// check if clients A and B can be in a match
static bool compatible(struct client *A, struct client *B);
// initialize clients A and B for a match
static void match(struct client *A, struct client *B);

// combat utils
// send status message to client p (show hitpoints and action menu)
static void status_update(struct client *top, struct client *p);
// create new combat details struct
static struct combat new_combat_details(int against);

// linked-list stuff
static struct client *findclient(struct client *top, int id);
static void move_to_end(struct client **top, struct client *node);

// leaderboard stuff
// expand the size of our leaderboard if necessary
static void expand_leaderboard(void);
// find a leaderboard entry by id
static struct lb_entry *lb_by_id(int id);
// compare two leaderboard entries by win/(wins+losses) ratio
static int compare_lb_entry(const void *a, const void *b);
// print leaderboard into a string
static char *print_lb(void);

// misc
// choose random integer in inclusive range [low, max]
static int rand_between(int low, int max);
// flip the bit
static void toggle(bool *b);

// from starter code
static struct client *addclient(struct client *top, int fd, struct in_addr addr);
static struct client *removeclient(struct client *top, int fd);
static void broadcast(struct client *top, char *s, int size);
int handleclient(struct client *p, struct client **top);
int bindandlisten(void);

int main(void) {
    int clientfd, maxfd, nready;
    struct client *p;
    struct client *head = NULL;
    socklen_t len;
    struct sockaddr_in q;
    fd_set allset;
    fd_set rset;

    int i;

    // seed random number generator
    srand(69420);

    // allocate leader board entries
    LB = calloc(INITIAL_LB_CAP, sizeof(struct lb_entry));
    if (LB == NULL) {
        perror("malloc leaderboard");
        exit(1);
    }


    int listenfd = bindandlisten();
    // initialize allset and add listenfd to the
    // set of file descriptors passed into select
    FD_ZERO(&allset);
    FD_SET(listenfd, &allset);
    // maxfd identifies how far into the set to search
    maxfd = listenfd;

    while (1) {
        // for debugging purposes
        // struct client *cur = head;
        // printf("clients: ");
        // while (cur != NULL) {
        //     printf("%d -> ", cur->id);
        //     cur = cur->next;
        // }
        // printf("NULL\n");

        // make a copy of the set before we pass it into select
        rset = allset;

        nready = select(maxfd + 1, &rset, NULL, NULL, NULL);

        if (nready == -1) {
            perror("select");
            continue;
        }

        if (FD_ISSET(listenfd, &rset)){
            printf("a new client is connecting\n");
            len = sizeof(q);
            if ((clientfd = accept(listenfd, (struct sockaddr *)&q, &len)) < 0) {
                perror("accept");
                exit(1);
            }
            FD_SET(clientfd, &allset);
            if (clientfd > maxfd) {
                maxfd = clientfd;
            }
            printf("connection from %s\n", inet_ntoa(q.sin_addr));
            
            head = addclient(head, clientfd, q.sin_addr);
            char* greeting = "What is your name? ";
            to_client(head, head, greeting, strlen(greeting));
        }

        for(i = 0; i <= maxfd; i++) {
            if (FD_ISSET(i, &rset)) {
                for (p = head; p != NULL; p = p->next) {
                    if (p->fd == i) {
                        int result = handleclient(p, &head);
                        if (result == -1) {
                            int tmp_fd = p->fd;
                            head = removeclient(head, p->id);
                            FD_CLR(tmp_fd, &allset);
                            close(tmp_fd);
                        }
                        break;
                    }
                }
            }
        }

        // check for possible matches here
        // after clients have (possibly) logged in
        // and/or dropped out
        make_matches(head);
    }
    return 0;
}

int handleclient(struct client *p, struct client **head) {
    char buf[256];
    char outbuf[1024];
    int len = read(p->fd, buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        switch (p->state) {
            case ST_LOGIN: {
                int flag = 0;
                int nlen = strlen(p->name);
                for (int i = 0; i < len; i++) {
                    if (buf[i] == '\n') {
                        flag = 1;
                        break;
                    }
                    if (nlen + i < NAME_SIZE - 1) {
                        p->name[nlen + i] = buf[i];
                    }
                }
                
                // encountered newline
                if (flag) {
                    p->state = ST_WAIT;
                    welcome(*head, p);

                    // add p to the leaderboard
                    expand_leaderboard();
                    struct lb_entry* lbe = LB + LB_SIZE;
                    lbe->id = p->id;
                    strncpy(lbe->name, p->name, NAME_SIZE);
                    lbe->wins = 0;
                    lbe->losses = 0;

                    LB_SIZE++;
                }
                break;
            }
            
            case ST_WAIT:
                if (*buf == 'L') {
                    qsort(LB, LB_SIZE, sizeof(struct lb_entry), compare_lb_entry);
                    char *lb = print_lb();
                    to_client(*head, p, lb, strlen(lb));
                }
                break;
            
            case ST_COMBAT: {
                if (!(p->combat.active)) break;

                struct client *opp = findclient(*head, p->combat.against);
                if (opp == NULL) {
                    fprintf(stderr, "couldn't find opponent, shouldn't happen!\n");
                    exit(1);
                }

                int dmg;
                int dealt_dmg = 0;

                if (*buf == 'a') {
                    dmg = rand_between(2, 6);
                    dealt_dmg = 1;

                } else if (*buf == 'p' && p->combat.pmoves > 0) {
                    p->combat.pmoves -= 1;
                    if (rand() % 2 == 0) {
                        dealt_dmg = 1;
                        dmg = rand_between(2, 6) * 3;
                    } else {
                        char* miss  = "\nYou missed!\n"; 
                        to_client(*head, p, miss, strlen(miss));

                        sprintf(outbuf, "%s missed you!\n", p->name);
                        to_client(*head, opp, outbuf, strlen(outbuf));
                    }
                } else if (*buf == 's') {
                    char* speak = "\nSpeak: ";
                    to_client(*head, p, speak, strlen(speak));
                    p->state = ST_SPEAK;
                    // exit early to avoid switching turns and sending status updates
                    break;
                } else if (*buf == 'f') {
                    sprintf(outbuf, "%s is a coward!. They forfeit and you win.\n\nAwaiting next opponent...", p->name);
                    to_client(*head, opp, outbuf, strlen(outbuf));

                    sprintf(outbuf, "Shameful cowardice! You run away from %s...\n\nAwaiting next opponent...", opp->name);
                    to_client(*head, p, outbuf, strlen(outbuf));

                    p->state = ST_WAIT;
                    opp->state = ST_WAIT;

                    for (int i = 0; i < LB_SIZE; i++) {
                        printf("LB[%d]: id=%d, name=%s, wins=%d, losses=%d\n", i, LB[i].id, LB[i].name, LB[i].wins, LB[i].losses);
                    }

                    // update leaderboard entries
                    (lb_by_id(p->id)->losses)++;
                    (lb_by_id(opp->id)->wins)++;

                    // move players to end of the list after match ends
                    move_to_end(head, p);
                    move_to_end(head, opp);

                    // early break, dont send status updates
                    break;
                } else {
                    // any other command is invalid, just skip
                    break;
                }
                if (dealt_dmg) {
                    dmg = dmg < opp->combat.hp ? dmg : opp->combat.hp;
                    sprintf(outbuf, "\nYou hit %s for %d damage!\n", opp->name, dmg);
                    to_client(*head, p, outbuf, strlen(outbuf));

                    sprintf(outbuf, "%s hits you for %d damage!\n", p->name, dmg);
                    to_client(*head, opp, outbuf, strlen(outbuf));

                    opp->combat.hp -= dmg;

                    if (opp->combat.hp <= 0) {
                        sprintf(outbuf, "%s gives up. You win!\n\nAwaiting next opponent...", opp->name);
                        to_client(*head, p, outbuf, strlen(outbuf));

                        sprintf(outbuf, "You are no match for %s. You scurry away...\n\nAwaiting next opponent...", p->name);
                        to_client(*head, opp, outbuf, strlen(outbuf));

                        p->state = ST_WAIT;
                        opp->state = ST_WAIT;

                        // update leaderboard entries
                        (lb_by_id(p->id)->wins)++;
                        (lb_by_id(opp->id)->losses)++;

                        // move players to end of the list after match ends
                        move_to_end(head, p);
                        move_to_end(head, opp);

                        // early break, dont send status updates
                        break;
                    }
                }
                toggle(&(p->combat.active));
                toggle(&(opp->combat.active));

                status_update(*head, p);
                status_update(*head, opp);
                break;
            }

            case ST_SPEAK: {
                struct client *opp = findclient(*head, p->combat.against);
                if (opp == NULL) {
                    fprintf(stderr, "couldn't find opponent, shouldn't happen!\n");
                    exit(1);
                }

                int flag = 0;
                int mlen = strlen(p->msg);
                for (int i = 0; i < len; i++) {
                    if (buf[i] == '\n') {
                        flag = 1;
                        break;
                    }
                    if (mlen + i < MSG_SIZE - 1) {
                        p->msg[mlen + i] = buf[i];
                    }
                }
                
                // encountered newline
                if (flag) {
                    p->state = ST_COMBAT;

                    sprintf(outbuf, "%s takes a break to tell you:\n%s\n\n", p->name, p->msg);
                    to_client(*head, opp, outbuf, strlen(outbuf));

                    status_update(*head, p);
                    status_update(*head, opp);

                    // clear p->msg
                    for (int i = 0; i < MSG_SIZE; i++)
                        p->msg[i] = 0;
                }
                break;
            }
                

            default:
                break;
        }
        printf("Received %d bytes: %s", len, buf);
        // sprintf(outbuf, "%s says: %s", inet_ntoa(p->ipaddr), buf);
        // broadcast(*head, outbuf, strlen(outbuf));
        return 0;
    } else if (len <= 0) {
        // client hasn't entered their name yet
        if (p->state == ST_LOGIN) return -1;

        // if client is in combat state, inform their opponent
        if (p->state == ST_COMBAT || p->state == ST_SPEAK) {
            struct client *opp = findclient(*head, p->combat.against);
            if (opp != NULL) {
                sprintf(outbuf, "\n--%s dropped. You win!\n\nAwaiting next opponent...\n", p->name);
                to_client(*head, opp, outbuf, strlen(outbuf));

                p->state = ST_WAIT; // not really necessary since p will be removed but whatever
                opp->state = ST_WAIT;

                lb_by_id(p->id)->losses++;
                lb_by_id(opp->id)->wins++;

                // move players to end of the list after match ends
                move_to_end(head, p);
                move_to_end(head, opp);
            }
        }

        sprintf(outbuf, "\n**%s leaves**\n", p->name);
        broadcast(*head, outbuf, strlen(outbuf));
        return -1;
    }
    return 0; // unreachable
}

 /* bind and listen, abort on error
  * returns FD of listening socket
  */
int bindandlisten(void) {
    struct sockaddr_in r;
    int listenfd;

    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        exit(1);
    }
    int yes = 1;
    if ((setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int))) == -1) {
        perror("setsockopt");
    }
    memset(&r, '\0', sizeof(r));
    r.sin_family = AF_INET;
    r.sin_addr.s_addr = INADDR_ANY;
    r.sin_port = htons(PORT);

    if (bind(listenfd, (struct sockaddr *)&r, sizeof r)) {
        perror("bind");
        exit(1);
    }

    if (listen(listenfd, 5)) {
        perror("listen");
        exit(1);
    }
    return listenfd;
}

static struct client *addclient(struct client *top, int fd, struct in_addr addr) {
    struct client *p = calloc(1, sizeof(struct client));
    if (!p) {
        perror("malloc");
        exit(1);
    }

    printf("Adding client %s\n", inet_ntoa(addr));

    p->id = IOTA++;
    p->state = ST_LOGIN;

    p->last_fought = -1;

    p->fd = fd;
    p->ipaddr = addr;
    p->next = top;
    top = p;

    return top;
}

static struct client *removeclient(struct client *top, int id) {
    struct client **p;

    for (p = &top; *p && (*p)->id != id; p = &(*p)->next)
        ;
    // Now, p points to (1) top, or (2) a pointer to another client
    // This avoids a special case for removing the head of the list
    if (*p) {
        struct client *t = (*p)->next;
        printf("Removing client %d %s\n", id, inet_ntoa((*p)->ipaddr));
        free(*p);
        *p = t;
    } else {
        fprintf(stderr, "Trying to remove id %d, but I don't know about it\n",
                 id);
    }
    return top;
}

static void move_to_end(struct client **top, struct client *node) {
    // precondition: node is a list in top (so top != null)
    struct client *prev = NULL;
    struct client *curr = *top;

    // Traverse the list to find the given node and its previous node
    while (curr && curr != node) {
        prev = curr;
        curr = curr->next;
    }

    // if node is already at the end
    if (curr == NULL || curr->next == NULL)
        return;

    // will hold the next position after curr
    struct client *temp = curr->next;

    // otherwise, remove cur from its current position
    if (prev != NULL) {
        prev->next = curr->next;
    }
    else {
        // if prev is null, then node == top
        // since curr->next != NULL, top->next is not null
        *top = curr->next;
    }
    // curr == node is at the end now
    curr->next = NULL;

    // now find the last node, we can start from temp
    while (temp->next)
        temp = temp->next;

    // Append the given node to the end
    temp->next = node;
}

static struct client *findclient(struct client *top, int id) {
    struct client *p;
    for (p = top; p != NULL && p->id != id; p = p->next)
        ;
    return p;
}


static void broadcast(struct client *top, char *s, int size) {
    broadcast_except(top, s, size, -1); // no negative id's, so this broadcasts to everyone
}

static void broadcast_except(struct client *top, char *s, int size, int skip) {
    struct client *p;
    for (p = top; p != NULL; p = p->next) {
        if (p->id == skip) continue;
        write(p->fd, s, size);
    }
}

static void to_client(struct client *top, struct client *p, char *s, int size) {
    if (top) {} // temporarily ease compiler
    // TODO: error check this call
    write(p->fd, s, size);
}

static void welcome(struct client *top, struct client *p) {
    // precondition, p's name is filled out
    
    char outbuf[128];

    sprintf(outbuf, "Welcome, %s! Awaiting opponent...", p->name);
    to_client(top, p, outbuf, strlen(outbuf));

    sprintf(outbuf, "\n**%s enters the arena**\n", p->name);
    broadcast_except(top, outbuf, strlen(outbuf), p->id);
}

static void make_matches(struct client *top) {
    // make all possible matches
    // do it the dumb n^2 way
    struct client *A, *B;
    for (A = top; A != NULL; A = A->next) {
        for (B = top; B != NULL; B = B->next) {
            if (!compatible(A, B)) continue;
            match(A, B);

            // communicate start of match to A and B
            char outbuf[128];

            sprintf(outbuf, "You engage %s!\n", A->name);
            to_client(top, B, outbuf, strlen(outbuf));

            sprintf(outbuf, "You engage %s!\n", B->name);
            to_client(top, A, outbuf, strlen(outbuf));

            // send status update to A and B
            status_update(top, A);
            status_update(top, B);
        }
    }
}

static void match(struct client *A, struct client *B) {
    A->combat = new_combat_details(B->id);
    B->combat = new_combat_details(A->id);

    A->last_fought = B->id;
    B->last_fought = A->id;

    A->state = ST_COMBAT;
    B->state = ST_COMBAT;

    // randomly decide which player to make active first
    if (rand() % 2 == 0) {
        A->combat.active = true;
        B->combat.active = false;
    } else {
        A->combat.active = false;
        B->combat.active = true;
    }
}

static void toggle(bool *b) {
    *b = !(*b);
}

static bool compatible(struct client *A, struct client *B) {
    return (
           A->id != B->id                                         /*have to be different clients*/
        && A->state == ST_WAIT && B->state == ST_WAIT             /*have to be in waiting state*/
        && (A->last_fought != B->id || B->last_fought != A->id)   /*can't have just fought each other*/
    );
}

static void status_update(struct client *top, struct client *p) {
    // precondition: p and opp are in ST_COMBAT state
    struct client *opp = findclient(top, p->combat.against);
    if (opp == NULL) {
        fprintf(stderr, "could not find client, shouldn't happen!\n");
        exit(1);
    }

    char outbuf[512];
    sprintf(
        outbuf
       ,("Your hitpoints: %d\n"
        "Your powermoves: %d\n\n"
        "%s's hitpoints: %d\n")
       ,p->combat.hp
       ,p->combat.pmoves
       ,opp->name
       ,opp->combat.hp
    );
    to_client(top, p, outbuf, strlen(outbuf));

    if (p->combat.active) {
        char* menu = (
            p->combat.pmoves <= 0
            ? "\n(a)ttack\n(s)peak something\n(f)orfeit\n"
            : "\n(a)ttack\n(p)owermove\n(s)peak something\n(f)orfeit\n"
        );
        to_client(top, p, menu, strlen(menu));
    } else {
        sprintf(outbuf, "Waiting for %s to strike...\n", opp->name);
        to_client(top, p, outbuf, strlen(outbuf));
    }
}

static int rand_between(int low, int max) {
    // return random integer in range [low, max]
    // where max <= RAND_MAX
    return low + (rand() % (max - low + 1));
}

static struct combat new_combat_details(int against) {
    struct combat combat = {0};
    combat.against = against;
    combat.hp = rand_between(20, 30);
    combat.pmoves = rand_between(1, 3);
    combat.active = 0;

    return combat;
}

static void expand_leaderboard() {
    if (LB_SIZE >= LB_CAP) {
        struct lb_entry *nLB = calloc(LB_CAP * 2, sizeof(struct lb_entry));
        if (nLB == NULL) {
            perror("malloc leaderboard");
            exit(1);
        }

        for (int i = 0; i < LB_SIZE; i++) {
            nLB[i] = LB[i];
        }
        free(LB);
        LB = nLB;
        LB_CAP *= 2;
    }
}

static struct lb_entry *lb_by_id(int id) {
    for (int i = 0; i < LB_SIZE; i++) {
        if (LB[i].id == id) return LB + i;
    }
    return NULL;
}

static int compare_lb_entry(const void *a, const void *b) {
    struct lb_entry ea = *((struct lb_entry *) a);
    struct lb_entry eb = *((struct lb_entry *) b);

    double sa = 0;
    double sb = 0;

    if (ea.wins + ea.losses > 0) {
        sa = ((double) ea.wins) / ((double) (ea.wins + ea.losses));
    }

    if (eb.wins + eb.losses > 0) {
        sb = ((double) eb.wins) / ((double) (eb.wins + eb.losses));
    }

    if (sa > sb) return -1;
    if (sa < sb) return  1;
    return 0;
}

static char *print_lb(void) {
    char* outbuf = calloc(128 * (LB_SIZE + 1), sizeof(char));
    char* head = outbuf;
    head = head + sprintf(head, "\nPlayer (id) | Wins | Losses\n");
    for (int i = 0; i < LB_SIZE; i++) {
        head = head + sprintf(head, "%s (%d) | %d | %d\n", LB[i].name, LB[i].id, LB[i].wins, LB[i].losses);
    }
    return outbuf;
}