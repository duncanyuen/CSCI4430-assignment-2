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

/* Extract the video chunk name from request */
string get_chunkname(string request) {
    int get_index = request.find("GET");
    int end_index = request.find(' ', get_index + 4);
    int vod_index = request.rfind("vod", end_index);
    string fn = request.substr(vod_index + 4, end_index - vod_index - 4);
    //printf("%s\n", fn.c_str());
    return fn;
}

/* Check if the str is the video data format */
bool check_video_data(string str) {
    if(str.find("Seg") == -1) return false;
    if(str.find("Frag") == -1) return false;
    if(str.find("Frag") > str.find("Seg")) return true;
    return false;
}

/* Get the value from str based on the key */
string get_value(string str, string key) {
    int key_index = str.find(key);
    int space_index = str.find(' ', key_index);
    int end = str.find('\n', key_index);
    return str.substr(space_index + 1, end - space_index - 1);
}

/* Recerive the response from the server */
string recv_response(int server_sd){
    string data = "";
    char buf;
    while(1) {
        int bytesRecvd = recv(server_sd, &buf, 1, 0);

        if (bytesRecvd < 0)
        {
            printf("%s %d\n", __FUNCTION__, __LINE__);
            perror("recv");
            exit(-1);
        }

        data += buf;
        if (data.size() >= 4) {
            if(data.substr(data.size() - 4) == "\r\n\r\n")
                break;
        }
    }
    return data;
}

