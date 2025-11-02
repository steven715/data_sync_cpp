#include <iostream>
#include <thread>
#include <vector>
#include <random>
#include <atomic>
#include <chrono>
#include <iomanip>
#include "resource.h"

// 全局統計變量
std::atomic<uint64_t> g_read_count{0};
std::atomic<uint64_t> g_write_count{0};
std::atomic<uint64_t> g_delete_count{0};
std::atomic<bool> g_running{true};

// 隨機數據生成器
class RandomObjectGenerator
{
private:
    std::vector<std::string> symbols = {"AAPL", "GOOGL", "MSFT", "AMZN", "TSLA", "META", "NVDA", "AMD"};
    std::vector<std::string> accounts = {"ACC001", "ACC002", "ACC003", "ACC004", "ACC005", "ACC006", "ACC007", "ACC008"};

    std::random_device rd;
    std::mt19937 gen;
    std::uniform_int_distribution<> symbol_dist;
    std::uniform_int_distribution<> account_dist;
    std::uniform_int_distribution<> direct_dist;
    std::uniform_real_distribution<> price_dist;
    std::uniform_real_distribution<> number_dist;

    static std::atomic<uint64_t> id_counter;

public:
    RandomObjectGenerator()
        : gen(rd()),
          symbol_dist(0, static_cast<int>(symbols.size()) - 1),
          account_dist(0, static_cast<int>(accounts.size()) - 1),
          direct_dist(0, 1),
          price_dist(50.0, 500.0),
          number_dist(1.0, 100.0)
    {
    }

    RealObj GenerateObject()
    {
        RealObj obj;
        obj.id = std::to_string(id_counter.fetch_add(1));
        obj.symbol = symbols[symbol_dist(gen)];
        obj.accountId = accounts[account_dist(gen)];
        obj.direct = direct_dist(gen);
        obj.price = price_dist(gen);
        obj.number = number_dist(gen);
        return obj;
    }

    std::string GetRandomKey()
    {
        return symbols[symbol_dist(gen)] + accounts[account_dist(gen)];
    }
};

std::atomic<uint64_t> RandomObjectGenerator::id_counter{0};

// 讀者線程
void ReaderThread(resource &res, int thread_id)
{
    RandomObjectGenerator generator;
    uint64_t local_read_count = 0;

    while (g_running)
    {
        std::this_thread::sleep_for(std::chrono::microseconds(100));

        std::string key = generator.GetRandomKey();
        auto sharedObj = res.GetObject(key);

        if (sharedObj)
        {
            // 方法1: 獲取數量
            size_t detail_count = sharedObj->GetDetailCount();
            size_t total_count = sharedObj->GetTotalCount();

            // 方法2: 遍歷 details map
            if (local_read_count % 5000 == 0)
            {
                std::cout << "[Reader-" << thread_id << "] Key: " << key
                          << " | Details: " << detail_count
                          << " | Totals: " << total_count << std::endl;

                // 遍歷並輸出 details
                sharedObj->ForEachDetail([thread_id](const std::string& id, const std::shared_ptr<RealObj>& obj) {
                    std::cout << "  [Detail] ID: " << id
                              << ", Symbol: " << obj->symbol
                              << ", Price: " << obj->price
                              << ", Number: " << obj->number << std::endl;
                });

                // 遍歷並輸出 totals
                sharedObj->ForEachTotal([thread_id](const std::string& key, const std::shared_ptr<CalObj>& obj) {
                    std::cout << "  [Total] Key: " << key
                              << ", Symbol: " << obj->symbol
                              << ", Avg Price: " << obj->avg_price
                              << ", Total Number: " << obj->total_number << std::endl;
                });
            }

            // 方法3: 獲取副本（適合需要長時間處理的情況）
            if (local_read_count % 10000 == 0)
            {
                auto details_copy = sharedObj->GetDetailsCopy();
                auto totals_copy = sharedObj->GetTotalsCopy();
                // 現在可以在沒有鎖的情況下處理副本
                // ... 處理邏輯 ...
            }
        }

        local_read_count++;
        g_read_count++;
    }

    std::cout << "[Reader-" << thread_id << "] Total reads: " << local_read_count << std::endl;
}

