#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <chrono>
#include <random>
#include <vector>
#include <iomanip>
#include <algorithm>
#include <memory>

using namespace std;
using namespace chrono;

// 共享资源定义 - 全部使用shared_ptr管理
struct RealObj {
    int value;
    string data;
};

struct CalObj {
    int count;
    double sum;
};

struct SharedObj {
    map<string, shared_ptr<RealObj>> details;
    map<string, shared_ptr<CalObj>> totals;
};

// 全局配置
const int NUM_READERS = 10;
const int NUM_WRITERS = 8;
const int TIME_LIMIT_SECONDS = 300;
const int READ_DELAY_MS = 10;

// 性能统计
struct Statistics {
    atomic<long long> read_count{0};
    atomic<long long> write_count{0};
    atomic<long long> add_count{0};
    atomic<long long> modify_count{0};
    atomic<long long> delete_count{0};
    atomic<bool> should_stop{false};
    
    void reset() {
        read_count = 0;
        write_count = 0;
        add_count = 0;
        modify_count = 0;
        delete_count = 0;
        should_stop = false;
    }
};

// ==================== 方案1: 粗粒度读写锁 ====================
class CoarseGrainedRWLock {
private:
    map<string, shared_ptr<SharedObj>> objects;
    shared_mutex mutex_;
    
public:
    void read_operation() {
        shared_lock<shared_mutex> lock(mutex_);
        for (const auto& [key, obj_ptr] : objects) {
            if (obj_ptr) {
                volatile int sum = 0;
                for (const auto& [k, real_ptr] : obj_ptr->details) {
                    if (real_ptr) {
                        sum += real_ptr->value;
                    }
                }
            }
        }
    }
    
    void write_operation(int op_type, int id) {
        unique_lock<shared_mutex> lock(mutex_);
        string key = "obj_" + to_string(id % 100);
        string detail_key = "detail_" + to_string(id);
        
        // 确保SharedObj存在
        if (objects.find(key) == objects.end()) {
            objects[key] = make_shared<SharedObj>();
        }
        
        if (op_type == 0) { // 新增
            objects[key]->details[detail_key] = make_shared<RealObj>(RealObj{id, "data_" + to_string(id)});
            objects[key]->totals[detail_key] = make_shared<CalObj>(CalObj{1, (double)id});
        } else if (op_type == 1) { // 修改
            auto detail_it = objects[key]->details.find(detail_key);
            if (detail_it != objects[key]->details.end() && detail_it->second) {
                detail_it->second->value += 1;
                
                auto total_it = objects[key]->totals.find(detail_key);
                if (total_it != objects[key]->totals.end() && total_it->second) {
                    total_it->second->count += 1;
                    total_it->second->sum += 1.0;
                }
            }
        } else { // 删除
            objects[key]->details.erase(detail_key);
            objects[key]->totals.erase(detail_key);
        }
    }
    
    void init_data() {
        for (int i = 0; i < 100; ++i) {
            string key = "obj_" + to_string(i);
            objects[key] = make_shared<SharedObj>();
            
            for (int j = 0; j < 10; ++j) {
                string detail_key = "detail_" + to_string(i * 10 + j);
                objects[key]->details[detail_key] = make_shared<RealObj>(
                    RealObj{i * 10 + j, "init_data"}
                );
                objects[key]->totals[detail_key] = make_shared<CalObj>(
                    CalObj{1, (double)(i * 10 + j)}
                );
            }
        }
    }
};

// ==================== 方案2: 细粒度锁 ====================
class FineGrainedLock {
private:
    struct LockedSharedObj {
        shared_ptr<SharedObj> obj;
        shared_mutex mutex;
        
        LockedSharedObj() : obj(make_shared<SharedObj>()) {}
    };
    map<string, shared_ptr<LockedSharedObj>> objects;
    shared_mutex map_mutex_;
    
public:
    void read_operation() {
        shared_lock<shared_mutex> map_lock(map_mutex_);
        for (auto& [key, locked_obj_ptr] : objects) {
            if (locked_obj_ptr) {
                shared_lock<shared_mutex> lock(locked_obj_ptr->mutex);
                if (locked_obj_ptr->obj) {
                    volatile int sum = 0;
                    for (const auto& [k, real_ptr] : locked_obj_ptr->obj->details) {
                        if (real_ptr) {
                            sum += real_ptr->value;
                        }
                    }
                }
            }
        }
    }
    
