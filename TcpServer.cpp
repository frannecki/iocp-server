#include <iterator>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>

#include "TcpServer.h"
#include "ThreadPool.h"

const int kMaxSendSize = 1024;
const int kMaxRecvSize = 1024;

LPFN_ACCEPTEX fn_acceptex = nullptr;

static void print_err(const char* str) {
  int err = WSAGetLastError();
	printf("%s failed: %d\n", str, err);
  if (err != WSAECONNRESET)
		exit(-1);
}

struct SocketIoStatus {
	WSAOVERLAPPED overlapped_;
	int status_;

	SocketIoStatus(int status = 0) 
		: status_(status) {
		memset(&overlapped_, 0, sizeof(overlapped_));
	};
};

namespace utils {

static int CreateSocket() {
	int ret;
	int zero = 0;

	SOCKET fd = INVALID_SOCKET;
	fd = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_IP, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (fd == INVALID_SOCKET) {
		print_err("WSASocket");
		return -1;
	}

	// disable buffering and set linger
	if ((ret = setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (char*)&zero, sizeof(zero))) == SOCKET_ERROR) {
		print_err("setsockopt");
		return -1;
	}

	return (int)fd;
}

static int CreateListenSocket(const char* ip, uint32_t port) {
	int ret;
	int fd = CreateSocket();
	if (fd == -1)  return -1;

	struct sockaddr_in addr;
	socklen_t addr_len;
	if (inet_pton(AF_INET, ip, &addr.sin_addr) < 0) {
		print_err("inet_pton");
		return -1;
	}
	addr.sin_family = AF_INET;
	addr.sin_port = ntohs(port);
	addr_len = sizeof(addr);

	if ((ret = bind(fd, (struct sockaddr*)&addr, addr_len)) == SOCKET_ERROR) {
		print_err("bind");
		return -1;
	}

	if ((ret = listen(fd, SOMAXCONN)) == SOCKET_ERROR) {
		print_err("listen");
		return -1;
	}

	return fd;
}

static HANDLE UpdateIocpPort(HANDLE port, const Channel* channel) {
	HANDLE updated_port = CreateIoCompletionPort((HANDLE)(channel->fd()), port, (DWORD_PTR)channel, 0);
	if (updated_port == NULL) {
		print_err("CreateIoCompletionPort");
	}
	return updated_port;
}

static int CreateAcceptSocket(int listenfd, char* buffer) {
	int ret;
	int fd = CreateSocket();
	if (fd == -1)  return -1;

	DWORD bytes_recved = 0;

	SocketIoStatus* status = new SocketIoStatus(kIocpEventRead);

	ret = fn_acceptex(listenfd, fd, (LPVOID)buffer,
		kMaxRecvSize - 2 * (sizeof(sockaddr_in) + 16),
		sizeof(sockaddr_in) + 16, sizeof(sockaddr_in) + 16,
		&bytes_recved, (LPOVERLAPPED)status);
	if (ret == SOCKET_ERROR && ERROR_IO_PENDING != WSAGetLastError()) {
		print_err("AcceptEx");
		return ret;
	}
	return fd;
}

static int WSASend(int fd, LPWSABUF buf) {
	DWORD bytes_sent = 0;
	SocketIoStatus* status = new SocketIoStatus(kIocpEventWrite);

	int ret = WSASend(fd, buf, 1, &bytes_sent, 0,
										(LPWSAOVERLAPPED)status, NULL);

	if (ret == SOCKET_ERROR && ERROR_IO_PENDING != WSAGetLastError()) { //&& WSAENOTSOCK != WSAGetLastError()) {
		print_err("WSASend");
	}

	return ret;
}

