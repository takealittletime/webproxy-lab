#include <stdio.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1024000
#define MAX_OBJECT_SIZE 102400
#define LRU_MAGIC_NUMBER 9999

#define CACHE_OBJS_COUNT 10

// User agent header for HTTP requests
static const char *user_agent_hdr =
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 "
    "Firefox/10.0.3\r\n";

// Format for the request line in HTTP
static const char *requestline_hdr_format = "GET %s HTTP/1.0\r\n";
static const char *endof_hdr = "\r\n"; // End of header marker
static const char *host_hdr_format = "Host: %s\r\n"; // Host header format
static const char *conn_hdr = "Connection: close\r\n"; // Connection header
static const char *prox_hdr = "Proxy-Connection: close\r\n"; // Proxy connection header

// Keys for specific headers
static const char *host_key = "Host";
static const char *connection_key = "Connection";
static const char *proxy_connection_key = "Proxy-Connection";
static const char *user_agent_key = "User-Agent";

// Function prototypes
void *thread(void *vargsp);
void doit(int connfd);
void parse_uri(char *uri, char *hostname, char *path, int *port);
void build_http_header(char *http_header, char *hostname, char *path, int port, rio_t *client_rio);
int connect_endServer(char *hostname, int port, char *http_header);

// Cache functions
void cache_init();
int cache_find(char *url);
void cache_uri(char *uri, char *buf);

// Reader functions for managing access to the cache
void readerPre(int i);
void readerAfter(int i);

// Structure to hold cache objects
typedef struct 
{
    char cache_obj[MAX_OBJECT_SIZE]; // Cache object data
    char cache_url[MAXLINE];          // Cached URL
    int LRU;                          // LRU (Least Recently Used) counter

    int isEmpty;                      // Flag to check if the cache block is empty

    int readCnt;                      // Number of readers accessing this block
    sem_t wmutex;                     // Mutex for writing
    sem_t rdcntmutex;                 // Mutex for reader count
} cache_block;   

// Structure to hold the entire cache
typedef struct
{
    cache_block cacheobjs[CACHE_OBJS_COUNT]; // Array of cache blocks
    int cache_num;                           // Current number of cached objects
} Cache;

Cache cache; // Global cache variable

int main(int argc, char **argv) {
    int listenfd, *connfdp;
    socklen_t clientlen;
    char hostname[MAXLINE], port[MAXLINE];
    pthread_t tid;
    struct sockaddr_storage clientaddr;

    // Initialize cache
    cache_init();

    // Check for correct usage
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port> \n", argv[0]);
        exit(1);  
    }
    Signal(SIGPIPE, SIG_IGN); // Ignore SIGPIPE signal
    listenfd = Open_listenfd(argv[1]); // Open listening socket
    while (1) {
        clientlen = sizeof(clientaddr);
        connfdp = Malloc(sizeof(int)); // Allocate memory for connection file descriptor
        *connfdp = Accept(listenfd, (SA *)&clientaddr, &clientlen); // Accept connection

        Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
        printf("Accepted connection from (%s %s).\n", hostname, port);

        Pthread_create(&tid, NULL, thread, connfdp); // Create a new thread to handle the connection
    }
    return 0;
}

// Thread function to handle connections
void* thread(void *vargp) {
    int connfd = *((int*)vargp); // Get connection file descriptor
    Pthread_detach(pthread_self()); // Detach thread
    Free(vargp);  // Free allocated memory for connection file descriptor
    doit(connfd); // Process the request
    Close(connfd); // Close connection
    return NULL;
}