    void write_operation(int op_type, int id) {
        string key = "obj_" + to_string(id % 100);
        string detail_key = "detail_" + to_string(id);
        
        {
            shared_lock<shared_mutex> map_lock(map_mutex_);
            auto it = objects.find(key);
            if (it != objects.end() && it->second) {
                unique_lock<shared_mutex> lock(it->second->mutex);
                
                if (op_type == 0) {
                    it->second->obj->details[detail_key] = make_shared<RealObj>(
                        RealObj{id, "data_" + to_string(id)}
                    );
                    it->second->obj->totals[detail_key] = make_shared<CalObj>(
                        CalObj{1, (double)id}
                    );
                } else if (op_type == 1) {
                    auto detail_it = it->second->obj->details.find(detail_key);
                    if (detail_it != it->second->obj->details.end() && detail_it->second) {
                        detail_it->second->value += 1;
                        
                        auto total_it = it->second->obj->totals.find(detail_key);
                        if (total_it != it->second->obj->totals.end() && total_it->second) {
                            total_it->second->count += 1;
                            total_it->second->sum += 1.0;
                        }
                    }
                } else {
                    it->second->obj->details.erase(detail_key);
                    it->second->obj->totals.erase(detail_key);
                }
                return;
            }
        }
        
        unique_lock<shared_mutex> map_lock(map_mutex_);
        if (objects.find(key) == objects.end()) {
            objects[key] = make_shared<LockedSharedObj>();
        }
        
        if (op_type == 0) {
            objects[key]->obj->details[detail_key] = make_shared<RealObj>(
                RealObj{id, "data_" + to_string(id)}
            );
            objects[key]->obj->totals[detail_key] = make_shared<CalObj>(
                CalObj{1, (double)id}
            );
        }
    }
    
    void init_data() {
        for (int i = 0; i < 100; ++i) {
            string key = "obj_" + to_string(i);
            objects[key] = make_shared<LockedSharedObj>();
            
            for (int j = 0; j < 10; ++j) {
                string detail_key = "detail_" + to_string(i * 10 + j);
                objects[key]->obj->details[detail_key] = make_shared<RealObj>(
                    RealObj{i * 10 + j, "init_data"}
                );
                objects[key]->obj->totals[detail_key] = make_shared<CalObj>(
                    CalObj{1, (double)(i * 10 + j)}
                );
            }
        }
    }
};

// ==================== 方案3: RCU风格(Copy-on-Write) ====================
class CopyOnWrite {
private:
    shared_ptr<map<string, shared_ptr<SharedObj>>> objects_ptr;
    mutex write_mutex_;
    
public:
    CopyOnWrite() {
        objects_ptr = make_shared<map<string, shared_ptr<SharedObj>>>();
    }
    
    void read_operation() {
        shared_ptr<map<string, shared_ptr<SharedObj>>> local_ptr = atomic_load(&objects_ptr);
        for (const auto& [key, obj_ptr] : *local_ptr) {
            if (obj_ptr) {
                volatile int sum = 0;
                for (const auto& [k, real_ptr] : obj_ptr->details) {
                    if (real_ptr) {
                        sum += real_ptr->value;
                    }
                }
            }
        }
    }
    
    void write_operation(int op_type, int id) {
        lock_guard<mutex> lock(write_mutex_);
        
        // 深拷贝整个数据结构
        auto new_objects = make_shared<map<string, shared_ptr<SharedObj>>>();
        auto current_objects = atomic_load(&objects_ptr);
        
        for (const auto& [k, obj_ptr] : *current_objects) {
            if (obj_ptr) {
                auto new_obj = make_shared<SharedObj>();
                
                // 拷贝details
                for (const auto& [dk, real_ptr] : obj_ptr->details) {
                    if (real_ptr) {
                        new_obj->details[dk] = make_shared<RealObj>(*real_ptr);
                    }
                }
                
                // 拷贝totals
                for (const auto& [tk, cal_ptr] : obj_ptr->totals) {
                    if (cal_ptr) {
                        new_obj->totals[tk] = make_shared<CalObj>(*cal_ptr);
                    }
                }
                
                (*new_objects)[k] = new_obj;
            }
        }
        
        string key = "obj_" + to_string(id % 100);
        string detail_key = "detail_" + to_string(id);
        
        // 确保对象存在
        if (new_objects->find(key) == new_objects->end()) {
            (*new_objects)[key] = make_shared<SharedObj>();
        }
        
        if (op_type == 0) {
            (*new_objects)[key]->details[detail_key] = make_shared<RealObj>(
                RealObj{id, "data_" + to_string(id)}
            );
            (*new_objects)[key]->totals[detail_key] = make_shared<CalObj>(
                CalObj{1, (double)id}
            );
        } else if (op_type == 1) {
            auto detail_it = (*new_objects)[key]->details.find(detail_key);
            if (detail_it != (*new_objects)[key]->details.end() && detail_it->second) {
                detail_it->second->value += 1;
                
                auto total_it = (*new_objects)[key]->totals.find(detail_key);
                if (total_it != (*new_objects)[key]->totals.end() && total_it->second) {
                    total_it->second->count += 1;
                    total_it->second->sum += 1.0;
                }
            }
        } else {
            (*new_objects)[key]->details.erase(detail_key);
            (*new_objects)[key]->totals.erase(detail_key);
        }
        
        atomic_store(&objects_ptr, new_objects);
    }
    