static int WSARecv(int fd, LPWSABUF buf) {
	DWORD bytes_recved = 0;
	DWORD flags = 0;
	buf->len = kMaxRecvSize;
	SocketIoStatus* status = new SocketIoStatus(kIocpEventRead);

	int ret = WSARecv(fd, buf, 1, &bytes_recved, &flags,
						(LPWSAOVERLAPPED)status, NULL);

	if (ret == SOCKET_ERROR && (ERROR_IO_PENDING != WSAGetLastError())) {
		print_err("WSARecv");
	}

  return ret;
}

} // namespace utils

Buffer::Buffer() :
	read_index_(0),
	write_index_(0)
{

}

std::string Buffer::ReadAll() {
	std::string result;
	result.assign(buffer_.begin() + read_index_,
		buffer_.begin() + write_index_);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		write_index_ = read_index_ = 0;
	}
	return result;
}

void Buffer::Write(const std::string& content) {
	std::copy(content.begin(), content.end(),
		//std::back_inserter(buffer_));
		std::inserter(buffer_, buffer_.begin() + write_index_));
	{
		std::lock_guard<std::mutex> lock(mutex_);
		write_index_ += content.size();
	}
}

void Buffer::HaveRead(uint32_t n) {
	// n bytes have been read
	int readable = min(n, write_index_ - read_index_);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		read_index_ += readable;
		if (read_index_ >= write_index_) {
			write_index_ = read_index_ = 0;
		}
	}
}

int Buffer::Peek(char* buf, int len) {
	int readable = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		readable = min(len, write_index_ - read_index_);
	}
	memcpy(buf, buffer_.data(), readable);
	return readable;
}

int Buffer::ReadableBytes() {
	std::lock_guard<std::mutex> lock(mutex_);
	return write_index_ - read_index_;
}

bool Buffer::Empty() const {
	return read_index_ == write_index_;
}

Channel::Channel(int fd) : fd_(fd) {}

Channel::~Channel() {
	shutdown(fd_, SD_SEND);
	fprintf(stdout, "Closing socket fd = %d.\n", fd_);
	closesocket(fd_);
}

int Channel::fd() const {
	return fd_;
}

void Channel::HandleEvents(int status, int io_size) {
	// std::lock_guard<std::mutex> lock(mutex_);
	if (io_size < 0) {
		error_callback_();
	}
	switch (status) {
		case kIocpEventWrite:
			if (write_callback_)
				write_callback_(io_size);
			break;
		case kIocpEventRead:
			if (io_size == 0) {
				close_callback_();
			}
			else if (read_callback_) {
				read_callback_(io_size);
			}
		default: break;
	}
}

void Channel::SetReadCallback(const ChannelReadWriteCallback& callback) {
	read_callback_ = callback;
}

void Channel::SetWriteCallback(const ChannelReadWriteCallback& callback) {
	write_callback_ = callback;
}

void Channel::SetErrorCallback(const Callback& callback) {
	error_callback_ = callback;
}

void Channel::SetCloseCallback(const Callback& callback) {
	close_callback_ = callback;
}

Connection::Connection(int fd) :
	channel_(new Channel(fd)),
	write_io_pending_(false)
{
	channel_->SetReadCallback(std::bind(&Connection::OnReadCallback,
										this, std::placeholders::_1, true));
	channel_->SetWriteCallback(std::bind(&Connection::OnWriteCallback,
											this, std::placeholders::_1));
	channel_->SetCloseCallback(std::bind(&Connection::OnCloseCallback, this));
	channel_->SetErrorCallback(std::bind(&Connection::OnErrorCallback, this));
	in_wsa_buf_.buf = new char[kMaxRecvSize + 1];
	out_wsa_buf_.buf = new char[kMaxSendSize + 1];
}

Connection::~Connection() {
	channel_->SetReadCallback(nullptr);
	channel_->SetWriteCallback(nullptr);
	channel_->SetCloseCallback(nullptr);
	channel_->SetErrorCallback(nullptr);
  delete in_wsa_buf_.buf;
  delete out_wsa_buf_.buf;
	delete channel_;
}

void Connection::Send(const std::string& buf) {
	this->Send(buf.c_str(), buf.size());
}

