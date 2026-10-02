#include "floyd_warshall.h"
#include <iostream>
#include <vector>

using namespace std;

int main() {
    // Пример графа с 4 вершинами
    vector<vector<int>> graph = {
        {0, 3, INT_MAX, 5},
        {2, 0, INT_MAX, 4},
        {INT_MAX, 1, 0, INT_MAX},
        {INT_MAX, INT_MAX, 2, 0}
    };
    
    FloydWarshall fw(graph);
    fw.run();
    
    cout << "Матрица кратчайших путей:" << endl;
    fw.printMatrix();
    
    cout << "\nРасстояние от 0 до 2: ";
    int dist = fw.getDistance(0, 2);
    if (dist == INT_MAX) {
        cout << "Пути не существует" << endl;
    } else {
        cout << dist << endl;
    }
    
    cout << "Путь от 0 до 2: ";
    vector<int> path = fw.getPath(0, 2);
    for (int node : path) {
        cout << node << " ";
    }
    cout << endl;
    
    if (fw.hasNegativeCycles()) {
        cout << "Граф содержит отрицательные циклы!" << endl;
    } else {
        cout << "Отрицательных циклов не обнаружено" << endl;
    }
    
    return 0;
}