int main(int argc, char const *argv[]) {
    if (argc < 6 || argc > 7)
    {
        perror("Usage: ./miProxy <log> <alpha> <listen-port> <dns-ip> <dns-port> [<www-ip>]");
        return -1;
    }

    /* Parse cmd arguments */
    char * log_path = (char *) argv[1];
    float alpha = atof(argv[2]);
    int listen_port = atoi(argv[3]);

    string ip;

    if (argc == 6) {
        char * dns_ip = (char *) argv[4];
        char * dns_port = (char *) argv[5];

        /* GET WWW_IP FROM NAMESERVER */
        int dns_sockfd;
        struct addrinfo hints, *servinfo, *p;
        int rv;
        int numbytes;

        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;

        if ((rv = getaddrinfo(dns_ip, dns_port, &hints, &servinfo)) != 0) {
            fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
            return 1;
        }

        // loop through all the results and make a socket
        for(p = servinfo; p != NULL; p = p->ai_next) {
            if ((dns_sockfd = socket(p->ai_family, p->ai_socktype,
                                     p->ai_protocol)) == -1) {
                perror("talker: socket");
                continue;
            }

            break;
        }

        // set DNS headers n stuff, leave answer blank

        DNSQuestion q;
        strcpy(q.QNAME, "video.cs.jhu.edu");
        q.QCLASS = 1;
        q.QTYPE = 1;

        DNSHeader h;
        h.AA = 0;
        h.RD = 0;
        h.RA = 0;
        h.Z = 0;
        h.NSCOUNT = 0;
        h.ARCOUNT = 0;

        DNSMessage message;
        message.question = q;
        message.header = h;

        // Serialize and send DNS message struct
        if ((numbytes = sendto(dns_sockfd, reinterpret_cast<const char*>(&message), sizeof(message), 0,
                               p->ai_addr, p->ai_addrlen)) == -1) {
            exit(1);
        }

        // Deserialize and recv DNS message
        int bytes_recv;
        DNSMessage dns_response;

        if ((bytes_recv = recvfrom(dns_sockfd, reinterpret_cast<char*>(&dns_response), MAXPACKETSIZE-1 , 0,
                                   p->ai_addr, &p->ai_addrlen)) == -1) {
            perror("recvfrom");
            exit(1);
        }

        if (dns_response.header.RCODE == '3') {
            cout << "RCODE = 3; domain name does not exist" << endl;
            exit(1);
        }


        ip = string(dns_response.answer.RDATA);
        freeaddrinfo(servinfo);

        close(dns_sockfd);
    } else {
        ip = string(argv[6]);
    }

    char * www_ip = (char *) ip.c_str();

    /* Create a socket */
    int sockfd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockfd < 0)
    {
        printf("%s %d\n", __FUNCTION__, __LINE__);
        perror("socket");
        return -1;
    }

    /* Create sockaddr */
    struct sockaddr_in proxy_addr;
    proxy_addr.sin_family = AF_INET;
    proxy_addr.sin_addr.s_addr = INADDR_ANY;
    proxy_addr.sin_port = htons(listen_port);


    /* Binding */
    int ret = bind(sockfd, (const struct sockaddr *) &proxy_addr, sizeof(proxy_addr));
    if (ret < 0)
    {
        printf("%s %d\n", __FUNCTION__, __LINE__);
        perror("bind");
        close(sockfd);
        return -1;
    }

    /* Listening */
    ret = listen(sockfd, BACKLOG);
    if (ret < 0)
    {
        printf("%s %d\n", __FUNCTION__, __LINE__);
        perror("listen");
        close(sockfd);
        return -1;
    }

    // Set of file descriptors to listen to
    fd_set readSet;
    // Keep track of each file descriptor accepted
    vector<int> fds;
    // Data structure for storing bitrates
    vector<int> bitrate_vector;
    // Data structure for mapping with fd and throughput
    unordered_map<int, double> throughput_map;
    //ofstream to write log info into log_path file
    ofstream log_out;
    log_out.open(log_path);
    // run with sudo to make sure write happens

    while (true)
    {
        // Set up the readSet
        FD_ZERO(&readSet);
        FD_SET(sockfd, &readSet);
        for(int i = 0; i < (int) fds.size(); ++i)
        {
            FD_SET(fds[i], &readSet);
        }

        int maxfd = 0;
        if(fds.size() > 0)
        {
            maxfd = *max_element(fds.begin(), fds.end());
        }
        maxfd = max(maxfd, sockfd);

        // maxfd + 1 is important
        int err = select(maxfd + 1, &readSet, NULL, NULL, NULL);
        assert(err != -1);

        if(FD_ISSET(sockfd, &readSet))
        {
            int clientsd = accept(sockfd, NULL, NULL);
            if(clientsd == -1)
            {
                printf("%s %d\n", __FUNCTION__, __LINE__);
                cout << "Error on accept" << endl;
            }
            else
            {
                fds.push_back(clientsd);
            }
        }

        for(int i = 0; i < (int) fds.size(); ++i)
        {
            if(FD_ISSET(fds[i], &readSet))
            {
                char buf;
                string request;
                bool closed = false;
                struct timeval t1, t2;
                gettimeofday(&t1, NULL);
                int cur_fd = fds[i];

                /* Complete receiving the request from clients */
                while (1)
                {
                    int bytesRecvd = recv(cur_fd, &buf, 1, 0);

                    if(bytesRecvd < 0)
                    {
                        printf("%s %d\n", __FUNCTION__, __LINE__);
                        cout << "Error recving bytes" << endl;
                        cout << strerror(errno) << endl;
                        close(sockfd);
                        exit(1);
                    }
                    else if(bytesRecvd == 0) //end of recv
                    {
                        printf("client: %d, fds i = %d\n", cur_fd, i);
                        perror("Connection closed");
                        if (fds.size() != 0)
                            fds.erase(fds.begin() + i);
                        if (throughput_map.find(cur_fd) != throughput_map.end())
                            throughput_map.erase(cur_fd);
                        closed = true;
                        break;
                    }

                    request += buf;
                    if (request.size() >= 4)
                    {
                        if (request.substr(request.size() - 4) == "\r\n\r\n")
                            break;
                    }
                }

                /* Client Keep alive */
                if (closed == false)
                {
                    bool is_video_data = check_video_data(request);
                    int cur_bitrate;
                    string chunk_name;

                    /* Client is requesting video data,
                       modify the request for bitrate adaption */
                    if (is_video_data)
                    {
                        chunk_name = get_chunkname(request);
                        int seg_index = chunk_name.find("Seg");
                        int bitrate = atoi(chunk_name.substr(0, seg_index).c_str());
                        cur_bitrate = bitrate;

                        if (throughput_map.find(cur_fd) != throughput_map.end())
                        {
                            int chosen_bitrate;
                            int last_bitrate = bitrate_vector.front();

                            for(int b : bitrate_vector)
                            {
                                if (b <= (throughput_map[cur_fd] / 1.5))
                                    chosen_bitrate = b;
                                else
                                {
                                    chosen_bitrate = last_bitrate;
                                    break;
                                }
                                last_bitrate = b;
                            }
                            cur_bitrate = chosen_bitrate;
                            if (cur_bitrate != bitrate)
                            {
                                string new_name = to_string(cur_bitrate) + chunk_name.substr(seg_index);
                                int get_index = request.find("GET");
                                int end_index = request.find(' ', get_index + 4);
                                int vod_index = request.rfind("vod", end_index);
                                request.replace(vod_index + 4, end_index - vod_index - 4, new_name);
                                chunk_name = new_name;
                            }
                        }
                    }

                    /* Start send request to the server */
                    /* Create a socket */
                    int server_sd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
                    if (server_sd < 0) {
                        printf("%s %d\n", __FUNCTION__, __LINE__);
                        perror("socket");
                        close(sockfd);
                        return -1;
                    }

                    /* Connect */
                    struct hostent * host = gethostbyname(www_ip);
                    struct sockaddr_in server_addr;
                    server_addr.sin_family = AF_INET;
                    server_addr.sin_addr.s_addr = *(unsigned long *) host->h_addr_list[0];
                    server_addr.sin_port = htons(80);
                    int ret = connect(server_sd, (const struct sockaddr *)&server_addr, sizeof(server_addr));
                    if (ret < 0) {
                        printf("%s %d\n", __FUNCTION__, __LINE__);
                        perror("connect");
                        close(sockfd);
                        close(server_sd);
                        return -1;
                    }

                    /* Send */
                    if (send(server_sd, request.c_str(), request.size(), 0) < 0) {
                        printf("%s %d\n", __FUNCTION__, __LINE__);
                        perror("send");
                        close(sockfd);
                        close(server_sd);
                        return -1;
                    }

                    /* Receive from server */
                    string response = recv_response(server_sd);
                    string content_type = get_value(response, "Content-Type");
                    int content_len = atoi(get_value(response, "Content-Length").c_str());
                    for (int i = 0; i < content_len; i++) {
                        if (recv(server_sd, &buf, 1, 0) < 0) {
                            printf("%s %d\n", __FUNCTION__, __LINE__);
                            perror("recv");
                            close(sockfd);
                            close(server_sd);
                            return -1;
                        }
                        response += buf;
                    }

                    /* Getting video xml data and parse available bitrates */
                    if (content_type.find("text/xml") != -1)
                    {
                        int index = 0;
                        int cur = response.find("bitrate", index);
                        while (cur != -1)
                        {
                            int fst_index = response.find('\"', cur);
                            int snd_index = response.find('\"', fst_index);
                            int br = atoi(response.substr(fst_index + 1, snd_index - fst_index - 1).c_str());
                            bool new_br = true;
                            for (int b : bitrate_vector) {
                                if (br == b) {
                                    new_br = false;
                                    break;
                                }
                                new_br = true;
                            }

                            if (new_br)
                                bitrate_vector.push_back(br);
                            sort(bitrate_vector.begin(), bitrate_vector.end());
                            index = snd_index;
                            cur = response.find("bitrate", index);
                        }

                        /* Request nolist.f4m*/
                        string request_cp = request;
                        request_cp.replace(request.find(".f4m"), 4, "_nolist.f4m");

                        if(send(server_sd, request_cp.c_str(), request_cp.size(), 0) < 0)
                        {
                            printf("%s %d\n", __FUNCTION__, __LINE__);
                            perror("send");
                            close(sockfd);
                            close(server_sd);
                            return -1;
                        }

                        response = recv_response(server_sd);
                        content_len = atoi(get_value(response, "Content-Length").c_str());
                        for (int i = 0; i < content_len; i++) {
                            if (recv(server_sd, &buf, 1, 0) < 0) {
                                printf("%s %d\n", __FUNCTION__, __LINE__);
                                perror("recv");
                                close(sockfd);
                                close(server_sd);
                                return -1;
                            }
                            response += buf;
                        }
                    }
                        /* Getting video chunk, calculate the throughput and log out info*/
                    else if (content_type.find("video/f4f") != -1)
                    {
                        gettimeofday(&t2, NULL);
                        double duration = t2.tv_sec-t1.tv_sec +(t2.tv_usec-t1.tv_usec)/1000000.0;
                        double t_new = (content_len / 1000) / duration * 8;
                        double t_cur;
                        if (throughput_map.find(cur_fd) == throughput_map.end())
                            t_cur = bitrate_vector.front();
                        else
                            t_cur = throughput_map[cur_fd];

                        t_cur = alpha * t_new + (1 - alpha) * t_cur;
                        throughput_map[cur_fd] = t_cur;

                        log_out << duration << " " << t_new << " " << t_cur << " " << cur_bitrate << " " << string(www_ip) << " " << chunk_name << endl;
                        log_out.flush();
                    }

                    /* Send response to client */
                    if (send(cur_fd, response.c_str(), response.size(), 0) < 0)
                    {
                        printf("%s %d\n", __FUNCTION__, __LINE__);
                        perror("send");
                        close(sockfd);
                        close(server_sd);
                        return -1;
                    }
                }
            }
        }
    }
    log_out.close();
    close(sockfd);
    return 0;
}