void Connection::Send(const char* buf, int len) {
	if (len <= 0)  return;
	out_buffer_.Write(std::string(buf, buf + len));

	// asynchronous send (startup)
  int send_len = out_wsa_buf_.len =
      out_buffer_.Peek(out_wsa_buf_.buf, kMaxSendSize);
  PostWrite();
}

void Connection::Recv(const char* buf, int len) {
	if (len <= 0)  return;
	memcpy(in_wsa_buf_.buf, buf, len);
	OnReadCallback(len, false);
}

const Channel* Connection::channel() const {
	return channel_;
}

void Connection::SetReadCallback(const ReadCallback& callback) {
	read_callback_ = callback;
}

void Connection::SetWriteCallback(const WriteCallback& callback) {
	write_callback_ = callback;
}

void Connection::SetErrorCallback(const Callback& callback) {
	error_callback_ = callback;
}

void Connection::SetCloseCallback(const Callback& callback) {
	close_callback_ = callback;
}

HANDLE Connection::UpdateToCompletionPort(HANDLE port, const char* buf, int len) {
	port = utils::UpdateIocpPort(port, channel_);
	Recv(buf, len);
	PostRead();
	return port;
}

void Connection::OnReadCallback(int io_size, bool post_read) {
	in_buffer_.Write(std::string(in_wsa_buf_.buf,
									 in_wsa_buf_.len = io_size));
	if (read_callback_) {
		read_callback_(this, &in_buffer_);
	}

	if(post_read) PostRead();
}

void Connection::OnWriteCallback(int io_size) {
	// retrieve remaining data from buffer to send out

	write_io_pending_ = false;
	out_buffer_.HaveRead(io_size);

	if (write_callback_) write_callback_(this);

	out_wsa_buf_.len =
    out_buffer_.Peek(out_wsa_buf_.buf, kMaxSendSize);
	PostWrite();
}

void Connection::OnErrorCallback() {
	if (error_callback_) {
		error_callback_();
	}
}

void Connection::OnCloseCallback() {
	if (close_callback_) {
		close_callback_();
	}
}

void Connection::PostRead() {
	int ret = utils::WSARecv(channel_->fd(), &in_wsa_buf_);
  if (ret == SOCKET_ERROR) {
    switch (WSAGetLastError()) {
      case WSA_IO_PENDING:
        break;
      default:
        if (error_callback_) error_callback_();
    }
  }
}

void Connection::PostWrite() {
  if (out_wsa_buf_.len == 0 || write_io_pending_) return;
  write_io_pending_ = true;
  int ret = utils::WSASend(channel_->fd(), &out_wsa_buf_);
  if (ret == SOCKET_ERROR) {
    switch (WSAGetLastError()) {
      case WSA_IO_PENDING:
        break;
      default:
        if (error_callback_) error_callback_();
    }
  }
}

TcpServer::TcpServer(const char* ip, uint16_t port, int thread_num) :
	iocp_port_(NULL),
	cur_accept_fd_(0),
	thread_pool_(new ThreadPool(thread_num)),
	thread_num_(thread_num)
{
	int listen_fd = utils::CreateListenSocket(ip, port);
	channel_ = new Channel(listen_fd);
	listen_buffer_ = new char[kMaxRecvSize + 1];
}

TcpServer::~TcpServer() {
	delete channel_;
}

void TcpServer::Start() {
	iocp_port_ = utils::UpdateIocpPort(NULL, channel_);
	channel_->SetReadCallback(std::bind(&TcpServer::HandleAccept,
										this, std::placeholders::_1));

	// enqueue iocp tasks to thread pool
	for (int i = 0; i < thread_num_; ++i) {
		thread_pool_->emplace(std::bind(&TcpServer::WaitAndHandleCompletionStatus, this));
	}

	// first accept
	HandleAccept(0);
}

void TcpServer::SetReadCallback(const ReadCallback& callback) {
	read_callback_ = callback;
}