// Function to handle client requests
void doit(int connfd) {
    int end_serverfd;

    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char endserver_http_header[MAXLINE];
    char hostname[MAXLINE], path[MAXLINE];
    int port;
    
    rio_t rio, server_rio;

    Rio_readinitb(&rio, connfd); // Initialize Rio for reading from client
    Rio_readlineb(&rio, buf, MAXLINE); // Read request line
    sscanf(buf, "%s %s %s", method, uri, version); // Parse request line

    // Check if the method is GET
    if (strcasecmp(method, "GET")) {
        printf("Proxy does not implement the method");
        return;
    }
    
    char url_store[100];
    strcpy(url_store, uri); // Store the URL for caching

    int cache_index;
    // Check if the URL is in the cache
    if ((cache_index = cache_find(url_store)) != -1) {
        readerPre(cache_index); // Acquire read lock
        Rio_writen(connfd, cache.cacheobjs[cache_index].cache_obj, strlen(cache.cacheobjs[cache_index].cache_obj)); // Send cached response
        readerAfter(cache_index); // Release read lock
        return;
    }
    
    parse_uri(uri, hostname, path, &port); // Parse URI to get hostname, path, and port

    build_http_header(endserver_http_header, hostname, path, port, &rio); // Build HTTP header for the request

    end_serverfd = connect_endServer(hostname, port, endserver_http_header); // Connect to the end server
    if (end_serverfd < 0) {
        printf("connection failed\n");
        return;
    }

    Rio_readinitb(&server_rio, end_serverfd); // Initialize Rio for reading from the end server

    Rio_writen(end_serverfd, endserver_http_header, strlen(endserver_http_header)); // Send request to the end server

    char cachebuf[MAX_OBJECT_SIZE];
    int sizebuf = 0; // Size of the received buffer
    size_t n; 
    while ((n = Rio_readlineb(&server_rio, buf, MAXLINE)) != 0) { // Read response from the end server
        sizebuf += n; // Update buffer size
        if (sizebuf < MAX_OBJECT_SIZE)  
            strcat(cachebuf, buf); // Append response to cache buffer
        Rio_writen(connfd, buf, n); // Send response to the client
    }
    Close(end_serverfd); // Close connection to the end server

    // Cache the response if it is not too large
    if (sizebuf < MAX_OBJECT_SIZE) {
        cache_uri(url_store, cachebuf); // Cache the response
    }
}

// Function to build the HTTP header for the request
void build_http_header(char *http_header, char *hostname, char *path, int port, rio_t *client_rio) {
    char buf[MAXLINE], request_hdr[MAXLINE], other_hdr[MAXLINE], host_hdr[MAXLINE];
    
    sprintf(request_hdr, requestline_hdr_format, path); // Create the request line

    while (Rio_readlineb(client_rio, buf, MAXLINE) > 0) { // Read headers from the client
        if (strcmp(buf, endof_hdr) == 0)
            break; // End of headers

        if (!strncasecmp(buf, host_key, strlen(host_key))) {
            strcpy(host_hdr, buf); // Store host header
            continue;
        }

        // Store other headers (Connection, Proxy-Connection, User-Agent)
        if (!strncasecmp(buf, connection_key, strlen(connection_key))
            && !strncasecmp(buf, proxy_connection_key, strlen(proxy_connection_key))
            && !strncasecmp(buf, user_agent_key, strlen(user_agent_key))) {
                strcat(other_hdr, buf);
        }
    }
    // If no host header is present, add it
    if (strlen(host_hdr) == 0) {
        sprintf(host_hdr, host_hdr_format, hostname);
    }
    // Combine all parts into the final HTTP header
    sprintf(http_header, "%s%s%s%s%s%s%s",
            request_hdr,
            host_hdr,
            conn_hdr,
            prox_hdr,
            user_agent_hdr,
            other_hdr,
            endof_hdr);
    return;
}

// Function to connect to the end server
inline int connect_endServer(char *hostname, int port, char *http_header) {
    char portStr[100];
    sprintf(portStr, "%d", port); // Convert port number to string
    return Open_clientfd(hostname, portStr); // Open client connection to the end server
}

// Function to parse the URI and extract hostname, path, and port
void parse_uri(char *uri, char *hostname, char *path, int *port) {
    *port = 80; // Default port is 80
    char *pos = strstr(uri, "//"); // Find the start of the hostname

    pos = pos != NULL ? pos + 2 : uri; // Move past the "//"

    char *pos2 = strstr(pos, ":"); // Check for port specification
    if (pos2 != NULL) {
        *pos2 = '\0'; // Null-terminate the hostname
        strcpy(hostname, pos); // Copy hostname
        *port = atoi(pos2 + 1); // Get port number after ':'
    } else {
        pos2 = strstr(pos, "/"); // Find the start of the path
        if (pos2 != NULL) {
            *pos2 = '\0'; // Null-terminate the hostname
            strcpy(hostname, pos); // Copy hostname
            strcpy(path, pos2); // Copy path
        } else {
            strcpy(hostname, pos); // Copy hostname
            path[0] = '/'; // Default path if not specified
            path[1] = '\0';
        }
    }
}

