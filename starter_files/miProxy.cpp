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

using std::cout;
using std::endl;
using std::max;
using std::ofstream;
using std::string;
using std::to_string;
using std::unordered_map;
using std::vector;

// TODO: fix style

/* Extract the video chunk name from request */
string get_chunkname(string request)
{
    int get_index = request.find("GET");
    int end_index = request.find(' ', get_index + 4);
    int vod_index = request.rfind("vod", end_index);
    string fn = request.substr(vod_index + 4, end_index - vod_index - 4);
    // printf("%s\n", fn.c_str());
    return fn;
}

/* Check if the str is the video data format */
bool check_video_data(string str)
{
    if (str.find("Seg") == -1)
        return false;
    if (str.find("Frag") == -1)
        return false;
    if (str.find("Frag") > str.find("Seg"))
        return true;
    return false;
}

/* Get the value from str based on the key */
string get_value(string str, string key)
{
    int key_index = str.find(key);
    int space_index = str.find(' ', key_index);
    int end = str.find('\n', key_index);
    return str.substr(space_index + 1, end - space_index - 1);
}

/* Recerive the response from the server */
string recv_response(int server_sd)
{
    string data = "";
    char buf;
    while (1)
    {
        int bytesRecvd = recv(server_sd, &buf, 1, 0);

        if (bytesRecvd < 0)
        {
            printf("%s %d\n", __FUNCTION__, __LINE__);
            perror("recv");
            exit(-1);
        }

        data += buf;
        if (data.size() >= 4)
        {
            if (data.substr(data.size() - 4) == "\r\n\r\n")
                break;
        }
    }
    return data;
}