void TcpServer::SetWriteCallback(const WriteCallback& callback) {
	write_callback_ = callback;
}

void TcpServer::SetConnectionCallback(const WriteCallback& callback) {
	connection_callback_ = callback;
}

void TcpServer::HandleAccept(int io_size) {
	if (cur_accept_fd_ != 0) {
		int ret;
		int enable = channel_->fd();
		ret = setsockopt((SOCKET)cur_accept_fd_, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
											(char*)&enable, sizeof(SOCKET));
		if (ret == SOCKET_ERROR) {
			print_err("setsockopt error");
		}

		sockaddr_in addr;
		socklen_t addr_len = sizeof(addr);
		ret = ::getpeername(cur_accept_fd_, (struct sockaddr*)&addr, &addr_len);
		if (ret < 0) {
			print_err("getsockname");
		}

		char buf[50] = {0};
		if (inet_ntop(AF_INET, &(addr.sin_addr), buf, sizeof(buf)) == NULL) {
			print_err("inet_pton");
		}
		uint16_t port = ntohs(addr.sin_port);
		fprintf(stdout, "Accepted connection from %s:%u\n", buf, port);

		Connection* conn = new Connection(cur_accept_fd_);
		conn->SetReadCallback(read_callback_);
		conn->SetWriteCallback(write_callback_);
    conn->SetCloseCallback(std::bind(&TcpServer::OnCloseCallback, this, cur_accept_fd_));
		conn->SetErrorCallback(std::bind(&TcpServer::OnCloseCallback, this, cur_accept_fd_));
		if (connection_callback_) {
			connection_callback_(conn);
		}
		{
			std::lock_guard<std::mutex> lock(mutex_connections_);
			connections_.insert(std::pair<int, Connection*>(cur_accept_fd_, conn));
		}
		iocp_port_ = conn->UpdateToCompletionPort(iocp_port_, listen_buffer_, io_size);
	}

	if (fn_acceptex == nullptr) {
		DWORD bytes = 0;
		GUID guid = WSAID_ACCEPTEX;
		int ret = WSAIoctl(channel_->fd(), SIO_GET_EXTENSION_FUNCTION_POINTER,
								&guid, sizeof(guid), &fn_acceptex, sizeof(fn_acceptex),
								&bytes, NULL, NULL);
		if (ret == SOCKET_ERROR) {
			print_err("WSAIoctl");
		}
	}

	cur_accept_fd_ = utils::CreateAcceptSocket(channel_->fd(), listen_buffer_);
}

void TcpServer::WaitAndHandleCompletionStatus() {
	DWORD io_size = 0;
	Channel* channel = nullptr;
	LPWSAOVERLAPPED overlapped = nullptr;

	while (1) {
		bool ret = GetQueuedCompletionStatus(
			iocp_port_, &io_size, (PULONG_PTR)&channel,
			(LPOVERLAPPED*)&overlapped, INFINITE);
		if (!ret) {
			if (!overlapped) {
				// GetQueuedCompletionStatus failure
				// Reference: https://stackoverflow.com/questions/11210995/how-to-detect-disconnection-rst-fin-using-windows-iocp
				print_err("GetQueuedCompletionStatus");
				return;
			}
			else {
				fprintf(stdout, "GetQueuedCompletionStatus failed: %d.\n", WSAGetLastError());
				io_size = -1;
			}
		}

		if (channel == nullptr && overlapped == nullptr) {
			continue;
		}

		SocketIoStatus* status = reinterpret_cast<SocketIoStatus*>(overlapped);

		channel->HandleEvents(status->status_, io_size);
		
		if (status) {
			delete status;
			status = nullptr;
		}
	}
}

void TcpServer::OnCloseCallback(int fd) {
	std::lock_guard<std::mutex> lock(mutex_connections_);
	auto iter = connections_.find(fd);
	if (iter != connections_.end()) {
		delete iter->second;
		connections_.erase(iter);
	}
}
