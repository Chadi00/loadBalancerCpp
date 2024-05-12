#include <iostream>
#include <string>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <csignal>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/select.h>
#include <atomic>
#include <functional>

using namespace std;

// Mock backend servers on host machine
const vector<string> servers = {"host.docker.internal:8081", "host.docker.internal:8082", "host.docker.internal:8083"};
atomic<int> rr_index(0); // Round-robin index
mutex rr_mutex; // Mutex for round-robin index

// Task queue and synchronization primitives for thread pool
queue<function<void()>> tasks;
mutex queue_mutex;
condition_variable condition;
atomic<bool> server_running(true);

void make_socket_non_blocking(int sockfd) {
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags == -1) {
        cerr << "Error getting socket flags\n";
        return;
    }
    flags |= O_NONBLOCK;
    if (fcntl(sockfd, F_SETFL, flags) == -1) {
        cerr << "Error setting socket to non-blocking\n";
    }
}

// Function to get server using round robin method
void get_round_robin_server(string &host, string &port) {
    lock_guard<mutex> lock(rr_mutex);
    string server = servers[rr_index % servers.size()];
    rr_index = (rr_index + 1) % servers.size(); // Increment index for round robin

    size_t pos = server.find(':');
    if (pos != string::npos) {
        host = server.substr(0, pos);
        port = server.substr(pos + 1);
    } else {
        host = server;
        port = "80"; 
    }
}

void worker_thread() {
    function<void()> task;
    while (true) {
        {
            unique_lock<mutex> lock(queue_mutex);
            condition.wait(lock, []{ return !tasks.empty() || !server_running; });
            if (!server_running && tasks.empty()) break;
            task = tasks.front();
            tasks.pop();
        }
        task(); 
    }
}

void handle_client(int client_sockfd) {
    char buffer[4096];
    string host, port;

    get_round_robin_server(host, port); // Get the server with round robin ditribution

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    getaddrinfo(host.c_str(), port.c_str(), &hints, &res);

    int server_sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (connect(server_sockfd, res->ai_addr, res->ai_addrlen) != 0) {
        cerr << "Failed to connect to host: " << host << ":" << port << endl;
        close(client_sockfd);
        return;
    }

    cout << "Client " << client_sockfd << " connected to server " << host << ":" << port << endl;

    ssize_t n;
    while ((n = read(client_sockfd, buffer, sizeof(buffer))) > 0) {
        write(server_sockfd, buffer, n);
        
        ssize_t s = read(server_sockfd, buffer, sizeof(buffer));
        if (s > 0) {
            write(client_sockfd, buffer, s);
        }
    }

    close(server_sockfd);
    close(client_sockfd);
}

void initialize_thread_pool(size_t num_threads) {
    for (size_t i = 0; i < num_threads; ++i) {
        thread(worker_thread).detach();
    }
}

int main() {
    signal(SIGINT, [](int){ server_running = false; condition.notify_all(); });
    signal(SIGTERM, [](int){ server_running = false; condition.notify_all(); });

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    make_socket_non_blocking(sockfd);  // Make the socket non blocking

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8080);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    listen(sockfd, 10);

    fd_set master_set;
    FD_ZERO(&master_set);
    FD_SET(sockfd, &master_set);
    int max_sd = sockfd;

    cout << "Load Balancer running on port 8080" << endl;

    initialize_thread_pool(10);

    while (server_running) {
        fd_set read_fds = master_set;
        if (select(max_sd + 1, &read_fds, NULL, NULL, NULL) < 0) {
            cerr << "Select error." << endl;
            continue;
        }

        if (FD_ISSET(sockfd, &read_fds)) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_sockfd = accept(sockfd, (struct sockaddr *)&client_addr, &client_len);
            if (client_sockfd < 0) {
                if (errno != EWOULDBLOCK) {
                    cerr << "Accept failed." << endl;
                    continue;
                }
            } else {
                make_socket_non_blocking(client_sockfd);  // Make client socket non blocking
                {
                    lock_guard<mutex> lock(queue_mutex);
                    tasks.emplace([client_sockfd]{ handle_client(client_sockfd); });
                    condition.notify_one();
                }
            }
        }
    }

    close(sockfd);
    return 0;
}
