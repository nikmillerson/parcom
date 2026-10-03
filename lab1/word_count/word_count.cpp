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
    
    // Карта для подсчета слов
    std::map<std::string, std::uint64_t> wordCountMap;
    file.exceptions(std::ios::badbit);
    const auto fileSize = std::filesystem::file_size(filename);
    std::size_t threadCount = argc > 3
        ? static_cast<std::size_t>(positiveNumber(argv[3]))
        : std::max(1u, std::thread::hardware_concurrency());
    // На маленький файл достаточно одного потока; ориентируемся на байты, не строки.
    threadCount = static_cast<std::size_t>(std::min<std::uintmax_t>(
        threadCount, std::max<std::uintmax_t>(1, fileSize / BLOCK_SIZE)));
    if (fileSize == 0) {
        threadCount = 0;
    }

    std::mutex fileMutex, resultMutex;
    std::atomic<bool> failed{false};
    std::exception_ptr error;
    std::vector<std::jthread> workers;
    workers.reserve(threadCount);
    for (std::size_t t = 0; t < threadCount; ++t) {
        workers.emplace_back([&]() {
            try {
                // Словарь и пул принадлежат только этому потоку
                // Узлы и символы новых ключей выделяются из локального пула
                std::pmr::unsynchronized_pool_resource pool;
                LocalMap localMap{&pool};
                std::string block;
                while (!failed.load()) {
                    {
                        std::lock_guard<std::mutex> lock(fileMutex);
                        if (!readBlock(file, block)) {
                            break;
                        }
                    }
                    // Подсчёт выполняется без блокировок, т.к. другой поток уже читает свой блок.
                    countWordsInBlock(block, localMap);
                }
                if (!failed.load()) {
                    std::lock_guard<std::mutex> lock(resultMutex);
                    for (const auto& [word, count] : localMap) {
                        wordCountMap[std::string(word)] += count;
                    }
                }
            } catch (...) {
                failed.store(true);
                std::lock_guard<std::mutex> lock(resultMutex);
                if (!error) {
                    error = std::current_exception();
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    if (error) {
        std::rethrow_exception(error);
    }
    std::cerr << "Рабочих потоков: " << threadCount << '\n';
    file.close();
    
    std::vector<WordCount> wordCounts;
    for (const auto& pair : wordCountMap) {
        wordCounts.push_back({pair.first, pair.second});
    }
    
    std::sort(wordCounts.begin(), wordCounts.end(), compareWordCount);
    
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
