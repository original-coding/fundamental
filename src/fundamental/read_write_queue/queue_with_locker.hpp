#pragma once

#include <condition_variable>
#include <mutex>
#include <queue>
#include <utility>

namespace Fundamental
{

// A Multiple-Producers Multiple-Consumer (MPMC) queue with max size
template <typename T>
class QueueMPMCBounded
{
public:
    explicit QueueMPMCBounded(std::size_t maxSize,
                              bool blockNow = true) :
    m_isBlocking(blockNow),
    m_maxSize(maxSize)
    {
    }

    // Same as std::queue::push
    // inserts element at the end
    bool Push(const T& item)
    {
        {
            std::unique_lock guard(m_queueMutex);

            // Wait if currently is blocking
            m_cvPush.wait(guard, [&]() {
                return m_queue.size() < m_maxSize || !m_isBlocking;
            });

            if (m_queue.size() == m_maxSize)
                return false;

            m_queue.push(item);
        }

        // Signal pop is available
        m_cvPop.notify_one();
        return true;
    }

    // Same as std::queue::push
    // inserts element at the end
    bool Push(T&& item)
    {
        {
            std::unique_lock guard(m_queueMutex);

            // Wait if currently is blocking
            m_cvPush.wait(guard, [&]() {
                return m_queue.size() < m_maxSize || !m_isBlocking;
            });

            if (m_queue.size() == m_maxSize)
                return false;

            m_queue.push(std::move(item));
        }

        // Signal pop is available
        m_cvPop.notify_one();
        return true;
    }

    // Same as std::queue::emplace
    // constructs element in-place at the end
    template <typename... Args>
    bool Emplace(Args&&... args)
    {
        // Same as Push
        {
            std::unique_lock guard(m_queueMutex);

            // Wait if currently is blocking
            m_cvPush.wait(guard, [&]() {
                return m_queue.size() < m_maxSize || !m_isBlocking;
            });

            if (m_queue.size() == m_maxSize)
                return false;

            m_queue.emplace(std::forward<Args>(args)...);
        }

        // Signal pop is available
        m_cvPop.notify_one();
        return true;
    }

    // Same as std::queue::pop
    // removes the first element
    bool Pop(T& outItem)
    {
        {
            std::unique_lock guard(m_queueMutex);

            // Wait if currently is blocking
            m_cvPop.wait(guard, [&]() {
                return !m_queue.empty() || !m_isBlocking;
            });

            if (m_queue.empty())
                return false;

            outItem = std::move(m_queue.front());
            m_queue.pop();
        }

        // Signal push is available
        m_cvPush.notify_one();
        return true;
    }

    // Same as std::queue::pop
    // removes the first element
    bool TryPop(T& outItem)
    {
        std::unique_lock guard(m_queueMutex, std::try_to_lock);

        if (!guard || m_queue.empty())
            return false;

        outItem = std::move(m_queue.front());
        m_queue.pop();
        return true;
    }

    std::size_t GetSize() const
    {
        std::scoped_lock guard(m_queueMutex);
        return m_queue.size();
    }

    std::size_t GetCapacity() const
    {
        return m_maxSize;
    }

    bool GetIsEmpty() const
    {
        std::scoped_lock guard(m_queueMutex);
        return m_queue.empty();
    }

    bool GetIsFull() const
    {
        std::scoped_lock lock(m_queueMutex);
        return m_queue.size() == GetCapacity();
    }

    // Manually block
    void Block()
    {
        std::scoped_lock guard(m_queueMutex);
        m_isBlocking = true;
    }

    // Manually unblock
    void UnBlock()
    {
        {
            std::scoped_lock guard(m_queueMutex);
            m_isBlocking = false;
        }
        m_cvPush.notify_all();
        m_cvPop.notify_all();
    }

    bool GetIsBlocking() const
    {
        std::scoped_lock guard(m_queueMutex);
        return m_isBlocking;
    }

private:
    std::queue<T> m_queue;
    bool m_isBlocking;
    const std::size_t m_maxSize;

    mutable std::mutex m_queueMutex;
    std::condition_variable m_cvPush;
    std::condition_variable m_cvPop;
};

// A Multiple-Producers Single-Consumer (MPSC) queue with NO max size
template <typename T>
class QueueMPSC
{
public:
    explicit QueueMPSC(bool blockNow = true) :
    m_isBlocking(blockNow)
    {
    }

    // Same as std::queue::push
    // inserts element at the end
    void Push(const T& item)
    {
        {
            std::scoped_lock guard(m_queueMutex);
            m_queue.push(item);
        }
        m_cv.notify_one();
    }

    // Same as std::queue::push
    // inserts element at the end
    void Push(T&& item)
    {
        {
            std::scoped_lock guard(m_queueMutex);
            m_queue.push(std::move(item));
        }
        m_cv.notify_one();
    }

    // Same as std::queue::emplace
    // constructs element in-place at the end
    template <typename... Args>
    void Emplace(Args&&... args)
    {
        {
            std::scoped_lock guard(m_queueMutex);
            m_queue.emplace(std::forward<Args>(args)...);
        }
        m_cv.notify_one();
    }

    // Same as std::queue::push
    // inserts element at the end and return if locked.
    bool TryPush(const T& item)
    {
        {
            std::unique_lock lock(m_queueMutex, std::try_to_lock);
            if (!lock)
                return false;
            m_queue.push(item);
        }
        m_cv.notify_one();
        return true;
    }

    // Same as std::queue::push
    // inserts element at the end and return if locked.
    bool TryPush(T&& item)
    {
        {
            // Use mutex::try_lock to test
            std::unique_lock lock(m_queueMutex, std::try_to_lock);
            if (!lock)
                return false;
            m_queue.push(std::move(item));
        }
        m_cv.notify_one();
        return true;
    }

    // Same as std::queue::pop
    // removes the first element
    bool Pop(T& outItem)
    {
        std::unique_lock guard(m_queueMutex);

        m_cv.wait(guard, [&]() { return !m_queue.empty() || !m_isBlocking; });
        if (m_queue.empty())
            return false;

        outItem = std::move(m_queue.front());
        m_queue.pop();
        return true;
    }

    // Same as std::queue::pop
    // removes the first element and return if locked
    bool TryPop(T& outItem)
    {
        std::unique_lock lock(m_queueMutex, std::try_to_lock);
        if (!lock || m_queue.empty())
            return false;
        outItem = std::move(m_queue.front());
        m_queue.pop();
        return true;
    }

    std::size_t GetSize() const
    {
        std::scoped_lock guard(m_queueMutex);
        return m_queue.size();
    }

    bool GetIsEmpty() const
    {
        std::scoped_lock guard(m_queueMutex);
        return m_queue.empty();
    }

    // Manually block
    void Block()
    {
        std::scoped_lock guard(m_queueMutex);
        m_isBlocking = true;
    }

    // Manually Unblock
    void UnBlock()
    {
        {
            std::scoped_lock guard(m_queueMutex);
            m_isBlocking = false;
        }
        m_cv.notify_all();
    }

    bool GetIsBlocking() const
    {
        std::scoped_lock guard(m_queueMutex);
        return m_isBlocking;
    }

private:
    std::queue<T> m_queue;
    bool m_isBlocking;

    mutable std::mutex m_queueMutex;
    std::condition_variable m_cv;
};

} // namespace RealiThreadSystem