// 寫入線程
void WriterThread(resource &res, int thread_id, int iterations)
{
    RandomObjectGenerator generator;
    uint64_t local_write_count = 0;

    while (g_running && (iterations == -1 || local_write_count < iterations))
    {
        std::this_thread::sleep_for(std::chrono::microseconds(500));

        RealObj obj = generator.GenerateObject();
        res.AddObject(obj);

        if (local_write_count % 1000 == 0 && local_write_count > 0)
        {
            std::cout << "[Writer-" << thread_id << "] Writes: " << local_write_count << std::endl;
        }

        local_write_count++;
        g_write_count++;
    }

    std::cout << "[Writer-" << thread_id << "] Total writes: " << local_write_count << std::endl;
}

// 刪除線程
void DeleterThread(resource &res, int thread_id)
{
    RandomObjectGenerator generator;
    uint64_t local_delete_count = 0;

    // 延遲啟動，確保有數據可刪除
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    while (g_running)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        RealObj obj = generator.GenerateObject();
        res.RemoveObject(obj);

        if (local_delete_count % 1000 == 0 && local_delete_count > 0)
        {
            std::cout << "[Deleter-" << thread_id << "] Deletes: " << local_delete_count << std::endl;
        }

        local_delete_count++;
        g_delete_count++;
    }

    std::cout << "[Deleter-" << thread_id << "] Total deletes: " << local_delete_count << std::endl;
}

// 性能監控線程
void MonitorThread()
{
    auto start_time = std::chrono::steady_clock::now();

    while (g_running)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();

        if (elapsed > 0)
        {
            std::cout << "\n========== Performance Stats (Elapsed: " << elapsed << "s) ==========\n"
                      << "Reads:   " << std::setw(10) << g_read_count.load()
                      << " (" << g_read_count.load() / elapsed << " ops/s)\n"
                      << "Writes:  " << std::setw(10) << g_write_count.load()
                      << " (" << g_write_count.load() / elapsed << " ops/s)\n"
                      << "Deletes: " << std::setw(10) << g_delete_count.load()
                      << " (" << g_delete_count.load() / elapsed << " ops/s)\n"
                      << "Total:   " << std::setw(10) << (g_read_count + g_write_count + g_delete_count)
                      << " (" << (g_read_count + g_write_count + g_delete_count) / elapsed << " ops/s)\n"
                      << "====================================================\n" << std::endl;
        }
    }
}

int main(int argc, char const *argv[])
{
    std::cout << "=== Multi-threaded Resource Manager Performance Test ===\n" << std::endl;

    resource res;

    // 預先填充一些數據
    std::cout << "Pre-populating data..." << std::endl;
    RandomObjectGenerator generator;
    for (int i = 0; i < 100; i++)
    {
        res.AddObject(generator.GenerateObject());
    }
    std::cout << "Pre-population complete.\n" << std::endl;

    std::vector<std::thread> threads;

    // 啟動性能監控線程
    threads.emplace_back(MonitorThread);

    // 啟動 8 個讀者線程
    std::cout << "Starting 8 reader threads..." << std::endl;
    for (int i = 0; i < 8; i++)
    {
        threads.emplace_back(ReaderThread, std::ref(res), i + 1);
    }

    // 啟動 8 個寫入線程（無限迭代，由運行標誌控制）
    std::cout << "Starting 8 writer threads..." << std::endl;
    for (int i = 0; i < 8; i++)
    {
        threads.emplace_back(WriterThread, std::ref(res), i + 1, -1);
    }

    // 啟動 8 個刪除線程
    std::cout << "Starting 8 deleter threads..." << std::endl;
    for (int i = 0; i < 8; i++)
    {
        threads.emplace_back(DeleterThread, std::ref(res), i + 1);
    }

    std::cout << "\nAll threads started. Running for 10 seconds...\n" << std::endl;

    // 運行 10 秒
    std::this_thread::sleep_for(std::chrono::seconds(10));

    std::cout << "\n\nStopping all threads..." << std::endl;
    g_running = false;

    // 等待所有線程結束
    for (auto &t : threads)
    {
        if (t.joinable())
        {
            t.join();
        }
    }

    // 最終統計
    std::cout << "\n==================== Final Statistics ====================\n"
              << "Total Reads:   " << g_read_count.load() << "\n"
              << "Total Writes:  " << g_write_count.load() << "\n"
              << "Total Deletes: " << g_delete_count.load() << "\n"
              << "Total Ops:     " << (g_read_count + g_write_count + g_delete_count) << "\n"
              << "==========================================================\n" << std::endl;

    return 0;
}