// Cache initialization function
void cache_init() {
    cache.cache_num = 0; // Set initial cache count to zero
    for (int i = 0; i < CACHE_OBJS_COUNT; i++) {
        cache.cacheobjs[i].isEmpty = 1; // Mark all cache blocks as empty
        cache.cacheobjs[i].LRU = LRU_MAGIC_NUMBER; // Initialize LRU counter
        cache.cacheobjs[i].readCnt = 0; // Initialize reader count
        Sem_init(&cache.cacheobjs[i].wmutex, 0, 1); // Initialize write mutex
        Sem_init(&cache.cacheobjs[i].rdcntmutex, 0, 1); // Initialize read count mutex
    }
}

// Function to find the index of a cached URL
int cache_find(char *url) {
    for (int i = 0; i < CACHE_OBJS_COUNT; i++) {
        if (!cache.cacheobjs[i].isEmpty && strcmp(cache.cacheobjs[i].cache_url, url) == 0) {
            return i; // Return index if URL is found in cache
        }
    }
    return -1; // URL not found
}

// Function to handle reader access for cache blocks
void readerPre(int i) {
    Sem_wait(&cache.cacheobjs[i].rdcntmutex); // Acquire read count mutex
    cache.cacheobjs[i].readCnt++; // Increment reader count
    if (cache.cacheobjs[i].readCnt == 1) {
        Sem_wait(&cache.cacheobjs[i].wmutex); // Acquire write lock if first reader
    }
    Sem_post(&cache.cacheobjs[i].rdcntmutex); // Release read count mutex
}

// Function to release reader access for cache blocks
void readerAfter(int i) {
    Sem_wait(&cache.cacheobjs[i].rdcntmutex); // Acquire read count mutex
    cache.cacheobjs[i].readCnt--; // Decrement reader count
    if (cache.cacheobjs[i].readCnt == 0) {
        Sem_post(&cache.cacheobjs[i].wmutex); // Release write lock if no readers left
    }
    Sem_post(&cache.cacheobjs[i].rdcntmutex); // Release read count mutex
}

// Function to evict the least recently used cache block
int cache_eviction() {
    int min = 0; // Minimum LRU value
    int minindex = 0; // Index of the block to evict
    for (int i = 0; i < CACHE_OBJS_COUNT; i++) {
        if (cache.cacheobjs[i].isEmpty) {
            continue; // Skip empty blocks
        }
        if (cache.cacheobjs[i].LRU < min) {  
            min = cache.cacheobjs[i].LRU; // Update minimum LRU value
            minindex = i; // Update index to evict
        }
        readerAfter(i); // Release reader lock after checking
    }
    return minindex; // Return index of block to evict
}

// Function to cache a URI and its associated data
void cache_uri(char *uri, char *buf) {
    if (cache.cache_num == CACHE_OBJS_COUNT) {
        int minindex = cache_eviction(); // Find block to evict
        cache.cacheobjs[minindex].isEmpty = 1; // Mark evicted block as empty
        cache.cache_num--; // Decrement cache count
    }
    int i;  
    for (i = 0; i < CACHE_OBJS_COUNT; i++) {
        if (cache.cacheobjs[i].isEmpty) {
            strcpy(cache.cacheobjs[i].cache_url, uri); // Copy URL into cache
            strcpy(cache.cacheobjs[i].cache_obj, buf); // Copy data into cache
            cache.cacheobjs[i].LRU = 0; // Reset LRU counter
            cache.cacheobjs[i].isEmpty = 0; // Mark block as occupied
            cache.cache_num++; // Increment cache count
            return;
        }
    }
}
