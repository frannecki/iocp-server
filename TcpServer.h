#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include <functional>
#include <vector>
#include <string>
#include <mutex>
#include <map>

class Buffer;
class Connection;

using Callback = std::function<void()>;
using ChannelReadWriteCallback = std::function<void(int)>;
using ReadCallback = std::function<void(Connection*, Buffer*)>;
using WriteCallback = std::function<void(Connection*)>;

enum kIocpEvent : int {
	kIocpEventRead = 1 << 1,
	kIocpEventWrite = 1 << 2,
	kIocpEventError = 1 << 3,
};

class Buffer {
 public:
	Buffer();
	std::string ReadAll();
	void Write(const std::string& content);
	void HaveRead(uint32_t n);
	int Peek(char* buf, int len);
	int ReadableBytes();
	bool Empty() const;

 private:
	uint32_t read_index_, write_index_;
	std::vector<char> buffer_;

	std::mutex mutex_;
};

class Channel {
 public:
	Channel(int fd);
	~Channel();
	int fd() const;
	void HandleEvents(int status, int io_size);
	void SetReadCallback(const ChannelReadWriteCallback& callback);
	void SetWriteCallback(const ChannelReadWriteCallback& callback);
	void SetErrorCallback(const Callback& callback);
	void SetCloseCallback(const Callback& callback);

 private:
	int fd_;

	ChannelReadWriteCallback read_callback_;
	ChannelReadWriteCallback write_callback_;
	Callback error_callback_;
	Callback close_callback_;
};

class Connection {
 public:
	Connection(int fd);
	~Connection();
	void Send(const std::string& buf);
	void Send(const char* buf, int len);
	const Channel* channel() const;
	void SetReadCallback(const ReadCallback& callback);
	void SetWriteCallback(const WriteCallback& callback);
	void SetErrorCallback(const Callback& callback);
	void SetCloseCallback(const Callback& callback);
	HANDLE UpdateToCompletionPort(HANDLE port, const char* buf, int len);

 private:
	void OnReadCallback(int io_size, bool post_read);
	void OnWriteCallback(int io_size);
	void OnErrorCallback();
	void OnCloseCallback();
	void PostRead();
	void PostWrite();
	void Recv(const char* buf, int len);

	Channel* channel_;
	WSABUF out_wsa_buf_;
	WSABUF in_wsa_buf_;
	Buffer in_buffer_;
	Buffer out_buffer_;
	bool write_io_pending_;

	ReadCallback read_callback_;
	WriteCallback write_callback_;
	Callback error_callback_;
	Callback close_callback_;
};

class ThreadPool;
class TcpServer {
 public:
  TcpServer(const char* ip, uint16_t port, int thread_num = 16);
  ~TcpServer();
  void Start();

  void SetReadCallback(const ReadCallback& callback);
  void SetWriteCallback(const WriteCallback& callback);
  void SetConnectionCallback(const WriteCallback& callback);

private:
  void HandleAccept(int io_size);
  void WaitAndHandleCompletionStatus();

  void OnCloseCallback(int fd);

  Channel* channel_;
  char* listen_buffer_;
  HANDLE iocp_port_;
  int cur_accept_fd_;
  int thread_num_;
  std::map<int, Connection*> connections_;
  std::mutex mutex_connections_;

  ThreadPool* thread_pool_;

  ReadCallback read_callback_;
  WriteCallback write_callback_;
  WriteCallback connection_callback_;
};

#endif