int main(int argc, char const *argv[])
{
    int listen_port;
    char *www_ip;
    float alpha;
    char *log_path;

    if (argc != 6 || argc != 7)
    {
        // error
        cout << "Usage: ./miProxy --nodns <listen-port> <www-ip> <alpha> <log>" << endl;
        cout << "Alternative: ./miProxy --dns <listen-port> <dns-ip> <dns-port> <alpha> <log>" << endl;
    }

    if (argc == 6)
    {
        // normal mode
        cout << "argc: 66666" << endl;
        listen_port = atoi(argv[2]);
        www_ip = (char *)argv[3];
        alpha = atof(argv[4]);
        log_path = (char *)argv[5];
    }
    else if (argc == 7)
    {
        // bonus part
    }

    // configure port to listen on
    int socket_fd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_fd < 0)
    {
        exit(1);
    }

    struct sockaddr_in proxy_addr;
    proxy_addr.sin_family = AF_INET;
    proxy_addr.sin_addr.s_addr = INADDR_ANY;
    proxy_addr.sin_port = htons(listen_port);

    int retv = bind(socket_fd, (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
    if (retv < 0)
    {
        perror("Error binding stream socket");
        exit(1);
    }

    retv = listen(socket_fd, BACKLOG);
    if (retv < 0)
    {
        exit(1);
    }

    fd_set read_fd_set;
    vector<int> fds;
    vector<int> bitrates;
    // stores the throughput for each "CDN"
    unordered_map<int, double> throughput_map;

    ofstream log_out;
    log_out.open(log_path);
                        
    while (true)
    {

        cout << "=====================Enter While Loop=====================" << endl;
        // why find max fd???
        FD_ZERO(&read_fd_set);
        FD_SET(socket_fd, &read_fd_set); // add socket_fd to read_fd_set
        for (int i = 0; i < (int)fds.size(); i++)
        {
            FD_SET(fds[i], &read_fd_set);
        }
        int max_fd = 0;
        if (fds.size() > 0)
        {
            max_fd = *max_element(fds.begin(), fds.end());
        }
        max_fd = max(max_fd, socket_fd);

        // maxfd + 1 is important
        // why?
        retv = select(max_fd + 1, &read_fd_set, NULL, NULL, NULL);
        if (retv < 0)
        {
            perror("Error selecting");
            exit(1);
        }

        if (FD_ISSET(socket_fd, &read_fd_set))
        {
            // socket_fd is part of read_fd_set
            int clientsd = accept(socket_fd, NULL, NULL);
            if (clientsd < 0)
            {
                perror("Error accepting connection");
                exit(1);
            }
            else
            {
                // append clientsd to fds
                fds.push_back(clientsd);
            }
        }

        // copy first, ask questions later
        for (int i = 0; i < (int)fds.size(); ++i)
        {
            if (FD_ISSET(fds[i], &read_fd_set))
            {
                char buf;
                string request;
                bool closed = false;
                struct timeval t1, t2;
                gettimeofday(&t1, NULL);
                int cur_fd = fds[i];

                cout << "=====================Receiving From the client=====================" << endl;

                /* Complete receiving the request from clients */
                while (1)
                {
                    int bytesRecvd = recv(cur_fd, &buf, 1, 0);

                    if (bytesRecvd < 0)
                    {
                        printf("%s %d\n", __FUNCTION__, __LINE__);
                        cout << "Error recving bytes" << endl;
                        cout << strerror(errno) << endl;
                        close(socket_fd);
                        exit(1);
                    }
                    else if (bytesRecvd == 0) // end of recv
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
                printf("client request: \n%s", request.c_str());

                /* Client Keep alive */
                if (closed == false)
                {
                    cout << "=====================Check request=====================" << endl;
                    bool is_video_data = check_video_data(request);
                    int cur_bitrate;
                    string chunk_name;

                    /* Client is requesting video data,
                       modify the request for bitrate adaption */
                    if (is_video_data)
                    {
                        cout << "=====================Processing data=====================" << endl;
                        chunk_name = get_chunkname(request);
                        int seg_index = chunk_name.find("Seg");
                        int bitrate = atoi(chunk_name.substr(0, seg_index).c_str());
                        cur_bitrate = bitrate;

                        if (throughput_map.find(cur_fd) != throughput_map.end())
                        {
                            int chosen_bitrate;
                            int last_bitrate = bitrates.front();

                            for (int b : bitrates)
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
                    if (server_sd < 0)
                    {
                        printf("%s %d\n", __FUNCTION__, __LINE__);
                        perror("socket");
                        close(socket_fd);
                        return -1;
                    }

                    /* Connect */
                    struct hostent *host = gethostbyname(www_ip);
                    struct sockaddr_in server_addr;
                    server_addr.sin_family = AF_INET;
                    server_addr.sin_addr.s_addr = *(unsigned long *)host->h_addr_list[0];
                    server_addr.sin_port = htons(80);
                    int ret = connect(server_sd, (const struct sockaddr *)&server_addr, sizeof(server_addr));
                    if (ret < 0)
                    {
                        printf("%s %d\n", __FUNCTION__, __LINE__);
                        perror("connect");
                        close(socket_fd);
                        close(server_sd);
                        return -1;
                    }

                    /* Send */
                    if (send(server_sd, request.c_str(), request.size(), 0) < 0)
                    {
                        printf("%s %d\n", __FUNCTION__, __LINE__);
                        perror("send");
                        close(socket_fd);
                        close(server_sd);
                        return -1;
                    }

                    cout << "=====================Receiving request from the server=====================" << endl;
                    /* Receive from server */
                    string response = recv_response(server_sd);
                    string content_type = get_value(response, "Content-Type");
                    int content_len = atoi(get_value(response, "Content-Length").c_str());
                    for (int i = 0; i < content_len; i++)
                    {
                        if (recv(server_sd, &buf, 1, 0) < 0)
                        {
                            printf("%s %d\n", __FUNCTION__, __LINE__);
                            perror("recv");
                            close(socket_fd);
                            close(server_sd);
                            return -1;
                        }
                        response += buf;
                    }
                    printf("server response: \n%s", response.c_str());

                    /* Getting video xml data and parse available bitrates */
                    if (content_type.find("text/xml") != -1)
                    {
                        cout << "=====================Getting Video Xml Data=====================" << endl;
                        int index = 0;
                        int cur = response.find("bitrate", index);
                        while (cur != -1)
                        {
                            int fst_index = response.find('\"', cur);
                            int snd_index = response.find('\"', fst_index);
                            int br = atoi(response.substr(fst_index + 1, snd_index - fst_index - 1).c_str());
                            bool new_br = true;
                            for (int b : bitrates)
                            {
                                if (br == b)
                                {
                                    new_br = false;
                                    break;
                                }
                                new_br = true;
                            }

                            if (new_br)
                                bitrates.push_back(br);
                            std::sort(bitrates.begin(), bitrates.end());
                            index = snd_index;
                            cur = response.find("bitrate", index);
                        }

                        /* Request nolist.f4m*/
                        string request_cp = request;
                        request_cp.replace(request.find(".f4m"), 4, "_nolist.f4m");

                        if (send(server_sd, request_cp.c_str(), request_cp.size(), 0) < 0)
                        {
                            printf("%s %d\n", __FUNCTION__, __LINE__);
                            perror("send");
                            close(socket_fd);
                            close(server_sd);
                            return -1;
                        }

                        response = recv_response(server_sd);
                        content_len = atoi(get_value(response, "Content-Length").c_str());
                        for (int i = 0; i < content_len; i++)
                        {
                            if (recv(server_sd, &buf, 1, 0) < 0)
                            {
                                printf("%s %d\n", __FUNCTION__, __LINE__);
                                perror("recv");
                                close(socket_fd);
                                close(server_sd);
                                return -1;
                            }
                            response += buf;
                        }
                    }
                    /* Getting video chunk, calculate the throughput and log out info*/
                    else if (content_type.find("video/f4f") != -1)
                    {
                        cout << "=====================Getting Video Chunk=====================" << endl;

                        gettimeofday(&t2, NULL);
                        double duration = t2.tv_sec - t1.tv_sec + (t2.tv_usec - t1.tv_usec) / 1000000.0;
                        double t_new = (content_len / 1000) / duration * 8;
                        double t_cur;
                        if (throughput_map.find(cur_fd) == throughput_map.end())
                            t_cur = bitrates.front();
                        else
                            t_cur = throughput_map[cur_fd];

                        t_cur = alpha * t_new + (1 - alpha) * t_cur;
                        throughput_map[cur_fd] = t_cur;

                        cout << " " << cur_bitrate << " " << chunk_name << " " << string(www_ip) << " " << duration << " " << throughput_map[cur_fd] << " " << t_cur << " " << cur_bitrate << endl;
                        log_out << " " << cur_bitrate << " " << chunk_name << " " << string(www_ip) << " " << duration << " " << throughput_map[cur_fd] << " " << t_cur << " " << cur_bitrate << endl;
                        log_out.flush();
                    }

                    /* Send response to client */
                    if (send(cur_fd, response.c_str(), response.size(), 0) < 0)
                    {
                        printf("%s %d\n", __FUNCTION__, __LINE__);
                        perror("send");
                        close(socket_fd);
                        close(server_sd);
                        return -1;
                    }
                }
            }
        }
    }
    log_out.close();
    close(socket_fd);
    return 0;
}
