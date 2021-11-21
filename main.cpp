#include <iostream>
#include <functional>

#include <winsock2.h>
#include <ws2tcpip.h>

#include "TcpServer.h"

#pragma comment(lib,"ws2_32.lib")

void OnMessage(Connection* conn, Buffer* buffer) {
	std::string message = buffer->ReadAll();
	std::cout << "Recv message: " << message << std::endl;
	conn->Send(message);
}

int main(int argc, char** argv) {
	WSADATA wsa_data;
	if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != NO_ERROR) {
		return -1;
	}

	TcpServer server("127.0.0.1", 8090);
	server.SetReadCallback(std::bind(OnMessage,
										std::placeholders::_1,
										std::placeholders::_2));
	server.Start();
	while (1);

	WSACleanup();
	return 0;
}
