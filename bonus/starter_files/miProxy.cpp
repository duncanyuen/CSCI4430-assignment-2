#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netdb.h>
#include <string.h>
#include <unistd.h>

#include <vector>
#include <unordered_map>
#include <fstream>
#include <algorithm>
#include <cassert>
#include <iostream>
#include <string>
#include <sys/types.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <csignal>
#include <sys/time.h>

#define BACKLOG 10
#define MAXPACKETSIZE 65536

using std::string;
using std::to_string;
using std::max;
using std::cout;
using std::endl;
using std::ofstream;
using std::vector;
using std::unordered_map;


int main(int argc, char const *argv[]) {
    if (argc != 5 || argc != 7){
        //error
        cout << "Usage: ./miProxy <listen-port> <www-ip> <alpha> <log>" << endl;
    } else if (argc == 5) {
        //normal mode
        int listen_port = atoi(argv[1]);
        string www_ip = string(argv[2]);
        float alpha = atof(argv[3]);
        char *log_path = (char *)argv[4];

    } else if (argc == 7) {
        //bonus part
    }

    //configure port to listen on
    int socket_fd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_fd < 0){
        exit(1);
    }

    struct sockaddr_in proxy_addr;
    proxy_addr.sin_family = AF_INET;
    proxy_addr.sin_addr.s_addr = INADDR_ANY;
    proxy_addr.sin_port = htons(listen_port);

    int retv = bind(socket_fd, (struct sockaddr *) proxy_addr, sizeof(proxy_addr));
    if (retv < 0){
        exit(1);
    }

    retv = listen(socket_fd, BACKLOG);
    if (retv < 0){
        exit(1);
    }

    fd_set read_fd_set;
    vector<int> fds;
    vector<int> bitrates;
    //stores the throughput for each "CDN"
    unordered_map<int, double> throughput_map;

    ofstream log_out;
    log_out.open(log_path);

    while (true){

        //why find max fd???
        FD_ZERO(&read_fd_set);
        FD_SET(socket_fd, &read_fd_set); //add socket_fd to read_fd_set
        for (int i = 0; i < (int)fds.size(); i++;){
            FD_SET(fds[i], &read_fd_set);
        }
        int max_fd = 0;
        if (fds.size() > 0){
            max_fd = *max_element(fds.begin(), fds.end());
        }
        max_fd = max(maxfd, socket_fd);

        // maxfd + 1 is important
        // why?
        retv = select(maxfd + 1, &readSet, NULL, NULL, NULL);
        if (retv < 0){
            exit(1);
        }

        if (FD_ISSET(socket_fd, &read_fd_set)){
            //socket_fd is part of read_fd_set
            int clientsd = accept(sockfd, NULL, NULL);
            if (clientsd < 0){
                exit(1);
            } else {
                //append clientsd to fds
                fds.push_back(clientsd);
            }
        }
    }
