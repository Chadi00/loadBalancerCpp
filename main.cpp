#include <iostream>
#include <string>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <thread>
#include <mutex>
#include <csignal>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <algorithm>

using namespace std;

// Mock backend servers
const vector<string> servers = { "host.docker.internal:8081", "host.docker.internal:8082", "host.docker.internal:8083" };
vector<int> active_connections(servers.size(), 0);
mutex connections_mutex;

// Function to choose servers based on the least connections
void get_least_connection_server(string& host, string& port) {
    lock_guard<mutex> lock(connections_mutex);
    int min_index = 0;
    for (int i = 1; i < active_connections.size(); ++i) {
        if (active_connections[i] < active_connections[min_index]) {
            min_index = i;
        }
    }
    string server = servers[min_index];
    auto pos = server.find(':');
    if (pos != string::npos) {
        host = server.substr(0, pos);
        port = server.substr(pos + 1);
    } else {
        host = server;  
        port = "80";    // Default HTTP port
    }
    active_connections[min_index]++;
}

// Function to handle each client request in a separate thread
void handle_client(int client_sockfd) {
    char buffer[4096];
    string host, port;

    get_least_connection_server(host, port);  // Get the server with least connections

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

    {
        lock_guard<mutex> lock(connections_mutex);
        auto pos = find(servers.begin(), servers.end(), host + ":" + port);
        if (pos != servers.end()) {
            ptrdiff_t index = distance(servers.begin(), pos);
            active_connections[index]--;
        }
    }

    close(server_sockfd);
    close(client_sockfd);
}

volatile sig_atomic_t keep_running = 1;

void signal_handler(int sig) {
    keep_running = 0;
}


int main() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in server_addr;

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8080);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    listen(sockfd, 10);

    cout << "Load Balancer running on port 8080" << endl;

    while (keep_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_sockfd = accept(sockfd, (struct sockaddr *)&client_addr, &client_len);
        if (client_sockfd < 0) continue;

        thread t(handle_client, client_sockfd);
        t.detach();
    }

    close(sockfd);
    return 0;
}
