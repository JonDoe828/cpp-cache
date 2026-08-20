#include "../include/arc/ArcCache.h"
#include "ICachePolicy.h"
#include "LfuCache.h"
#include "LruCache.h"

#include <array>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

// 辅助函数：打印结果
void printResults(const std::string &testName, int capacity,
                  const std::vector<std::uint64_t> &get_operations,
                  const std::vector<std::uint64_t> &hits) {
  std::cout << "=== " << testName << " 结果汇总 ===\n";
  std::cout << "缓存大小: " << capacity << "\n";

  std::vector<std::string> names;
  if (hits.size() == 3) {
    names = {"LRU", "LFU", "ARC"};
  } else if (hits.size() == 4) {
    names = {"LRU", "LFU", "ARC", "LRU-K"};
  } else if (hits.size() == 5) {
    names = {"LRU", "LFU", "ARC", "LRU-K", "LFU-Aging"};
  }

  for (std::size_t i = 0; i < hits.size(); ++i) {
    const auto ops = get_operations[i];
    const auto h = hits[i];
    const double hitRate =
        (ops == 0)
            ? 0.0
            : (100.0 * static_cast<double>(h) / static_cast<double>(ops));

    std::cout << (i < names.size() ? names[i]
                                   : "Algorithm " + std::to_string(i + 1))
              << " - 命中率: " << std::fixed << std::setprecision(2) << hitRate
              << "% " << "(" << h << "/" << ops << ")\n";
  }

  std::cout << "\n";
}

// 用 distribution 替代 gen()%N，消除符号比较 + 减少随机偏差
struct Rng {
  std::mt19937 gen;
  std::uniform_int_distribution<int> pct{0, 99}; // 0..99

  explicit Rng(std::uint32_t seed) : gen(seed) {}

  int percent() { return pct(gen); }

  int uniform_int(int lo, int hi) {
    std::uniform_int_distribution<int> dist(lo, hi);
    return dist(gen);
  }
};

} // namespace

void testHotDataAccess() {
  std::cout << "\n=== 测试场景1：热点数据访问测试 ===\n";

  constexpr int CAPACITY = 20;
  constexpr int OPERATIONS = 500000;
  constexpr int HOT_KEYS = 20;
  constexpr int COLD_KEYS = 5000;

  LruCache<int, std::string> lru(CAPACITY);
  LfuCache<int, std::string> lfu(CAPACITY);
  ArcCache<int, std::string> arc(CAPACITY);

  // LRU-K:
  // - 主缓存容量 CAPACITY
  // - 历史记录容量：HOT_KEYS + COLD_KEYS
  // - k=2
  LruKCache<int, std::string> lruk(CAPACITY, HOT_KEYS + COLD_KEYS, 2);

  // LFU-Aging（你原来传了两个参数）
  LfuCache<int, std::string> lfuAging(CAPACITY, 20000);

  std::array<ICachePolicy<int, std::string> *, 5> caches = {&lru, &lfu, &arc,
                                                            &lruk, &lfuAging};

  std::vector<std::uint64_t> hits(caches.size(), 0);
  std::vector<std::uint64_t> get_operations(caches.size(), 0);

  // 这些分布只依赖常量，放外面复用
  std::uniform_int_distribution<int> hotKey(0, HOT_KEYS - 1);
  std::uniform_int_distribution<int> coldKey(0, COLD_KEYS - 1);

  for (std::size_t i = 0; i < caches.size(); ++i) {
    Rng rng(0xCACE0001u);
    // 预热：插 HOT_KEYS
    for (int key = 0; key < HOT_KEYS; ++key) {
      caches[i]->put(key, "value" + std::to_string(key));
    }

    for (int op = 0; op < OPERATIONS; ++op) {
      const bool isPut = (rng.percent() < 30); // 30% 写
      int key;

      // 70% 热点，30% 冷点
      if (rng.percent() < 70) {
        key = hotKey(rng.gen);
      } else {
        key = HOT_KEYS + coldKey(rng.gen);
      }

      if (isPut) {
        const std::string value =
            "value" + std::to_string(key) + "_v" + std::to_string(op % 100);
        caches[i]->put(key, value);
      } else {
        std::string result;
        ++get_operations[i];
        if (caches[i]->get(key, result)) {
          ++hits[i];
        }
      }
    }
  }

  printResults("热点数据访问测试", CAPACITY, get_operations, hits);
}

