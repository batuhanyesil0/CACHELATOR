#include <iostream>
#include <fstream>
#include <string>
#include <vector>

struct CacheLine {
    bool valid;
    int tag;
    int data;
    CacheLine() : valid(false), tag(0), data(0) {}
};

bool cacheAccess(std::vector<CacheLine>& cache, int address, int blockSize, int cacheSize) {
    int index = (address / blockSize) % cacheSize;
    int tag = address / (blockSize * cacheSize);
    if (cache[index].valid && cache[index].tag == tag) {
        return true;
    } else {
        cache[index].valid = true;
        cache[index].tag = tag;
        return false;
    }
}

int main() {
    int cacheSize = 4;
    int blockSize = 4;
    std::vector<CacheLine> cache(cacheSize);

    int hits = 0;
    int misses = 0;

    std::ifstream input("trace.txt");
    std::string line;
    while (std::getline(input, line)) {
    int address = std::stoi(line, nullptr, 16);
    bool hit = cacheAccess(cache, address, blockSize, cacheSize);
    if (hit) {
        hits++;
        std::cout << "Address " << address << ": HIT" << std::endl;
    } else {
        misses++;
        std::cout << "Address " << address << ": MISS" << std::endl;
    }
}
    input.close();


    std::cout << "\nTotal hits: " << hits << std::endl;
    std::cout << "Total misses: " << misses << std::endl;
    std::cout << "Hit rate: " << (float)hits / (hits + misses) * 100 << "%" << std::endl;

    return 0;
}
