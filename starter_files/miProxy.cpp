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

using std::cout;
using std::endl;
using std::max;
using std::ofstream;
using std::string;
using std::to_string;
using std::unordered_map;
using std::vector;

// Get client ip address
string getClientIp(int fd)
{
    char client_ip[INET_ADDRSTRLEN];
    struct sockaddr_in client_addr;
    int client_len = sizeof(client_addr);
    getpeername(fd, (struct sockaddr *)&client_addr, (socklen_t *)&client_len);
    inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
    return (string)client_ip;
}

// Get the Chunkname
string getChunkName(string request)
{
    int get_index = request.find("GET");
    int end_index = request.find(' ', get_index + 4);
    int vod_index = request.rfind("vod", end_index);
    return request.substr(vod_index + 4, end_index - vod_index - 4);
}

// Get Response With New Chunkname which have the adaptive bitrate
string getResponseWithNewChunkName(string request, string newChunkName)
{
    int newChunkStartPos = request.find("GET") + 4;
    int newChunkEndPos = request.find(' ', newChunkStartPos);
    newChunkStartPos = request.rfind("vod", newChunkEndPos);
    return request.replace(newChunkStartPos + 4, newChunkEndPos - newChunkStartPos - 4, newChunkName);
}

// Check whether is the video
bool checkIsVideo(string str)
{
    if (str.find("Seg") == -1)
        return false;
    if (str.find("Frag") == -1)
        return false;
    if (str.find("Frag") > str.find("Seg"))
        return true;
    return false;
}

// Get the value based on the key
string getValue(string str, string key)
{
    int key_index = str.find(key);
    int space_index = str.find(' ', key_index);
    int end = str.find('\n', key_index);
    return str.substr(space_index + 1, end - space_index - 1);
}

// Receive the header response
string receiveResponseHeader(int server_sd)
{
    string response = "";
    char buf;
    while (true)
    {
        int bytesRecvd = recv(server_sd, &buf, 1, 0);

        if (bytesRecvd < 0)
        {
            perror("Error in recv");
            exit(-1);
        }

        response += buf;
        if (response.size() >= 4)
        {
            if (response.substr(response.size() - 4) == "\r\n\r\n")
                break;
        }
    }
    return response;
}

