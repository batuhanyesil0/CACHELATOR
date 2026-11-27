#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <cstdint>
#include <cstdlib>   // exit() için
#include <sstream>   // istringstream için
#include <unordered_set>
#include <unordered_map>   // oracle araması için
#include <list>            // oracle LRU sırası için
using namespace std;

// Bir cache satırı. 'data' sildik — veri içeriğini modellemiyoruz.
struct CacheLine {
    bool dirtyBit = false;
    bool valid;
    uint64_t tag;
    CacheLine() : valid(false), tag(0) {}
};

// Cache yapılandırması: bu cache "nasıl bir cache".
struct CacheConfig {
    char writePolicy;   // 'B' = write-back, 'T' = write-through
    int cacheSize;      // toplam cache boyutu (byte)
    int numWays;
    int blockSize;
    string name;
};

// ----- Yardımcı / doğrulama fonksiyonları (Cache'ten ÖNCE tanımlı) -----
bool ispoweroftwo(int n) {
    return n > 0 && (n & (n - 1)) == 0;
}

void validateConfig(const CacheConfig& cfg) {
    if (!ispoweroftwo(cfg.blockSize)) {
        cerr << "Hata: blockSize 2'nin kuvveti olmali: " << cfg.blockSize << endl;
        exit(1);
    }
    if (cfg.numWays < 1) {
        cerr << "Hata: numWays en az 1 olmali: " << cfg.numWays << endl;
        exit(1);
    }
    int denominator = cfg.blockSize * cfg.numWays;
    if (cfg.cacheSize % denominator != 0) {
        cerr << "Hata: cacheSize (blockSize x numWays)'e tam bolunmeli: "
             << cfg.cacheSize << " / " << denominator << endl;
        exit(1);
    }
    int sets = cfg.cacheSize / denominator;
    if (!ispoweroftwo(sets)) {
        cerr << "Hata: hesaplanan numSets 2'nin kuvveti degil: " << sets << endl;
        exit(1);
    }
}

// ----- Cache sinifi -----
class Cache {
private:
    CacheConfig config;
    int numSets;
    vector<vector<CacheLine>> sets;
    vector<vector<vector<bool>>> lruMatrix;

    long long hits = 0;
    long long misses = 0;
    long long writebacks = 0;
    long long writeThroughs = 0;

    // --- 3C sinflandirma ---
    unordered_set<uint64_t> seenBlocks;   // compulsory: gorulen bloklar
    long long compulsory = 0;

    // --- oracle (fully-associative, capacity/conflict ayrimi icin) ---
    int oracleCapacity;                                            // kac blok tutabilir
    list<uint64_t> oracleList;                                     // LRU sirasi (on=yeni, arka=eski)
    unordered_map<uint64_t, list<uint64_t>::iterator> oracleMap;   // blok -> list'teki yeri
    long long capacity = 0;
    long long conflict = 0;

    void updateLRU(int index, int way);
    int  findLRUWay(int index);
    bool accessOracle(uint64_t blockNum);   // oracle erisimi: hit=true

public:
    Cache(const CacheConfig& cfg);
    bool access(uint64_t address, char op);
    void printStats() const;
};

// ----- Metot govdeleri -----

Cache::Cache(const CacheConfig& cfg) : config(cfg) {
    validateConfig(cfg);
    numSets = cfg.cacheSize / (cfg.blockSize * cfg.numWays);
    sets.resize(numSets, vector<CacheLine>(config.numWays));
    lruMatrix.resize(numSets, vector<vector<bool>>(config.numWays,
                                                   vector<bool>(config.numWays, false)));
    // oracle kapasitesi = gercek cache'in tuttugu toplam blok sayisi
    oracleCapacity = numSets * config.numWays;
}

void Cache::updateLRU(int index, int way) {
    for (int j = 0; j < config.numWays; j++) {
        lruMatrix[index][way][j] = true;
        lruMatrix[index][j][way] = false;
    }
}

int Cache::findLRUWay(int index) {
    for (int i = 0; i < config.numWays; i++) {
        bool isLRU = true;
        for (int j = 0; j < config.numWays; j++) {
            if (lruMatrix[index][i][j] == true) {
                isLRU = false;
                break;
            }
        }
        if (isLRU)
            return i;
    }
    return 0;
}

