#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <thread>
#include <mutex>
#include <atomic>
#include <exception>
#include <memory_resource>
#include <string_view>
#include <filesystem>
#include <cstdint>
#include <stdexcept>
#include <future>
#include <memory>

constexpr std::size_t BLOCK_SIZE = 256 * 1024;

std::string toLower(const std::string& str) {
    std::string result;
    result.reserve(str.size());
    for (unsigned char c : str) {
        result += static_cast<char>(std::tolower(c));
    }
    return result;
}

std::string_view cleanWord(std::string_view word) {
    // Удаляем знаки препинания в начале слова
    while (!word.empty() && std::ispunct(static_cast<unsigned char>(word.front()))) {
        word.remove_prefix(1);
    }

    // Удаляем знаки препинания в конце слова
    while (!word.empty() && std::ispunct(static_cast<unsigned char>(word.back()))) {
        word.remove_suffix(1);
    }

    return word;
}

struct WordCount {
    std::string word;
    std::uint64_t count;
};

bool compareWordCount(const WordCount& a, const WordCount& b) {
    return a.count != b.count ? a.count > b.count : a.word < b.word;
}

// Читаем блок байтов и дочитываем последнее слово, чтобы не разорвать его.
bool readBlock(std::ifstream& file, std::string& block) {
    block.resize(BLOCK_SIZE);
    file.read(block.data(), static_cast<std::streamsize>(block.size()));
    block.resize(static_cast<std::size_t>(file.gcount()));
    char c;
    while (!block.empty() &&
           !std::isspace(static_cast<unsigned char>(block.back())) && file.get(c)) {
        block += c;
    }
    return !block.empty();
}

using LocalMap = std::pmr::map<std::pmr::string, std::uint64_t, std::less<>>;

// Отдельный объект на каждый поток: словарь и его пул памяти не разделяются.
struct WorkerData {
    std::pmr::unsynchronized_pool_resource pool;
    LocalMap counts;

    WorkerData() : counts(&pool) {}
};

void countWordsInBlock(const std::string& block, LocalMap& counts) {
    // Одна нормализация блока вместо выделения новой строки для каждого слова
    const std::string lower = toLower(block);
    std::size_t pos = 0;
    while (pos < lower.size()) {
        while (pos < lower.size() &&
               std::isspace(static_cast<unsigned char>(lower[pos]))) {
            ++pos;
        }
        const std::size_t begin = pos;
        while (pos < lower.size() &&
               !std::isspace(static_cast<unsigned char>(lower[pos]))) {
            ++pos;
        }
        const auto word = cleanWord(std::string_view(lower).substr(begin, pos - begin));
        if (!word.empty()) {
            const auto found = counts.find(word);
            if (found != counts.end()) {
                ++found->second;
            } else {
                counts.emplace(std::pmr::string(word, counts.get_allocator().resource()), 1);
            }
        }
    }
}

void mergeMaps(LocalMap& destination, LocalMap& source) {
    for (const auto& [word, count] : source) {
        const std::string_view key(word.data(), word.size());
        const auto found = destination.find(key);
        if (found != destination.end()) {
            found->second += count;
        } else {
            destination.emplace(
                std::pmr::string(word.data(), word.size(),
                                 destination.get_allocator().resource()),
                count);
        }
    }
    source.clear();
}

// На каждом шаге независимые пары словарей объединяются одновременно.
void parallelMerge(std::vector<std::unique_ptr<WorkerData>>& workerData) {
    for (std::size_t step = 1; step < workerData.size(); step *= 2) {
        std::vector<std::future<void>> tasks;
        for (std::size_t left = 0; left + step < workerData.size();
             left += 2 * step) {
            tasks.emplace_back(std::async(std::launch::async, [&, left, step]() {
                mergeMaps(workerData[left]->counts,
                          workerData[left + step]->counts);
            }));
        }
        for (auto& task : tasks) {
            task.get();
        }
        if (step > workerData.size() / 2) {
            break;
        }
    }
}