void testLoopPattern() {
  std::cout << "\n=== 测试场景2：循环扫描测试 ===\n";

  constexpr int CAPACITY = 50;
  constexpr int LOOP_SIZE = 500;
  constexpr int OPERATIONS = 200000;

  LruCache<int, std::string> lru(CAPACITY);
  LfuCache<int, std::string> lfu(CAPACITY);
  ArcCache<int, std::string> arc(CAPACITY);

  // LRU-K:
  // - 历史记录容量：LOOP_SIZE * 2
  // - k=2
  LruKCache<int, std::string> lruk(CAPACITY, LOOP_SIZE * 2, 2);

  LfuCache<int, std::string> lfuAging(CAPACITY, 3000);

  std::array<ICachePolicy<int, std::string> *, 5> caches = {&lru, &lfu, &arc,
                                                            &lruk, &lfuAging};

  std::vector<std::uint64_t> hits(caches.size(), 0);
  std::vector<std::uint64_t> get_operations(caches.size(), 0);

  std::uniform_int_distribution<int> inLoop(0, LOOP_SIZE - 1);

  for (std::size_t i = 0; i < caches.size(); ++i) {
    Rng rng(0xCACE0002u);
    // 预热：加载 20%
    for (int key = 0; key < LOOP_SIZE / 5; ++key) {
      caches[i]->put(key, "loop" + std::to_string(key));
    }

    int current_pos = 0;

    for (int op = 0; op < OPERATIONS; ++op) {
      const bool isPut = (rng.percent() < 20); // 20% 写
      int key;

      const int mod100 = op % 100;
      if (mod100 < 60) { // 60% 顺序扫描
        key = current_pos;
        current_pos = (current_pos + 1) % LOOP_SIZE;
      } else if (mod100 < 90) { // 30% 随机跳跃
        key = inLoop(rng.gen);
      } else { // 10% 范围外
        key = LOOP_SIZE + inLoop(rng.gen);
      }

      if (isPut) {
        const std::string value =
            "loop" + std::to_string(key) + "_v" + std::to_string(op % 100);
        caches[i]->put(key, value);
      } else {
        std::string result;
        ++get_operations[i];
        if (caches[i]->get(key, result)) {
          ++hits[i];
        }
      }
    }
  }

  printResults("循环扫描测试", CAPACITY, get_operations, hits);
}

void testWorkloadShift() {
  std::cout << "\n=== 测试场景3：工作负载剧烈变化测试 ===\n";

  constexpr int CAPACITY = 30;
  constexpr int OPERATIONS = 80000;
  constexpr int PHASE_LENGTH = OPERATIONS / 5;

  LruCache<int, std::string> lru(CAPACITY);
  LfuCache<int, std::string> lfu(CAPACITY);
  ArcCache<int, std::string> arc(CAPACITY);
  LruKCache<int, std::string> lruk(CAPACITY, 500, 2);
  LfuCache<int, std::string> lfuAging(CAPACITY, 10000);

  std::array<ICachePolicy<int, std::string> *, 5> caches = {&lru, &lfu, &arc,
                                                            &lruk, &lfuAging};

  std::vector<std::uint64_t> hits(caches.size(), 0);
  std::vector<std::uint64_t> get_operations(caches.size(), 0);

  // 预建一些常用分布（避免到处 %）
  std::uniform_int_distribution<int> hot5(0, 4);
  std::uniform_int_distribution<int> range400(0, 399);
  std::uniform_int_distribution<int> range100(0, 99);
  std::uniform_int_distribution<int> in15(0, 14);
  std::uniform_int_distribution<int> mid45(0, 44);
  std::uniform_int_distribution<int> big350(0, 349);

  for (std::size_t i = 0; i < caches.size(); ++i) {
    Rng rng(0xCACE0003u);
    // 预热：30 个键
    for (int key = 0; key < 30; ++key) {
      caches[i]->put(key, "init" + std::to_string(key));
    }

    for (int op = 0; op < OPERATIONS; ++op) {
      const int phase = op / PHASE_LENGTH;

      // 概率就是百分比 0..99，用 int（避免 size_t/int 混比较）
      int putProbability = 20;
      switch (phase) {
      case 0:
        putProbability = 15;
        break;
      case 1:
        putProbability = 30;
        break;
      case 2:
        putProbability = 10;
        break;
      case 3:
        putProbability = 25;
        break;
      case 4:
        putProbability = 20;
        break;
      default:
        putProbability = 20;
        break;
      }

      const bool isPut = (rng.percent() < putProbability);

      int key = 0;
      if (op < PHASE_LENGTH) {
        // 阶段1：热点 5
        key = hot5(rng.gen);
      } else if (op < PHASE_LENGTH * 2) {
        // 阶段2：大范围随机 400
        key = range400(rng.gen);
      } else if (op < PHASE_LENGTH * 3) {
        // 阶段3：顺序扫描 100
        key = (op - PHASE_LENGTH * 2) % 100;
      } else if (op < PHASE_LENGTH * 4) {
        // 阶段4：局部性随机：5 个区域 * 15
        const int locality = (op / 800) % 5;
        key = locality * 15 + in15(rng.gen);
      } else {
        // 阶段5：混合访问
        const int r = rng.percent();
        if (r < 40) {
          key = hot5(rng.gen);
        } else if (r < 70) {
          key = 5 + mid45(rng.gen); // 5..49
        } else {
          key = 50 + big350(rng.gen); // 50..399
        }
      }

      if (isPut) {
        const std::string value =
            "value" + std::to_string(key) + "_p" + std::to_string(phase);
        caches[i]->put(key, value);
      } else {
        std::string result;
        ++get_operations[i];
        if (caches[i]->get(key, result)) {
          ++hits[i];
        }
      }
    }
  }

  printResults("工作负载剧烈变化测试", CAPACITY, get_operations, hits);
}

int main() {
  testHotDataAccess();
  testLoopPattern();
  testWorkloadShift();
  return 0;
}