// Oracle erisimi: blok varsa hit (one tasi), yoksa miss (ekle, doluysa en eskiyi at).
// Donus: true = hit, false = miss
bool Cache::accessOracle(uint64_t blockNum) {
    auto it = oracleMap.find(blockNum);
    if (it != oracleMap.end()) {
        // HIT: bu blok oracle'da var. List'te one tasi (en yeni yap).
        oracleList.erase(it->second);              // eski yerinden cikar
        oracleList.push_front(blockNum);           // basa koy
        oracleMap[blockNum] = oracleList.begin();  // yeni yerini map'e yaz
        return true;
    }
    // MISS: blok oracle'da yok.
    if ((int)oracleList.size() >= oracleCapacity) {
        // doluysa en eskiyi (arkadakini) at
        uint64_t oldest = oracleList.back();
        oracleList.pop_back();
        oracleMap.erase(oldest);
    }
    // yeni blogu basa ekle
    oracleList.push_front(blockNum);
    oracleMap[blockNum] = oracleList.begin();
    return false;
}

bool Cache::access(uint64_t address, char op) {
    uint64_t blockNum = address / config.blockSize;
    int      index    = blockNum % numSets;
    uint64_t tag      = blockNum / numSets;

    // 1) HIT ara
    for (int i = 0; i < config.numWays; i++) {
        if (sets[index][i].valid && sets[index][i].tag == tag) {
            hits += 1;
            if (op == 'W') {
                if (config.writePolicy == 'B')
                    sets[index][i].dirtyBit = true;
                else if (config.writePolicy == 'T')
                    writeThroughs += 1;
            }
            updateLRU(index, i);
            accessOracle(blockNum);   // oracle'i da guncelle (senkron kalsin)
            return true;
        }
    }

    // Buraya geldiysek kesin MISS
    misses += 1;

    // --- 3C sinflandirma ---
    if (seenBlocks.find(blockNum) == seenBlocks.end()) {
        // ilk kez goruluyor -> COMPULSORY
        compulsory += 1;
        seenBlocks.insert(blockNum);
        accessOracle(blockNum);   // oracle'a da ekle (senkron)
    } else {
        // gorulmus ama miss -> oracle'a sor: capacity mi conflict mi?
        bool oracleHit = accessOracle(blockNum);
        if (oracleHit)
            conflict += 1;    // oracle tutuyordu, set yerlesimi kacirdi
        else
            capacity += 1;    // oracle bile tutamadi, kapasite yetmedi
    }

    // 2) Bos way ara
    for (int i = 0; i < config.numWays; i++) {
        if (sets[index][i].valid == false) {
            sets[index][i].valid = true;
            sets[index][i].tag = tag;
            sets[index][i].dirtyBit = (op == 'W' && config.writePolicy == 'B');
            if (op == 'W' && config.writePolicy == 'T')
                writeThroughs += 1;
            updateLRU(index, i);
            return false;
        }
    }

    // 3) Set dolu -> evict
    int deleted = findLRUWay(index);
    if (config.writePolicy == 'B' && sets[index][deleted].dirtyBit)
        writebacks += 1;
    sets[index][deleted].tag = tag;
    sets[index][deleted].dirtyBit = (op == 'W' && config.writePolicy == 'B');
    if (op == 'W' && config.writePolicy == 'T')
        writeThroughs += 1;
    updateLRU(index, deleted);
    return false;
}

void Cache::printStats() const {
    long long total = hits + misses;
    cout << "Total accesses: " << total << endl;
    cout << "Total hits: " << hits << endl;
    cout << "Total misses: " << misses << endl;
    if (total > 0)
        cout << "Hit rate: " << (double)hits / total * 100 << "%" << endl;
    else
        cout << "Hit rate: N/A (no accesses)" << endl;
    cout << "Writebacks: " << writebacks << endl;
    cout << "WriteThroughs: " << writeThroughs << endl;
    // --- 3C dokumu ---
    cout << "--- 3C miss breakdown ---" << endl;
    cout << "Compulsory: " << compulsory << endl;
    cout << "Capacity:   " << capacity << endl;
    cout << "Conflict:   " << conflict << endl;

    // --- AMAT ve MPKI ---
    // Temsili sabitler (gercek donanimdan olculmedi, varsayim):
    const double HIT_TIME = 1.0;        // cycle (L1 varsayimi)
    const double MISS_PENALTY = 100.0;  // cycle (tek seviye, ana bellege gidis varsayimi)
    if (total > 0) {
        double missRate = (double)misses / total;
        double amat = HIT_TIME + missRate * MISS_PENALTY;
        // MPKI proxy: gercek instruction count yok, per-access yaklasim
        double mpkiProxy = ((double)misses / total) * 1000.0;
        cout << "--- Performans (temsili) ---" << endl;
        cout << "AMAT: " << amat << " cycle (hit_time=" << HIT_TIME
             << ", miss_penalty=" << MISS_PENALTY << " varsayimiyla)" << endl;
        cout << "MPKI (proxy, per-access): " << mpkiProxy << endl;
    }
}

