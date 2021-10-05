#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <queue>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <functional>

using Callback = std::function<void()>;

class ThreadPool {
public:
	ThreadPool(size_t capacity = 10);
	~ThreadPool();
	void emplace(Callback&& f);
	void join();

private:
	size_t capacity_;	// total number of threads
	bool stopped_;
	std::queue<Callback> tasks_;
	std::mutex mutex_;
	std::vector<std::thread> threads_;
	std::condition_variable condition_;
};

#endif
