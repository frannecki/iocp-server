# TCP server based on Windows IOCP

A minimal tcp server framework which uses Windows IOCP for asynchronous socket io.

<details>
  <summary>Client testing code</summary>
  
  ```cpp
  #include <iostream>

  #include <thread>
  #include <mutex>
  #include <vector>

  #include <Winsock2.h>
  #include <ws2tcpip.h>

  #pragma comment(lib,"ws2_32.lib")

  const int kBufferLen = 1024;
  const int kClientNumber = 120;
  static std::mutex mutex;
  static int cnt = 0;

  void _perror(const char* msg) {
      fprintf(stderr, "%s failed: %d\n", msg, WSAGetLastError());
  }

  class EchoClient {
  public:
      EchoClient() :
          running_(true)
      {
          thread_ = std::thread(&EchoClient::ThreadFunc, this);
      }

      ~EchoClient() {
          Stop();
      }

  private:

      void Stop() {
          running_ = false;
          thread_.join();
      }

      int ThreadFunc() {
          char buffer[kBufferLen] = "areyouok";
          int sockfd;
          struct sockaddr_in addr;
          socklen_t addr_len;
          int count = 0;
          int ret;
          {
              std::lock_guard<std::mutex> lock(mutex);
              sockfd = socket(AF_INET, SOCK_STREAM, 0);
              fprintf(stdout, "Socket created. fd = %d\n", sockfd);
          }
          if (sockfd < 0) {
              _perror("socket");
              return -1;
          }

          if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) < 0) {
              _perror("inet_pton");
              return -1;
          }
          addr.sin_family = AF_INET;
          addr.sin_port = ntohs(8090);
          addr_len = sizeof(addr);
          if (connect(sockfd, (struct sockaddr*)&addr, addr_len) < 0) {
              _perror("connect");
              return -1;
          }
          else {
              fprintf(stdout, "Connected to server. rank: %d. fd = %d\n", ++cnt, sockfd);
          }

          while (count < 5) {
              ret = send(sockfd, buffer, strlen(buffer), 0);
              if (ret < 0) {
                  _perror("send");
                  break;
              }
              else if (ret == 0) {
                  break;
              }
              else {
                  ret = recv(sockfd, buffer, ret, 0);
                  if (ret < 0) {
                      _perror("recv");
                      break;
                  }
                  else {
                      // fprintf(stdout, "%d bytes written.\n", ret);
                  }
              }

              count += 1;
          }

          if (shutdown(sockfd, SD_SEND) < 0) {
              _perror("shutdown write");
              return -1;
          }

          while ((ret = recv(sockfd, buffer, kBufferLen, 0)) != 0) {
              if (ret < 0) {
                  _perror("read");
              }
              else {
                  buffer[ret] = 0;
                  fprintf(stdout, "Received buffer of %d bytes from server after shutdown: %s\n", ret, buffer);
              }
          }

          fprintf(stdout, "connection closed\n");

          if (closesocket(sockfd) < 0) {
              _perror("close");
              return -1;
          }
          else {
              fprintf(stdout, "Closed socket. fd = %d\n", sockfd);
          }
          return 0;
      }

  private:
      std::thread thread_;
      bool running_;
  };

  int main() {
      WSADATA wsa_data;

      if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != NO_ERROR) {
          return -1;
      }

      std::vector<EchoClient*> clients(kClientNumber, nullptr);
      for (int i = 0; i < kClientNumber; ++i) {
          clients[i] = new EchoClient;
      }
      for (int i = 0; i < kClientNumber; ++i) {
          delete clients[i];
          clients[i] = nullptr;
      }
      WSACleanup();

      return 0;
  }

  ```
</details>


## References
* [Windows-classic-samples](https://github.com/microsoft/Windows-classic-samples/blob/main/Samples/Win7Samples/netds/winsock/iocp/serverex/IocpServerex.Cpp)
* [How to detect disconnection (RST/FIN) using Windows IOCP?](https://stackoverflow.com/questions/11210995/how-to-detect-disconnection-rst-fin-using-windows-iocp)
