// KOPNET 有界队列回归测试。
//
// 核心所有权约定：put() 仅在返回 Ok 时接管 Packet（含其 fd）；
// Timeout/Closed 时调用方保留所有权。早期实现按值接收 Packet，
// 导致队列满后重试时把“已被移空的壳”入队——这里直接锁定该行为。
#include "kopnet/bounded_queue.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unistd.h>

static int failures = 0;

static void check(bool cond, const char* what) {
    if (cond) {
        std::printf("ok: %s\n", what);
    } else {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

using kopnet::BoundedPacketQueue;
using kopnet::Packet;
using kopnet::QueueStatus;

int main() {
    // ---- 容量与背压 ----
    {
        BoundedPacketQueue q(2);
        Packet a;
        a.data = {1, 2, 3};
        check(q.try_put(std::move(a)) == QueueStatus::Ok, "try_put 入队");
        Packet b;
        b.data = {4};
        check(q.try_put(std::move(b)) == QueueStatus::Ok, "try_put 满前最后一格");
        Packet c;
        c.data = {9};
        check(q.try_put(std::move(c)) == QueueStatus::Timeout, "满时 try_put 返回 Timeout");
        check(q.size_approx() == 2, "队列长度 == 容量");
        Packet out;
        check(q.try_get(&out) == QueueStatus::Ok && out.data.size() == 3, "try_get 取出首包");
    }

    // ---- 关键回归：Timeout 不得移空调用方的包 ----
    {
        BoundedPacketQueue q(1);
        Packet first;
        first.data = {1};
        q.try_put(std::move(first));
        Packet pkt;
        pkt.data = {7, 7};
        int fd = ::dup(STDOUT_FILENO);  // 模拟携带的 fd
        pkt.fds.push_back(fd);
        // 队列已满：必须 Timeout 且 pkt 完好
        check(q.put(std::move(pkt), 1) == QueueStatus::Timeout, "满时 put 返回 Timeout");
        Packet same;
        same.data = {7, 7};
        same.fds.push_back(fd);
        // 重试同一个包：先腾空一格，再入队必须成功且内容/fd 不变
        Packet drained;
        q.try_get(&drained);
        check(q.put(std::move(same), 1000) == QueueStatus::Ok, "重试同一包后入队成功");
        Packet out;
        check(q.get(&out, 1000) == QueueStatus::Ok, "get 成功");
        check(out.data.size() == 2 && out.data[0] == 7, "重试包内容未被移空");
        check(out.fds.size() == 1 && out.fds[0] == fd, "重试包 fd 未丢失");
        for (int f : out.fds) ::close(f);
    }

    // ---- 阻塞 get 与 close ----
    {
        BoundedPacketQueue q(4);
        std::thread t([&q] {
            Packet p;
            q.get(&p, -1);  // 无限等待
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        q.close();
        t.join();
        Packet p;
        check(q.get(&p, 100) == QueueStatus::Closed, "关闭后 get 返回 Closed");
    }

    // ---- 跨线程生产/消费 ----
    {
        BoundedPacketQueue q(8);
        const int n = 500;
        std::thread producer([&q, n] {
            for (int i = 0; i < n; ++i) {
                Packet p;
                p.data.resize(sizeof(int));
                std::memcpy(p.data.data(), &i, sizeof(int));
                while (q.put(std::move(p), 1000) != QueueStatus::Ok) {
                    // 背压下重试（包未被移空才能安全重试）
                }
            }
        });
        int got = 0;
        std::thread consumer([&q, n, &got] {
            for (int i = 0; i < n; ++i) {
                Packet p;
                if (q.get(&p, 5000) != QueueStatus::Ok) return;
                int v = 0;
                std::memcpy(&v, p.data.data(), sizeof(int));
                if (v == i) ++got;
            }
        });
        producer.join();
        consumer.join();
        check(got == n, "500 包严格有序跨线程传递");
    }

    if (failures == 0) {
        std::printf("KOPNET bounded queue tests: ALL PASS\n");
        return 0;
    }
    std::printf("KOPNET bounded queue tests: %d FAILURES\n", failures);
    return 1;
}
