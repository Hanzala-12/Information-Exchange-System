#include <iostream>
#include <string>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <algorithm> // For std::max

// --- Configuration ---
#define SERVER_IP "127.0.0.1"   // Server IP address
#define TCP_PORT 5000           // Server TCP Port
#define BUFFER_SIZE 1024

// --- Global Variables ---
int tcp_sock = -1;
int udp_sock = -1;
std::string campus_name;
bool running = true;
int local_udp_port = -1; // New global variable for the unique port

// --- Function Prototypes ---
void receive_handler(int tcp_fd, int udp_fd);
int setup_udp_listener(int port_num); // Now accepts a port number
int setup_tcp_connection(const std::string& name, int udp_port);

// ====================================================================
//                           MAIN CLIENT LOGIC
// ====================================================================

int main(int argc, char *argv[]) {
    // Must now expect 3 arguments: ./client <CampusName> <Local_UDP_Port>
    if (argc != 3) { 
        std::cerr << "Usage: " << argv[0] << " <CampusName> <Local_UDP_Port (e.g., 5001, 5002)>" << std::endl;
        return EXIT_FAILURE;
    }

    campus_name = argv[1];
    
    // 1. Get the unique UDP port from arguments
    try {
        local_udp_port = std::stoi(argv[2]);
    } catch (...) {
        std::cerr << "Invalid port number provided: " << argv[2] << std::endl;
        return EXIT_FAILURE;
    }
    
    // 2. Setup UDP Listener Socket (for Server Broadcasts)
    udp_sock = setup_udp_listener(local_udp_port); // Pass the unique port
    if (udp_sock < 0) return EXIT_FAILURE;

    // 3. Setup TCP Connection (for Routing)
    tcp_sock = setup_tcp_connection(campus_name, local_udp_port); // Pass the unique port for registration
    if (tcp_sock < 0) {
        close(udp_sock);
        return EXIT_FAILURE;
    }
    
    std::cout << "🚀 Client '" << campus_name << "' started (TCP:" << TCP_PORT << ", UDP:" << local_udp_port << ")" << std::endl;

    // 4. Start dedicated thread for receiving TCP & UDP messages
    std::thread receiver_thread(receive_handler, tcp_sock, udp_sock);
    
    // 5. Main Thread: User Input and TCP Sending
    std::string line;
    std::cout << "\n[HELP] Commands:" << std::endl;
    std::cout << "       <DESTINATION>:<MESSAGE>  (e.g., Karachi:Hello)" << std::endl;
    std::cout << "       BROADCAST:<MESSAGE>      (Sends routing message to Server)" << std::endl;
    std::cout << "       exit / quit" << std::endl;

    while (running) {
        std::cout << "\n" << campus_name << " > ";
        // Clear input stream if needed, then get line
        if (!(std::getline(std::cin, line))) break; 

        if (line == "exit" || line == "quit") {
            running = false;
            shutdown(tcp_sock, SHUT_RDWR); 
            break;
        }

        if (line.empty()) continue;

        if (send(tcp_sock, line.c_str(), line.length(), 0) < 0) {
            perror("TCP send failed");
        }
    }

    if (receiver_thread.joinable()) {
        receiver_thread.join();
    }
    
    close(tcp_sock);
    close(udp_sock);
    std::cout << "\nClient '" << campus_name << "' shutting down." << std::endl;

    return EXIT_SUCCESS;
}

// ====================================================================
//                         SOCKET SETUP FUNCTIONS
// ====================================================================

// UDP listener now takes a port number
int setup_udp_listener(int port_num) {
    int sock;
    struct sockaddr_in addr;
    
    if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("UDP socket creation failed");
        return -1;
    }
    
    // Set socket options for reuse (optional but good practice)
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY; 
    addr.sin_port = htons(port_num); // Use the provided unique port

    if (bind(sock, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
        // The error will be reported here if the port is in use
        perror("UDP bind failed");
        close(sock);
        return -1;
    }
    
    std::cout << "[INFO] UDP listener bound to port " << port_num << std::endl;
    return sock;
}

// TCP connection logic remains mostly the same, just passing the port along
int setup_tcp_connection(const std::string& name, int udp_port) {
    int sock;
    struct sockaddr_in server_addr;

    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("TCP socket creation failed");
        return -1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(TCP_PORT);

    if (inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr) <= 0) {
        std::cerr << "Invalid address/Address not supported" << std::endl;
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("TCP connection failed");
        close(sock);
        return -1;
    }
    
    std::cout << "[INFO] TCP connection established with server." << std::endl;

    // Send initial registration message: <CAMPUS_NAME>:<UDP_PORT>
    std::string registration_msg = name + ":" + std::to_string(udp_port);
    if (send(sock, registration_msg.c_str(), registration_msg.length(), 0) < 0) {
        perror("Registration send failed");
        close(sock);
        return -1;
    }
    
    return sock;
}

// ====================================================================
//                         RECEIVER THREAD LOGIC
// ====================================================================

void receive_handler(int tcp_fd, int udp_fd) {
    char buffer[BUFFER_SIZE];
    fd_set readfds;
    int max_sd = std::max(tcp_fd, udp_fd);
    
    while (running) {
        FD_ZERO(&readfds);
        if (tcp_fd > 0) FD_SET(tcp_fd, &readfds);
        if (udp_fd > 0) FD_SET(udp_fd, &readfds);

        // Blocking call waits for activity on either socket
        int activity = select(max_sd + 1, &readfds, NULL, NULL, NULL);

        if ((activity < 0) && (errno != EINTR)) {
            if (running) {
                 perror("select error");
            }
            break;
        }

        // 1. Check TCP Socket (Inter-Campus Messages)
        if (FD_ISSET(tcp_fd, &readfds)) {
            int bytes_received = recv(tcp_fd, buffer, BUFFER_SIZE - 1, 0);
            if (bytes_received > 0) {
                buffer[bytes_received] = '\0';
                std::cout << "\n<-- TCP MESSAGE RECEIVED -->" << std::endl;
                std::cout << "   " << buffer << std::endl;
                std::cout << campus_name << " > " << std::flush;
            } else if (bytes_received == 0) {
                std::cout << "\n[SERVER] Server closed the connection. Exiting..." << std::endl;
                running = false;
                break;
            } else {
                if (running) {
                    perror("TCP recv failed");
                }
                running = false;
                break;
            }
        }

        // 2. Check UDP Socket (Broadcast/Status Messages)
        if (FD_ISSET(udp_fd, &readfds)) {
            struct sockaddr_in server_addr;
            socklen_t addr_len = sizeof(server_addr);
            
            int bytes_received = recvfrom(udp_fd, buffer, BUFFER_SIZE - 1, 0, (struct sockaddr*)&server_addr, &addr_len);
            
            if (bytes_received > 0) {
                buffer[bytes_received] = '\0';
                std::cout << "\n*** UDP BROADCAST RECEIVED ***" << std::endl;
                std::cout << "   " << buffer << std::endl;
                std::cout << campus_name << " > " << std::flush;
            }
        }
    }
}