// ----- main -----
int main(int argc, char* argv[]) {
    // Calistirilabilirin bulundugu dizin (varsayilan trace.txt icin)
    string exePath(argv[0]);
    size_t lastSlash = exePath.find_last_of("/\\");
    string exeDir = (lastSlash != string::npos) ? exePath.substr(0, lastSlash) : ".";

    // ----- Mod secimi -----
    // ./cache_sim                                              -> exeDir/trace.txt (varsayilan)
    // ./cache_sim --file <trace.txt>                           -> verilen dosya
    // ./cache_sim --pin <pin_yolu> <pintool_yolu> <program>    -> Pin ile trace uret
    //
    // Pin modu OPSIYONEL: Pin kurulu degilse --file ile elle trace de kullanilabilir.
    // Cekirdek (cache simulasyonu) Pin'den bagimsizdir.
    string traceFile = exeDir + "/trace.txt";   // varsayilan

    if (argc >= 2 && string(argv[1]) == "--file") {
        if (argc < 3) {
            cerr << "Kullanim: " << argv[0] << " --file <trace.txt>" << endl;
            return 1;
        }
        traceFile = argv[2];
    }
    else if (argc >= 2 && string(argv[1]) == "--pin") {
        if (argc < 5) {
            cerr << "Kullanim: " << argv[0]
                 << " --pin <pin_yolu> <pintool_yolu> <program>" << endl;
            cerr << "Ornek: " << argv[0]
                 << " --pin ./pin-3.30/pin ./pin-3.30/.../pinatrace.so ./my_program" << endl;
            return 1;
        }
        string pinPath = argv[2];
        string pintool = argv[3];
        string program = argv[4];

        // Pin'i subprocess olarak cagir. system() blokladigi icin
        // Pin bitene kadar burada bekler, sonra pinatrace.out hazir olur.
        string cmd = pinPath + " -t " + pintool + " -- " + program;
        cout << "[Pin calistiriliyor] " << cmd << endl;
        int ret = system(cmd.c_str());
        if (ret != 0) {
            cerr << "Hata: Pin calistirilamadi (cikis kodu " << ret << ")." << endl;
            cerr << "Pin yolu / pintool / program dogru mu, pinatrace derlendi mi?" << endl;
            return 1;
        }
        // pinatrace pintool ciktiyi 'pinatrace.out' dosyasina yazar.
        traceFile = "pinatrace.out";
    }
    else if (argc >= 2) {
        cerr << "Bilinmeyen mod: " << argv[1] << endl;
        cerr << "Kullanim:" << endl;
        cerr << "  " << argv[0] << "                                        (varsayilan trace.txt)" << endl;
        cerr << "  " << argv[0] << " --file <trace.txt>" << endl;
        cerr << "  " << argv[0] << " --pin <pin_yolu> <pintool_yolu> <program>" << endl;
        return 1;
    }
    // argc < 2 ise: varsayilan traceFile (exeDir/trace.txt) kullanilir

    vector<CacheConfig> configs = {
       {'B', 16, 1, 4, "Direct-mapped (WB)"},
       {'T', 16, 1, 4, "Direct-mapped (WT)"},
       {'B', 32, 2, 4, "2-way (WB)"},
       {'T', 32, 2, 4, "2-way (WT)"},
       {'B', 64, 4, 4, "4-way (WB)"},
       {'T', 64, 4, 4, "4-way (WT)"},
       {'B', 64, 16, 4, "Fully-associative (WB)"},
       {'T', 64, 16, 4, "Fully-associative (WT)"}
    };

    for (const CacheConfig& config : configs) {
        cout << "========================" << endl;
        cout << "Konfigurasyon: " << config.name << endl;
        cout << "========================" << endl;

        Cache cache(config);

        ifstream input(traceFile);
        if (!input.is_open()) {
            cerr << "Hata: trace.txt acilamadi: " << traceFile << endl;
            return 1;
        }
        string line;
        while (getline(input, line)) {
            if (line.empty() || line[0] == '#') continue;
            istringstream iss(line);
            char op;
            string addrStr;
            iss >> op >> addrStr;
            uint64_t address = stoull(addrStr, nullptr, 16);
            bool hit = cache.access(address, op);
            cout << op << " 0x" << hex << uppercase << address
                 << dec << ": " << (hit ? "HIT" : "MISS") << endl;
        }
        input.close();

        cache.printStats();
        cout << endl;
    }
    return 0;
}
