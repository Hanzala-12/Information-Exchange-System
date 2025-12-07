#include <iostream>
#include <vector>
#include <map>
#include <string>
#include <sstream>
#include <thread>
#include <mutex>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// --- Configuration ---
#define TCP_PORT 5000       // Server TCP Listening Port
#define BUFFER_SIZE 1024
#define SERVER_BROADCAST_IP "127.0.0.1" // Server sends UDP from this IP

// --- Global Structures & Synchronization ---
struct ClientInfo {
    int tcp_socket;             // TCP socket file descriptor for sending/receiving
    std::string campus_name;    // Unique campus identifier (e.g., "Lahore")
    struct sockaddr_in udp_addr; // UDP address for sending broadcasts to this client
};

// Map to store active clients: Key = Campus Name, Value = ClientInfo
std::map<std::string, ClientInfo> active_clients;
std::mutex clients_mutex;       // Mutex to protect access to active_clients map
int udp_broadcast_socket;       // Single UDP socket for all broadcast sending

// --- Function Prototypes ---
void handle_client(int client_sock, struct sockaddr_in client_addr);
void handle_server_input();
void send_udp_broadcast(const std::string& message);
void route_tcp_message(const std::string& sender_name, const std::string& full_message);

// ====================================================================
//                             MAIN SERVER LOGIC
// ====================================================================