void parallelSort(std::vector<WordCount>& values, std::size_t threadCount) {
    if (values.size() < 2 || threadCount < 2) {
        std::sort(values.begin(), values.end(), compareWordCount);
        return;
    }

    const std::size_t partCount = std::min(threadCount, values.size());
    std::vector<std::size_t> borders(partCount + 1);
    for (std::size_t i = 0; i <= partCount; ++i) {
        borders[i] = values.size() * i / partCount;
    }

    std::vector<std::future<void>> tasks;
    for (std::size_t part = 0; part < partCount; ++part) {
        tasks.emplace_back(std::async(std::launch::async, [&, part]() {
            std::sort(values.begin() + borders[part],
                      values.begin() + borders[part + 1], compareWordCount);
        }));
    }
    for (auto& task : tasks) {
        task.get();
    }

    // Объединяем уже отсортированные части попарно и параллельно.
    for (std::size_t step = 1; step < partCount; step *= 2) {
        tasks.clear();
        for (std::size_t left = 0; left + step < partCount;
             left += 2 * step) {
            const std::size_t middle = borders[left + step];
            const std::size_t right = borders[std::min(left + 2 * step, partCount)];
            tasks.emplace_back(std::async(std::launch::async,
                                          [&, left, middle, right]() {
                std::inplace_merge(values.begin() + borders[left],
                                   values.begin() + middle,
                                   values.begin() + right, compareWordCount);
            }));
        }
        for (auto& task : tasks) {
            task.get();
        }
        if (step > partCount / 2) {
            break;
        }
    }
}

int positiveNumber(const char* value) {
    const std::string text(value);
    std::size_t used = 0;
    const int result = std::stoi(text, &used);
    if (result <= 0 || used != text.size()) {
        throw std::invalid_argument("Ожидалось положительное целое число");
    }
    return result;
}

int main(int argc, char* argv[]) try {
    if (argc < 2 || argc > 4) {
        std::cerr << "Использование: " << argv[0]
                  << " <файл> [количество слов=100] [потоки]" << std::endl;
        return 1;
    }
    std::string filename = argv[1];
    int topN = 100;
    if (argc > 2) {
        topN = positiveNumber(argv[2]);
    }

    // Открываем файл
    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Не удалось открыть файл: " << filename << std::endl;
        return 1;
    }

    file.exceptions(std::ios::badbit);
    const auto fileSize = std::filesystem::file_size(filename);
    std::size_t threadCount = argc > 3
        ? static_cast<std::size_t>(positiveNumber(argv[3]))
        : std::max(1u, std::thread::hardware_concurrency());
    if (fileSize == 0) {
        threadCount = 0;
    } else {
        // Округляем число блоков вверх: непустой файл всегда получит хотя бы поток.
        const std::uintmax_t blockCount = 1 + (fileSize - 1) / BLOCK_SIZE;
        threadCount = static_cast<std::size_t>(
            std::min<std::uintmax_t>(threadCount, blockCount));
    }

    std::mutex fileMutex, errorMutex;
    std::atomic<bool> failed{false};
    std::exception_ptr error;
    std::vector<std::unique_ptr<WorkerData>> workerData;
    workerData.reserve(threadCount);
    for (std::size_t t = 0; t < threadCount; ++t) {
        workerData.push_back(std::make_unique<WorkerData>());
    }

    {
        std::vector<std::jthread> workers;
        workers.reserve(threadCount);
        for (std::size_t t = 0; t < threadCount; ++t) {
            workers.emplace_back([&, t]() {
                try {
                    std::string block;
                    while (!failed.load()) {
                        {
                            std::lock_guard<std::mutex> lock(fileMutex);
                            if (!readBlock(file, block)) {
                                break;
                            }
                        }
                        // Подсчёт выполняется без блокировок в словаре этого потока.
                        countWordsInBlock(block, workerData[t]->counts);
                    }
                } catch (...) {
                    failed.store(true);
                    std::lock_guard<std::mutex> lock(errorMutex);
                    if (!error) {
                        error = std::current_exception();
                    }
                }
            });
        }
        // std::jthread автоматически присоединяются при выходе из этой области.
    }
    if (error) {
        std::rethrow_exception(error);
    }
    std::cerr << "Рабочих потоков: " << threadCount << '\n';
    file.close();

    parallelMerge(workerData);

    std::vector<WordCount> wordCounts;
    if (!workerData.empty()) {
        const LocalMap& result = workerData.front()->counts;
        wordCounts.reserve(result.size());
        for (const auto& [word, count] : result) {
            wordCounts.push_back({std::string(word), count});
        }
    }

    parallelSort(wordCounts, threadCount);

    // Выводим результаты
    std::cout << "Топ-" << topN << " самых частых слов в файле '" << filename << "':" << std::endl;
    std::cout << std::setw(20) << std::left << "Слово" << "Количество" << std::endl;
    std::cout << std::string(30, '-') << std::endl;

    const std::size_t limit = std::min(static_cast<std::size_t>(topN), wordCounts.size());
    for (std::size_t i = 0; i < limit; i++) {
        std::cout << std::setw(20) << std::left << wordCounts[i].word
                  << wordCounts[i].count << std::endl;
    }

    return 0;
} catch (const std::exception& e) {
    std::cerr << "Ошибка: " << e.what() << '\n';
    return 1;
} catch (...) {
    std::cerr << "Неизвестная ошибка\n";
    return 1;
}