int main(int argc, char const *argv[])
{
    int listen_port;
    char *www_ip;
    float alpha;
    char *log_path;

    if (argc == 6)
    {
        listen_port = atoi(argv[2]);
        www_ip = (char *)argv[3];
        alpha = atof(argv[4]);
        log_path = (char *)argv[5];
    }
    else if (argc == 5)
    {
        listen_port = atoi(argv[1]);
        www_ip = (char *)argv[2];
        alpha = atof(argv[3]);
        log_path = (char *)argv[4];
    }
    else
    {
        cout << "Usage: ./miProxy [--nodns] <listen-port> <www-ip> <alpha> <log>" << endl;
    }

    // Configure port to listen on
    int socket_fd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_fd < 0)
    {
        return (-1);
    }

    struct sockaddr_in proxy_addr;
    proxy_addr.sin_family = AF_INET;
    proxy_addr.sin_addr.s_addr = INADDR_ANY;
    proxy_addr.sin_port = htons(listen_port);

    int retv = bind(socket_fd, (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
    if (retv < 0)
    {
        perror("Error binding stream socket");
        return (-1);
    }

    retv = listen(socket_fd, BACKLOG);
    if (retv < 0)
    {
        return (-1);
    }

    fd_set read_fd_set;
    vector<int> fds;
    vector<int> bitrates;
    unordered_map<int, double> throughput;
    string client_ip;

    ofstream logfile;
    logfile.open(log_path);

    while (true)
    {
        FD_ZERO(&read_fd_set);
        FD_SET(socket_fd, &read_fd_set);
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

        retv = select(max_fd + 1, &read_fd_set, NULL, NULL, NULL);
        if (retv < 0)
        {
            perror("Error selecting");
            return (-1);
        }

        if (FD_ISSET(socket_fd, &read_fd_set))
        {
            int clientsd = accept(socket_fd, NULL, NULL);
            if (clientsd < 0)
            {
                perror("Error accepting connection");
                return (-1);
            }
            else
            {
                fds.push_back(clientsd);
            }
        }

        for (int i = 0; i < (int)fds.size(); ++i)
        {
            if (FD_ISSET(fds[i], &read_fd_set))
            {
                char buf;
                string request;
                bool closed = false;
                struct timeval t1, t2;
                int cur_fd = fds[i];
                client_ip = getClientIp(fds[i]);

                while (true)
                {
                    // Receiving the request from clients
                    int bytesRecvd = recv(cur_fd, &buf, 1, 0);

                    if (bytesRecvd < 0)
                    {
                        perror("Error recving bytes");
                        close(socket_fd);
                        return (-1);
                    }
                    else if (bytesRecvd == 0) // end of recv
                    {
                        perror("Connection closed");
                        if (fds.size() != 0)
                            fds.erase(fds.begin() + i);
                        if (throughput.find(cur_fd) != throughput.end())
                            throughput.erase(cur_fd);
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

                if (closed == false)
                {
                    bool isVideo = checkIsVideo(request);
                    int currentBitrate;
                    string chunkName;

                    // get the correct chunkname if the received data is video
                    if (isVideo)
                    {
                        chunkName = getChunkName(request);
                        int seg_index = chunkName.find("Seg");
                        int bitrate = atoi(chunkName.substr(0, seg_index).c_str());
                        currentBitrate = bitrate;
                        int newBitrate;
                        int smallestBitrate = bitrates.front();
                        int calculatedThroughput = throughput[cur_fd] / 1.5;

                        for (int b : bitrates)
                        {
                            if (b <= calculatedThroughput)
                                newBitrate = b;
                            else
                            {
                                newBitrate = smallestBitrate;
                                break;
                            }
                            smallestBitrate = b;
                        }

                        currentBitrate = newBitrate;
                        if (currentBitrate != bitrate)
                        {
                            string newChunkName = to_string(currentBitrate) + chunkName.substr(seg_index);
                            request = getResponseWithNewChunkName(request, newChunkName);
                            chunkName = newChunkName;
                        }
                    }

                    int server_sd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
                    if (server_sd < 0)
                    {
                        perror("Error creating socket");
                        close(socket_fd);
                        return -1;
                    }

                    struct hostent *host = gethostbyname(www_ip);
                    struct sockaddr_in server_addr;
                    server_addr.sin_family = AF_INET;
                    server_addr.sin_addr.s_addr = *(unsigned long *)host->h_addr_list[0];
                    server_addr.sin_port = htons(80);
                    int ret = connect(server_sd, (const struct sockaddr *)&server_addr, sizeof(server_addr));
                    if (ret < 0)
                    {
                        perror("Error connection");
                        close(socket_fd);
                        close(server_sd);
                        return -1;
                    }

                    /* Send request to server */
                    if (send(server_sd, request.c_str(), request.size(), 0) < 0)
                    {
                        perror("Error sending to server");
                        close(socket_fd);
                        close(server_sd);
                        return -1;
                    }

                    /* Receive response from server */
                    string response = receiveResponseHeader(server_sd);
                    string contentType = getValue(response, "Content-Type");
                    int contentLength = atoi(getValue(response, "Content-Length").c_str());
                    gettimeofday(&t1, NULL);
                    for (int i = 0; i < contentLength; i++)
                    {
                        if (recv(server_sd, &buf, 1, 0) < 0)
                        {
                            perror("Error recv from server");
                            close(socket_fd);
                            close(server_sd);
                            return -1;
                        }
                        response += buf;
                    }
                    gettimeofday(&t2, NULL);

                    /* Get the available bitrates */
                    if (contentType.find("text/xml") != -1)
                    {
                        int index = 0;
                        int currentPos = response.find("bitrate=", index);
                        while (currentPos != -1)
                        {
                            int bitrateStartPos = response.find('\"', currentPos);
                            int bitrateEndPos = response.find('\"', bitrateStartPos + 1);
                            int bitrate = atoi(response.substr(bitrateStartPos + 1, bitrateEndPos - bitrateStartPos - 1).c_str());
                            bool isNew = true;
                            for (int b : bitrates)
                            {
                                if (bitrate == b)
                                {
                                    isNew = false;
                                    break;
                                }
                                isNew = true;
                            }

                            if (isNew)
                            {
                                bitrates.push_back(bitrate);
                            }

                            std::sort(bitrates.begin(), bitrates.end());
                            index = bitrateEndPos;
                            currentPos = response.find("bitrate=", index);
                        }

                        string request_chunk = request;
                        request_chunk.replace(request.find(".f4m"), 4, "_nolist.f4m");

                        // Sending video data request to server
                        if (send(server_sd, request_chunk.c_str(), request_chunk.size(), 0) < 0)
                        {
                            perror("Error sending to server");
                            close(socket_fd);
                            close(server_sd);
                            return -1;
                        }

                        // Receiving video xml data from server
                        response = receiveResponseHeader(server_sd);
                        contentLength = atoi(getValue(response, "Content-Length").c_str());
                        for (int i = 0; i < contentLength; i++)
                        {
                            if (recv(server_sd, &buf, 1, 0) < 0)
                            {
                                perror("Error recv from server");
                                close(socket_fd);
                                close(server_sd);
                                return -1;
                            }
                            response += buf;
                        }
                    }

                    // Calculate the throughput
                    else if (contentType.find("video/f4f") != -1)
                    {
                        double duration = t2.tv_sec - t1.tv_sec + (t2.tv_usec - t1.tv_usec) / 1000000.0;
                        double t_new = (contentLength / 1000) / duration * 8;
                        double t_cur;
                        if (throughput.find(cur_fd) == throughput.end())
                            t_cur = bitrates.front();
                        else
                            t_cur = throughput[cur_fd];

                        t_cur = alpha * t_new + (1 - alpha) * t_cur;
                        throughput[cur_fd] = t_cur;

                        logfile << client_ip << " " << chunkName << " " << string(www_ip) << " " << duration << " " << t_new << " " << t_cur << " " << currentBitrate << endl;
                        logfile.flush();
                    }

                    // Send response to client 
                    if (send(cur_fd, response.c_str(), response.size(), 0) < 0)
                    {
                        perror("Error sending to client");
                        close(socket_fd);
                        close(server_sd);
                        return -1;
                    }
                }
            }
        }
    }
    logfile.close();
    close(socket_fd);
    return 0;
}