    void init_data() {
        auto new_objects = make_shared<map<string, shared_ptr<SharedObj>>>();
        
        for (int i = 0; i < 100; ++i) {
            string key = "obj_" + to_string(i);
            (*new_objects)[key] = make_shared<SharedObj>();
            
            for (int j = 0; j < 10; ++j) {
                string detail_key = "detail_" + to_string(i * 10 + j);
                (*new_objects)[key]->details[detail_key] = make_shared<RealObj>(
                    RealObj{i * 10 + j, "init_data"}
                );
                (*new_objects)[key]->totals[detail_key] = make_shared<CalObj>(
                    CalObj{1, (double)(i * 10 + j)}
                );
            }
        }
        
        atomic_store(&objects_ptr, new_objects);
    }
};

// ==================== 测试框架 ====================
template<typename SyncMechanism>
class Benchmark {
private:
    SyncMechanism& sync_obj;
    Statistics stats;
    
    void reader_thread() {
        while (!stats.should_stop.load()) {
            sync_obj.read_operation();
            stats.read_count++;
            this_thread::sleep_for(milliseconds(READ_DELAY_MS));
        }
    }
    
    void writer_thread(int thread_id) {
        random_device rd;
        mt19937 gen(rd() + thread_id);
        uniform_int_distribution<> op_dis(0, 2);
        uniform_int_distribution<> id_dis(0, 999);
        
        while (!stats.should_stop.load()) {
            int op_type = op_dis(gen);
            int id = id_dis(gen);
            
            sync_obj.write_operation(op_type, id);
            stats.write_count++;
            
            if (op_type == 0) stats.add_count++;
            else if (op_type == 1) stats.modify_count++;
            else stats.delete_count++;
            
            this_thread::sleep_for(microseconds(100));
        }
    }
    
    void timer_thread() {
        this_thread::sleep_for(seconds(TIME_LIMIT_SECONDS));
        stats.should_stop.store(true);
    }
    
public:
    Benchmark(SyncMechanism& obj) : sync_obj(obj) {}
    
    void run() {
        stats.reset();
        sync_obj.init_data();
        
        vector<thread> readers;
        vector<thread> writers;
        
        auto start_time = high_resolution_clock::now();
        
        thread timer(&Benchmark::timer_thread, this);
        
        for (int i = 0; i < NUM_READERS; ++i) {
            readers.emplace_back(&Benchmark::reader_thread, this);
        }
        
        for (int i = 0; i < NUM_WRITERS; ++i) {
            writers.emplace_back(&Benchmark::writer_thread, this, i);
        }
        
        timer.join();
        
        for (auto& t : readers) t.join();
        for (auto& t : writers) t.join();
        
        auto end_time = high_resolution_clock::now();
        auto duration = duration_cast<milliseconds>(end_time - start_time).count();
        
        cout << "执行时间: " << duration << " ms" << endl;
        cout << "读操作总数: " << stats.read_count.load() << endl;
        cout << "写操作总数: " << stats.write_count.load() << endl;
        cout << "  - 新增: " << stats.add_count.load() << endl;
        cout << "  - 修改: " << stats.modify_count.load() << endl;
        cout << "  - 删除: " << stats.delete_count.load() << endl;
        cout << "读吞吐量: " << (stats.read_count.load() * 1000.0 / duration) << " ops/s" << endl;
        cout << "写吞吐量: " << (stats.write_count.load() * 1000.0 / duration) << " ops/s" << endl;
        cout << "总吞吐量: " << ((stats.read_count.load() + stats.write_count.load()) * 1000.0 / duration) << " ops/s" << endl;
    }
};

// ==================== 主程序 ====================
int main() {
    cout << "========================================" << endl;
    cout << "线程安全同步机制性能测试 (使用shared_ptr)" << endl;
    cout << "========================================" << endl;
    cout << "配置参数:" << endl;
    cout << "  读者线程数: " << NUM_READERS << endl;
    cout << "  写者线程数: " << NUM_WRITERS << endl;
    cout << "  测试时长: " << TIME_LIMIT_SECONDS << " 秒" << endl;
    cout << "  读操作间隔: " << READ_DELAY_MS << " ms" << endl;
    cout << "========================================" << endl << endl;
    
    // 方案1: 粗粒度读写锁
    cout << "【方案1: 粗粒度读写锁】" << endl;
    {
        CoarseGrainedRWLock sync_obj;
        Benchmark<CoarseGrainedRWLock> benchmark(sync_obj);
        benchmark.run();
    }
    cout << endl;
    
    // 方案2: 细粒度锁
    cout << "【方案2: 细粒度锁】" << endl;
    {
        FineGrainedLock sync_obj;
        Benchmark<FineGrainedLock> benchmark(sync_obj);
        benchmark.run();
    }
    cout << endl;
    
    // 方案3: Copy-on-Write
    cout << "【方案3: Copy-on-Write (RCU风格)】" << endl;
    {
        CopyOnWrite sync_obj;
        Benchmark<CopyOnWrite> benchmark(sync_obj);
        benchmark.run();
    }
    cout << endl;
    
    cout << "========================================" << endl;
    cout << "测试完成" << endl;
    cout << "========================================" << endl;
    
    return 0;
}