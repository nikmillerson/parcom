#include "floyd_warshall.h"
#include <iostream>
#include <vector>
#include <climits>
#include <algorithm>
#include <thread>
#include <barrier>
#include <exception>
#include <atomic>

using namespace std;

FloydWarshall::FloydWarshall(const vector<vector<int>>& graph) {
    n = graph.size();
    dist = graph;
    next.resize(n, vector<int>(n, -1));
    hasNegativeCycle = false;


    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            if (graph[i][j] != INT_MAX && i != j) {
                next[i][j] = j;
            }
        }
    }
}

void FloydWarshall::run() {
    if (n == 0) {
        return;
    }

    unsigned int numThreads = thread::hardware_concurrency();
    if (numThreads == 0) {
        numThreads = 1;
    }
    numThreads = min(numThreads, static_cast<unsigned int>(n));

    vector<int> rowK(n);
    vector<int> columnK(n);
    vector<int> nextToK(n);

    barrier syncPoint(numThreads);
    vector<thread> workers;
    vector<exception_ptr> exceptions(numThreads);
    atomic<bool> failed(false);
    workers.reserve(numThreads);

    for (unsigned int threadId = 0; threadId < numThreads; threadId++) {
        int begin = n * static_cast<int>(threadId) / static_cast<int>(numThreads);
        int end = n * static_cast<int>(threadId + 1) / static_cast<int>(numThreads);

        workers.emplace_back([&, threadId, begin, end]() {
            for (int k = 0; k < n; k++) {
                try {
                    if (!failed.load()) {
                        // Каждый поток подготавливает свою часть снимков.
                        // Блоки не пересекаются и отличаются не более чем на
                        // один элемент.
                        for (int index = begin; index < end; index++) {
                            columnK[index] = dist[index][k];
                            rowK[index] = dist[k][index];
                            nextToK[index] = next[index][k];
                        }
                    }
                } catch (...) {
                    exceptions[threadId] = current_exception();
                    failed.store(true);
                }

                // Все снимки должны быть готовы до обновления матрицы.
                syncPoint.arrive_and_wait();

                try {
                    if (!failed.load()) {
                        for (int i = begin; i < end; i++) {
                            if (columnK[i] == INT_MAX) {
                                continue;
                            }

                            for (int j = 0; j < n; j++) {
                                if (rowK[j] != INT_MAX &&
                                    dist[i][j] > columnK[i] + rowK[j]) {
                                    dist[i][j] = columnK[i] + rowK[j];
                                    next[i][j] = nextToK[i];
                                }
                            }
                        }
                    }
                } catch (...) {
                    exceptions[threadId] = current_exception();
                    failed.store(true);
                }

                // Следующая итерация k начнётся только после завершения всех
                // записей текущей итерации.
                syncPoint.arrive_and_wait();
            }
        });
    }

    for (thread& worker : workers) {
        worker.join();
    }

    for (const exception_ptr& exception : exceptions) {
        if (exception) {
            rethrow_exception(exception);
        }
    }

    for (int i = 0; i < n; i++) {
        if (dist[i][i] < 0) {
            hasNegativeCycle = true;
            break;
        }
    }
}

int FloydWarshall::getDistance(int from, int to) const {
    if (from < 0 || from >= n || to < 0 || to >= n) {
        return INT_MAX;
    }
    return dist[from][to];
}

bool FloydWarshall::hasNegativeCycles() const {
    return hasNegativeCycle;
}

vector<int> FloydWarshall::getPath(int from, int to) const {
    vector<int> path;

    if (from < 0 || from >= n || to < 0 || to >= n || dist[from][to] == INT_MAX) {
        return path;
    }

    int current = from;
    while (current != to) {
        path.push_back(current);
        current = next[current][to];
        if (current == -1) {
            return vector<int>();
        }
    }
    path.push_back(to);

    return path;
}

const vector<vector<int>>& FloydWarshall::getDistanceMatrix() const {
    return dist;
}

void FloydWarshall::printMatrix() const {
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            if (dist[i][j] == INT_MAX) {
                cout << "INF\t";
            } else {
                cout << dist[i][j] << "\t";
            }
        }
        cout << endl;
    }
}