int main() {
    int listen_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(struct sockaddr_in);

    // 1. Setup TCP Listener Socket
    if ((listen_sock = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("TCP socket creation failed");
        exit(EXIT_FAILURE);
    }
    
    // Set socket options (optional, but good practice for reuse)
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(TCP_PORT);

    if (bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("TCP bind failed");
        close(listen_sock);
        exit(EXIT_FAILURE);
    }
    
    if (listen(listen_sock, 5) < 0) {
        perror("TCP listen failed");
        close(listen_sock);
        exit(EXIT_FAILURE);
    }
    
    // 2. Setup UDP Broadcast Sender Socket
    if ((udp_broadcast_socket = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("UDP socket creation failed");
        close(listen_sock);
        exit(EXIT_FAILURE);
    }

    std::cout << "🌐 NU-Information Exchange Server started." << std::endl;
    std::cout << "TCP listening on port " << TCP_PORT << " for client connections..." << std::endl;
    
    // 3. Start Server Input Thread for Broadcasts
    std::thread input_thread(handle_server_input);
    input_thread.detach(); // Allow the thread to run independently

    // 4. Main TCP Accept Loop
    while (true) {
        int client_sock = accept(listen_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (client_sock < 0) {
            perror("TCP accept failed");
            continue;
        }

        std::cout << "\n[INFO] New TCP connection accepted from " 
                  << inet_ntoa(client_addr.sin_addr) << ":" << ntohs(client_addr.sin_port) 
                  << std::endl;

        // Spawn a new thread to handle the client's communication
        std::thread client_thread(handle_client, client_sock, client_addr);
        client_thread.detach(); // Detach the thread to run independently
    }

    // Cleanup (This part is typically unreachable in an infinite loop server)
    close(listen_sock);
    close(udp_broadcast_socket);
    return 0;
}

// ====================================================================
//                        CLIENT HANDLER THREAD
// ====================================================================

void handle_client(int client_sock, struct sockaddr_in client_addr) {
    char buffer[BUFFER_SIZE];
    std::string campus_name;
    bool is_registered = false;
    int udp_port = -1;
    
    // 1. Initial Registration (Expecting: <CAMPUS_NAME>:<UDP_PORT>)
    int bytes_received = recv(client_sock, buffer, BUFFER_SIZE - 1, 0);
    if (bytes_received > 0) {
        buffer[bytes_received] = '\0';
        std::string initial_msg(buffer);
        size_t colon_pos = initial_msg.find(':');
        
        if (colon_pos != std::string::npos) {
            campus_name = initial_msg.substr(0, colon_pos);
            try {
                udp_port = std::stoi(initial_msg.substr(colon_pos + 1));
                is_registered = true;
                
                // Construct UDP address for future broadcasts
                struct sockaddr_in udp_dest_addr;
                memset(&udp_dest_addr, 0, sizeof(udp_dest_addr));
                udp_dest_addr.sin_family = AF_INET;
                // Client's TCP IP address is used for UDP broadcast destination
                udp_dest_addr.sin_addr.s_addr = client_addr.sin_addr.s_addr; 
                udp_dest_addr.sin_port = htons(udp_port);

                // Register the client in the global map
                std::lock_guard<std::mutex> lock(clients_mutex);
                active_clients[campus_name] = {client_sock, campus_name, udp_dest_addr};

                std::cout << "[REGISTRATION] Client '" << campus_name << "' registered. UDP port: " << udp_port << std::endl;
                
                // Acknowledge registration
                std::string welcome_msg = "SERVER: Welcome, " + campus_name + "! TCP and UDP services active.";
                send(client_sock, welcome_msg.c_str(), welcome_msg.length(), 0);

            } catch (...) {
                std::cerr << "[ERROR] Invalid UDP port format during registration." << std::endl;
            }
        }
    }
    
    if (!is_registered) {
        std::cerr << "[ERROR] Client failed to register name/port. Closing socket." << std::endl;
        close(client_sock);
        return;
    }

    // 2. Main TCP Message Receiving Loop (Inter-Campus Routing)
    while ((bytes_received = recv(client_sock, buffer, BUFFER_SIZE - 1, 0)) > 0) {
        buffer[bytes_received] = '\0';
        std::string message(buffer);
        
        // Route the message
        route_tcp_message(campus_name, message);
    }
    
    // 3. Client Disconnect/Error
    if (bytes_received == 0) {
        std::cout << "[DISCONNECT] Client '" << campus_name << "' disconnected gracefully." << std::endl;
    } else {
        perror("[ERROR] recv failed");
    }

    // Unregister and cleanup
    close(client_sock);
    std::lock_guard<std::mutex> lock(clients_mutex);
    active_clients.erase(campus_name);
    std::cout << "[INFO] Client '" << campus_name << "' removed from active list." << std::endl;
}

// ====================================================================
//                           ROUTING LOGIC
// ====================================================================

void route_tcp_message(const std::string& sender_name, const std::string& full_message) {
    size_t colon_pos = full_message.find(':');
    
    if (colon_pos == std::string::npos) {
        std::cerr << "[ERROR] Invalid message format from " << sender_name << ": " << full_message << std::endl;
        return;
    }
    
    std::string destination = full_message.substr(0, colon_pos);
    std::string content = full_message.substr(colon_pos + 1);
    
    // Determine the final message string to send to the recipient
    std::string final_msg = "FROM " + sender_name + ": " + content;

    std::cout << "[TCP ROUTING] " << sender_name << " -> " << destination << std::endl;

    // Check for BROADCAST keyword
    if (destination == "BROADCAST") {
        std::string broadcast_msg = "BROADCAST FROM " + sender_name + ": " + content;
        send_udp_broadcast(broadcast_msg);
        return;
    }

    // Attempt to route to a specific campus
    std::lock_guard<std::mutex> lock(clients_mutex);
    auto it = active_clients.find(destination);

    if (it != active_clients.end()) {
        // Found the recipient, send TCP message
        int dest_sock = it->second.tcp_socket;
        if (send(dest_sock, final_msg.c_str(), final_msg.length(), 0) < 0) {
            perror("[ERROR] Failed to send routed TCP message");
            // Optionally remove the client if send fails
        } else {
            std::cout << "[SUCCESS] Routed to " << destination << "." << std::endl;
        }
    } else {
        // Recipient not found, inform the sender
        auto sender_it = active_clients.find(sender_name);
        if (sender_it != active_clients.end()) {
            std::string error_msg = "SERVER: Error: Campus '" + destination + "' is not currently active.";
            send(sender_it->second.tcp_socket, error_msg.c_str(), error_msg.length(), 0);
        }
        std::cerr << "[FAIL] Campus '" << destination << "' not found for routing." << std::endl;
    }
}

// ====================================================================
//                          BROADCAST LOGIC
// ====================================================================

void send_udp_broadcast(const std::string& message) {
    std::lock_guard<std::mutex> lock(clients_mutex);
    
    std::cout << "\n--- STARTING UDP BROADCAST ---" << std::endl;
    std::cout << "Message: " << message << std::endl;

    int success_count = 0;
    int fail_count = 0;

    for (const auto& pair : active_clients) {
        const ClientInfo& client = pair.second;
        // Broadcast messages are sent as UDP datagrams
        if (sendto(udp_broadcast_socket, 
                   message.c_str(), 
                   message.length(), 
                   0, 
                   (const struct sockaddr *)&client.udp_addr, 
                   sizeof(client.udp_addr)) < 0) 
        {
            std::cerr << "[UDP FAIL] Failed to send to " << pair.first << std::endl;
            fail_count++;
        } else {
            // std::cout << "[UDP SENT] to " << pair.first << std::endl;
            success_count++;
        }
    }
    
    std::cout << "--- BROADCAST SUMMARY ---" << std::endl;
    std::cout << "Total Active Clients: " << active_clients.size() << std::endl;
    std::cout << "Sent Successfully: " << success_count << std::endl;
    std::cout << "---------------------------\n" << std::endl;
}

// ====================================================================
//                           SERVER CONSOLE INPUT
// ====================================================================

void handle_server_input() {
    std::string line;
    std::cout << "\n[INFO] Console input active. Type 'BROADCAST:<message>' to send global UDP message." << std::endl;
    
    while (true) {
        std::cout << "Server > ";
        std::getline(std::cin, line);

        if (line.substr(0, 10) == "BROADCAST:") {
            // Use the dedicated UDP broadcast function
            send_udp_broadcast("SERVER BROADCAST: " + line.substr(10));
        } else if (line == "exit" || line == "quit") {
            std::cout << "Shutting down server..." << std::endl;
            // Note: Proper shutdown requires more complex signal handling, 
            // but for a simple console app, a manual kill is often used.
            exit(0);
        } else if (!line.empty()) {
            std::cout << "[WARNING] Unknown command. Use 'BROADCAST:<message>' or 'exit'." << std::endl;
        }
    }
}